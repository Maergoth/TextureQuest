"""One verified VPK cache for direct release downloads and explicit installs.

Legacy prepared files move only within this app's data directory. Unknown
downloads, journals and interrupted bytes are preserved for recovery.
"""
from __future__ import annotations

from contextlib import ExitStack, nullcontext
import copy
import hashlib
import os
from pathlib import Path
import re
import shutil
import uuid
from urllib.parse import urlsplit
from urllib.request import Request, urlopen

from texture_swapper import core
import texturequest_release as release
from texturequest_custody import DirectoryCustody
from texturequest_fast_install import FastFileCustody as FileCustody, DownloadCustody
import texturequest_fast_install as fast_install
from texturequest_legacy import LEGACY_ARCHIVES
from texturequest_progress import OverallProgress

SCHEMA = "texturequest-direct-prepared-v1"
REPOSITORY = "https://github.com/Maergoth/TextureQuest"
CHECKPOINT = 8 * 1024**2
# Published immutable containers. 1.1.1 reused the 1.1.0 texture package.
# Their original release manifests and delivery proofs remain preserved.
LEGACY_PACKAGES = (
    {"version": "1.0.0", "name": "Texture-Pack.zip", "bytes": 27763745032,
     "sha256": "9501071759d57c26175ee9245b3f5dfee4d4cee7f19a57c039289d09d2041caf"},
    {"version": "1.1.0", "name": "TextureQuest-Update-1.1.0.zip", "bytes": 1041986839,
     "sha256": "7c80130a1cdb33b7fd44ac9d770249b4fe4781ef81e4237b53cffde023679f02"},
    {"version": "1.2.0", "name": "TextureQuest-Update-1.2.0.zip", "bytes": 1624207428,
     "sha256": "1a5fca080a48949d98a73833c3b4dba849e99e45a9e5069377890cf2aa6bd375"},
)


def _version(value):
    if not isinstance(value, str) or not re.fullmatch(r"1\.(?:0|[1-9][0-9]{0,3})\.(?:0|[1-9][0-9]{0,3})", value):
        raise ValueError("Unsupported direct archive version")
    return tuple(map(int, value.split(".")))


def validate_manifest(value, pinned=True):
    if not isinstance(value, dict) or set(value) != {"schema", "version", "texture_version", "texture_count", "archives"}:
        raise ValueError("Unsupported direct archive manifest")
    if value["schema"] != "texturequest-release-v3" or _version(value["version"]) < (1, 3, 0):
        raise ValueError("Unsupported direct archive release")
    if not (1, 0, 0) <= _version(value["texture_version"]) <= _version(value["version"]):
        raise ValueError("Invalid direct texture version")
    rows = value["archives"]
    if not isinstance(rows, list) or not 1 <= len(rows) <= core.MAX_ARCHIVES or (pinned and len(rows) != 310):
        raise ValueError("Invalid complete direct archive inventory")
    if type(value["texture_count"]) is not int or value["texture_count"] <= 0:
        raise ValueError("Invalid direct texture count")
    names = set()
    for row in rows:
        if not isinstance(row, dict) or set(row) != {"name", "bytes", "sha256", "entries", "url"}:
            raise ValueError("Invalid direct archive identity")
        name = core._name(row["name"])
        if (name.casefold() in names or type(row["bytes"]) is not int or not 16 <= row["bytes"] < 2**31
                or row["bytes"] > core.MAX_ARCHIVE_BYTES or type(row["entries"]) is not int or row["entries"] < 1
                or not isinstance(row["sha256"], str) or not re.fullmatch(r"[0-9a-f]{64}", row["sha256"])):
            raise ValueError("Invalid direct archive metadata")
        names.add(name.casefold())
        prefix = REPOSITORY + "/releases/download/v"
        url = row["url"]
        if not isinstance(url, str) or not url.startswith(prefix) or not url.endswith("/" + name):
            raise ValueError("Unexpected direct archive URL")
        tag = url[len(prefix):-(len(name) + 1)]
        if not (1, 3, 0) <= _version(tag) <= _version(value["version"]) or url != prefix + tag + "/" + name:
            raise ValueError("Invalid immutable direct archive URL")
    if sum(row["bytes"] for row in rows) > core.MAX_TOTAL_BYTES:
        raise ValueError("Direct pack exceeds the supported size")
    return value


