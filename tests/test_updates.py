"""Complete version overlays, verified reuse and recovery; D-only fixtures."""
import copy
import hashlib
import io
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch
import zipfile
import zlib

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "src"))
import texturequest_release as release
import texturequest_updates as updates
from texture_swapper import core
from test_release import fixture, vpk, sha, Response


def update_fixture():
    base_data, base_parts, base = fixture()
    base["texture_count"] = 2
    changed = vpk(b"new 4K detail including retained other entries")
    rows = [{"name": "a.vpk", "member": "a.vpk", "bytes": len(changed), "sha256": sha(changed), "entries": 1}]
    internal = {"schema": "eq2-texture-pack-v2", "archives": rows}
    internal["pack_id"] = core._digest(internal)
    output = io.BytesIO()
    with zipfile.ZipFile(output, "w", zipfile.ZIP_STORED) as archive:
        archive.writestr("a.vpk", changed)
        archive.comment = b"EQ2-Texture-Pack-v2\0" + zlib.compress(json.dumps(internal).encode())
    blob = output.getvalue()
    update = {"version": "1.1.0", "archive": {"name": updates.UPDATE_NAME, "bytes": len(blob), "sha256": sha(blob)},
              "parts": [{"name": updates.UPDATE_NAME + ".001", "url": updates.UPDATE_URL + updates.UPDATE_NAME + ".001", "bytes": len(blob), "sha256": sha(blob)}],
              "archives": rows}
    manifest = {"schema": "texturequest-release-v2", "version": "1.1.0", "texture_count": 3,
                "base": base, "update": update, "archives": copy.deepcopy(rows + [base["archives"][1]])}
    parts = {part["url"]: data for part, data in zip(base["parts"], base_parts)}
    parts[update["parts"][0]["url"]] = blob
    return manifest, base_data, parts, changed


