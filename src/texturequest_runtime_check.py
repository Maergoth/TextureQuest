"""Frozen-runtime acceptance with a disposable game fixture only."""
import hashlib
import io
import json
from pathlib import Path
import struct
import tempfile
import zlib
from contextlib import contextmanager


@contextmanager
def _fixture_closed_guard(folder, swapper, game):
    """Only --verify-runtime's fresh D-only fake game may use this predicate."""
    from texture_swapper import core
    folder = folder.absolute()
    game = game.absolute()
    def fixture_closed():
        if (folder.drive.casefold() != 'd:' or game != folder / 'fixture-game'
                or not swapper.state_dir.is_relative_to(folder)
                or core.validate_game(game) != folder / 'fixture-game/paks'):
            raise ValueError('Runtime fixture guard cannot accept a real game path')
    fixture_closed()
    original = core.ensure_game_closed
    core.ensure_game_closed = fixture_closed
    try:
        yield
    finally:
        core.ensure_game_closed = original


def _direct_runtime(folder, vpk):
    """Exercise the frozen direct-cache path using tiny, isolated VPK fixtures."""
    from texture_swapper import core
    from texturequest_direct import DirectSwapper, REPOSITORY, prepare_direct
    names = [f"runtime-{number:03d}.vpk" for number in range(310)]
    rows = [dict(name=name, bytes=len(vpk), sha256=hashlib.sha256(vpk).hexdigest(),
                 entries=1, url=REPOSITORY + "/releases/download/v1.3.0/" + name)
            for name in names]
    manifest = dict(schema="texturequest-release-v3", version="1.3.0",
                    texture_version="1.2.0", texture_count=310, archives=rows)
    requests = []

    class Response(io.BytesIO):
        status = 200
        headers = {"Content-Length": str(len(vpk))}
        def geturl(self):
            return "https://release-assets.githubusercontent.com/runtime-fixture"
        def getcode(self):
            return self.status

    def opener(request, **_):
        requests.append(request.full_url)
        return Response(vpk)

    cache = DirectSwapper(folder / "direct-data")
    downloads = []
    ready = prepare_direct(cache, manifest, opener=opener, overall_progress=lambda *row: downloads.append(row))
    assert downloads[0][1] == 0 and downloads[-1][1] == downloads[-1][2] == sum(row['bytes'] for row in rows)
    assert downloads[-1][3:] == (310, 310)
    assert [row[1] for row in downloads] == sorted(row[1] for row in downloads)
    assert len(requests) == 310
    assert all(Path(row["path"]).parent == cache.state_dir / "downloads" for row in ready["archives"])
    assert not (cache.state_dir / "prepared").exists()
    requests.clear()
    ready = prepare_direct(cache, manifest, opener=opener)
    assert not requests, "Matching direct cache must not download again"

    # Actual frozen/source direct installation, restricted to this helper's
    # own freshly generated D fake game. Product/UI process guard is unchanged.
    game = folder / 'fixture-game'
    for name in names:
        (game / 'paks' / name).write_bytes(b'own tiny fixture stock bytes')
    with _fixture_closed_guard(folder, cache, game):
        installs = []
        installed = cache.install(ready, game, overall_progress=lambda *row: installs.append(row))
        assert installs[0][1] == 0 and installs[-1][1] == installs[-1][2] == sum(row['bytes'] for row in rows)
        assert installs[-1][3:] == (310, 310)
        assert [row[1] for row in installs] == sorted(row[1] for row in installs)
        assert installed['intact_count'] == 310
        identities = [(game / 'paks' / name).stat().st_ino for name in names]
        cache.install(ready, game)
        assert identities == [(game / 'paks' / name).stat().st_ino for name in names]
    assert all((game / 'paks' / name).read_bytes() == vpk for name in names)

    source = folder / "legacy-input"
    source.mkdir()
    paths = []
    for name in names:
        path = source / name
        path.write_bytes(vpk)
        paths.append(path)
    migrated = DirectSwapper(folder / "migration-data")
    legacy = core.Swapper.prepare(migrated, paths)
    core._atomic(migrated.state_dir / "texturequest-prepared.json", legacy)
    complete = prepare_direct(migrated, manifest, opener=opener)
    assert not requests, "Matching legacy prepared files must migrate without downloads"
    assert len(complete["archives"]) == 310
    assert all(not Path(row["path"]).exists() for row in legacy["archives"])
    assert core._load(migrated.state_dir / "texturequest-prepared.json") == complete
    return dict(direct_fixture_archives=310, single_ready_cache=True,
                matching_cache_network_free=True, legacy_migration_network_free=True,
                direct_fast_install=True, matching_installed_copy_skipped=True,
                overall_download_progress=True, overall_install_progress=True,
                runtime_install_guard='own-fresh-D-fixture-only')


def _historical_pak_runtime(output):
    from texture_swapper import core
    from texturequest_release import load_manifest
    from texturequest_app import resource, APP_VERSION
    from texturequest_version_check import texture_version
    output = Path(output).absolute()
    if output.drive.casefold() != 'd:':
        raise ValueError('Internal runtime fixture outputs must be on D:')
    output.parent.mkdir(parents=True, exist_ok=True)
    result = {"status": "error", "game_files_touched": False, "gui_opened": False}
    try:
        with tempfile.TemporaryDirectory(prefix="texturequest-runtime-", dir=output.parent) as tmp:
            folder = Path(tmp)
            game = folder / "fixture-game"
            (game / "paks").mkdir(parents=True)
            (game / "EverQuest2.exe").write_bytes(b"isolated runtime fixture")
            name, blob = b"textures/runtime.txt", b"TextureQuest fixture"
            packed = zlib.compress(struct.pack("<II", len(name) + 1, len(blob)) + name + b"\0" + blob)
            entry = struct.pack("<III", 0, len(packed), len(name)) + name
            index = zlib.compress(struct.pack("<II", len(entry) + 8, 1) + entry)
            body = struct.pack("<I", len(packed)) + packed
            vpk = body + struct.pack("<I", len(index) + 12) + index + struct.pack("<III", 0x33744, len(body), 2)
            archive = game / "paks/runtime.vpk"
            archive.write_bytes(vpk)
            assert core.archive_read(archive, name.decode()) == blob
            swapper = core.Swapper(folder / "state")
            prepared = swapper.prepare([archive])
            assert not swapper.preflight(prepared, game)["missing"]
            with _fixture_closed_guard(folder, swapper, game):
                installed = swapper.install(prepared, game)
            assert installed["status"] == "installed" and not core._readonly(archive)
            direct_result = _direct_runtime(folder, vpk)
            try:
                from texturequest_build_pin import MANIFEST_SHA256
            except ImportError:
                import sys
                if getattr(sys, "frozen", False):
                    raise ValueError("Missing bundled release pin")
                MANIFEST_SHA256 = None
            manifest = load_manifest(resource("release-manifest.json"), MANIFEST_SHA256)
            result.update(status="passed", archive_read=True, writable_install=True, prepared_archive=True,
                          release_sha256=core._digest(manifest), portable_runtime=True,
                          app_version=APP_VERSION, texture_version=texture_version(manifest),
                          **direct_result)
    except Exception as error:
        result["error"] = repr(error)
        raise
    finally:
        output.write_text(json.dumps(result, indent=2), encoding="utf-8")
    return result


def run(output):
    from texturequest_channel_check import run as check_channels
    return check_channels(output)