def _identity(row):
    return {key: row[key] for key in ("name", "bytes", "sha256", "entries")}


def _matches(path, row, pause=None):
    path = core._plain(path)
    if not path.is_file() or path.stat().st_size != row["bytes"]:
        return False
    with path.open("rb") as stream:
        return release.hash_stream(stream, pause=pause) == row["sha256"]


def _checked_archive(path, row, pause=None):
    if not _matches(path, row, pause):
        raise ValueError("Archive checksum differs: " + row["name"])
    release.check_pause(pause)


class DirectSwapper(core.Swapper):
    """Inherit game locks/staging/install guards; specialize only cache custody."""
    trusted_manifest = None

    def trusted_prepared(self, value, manifest):
        """Recognize an existing official ready receipt without moving or rehashing."""
        try:
            validate_manifest(manifest)
            saved = self._prepared(value)
            wanted = {row['name'].casefold(): (row['bytes'], row['sha256']) for row in manifest['archives']}
            rows = saved['archives']
            if len(rows) != len(wanted):
                return False
            actual = {row['name'].casefold(): (row['bytes'], row['sha256']) for row in rows}
            if actual != wanted or len(actual) != len(rows):
                return False
            return all(core._plain(row['path']).is_file() and Path(row['path']).stat().st_size == row['bytes'] for row in rows)
        except (OSError, ValueError, KeyError, TypeError):
            return False

    def saved_ready(self, manifest):
        pointer = core._plain(self.state_dir / 'texturequest-prepared.json', self.state_dir)
        try:
            value = core._load(pointer)
        except (OSError, ValueError, KeyError, TypeError):
            return None
        return value if self.trusted_prepared(value, manifest) else None

    def _prepared(self, value):
        if not isinstance(value, dict) or value.get("schema") != SCHEMA:
            return super()._prepared(value)
        saved = core._load(core._plain(self.state_dir / "direct-prepared.json", self.state_dir))
        if saved != value:
            raise ValueError("Direct prepared receipt changed")
        manifest = validate_manifest(saved.get("release", {}))
        rows = saved.get("archives")
        expected = [{**_identity(row), "path": str(self.state_dir / "downloads" / row["name"])} for row in manifest["archives"]]
        if (rows != expected or saved.get("total_bytes") != sum(row["bytes"] for row in expected)
                or saved.get("release_sha256") != core._digest(manifest)
                or saved.get("id") != core._digest([_identity(row) for row in manifest["archives"]])[:32]):
            raise ValueError("Direct prepared inventory changed")
        for row in rows:
            if core._plain(row["path"], self.state_dir / "downloads") != self.state_dir / "downloads" / row["name"]:
                raise ValueError("Direct prepared path changed")
        return saved

    def _assert_background_inactive(self):
        # Recheck beneath the operation's helper lock, after any stale UI view.
        # Import here: the loader uses DirectSwapper only for managed-PAK status.
        try:
            from texturequest_sideload import LoaderManager
        except ModuleNotFoundError as error:
            if error.name == "texturequest_sideload":
                return  # Historical builds may omit the optional loader entirely.
            raise
        if LoaderManager(self.state_dir).cache_in_use():
            raise RuntimeError("Uninstall the background loader and close its active game sessions before replacing PAK files.")
        from texturequest_graphics import GraphicsManager
        if GraphicsManager(self.state_dir).cache_in_use():
            raise RuntimeError("Uninstall Graphics hook and close EQ2 before replacing PAK files.")

    def install(self, prepared, game_dir, progress=None, overall_progress=None):
        if (prepared.get("schema") == SCHEMA or self.trusted_manifest is not None
                and self.trusted_prepared(prepared, self.trusted_manifest)):
            return fast_install.install(self, prepared, game_dir, progress, overall_progress=overall_progress)
        with core._lock(self.state_dir / "sideload/helper-owner.lock"):
            self._assert_background_inactive()
            return super().install(prepared, game_dir, progress)

    def status(self, game_dir, verify=False):
        return super().status(game_dir) if verify else fast_install.status(self, game_dir)