class UpdateTests(unittest.TestCase):
    def setUp(self):
        testroot = Path(__file__).resolve().parents[1] / "build"
        testroot.mkdir(exist_ok=True)
        self.temp = tempfile.TemporaryDirectory(prefix="update-", dir=testroot)
        self.addCleanup(self.temp.cleanup)
        self.folder = Path(self.temp.name)
        self.swapper = core.Swapper(self.folder / "state")
        self.manifest, self.base_data, self.parts, self.changed = update_fixture()
        self.requests = []

    def opener(self, request, **kwargs):
        url = request.full_url
        self.requests.append(url)
        data = self.parts[url]
        start = int(request.get_header("Range", "bytes=0-")[6:-1])
        return Response(data[start:], 206 if start else 200,
                        {"Content-Length": str(len(data) - start), "Content-Range": f"bytes {start}-{len(data)-1}/{len(data)}"})

    def prepare(self, **kwargs):
        return updates.prepare_update(self.swapper, self.manifest, opener=self.opener, **kwargs)

    def old_cache(self):
        path = self.folder / "base.zip"
        path.write_bytes(self.base_data)
        return self.swapper.prepare([path])

    def game(self, latest=False):
        game = self.folder / "game"
        (game / "paks").mkdir(parents=True)
        (game / "EverQuest2.exe").write_bytes(b"fixture only")
        with zipfile.ZipFile(io.BytesIO(self.base_data)) as archive:
            for name in ("a.vpk", "b.vpk"):
                (game / "paks" / name).write_bytes(self.changed if latest and name == "a.vpk" else archive.read(name))
        return game

    def test_manifest_is_exact_complete_overlay(self):
        updates.validate_manifest(self.manifest, pinned=False)
        for mutate in (
            lambda m: m["archives"].pop(),
            lambda m: m["archives"][1].update(sha256="0"*64),
            lambda m: m["update"]["archives"][0].update(name="unrelated.vpk", member="unrelated.vpk"),
            lambda m: m["update"]["parts"][0].update(url="https://example.org/file"),
            lambda m: m["update"]["parts"][0].update(bytes=1),
            lambda m: m["archives"].append(copy.deepcopy(m["archives"][0])),
            lambda m: m.update(version="1.0.0"),
            lambda m: m["base"].update(schema="texturequest-release-v2"),
        ):
            broken = copy.deepcopy(self.manifest); mutate(broken)
            with self.subTest(broken=broken), self.assertRaises(ValueError):
                updates.validate_manifest(broken, pinned=False)

    def test_public_base_omits_optional_entry_counts(self):
        for rows in (self.manifest["base"]["archives"], self.manifest["archives"]):
            for row in rows:
                row.pop("entries", None)
        updates.validate_manifest(self.manifest, pinned=False)
        ready = self.prepare()
        self.assertTrue(all(row["entries"] == 1 for row in ready["archives"]))

    def test_fresh_install_prepares_base_and_update_without_installing(self):
        with patch.object(self.swapper, "install", side_effect=AssertionError("automatic install")):
            ready = self.prepare()
        self.assertEqual(len(self.requests), 3)
        self.assertEqual(ready["texturequest_version"], "1.1.0")
        self.assertEqual(len(ready["archives"]), 2)
        self.assertEqual(Path(ready["archives"][0]["path"]).read_bytes(), self.changed)
        release.verify_prepared(ready, self.manifest)

    def test_old_cache_downloads_only_update_and_reuses_unchanged_payload(self):
        old = self.old_cache()
        ready = self.prepare()
        self.assertEqual(self.requests, [self.manifest["update"]["parts"][0]["url"]])
        self.assertEqual(Path(old["archives"][0]["path"]).read_bytes(), vpk(b"approved"))
        self.assertEqual(Path(ready["archives"][1]["path"]).read_bytes(), vpk(b"retained"))
        self.assertNotEqual(Path(old["archives"][1]["path"]).stat().st_ino, Path(ready["archives"][1]["path"]).stat().st_ino)

    def test_installed_base_reuses_verified_files_without_game_mutation(self):
        game = self.game()
        before = {p.name: (p.read_bytes(), p.stat().st_mtime_ns) for p in (game / "paks").iterdir()}
        ready = self.prepare(game=game)
        self.assertEqual(len(self.requests), 1)
        self.assertEqual(before, {p.name: (p.read_bytes(), p.stat().st_mtime_ns) for p in (game / "paks").iterdir()})
        self.assertNotEqual((game / "paks/b.vpk").stat().st_ino, Path(ready["archives"][1]["path"]).stat().st_ino)

    def test_fully_current_game_requires_no_download(self):
        game = self.game(latest=True)
        self.prepare(game=game)
        self.assertEqual(self.requests, [])

    def test_explicit_install_applies_complete_overlay_and_skips_unchanged_bytes(self):
        game = self.game()
        extra = game / "paks/other.vpk"; extra.write_bytes(vpk(b"unrelated game archive"))
        old_b = (game / "paks/b.vpk").stat().st_mtime_ns
        ready = self.prepare(game=game)
        with patch.object(core, "ensure_game_closed"):
            status = self.swapper.install(ready, game)
            self.assertEqual(status["status"], "installed")
            self.assertEqual((game / "paks/a.vpk").read_bytes(), self.changed)
            self.assertEqual((game / "paks/b.vpk").stat().st_mtime_ns, old_b)
            self.assertEqual(extra.read_bytes(), vpk(b"unrelated game archive"))
            self.swapper.uninstall(game)

    def test_patched_game_downloads_base_to_restore_missing_matching_bytes(self):
        game = self.game()
        (game / "paks/b.vpk").write_bytes(vpk(b"stock after a patch"))
        self.prepare(game=game)
        self.assertEqual(len(self.requests), 3)
        self.assertEqual((game / "paks/b.vpk").read_bytes(), vpk(b"stock after a patch"))

    def test_saved_external_base_avoids_base_download(self):
        path = self.folder / "my-base.zip"; path.write_bytes(self.base_data)
        self.prepare(base_zip=path)
        self.assertEqual(len(self.requests), 1)

    def test_original_installers_download_path_is_reused(self):
        path = self.swapper.state_dir / "downloads/Texture-Pack.zip"
        path.parent.mkdir(); path.write_bytes(self.base_data)
        self.prepare()
        self.assertEqual(len(self.requests), 1)

    def test_completed_preparation_resumes_offline(self):
        ready = self.prepare()
        with patch.object(release, "download", side_effect=AssertionError("download after complete")):
            self.assertEqual(self.prepare(), ready)

    def test_corrupt_matching_cache_is_preserved_and_rejected(self):
        old = self.old_cache(); path = Path(old["archives"][1]["path"])
        path.write_bytes(b"damaged")
        with self.assertRaisesRegex(ValueError, "cache changed"):
            self.prepare()
        self.assertEqual(path.read_bytes(), b"damaged")
        self.assertEqual(self.requests, [])

    def test_changed_download_cannot_publish_prepared_set(self):
        self.old_cache()
        url = self.manifest["update"]["parts"][0]["url"]
        self.parts[url] = b"x" + self.parts[url][1:]
        with self.assertRaisesRegex(ValueError, "checksum"):
            self.prepare()
        self.assertFalse(updates.prepared_pointer(self.swapper, self.manifest)[1].exists())

    def test_copy_pause_and_exact_prefix_resume(self):
        row = self.manifest["archives"][0]
        target = self.folder / row["name"]
        calls = [0]
        def pause():
            calls[0] += 1
            return calls[0] == 3
        with patch.object(core, "CHUNK", 8), self.assertRaises(release.Paused):
            updates._copy_checked(io.BytesIO(self.changed), target, row, pause)
        self.assertFalse(target.exists())
        prefix = target.with_name(target.name + ".pending").read_bytes()
        self.assertEqual(prefix, self.changed[:len(prefix)])
        updates._copy_checked(io.BytesIO(self.changed), target, row, None)
        self.assertEqual(target.read_bytes(), self.changed)

    def test_unknown_partial_is_never_overwritten(self):
        row = self.manifest["archives"][0]; target = self.folder / row["name"]
        partial = target.with_name(target.name + ".pending"); partial.write_bytes(b"unowned")
        with self.assertRaisesRegex(ValueError, "Unknown interrupted"):
            updates._copy_checked(io.BytesIO(self.changed), target, row, None)
        self.assertEqual(partial.read_bytes(), b"unowned")

    def test_mutated_owned_partial_is_preserved(self):
        row = self.manifest["archives"][0]; target = self.folder / row["name"]
        partial = target.with_name(target.name + ".pending"); partial.write_bytes(b"changed")
        core._atomic(target.with_name(target.name + ".pending.json"), {k: row[k] for k in ("name", "bytes", "sha256")})
        with self.assertRaisesRegex(ValueError, "prefix changed"):
            updates._copy_checked(io.BytesIO(self.changed), target, row, None)
        self.assertEqual(partial.read_bytes(), b"changed")

    def test_complete_member_checkpoint_survives_interruption(self):
        self.old_cache()
        folder, receipt = updates.prepared_pointer(self.swapper, self.manifest)
        folder.mkdir(parents=True); (folder / "a.vpk").write_bytes(self.changed)
        ready = self.prepare()
        self.assertEqual(self.requests, [])
        self.assertEqual(Path(ready["archives"][0]["path"]).read_bytes(), self.changed)

    def test_whole_manifest_pin_rejects_changed_bytes(self):
        path = self.folder / "manifest.json"; path.write_text(json.dumps(self.manifest))
        with self.assertRaisesRegex(ValueError, "Bundled release manifest changed"):
            release.load_manifest(path, "0"*64)


if __name__ == "__main__":
    unittest.main()