def _resume_journal(path, row, partial):
    if not path.exists():
        if partial.exists():
            raise ValueError("Unknown interrupted download is preserved: " + partial.name)
        value = {"schema": "texturequest-direct-download-v1", "archive": dict(row),
                 "bytes_written": 0, "prefix_sha256": hashlib.sha256(b"").hexdigest()}
        core._atomic(path, value)
        return value
    if path.stat().st_size > core.MAX_MANIFEST_BYTES:
        raise ValueError("Download journal exceeds supported bounds")
    value = core._json(path.read_bytes())
    if (not isinstance(value, dict) or set(value) != {"schema", "archive", "bytes_written", "prefix_sha256"}
            or value["schema"] != "texturequest-direct-download-v1" or value["archive"] != row
            or type(value["bytes_written"]) is not int or not 0 <= value["bytes_written"] <= row["bytes"]
            or not isinstance(value["prefix_sha256"], str) or not re.fullmatch(r"[0-9a-f]{64}", value["prefix_sha256"])):
        raise ValueError("Unknown or changed download journal is preserved")
    if not partial.exists() and value["bytes_written"]:
        raise ValueError("Journaled download bytes are missing")
    if partial.exists() and (not partial.is_file() or partial.stat().st_size != value["bytes_written"]):
        raise ValueError("Interrupted download length changed; preserved for recovery")
    return value


def _native_id(value):
    return (type(value) is dict and set(value) == {"volume", "file_id"}
            and type(value["volume"]) is int and 0 <= value["volume"] < 2**32
            and type(value["file_id"]) is int and 0 < value["file_id"] < 2**64)


def _retirement_record(folder, row, old):
    path = core._plain(folder / (row["name"] + ".retirement.json"), folder)
    if not path.exists():
        return None
    value = core._load(path)
    fields = {"schema", "target", "partial", "retired", "old_archive", "new_archive", "old_file_id", "new_file_id",
              "phase", "updated_at", "record_sha256"}
    retired = core._plain(value.get("retired", ""), folder)
    pattern = re.escape("." + row["name"] + ".retired-") + r"[0-9a-f]{32}\.vpk"
    if (set(value) != fields or value["schema"] != "texturequest-direct-retirement-v1"
            or old is None or value["old_archive"] != _identity(old) or value["new_archive"] != row
            or value["target"] != str(folder / row["name"])
            or value["partial"] != str(folder / (row["name"] + ".pending"))
            or retired.parent != folder or not re.fullmatch(pattern, retired.name)
            or not _native_id(value["old_file_id"]) or not _native_id(value["new_file_id"])
            or value["phase"] not in {"prepared", "old-retired", "new-committed"}):
        raise ValueError("Unknown cache retirement is preserved")
    return value


def _complete_retirement(folder, journal, binding, new, old, row, pause):
    new.checked(row, pause)
    if new.identity() != binding["new_file_id"]:
        raise ValueError("Committed cache identity changed")
    if old is not None:
        old.checked(binding["old_archive"], pause)
        if old.identity() != binding["old_file_id"]:
            raise ValueError("Retired cache identity changed")
        old.remove()
    receipt = core._plain(folder.parent / "cache-commits" / (uuid.uuid4().hex + ".json"), folder.parent)
    core._atomic(receipt, dict(binding, phase="complete", completed_at=core._now()))
    journal.unlink()


def _commit_download(folder, partial, target, row, old, notify, pause, new_owner=None):
    with DirectoryCustody(folder) as directories, ExitStack() as leases:
        held_new = new_owner if new_owner is not None else leases.enter_context(FileCustody(partial))
        held_new.checked(row, pause)
        held_old = leases.enter_context(FileCustody(target)) if target.exists() else None
        if held_old is not None:
            if old is None:
                raise ValueError("New cache destination appeared; preserved")
            held_old.checked(old, pause)
            retired = core._plain(folder / ("." + row["name"] + ".retired-" + uuid.uuid4().hex + ".vpk"), folder)
            journal = core._plain(folder / (row["name"] + ".retirement.json"), folder)
            if journal.exists():
                raise ValueError("Unresolved cache retirement is preserved")
            binding = {"schema": "texturequest-direct-retirement-v1", "target": str(target), "partial": str(partial),
                       "retired": str(retired), "old_archive": _identity(old), "new_archive": dict(row),
                       "old_file_id": held_old.identity(), "new_file_id": held_new.identity(), "phase": "prepared"}
            core._save(journal, binding)
            release.check_pause(pause)
            notify("Retiring saved " + row["name"], 0, 1)
            held_old.rename_new(retired, directories)
            binding["phase"] = "old-retired"
            core._save(journal, binding)
        release.check_pause(pause)
        notify("Committing downloaded " + row["name"], 0, 1)
        held_new.rename_new(target, directories)
        if held_old is not None:
            binding["phase"] = "new-committed"
            core._save(journal, binding)
            _complete_retirement(folder, journal, binding, held_new, held_old, row, pause)


def _recover_retirement(folder, row, old, notify, pause):
    binding = _retirement_record(folder, row, old)
    if binding is None:
        return
    target, partial, retired = map(Path, (binding["target"], binding["partial"], binding["retired"]))
    journal = folder / (row["name"] + ".retirement.json")
    with DirectoryCustody(folder) as directories, ExitStack() as leases:
        held_target = leases.enter_context(FileCustody(target)) if target.exists() else None
        held_retired = leases.enter_context(FileCustody(retired)) if retired.exists() else None
        if held_target is not None and held_target.identity() == binding["new_file_id"]:
            _complete_retirement(folder, journal, binding, held_target, held_retired, row, pause)
            return
        if held_target is not None:
            if held_target.identity() != binding["old_file_id"] or held_retired is not None:
                raise ValueError("Unrecognized cache destination preserved during recovery")
            held_target.checked(old, pause)
        elif held_retired is None:
            raise ValueError("Journaled old cache is missing; preserved for recovery")
        if held_retired is not None:
            held_retired.checked(old, pause)
            if held_retired.identity() != binding["old_file_id"]:
                raise ValueError("Retired cache identity differs")
        held_new = leases.enter_context(FileCustody(partial))
        held_new.checked(row, pause)
        if held_new.identity() != binding["new_file_id"]:
            raise ValueError("Journaled new cache identity differs")
        if held_target is not None:
            release.check_pause(pause)
            held_target.rename_new(retired, directories)
            held_retired = held_target
            binding["phase"] = "old-retired"
            core._save(journal, binding)
        release.check_pause(pause)
        notify("Committing downloaded " + row["name"], 0, 1)
        held_new.rename_new(target, directories)
        binding["phase"] = "new-committed"
        core._save(journal, binding)
        _complete_retirement(folder, journal, binding, held_new, held_retired, row, pause)


def _download(row, folder, old=None, notify=None, pause=None, opener=urlopen):
    notify = notify or (lambda *_: None)
    folder = core._plain(folder)
    target = core._plain(folder / row["name"], folder)
    partial = core._plain(folder / (row["name"] + ".pending"), folder)
    journal = core._plain(folder / (row["name"] + ".pending.json"), folder)
    _recover_retirement(folder, row, old, notify, pause)
    if target.exists():
        if _matches(target, row, pause):
            return target
        if old is None or not _matches(target, old, pause):
            raise ValueError("Unrecognized cached archive is preserved: " + target.name)
    saved = _resume_journal(journal, row, partial)
    with DownloadCustody(partial) as owner:
        digest = hashlib.sha256()
        if saved["bytes_written"]:
            with nullcontext(owner.stream) as stream:
                while chunk := stream.read(core.CHUNK):
                    release.check_pause(pause)
                    digest.update(chunk)
        if digest.hexdigest() != saved["prefix_sha256"]:
            raise ValueError("Interrupted download prefix changed; preserved for recovery")
        count = saved["bytes_written"]
        if shutil.disk_usage(folder).free < row["bytes"] - count + 64 * core.CHUNK:
            raise OSError("Not enough free space for " + row["name"])
        if count < row["bytes"]:
            headers = {"User-Agent": "TextureQuest/1.3.1", "Accept-Encoding": "identity"}
            if count:
                headers["Range"] = f"bytes={count}-"
            with opener(Request(row["url"], headers=headers), timeout=45) as response:
                final = urlsplit(response.geturl())
                if (final.scheme != "https" or final.username or final.password or final.port not in (None, 443)
                        or final.hostname not in {"github.com", "release-assets.githubusercontent.com", "objects.githubusercontent.com"}):
                    raise ValueError("Unexpected direct archive response")
                status = response.getcode()
                if status == 206:
                    if response.headers.get("Content-Range") != f"bytes {count}-{row['bytes'] - 1}/{row['bytes']}":
                        raise ValueError("Direct archive range differs")
                    received = count
                elif status == 200:
                    received = 0
                else:
                    raise OSError("Archive server returned HTTP " + str(status))
                length = response.headers.get("Content-Length")
                if length is not None and int(length) != row["bytes"] - received:
                    raise ValueError("Direct archive response length differs")
                if not received and count:
                    with nullcontext(owner.stream) as prefix:
                        prefix.seek(0)
                        left = count
                        while left:
                            release.check_pause(pause)
                            expected = prefix.read(min(core.CHUNK, left))
                            if response.read(len(expected)) != expected:
                                raise ValueError("Server resume prefix differs; saved bytes preserved")
                            left -= len(expected)
                with nullcontext(owner.stream) as output:
                    output.seek(count)
                    while count < row["bytes"]:
                        release.check_pause(pause)
                        maximum = min(CHECKPOINT, row["bytes"] - count)
                        chunk = response.read(maximum)
                        if not chunk:
                            raise OSError("Download interrupted; resume will use the verified prefix")
                        if len(chunk) > maximum:
                            raise ValueError("Archive response exceeded the requested bound")
                        output.write(chunk)
                        output.flush()
                        os.fsync(output.fileno())
                        digest.update(chunk)
                        count += len(chunk)
                        saved.update(bytes_written=count, prefix_sha256=digest.hexdigest())
                        core._atomic(journal, saved)
                        notify("Downloading " + row["name"], count, row["bytes"])
                    if response.read(1):
                        raise ValueError("Archive response exceeds advertised length")
        owner.remember_verified(row, digest.hexdigest())
        _commit_download(folder, partial, target, row, old, notify, pause, new_owner=owner)
        journal.unlink()
        return target


def _migration(swapper, source, row, notify, pause):
    """A journaled same-drive move, including exact duplicate coalescing."""
    folder = core._plain(swapper.state_dir / "downloads", swapper.state_dir)
    target = core._plain(folder / row["name"], folder)
    source = core._plain(source, swapper.state_dir / "prepared")
    journal = core._plain(folder / (row["name"] + ".migration.json"), folder)
    with DirectoryCustody(source.parent, folder) as directories, ExitStack() as leases:
        held_source = leases.enter_context(FileCustody(source))
        held_source.checked(row, pause)
        held_target = leases.enter_context(FileCustody(target)) if target.exists() else None
        if held_target is not None:
            held_target.checked(row, pause)
        binding = {"schema": "texturequest-direct-migration-v2", "source": str(source), "target": str(target),
                   "archive": _identity(row), "source_file_id": held_source.identity(),
                   "target_file_id": held_target.identity() if held_target is not None else held_source.identity()}
        if journal.exists():
            if journal.stat().st_size > core.MAX_MANIFEST_BYTES or core._json(journal.read_bytes()) != binding:
                raise ValueError("Unknown cache migration is preserved")
        else:
            core._atomic(journal, binding)
        release.check_pause(pause)
        notify("Moving saved " + row["name"], 0, 1)
        if held_target is None:
            held_source.rename_new(target, directories)
        else:
            # Keep the existing verified target in custody. Only the exact
            # duplicate source is deleted; the target is never replaced.
            held_source.remove()
        receipt = core._plain(swapper.state_dir / "cache-migrations" / (uuid.uuid4().hex + ".json"), swapper.state_dir)
        core._atomic(receipt, dict(binding, completed_at=core._now()))
        journal.unlink()


def _recover_migrations(swapper, rows, pause):
    folder = core._plain(swapper.state_dir / "downloads", swapper.state_dir)
    for row in rows:
        journal = core._plain(folder / (row["name"] + ".migration.json"), folder)
        if not journal.exists():
            continue
        if journal.stat().st_size > core.MAX_MANIFEST_BYTES:
            raise ValueError("Unknown migration journal size")
        saved = core._json(journal.read_bytes())
        target = core._plain(folder / row["name"], folder)
        if (not isinstance(saved, dict) or set(saved) != {"schema", "source", "target", "archive", "source_file_id", "target_file_id"}
                or saved["schema"] != "texturequest-direct-migration-v2" or saved["archive"] != _identity(row)
                or saved["target"] != str(target)):
            raise ValueError("Unknown migration journal is preserved")
        source = core._plain(saved["source"], swapper.state_dir / "prepared")
        if (source.name != row["name"] or source.parent.parent != swapper.state_dir / "prepared"
                or not re.fullmatch(r"[0-9a-f]{32}", source.parent.name)):
            raise ValueError("Migration source path differs")
        owner = core.Swapper._prepared(swapper, core._load(source.parent / "manifest.json"))
        if not any(_identity(item) == _identity(row) and item["path"] == str(source) for item in owner["archives"]):
            raise ValueError("Migration is not bound to a recognized prepared archive")
        if source.exists():
            _migration(swapper, source, row, lambda *_: None, pause)
        else:
            with DirectoryCustody(source.parent, folder), FileCustody(target) as held:
                held.checked(row, pause)
                if held.identity() != saved["target_file_id"]:
                    raise ValueError("Migrated cache file identity changed")
                receipt = core._plain(swapper.state_dir / "cache-migrations" / (uuid.uuid4().hex + ".json"), swapper.state_dir)
                core._atomic(receipt, dict(saved, completed_at=core._now()))
                journal.unlink()


def _remove_exact_download(path, package, pause):
    """Remove only the same verified file, with its ancestors kept in custody."""
    path = core._plain(path)
    with DirectoryCustody(path.parent), FileCustody(path) as held:
        if not held.matches(package, pause):
            return False
        release.check_pause(pause)
        held.remove()
    return True

def _obsolete_archives(swapper, prepared, previous):
    """Recognize only published historical bytes and exact app receipt paths."""
    candidates = {}
    desired = {row["name"].casefold() for row in prepared["archives"]}
    legacy = core._plain(swapper.state_dir / "prepared", swapper.state_dir)
    if legacy.exists():
        children = list(legacy.iterdir())
        if len(children) > core.MAX_ARCHIVES:
            raise ValueError("Too many historical caches; preserved for review")
        for child in sorted(children):
            if not re.fullmatch(r"[0-9a-f]{32}", child.name):
                continue
            try:
                child = core._plain(child, legacy)
                receipt = core._plain(child / "manifest.json", child)
                saved = core.Swapper._prepared(swapper, core._load(receipt))
                receipt_bytes = receipt.read_bytes()
                if core._json(receipt_bytes) != saved:
                    continue
                owner = {"path": str(receipt), "bytes": len(receipt_bytes),
                         "sha256": hashlib.sha256(receipt_bytes).hexdigest()}
                for row in saved["archives"]:
                    key = (row["name"].casefold(), row["bytes"], row["sha256"])
                    if key not in LEGACY_ARCHIVES or not Path(row["path"]).exists():
                        continue
                    path = core._plain(row["path"], child)
                    candidate = (path, _identity(row), "legacy-prepared-vpk", owner)
                    prior = candidates.get(str(path).casefold())
                    if prior is not None and prior[1] != candidate[1]:
                        raise ValueError("Ambiguous historical cache inventory")
                    candidates[str(path).casefold()] = candidate
            except (ValueError, OSError, KeyError, TypeError):
                continue  # Unknown/custom receipts and pixels remain untouched.
    if previous is not None:
        for row in previous["archives"]:
            if row["name"].casefold() in desired:
                continue
            path = core._plain(row["path"], swapper.state_dir / "downloads")
            if path != swapper.state_dir / "downloads" / row["name"]:
                raise ValueError("Historical direct cache path changed")
            if path.exists():
                candidates[str(path).casefold()] = (path, _identity(row), "obsolete-direct-vpk", None)
    if len(candidates) > core.MAX_ARCHIVES:
        raise ValueError("Too many historical archives; preserved for review")
    return list(candidates.values())


def _retire_legacy_downloads(swapper, prepared, notify, pause, idle, previous=None):
    """Called only while operation, helper and download locks remain held."""
    downloads = core._plain(swapper.state_dir / "downloads", swapper.state_dir)
    candidates = [(downloads / item["version"] / item["name"], item) for item in LEGACY_PACKAGES]
    candidates.insert(0, (downloads / "Texture-Pack.zip", LEGACY_PACKAGES[0]))
    candidates = [(path, item) for path, item in candidates if path.exists() or path.is_symlink()]
    obsolete = _obsolete_archives(swapper, prepared, previous)
    if not candidates and not obsolete:
        return
    with DirectoryCustody(downloads), ExitStack() as retained:
        # Complete new custody must exist and be verified before reclaiming bytes.
        swapper._prepared(prepared)
        if core._load(swapper.state_dir / "texturequest-prepared.json") != prepared:
            raise ValueError("Current direct cache pointer changed")
        for pointer in (swapper.state_dir / "direct-prepared.json", swapper.state_dir / "texturequest-prepared.json"):
            owner = retained.enter_context(FileCustody(pointer))
            owner.stream.seek(0)
            raw = owner.stream.read(core.MAX_MANIFEST_BYTES + 1)
            if len(raw) > core.MAX_MANIFEST_BYTES or core._json(raw) != prepared:
                raise ValueError("Current direct cache receipt changed")
        for row in prepared["archives"]:
            idle()
            notify("Verifying ready cache before reclaiming downloads: " + row["name"], 0, 1)
            held = retained.enter_context(FileCustody(row["path"]))
            held.checked(row, pause)
        result = {"schema": "texturequest-legacy-download-retirement-v1", "prepared_id": prepared["id"],
                  "release_sha256": prepared["release_sha256"], "created_at": core._now(), "files": []}
        receipt = core._plain(swapper.state_dir / "cache-migrations" / (uuid.uuid4().hex + "-legacy-downloads.json"), swapper.state_dir)
        core._save(receipt, result)
        for path, row, kind, owner in obsolete:
            idle()
            release.check_pause(pause)
            record = dict(row, path=str(path), kind=kind, status="planned")
            result["files"].append(record)
            core._save(receipt, result)
            try:
                with DirectoryCustody(path.parent), ExitStack() as old_leases:
                    if owner is not None:
                        held_receipt = old_leases.enter_context(FileCustody(owner["path"]))
                        if not held_receipt.matches(owner, pause):
                            raise ValueError("Historical receipt changed")
                    held = old_leases.enter_context(FileCustody(path))
                    held.checked(row, pause)
                    notify("Reclaiming verified obsolete cache: " + row["name"], 0, 1)
                    release.check_pause(pause)
                    held.remove()
                record["status"] = "removed"
            except (OSError, ValueError) as error:
                record.update(status="preserved-unknown", reason=type(error).__name__)
            core._save(receipt, result)
        with ExitStack() as locks:
            # Old installers lock their version directories; take the same locks.
            for folder in sorted({path.parent for path, _ in candidates} - {downloads}):
                locks.enter_context(core._lock(core._plain(folder / "download.lock", downloads)))
            for path, item in candidates:
                idle()
                release.check_pause(pause)
                record = {**item, "path": str(path), "status": "planned"}
                result["files"].append(record)
                core._save(receipt, result)
                notify("Reclaiming verified legacy download: " + item["name"], 0, 1)
                try:
                    path = core._plain(path, downloads)
                    if not path.is_file():
                        record["status"] = "preserved-unknown"
                    else:
                        record["status"] = "removed" if _remove_exact_download(path, item, pause) else "preserved-mismatch"
                except (OSError, ValueError) as error:
                    # Busy, linked, changed or foreign bytes never justify removal.
                    record.update(status="preserved-unknown", reason=type(error).__name__)
                core._save(receipt, result)
        result.update(completed_at=core._now(), removed_bytes=sum(row["bytes"] for row in result["files"] if row["status"] == "removed"))
        core._save(receipt, result)



def prepare_direct(swapper, manifest, progress=None, pause=None, opener=urlopen, cache_in_use=None,
                   overall_progress=None):
    """Verify/download ready VPKs. Game files change only through explicit install."""
    manifest = copy.deepcopy(manifest)
    validate_manifest(manifest)
    legacy_notify = progress or (lambda *_: None)
    overall = OverallProgress(manifest["archives"], overall_progress)
    def notify(message, done, total):
        legacy_notify(message, done, total)
        overall.emit(message)
    if cache_in_use is not None and not callable(cache_in_use):
        raise ValueError("Invalid cache ownership guard")
    def idle():
        if cache_in_use is not None and cache_in_use():
            raise RuntimeError("Stop sideload before changing the saved texture cache")
    idle()
    folder = core._plain(swapper.state_dir / "downloads", swapper.state_dir)
    with ExitStack() as locks:
        locks.enter_context(core._lock(swapper.state_dir / "operation.lock"))
        locks.enter_context(core._lock(swapper.state_dir / "sideload/helper-owner.lock"))
        idle()
        folder.mkdir(parents=True, exist_ok=True)
        locks.enter_context(core._lock(folder / "download.lock"))
        _recover_migrations(swapper, manifest["archives"], pause)
        previous = {}
        previous_direct = None
        receipt = core._plain(swapper.state_dir / "direct-prepared.json", swapper.state_dir)
        if receipt.exists():
            old = swapper._prepared(core._load(receipt))
            previous_direct = old
            previous = {row["name"].casefold(): row for row in old["archives"]}
        legacy = core._plain(swapper.state_dir / "prepared", swapper.state_dir)
        if legacy.exists():
            children = list(legacy.iterdir())
            if len(children) > core.MAX_ARCHIVES:
                raise ValueError("Too many saved cache directories; preserve them for review")
            for child in sorted(children):
                release.check_pause(pause)
                if not re.fullmatch(r"[0-9a-f]{32}", child.name):
                    continue
                child = core._plain(child, legacy)
                old_receipt = core._plain(child / "manifest.json", legacy)
                if not old_receipt.is_file():
                    continue
                try:
                    old = core.Swapper._prepared(swapper, core._load(old_receipt))
                except (ValueError, OSError, KeyError, TypeError):
                    continue  # Unknown receipts are not custody evidence.
                wanted = {row["name"].casefold(): row for row in manifest["archives"]}
                matches = [row for row in old["archives"] if row["name"].casefold() in wanted and
                           _identity(row) == _identity(wanted[row["name"].casefold()]) and Path(row["path"]).exists()]
                if not matches:
                    continue
                # Verify the complete recognized legacy inventory before any move.
                for row in old["archives"]:
                    if Path(row["path"]).exists():
                        notify("Verifying saved " + row["name"], 0, 1)
                        _checked_archive(row["path"], row, pause)
                    elif not _matches(folder / row["name"], row, pause):
                        raise ValueError("Saved legacy inventory is incomplete; preserve for recovery")
                for row in matches:
                    idle()
                    _migration(swapper, Path(row["path"]), wanted[row["name"].casefold()], notify, pause)
        records = []
        for number, row in enumerate(manifest["archives"], 1):
            release.check_pause(pause)
            idle()
            message = f"Verifying archive {number}/{len(manifest['archives'])}: " + row["name"]
            overall.start(row, message)
            legacy_notify(message, 0, 1)
            def archive_notify(message, done, total):
                legacy_notify(message, done, total)
                if total == row["bytes"]:
                    overall.update(done, message)
                else:
                    overall.emit(message)
            path = _download(row, folder, previous.get(row["name"].casefold()), archive_notify, pause, opener)
            records.append(dict(_identity(row), path=str(path)))
            if number < len(manifest["archives"]):
                overall.complete("Saved " + row["name"])
            else:
                # Full bytes do not mean the prepared receipts/reclamation have
                # committed. Retain the final file until preparation succeeds.
                overall.update(row["bytes"], "Finishing saved texture cache…")
        value = {"schema": SCHEMA, "id": core._digest([_identity(row) for row in manifest["archives"]])[:32],
                 "created_at": core._now(), "archives": records, "total_bytes": sum(row["bytes"] for row in records),
                 "source_paths": [], "release": manifest, "release_sha256": core._digest(manifest)}
        core._save(receipt, value)
        swapper._prepared(value)
        core._atomic(core._plain(swapper.state_dir / "texturequest-prepared.json", swapper.state_dir), value)
        _retire_legacy_downloads(swapper, value, notify, pause, idle, previous_direct)
        message = "Ready VPKs verified. Click Install to apply them."
        legacy_notify(message, 1, 1)
        overall.complete(message)
        return value
