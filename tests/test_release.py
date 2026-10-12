import copy
import hashlib
import io
import json
from pathlib import Path
import struct
import sys
import tempfile
import unittest
from unittest.mock import patch
import zipfile
import zlib

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "src"))
import texturequest_release as release
from texture_swapper import core


def sha(data):
    return hashlib.sha256(data).hexdigest()


def vpk(payload=b"fixture"):
    name = b"fixture.txt"
    packed = zlib.compress(struct.pack("<II", len(name) + 1, len(payload)) + name + b"\0" + payload)
    entry = struct.pack("<III", 0, len(packed), len(name)) + name
    index = zlib.compress(struct.pack("<II", len(entry) + 8, 1) + entry)
    body = struct.pack("<I", len(packed)) + packed
    return body + struct.pack("<I", len(index) + 12) + index + struct.pack("<III", 0x33744, len(body), 2)


def fixture():
    files = {"a.vpk": vpk(b"approved"), "b.vpk": vpk(b"retained")}
    rows = [{"name": name, "member": name, "bytes": len(blob), "sha256": sha(blob), "entries": 1} for name, blob in files.items()]
    internal = {"schema": "eq2-texture-pack-v2", "archives": rows}
    internal["pack_id"] = core._digest(internal)
    output = io.BytesIO()
    with zipfile.ZipFile(output, "w", zipfile.ZIP_STORED) as archive:
        for name, blob in files.items():
            archive.writestr(name, blob)
        archive.comment = b"EQ2-Texture-Pack-v2\0" + zlib.compress(json.dumps(internal).encode())
    data = output.getvalue()
    parts = [data[:len(data)//2], data[len(data)//2:]]
    manifest = {"schema": "texturequest-release-v1", "version": "1.0.0", "archive": {"name": "Texture-Pack.zip", "bytes": len(data), "sha256": sha(data)},
                "archives": rows, "parts": [{"name": f"TextureQuest-1.0.0.zip.{i:03d}", "url": release.RELEASE + f"TextureQuest-1.0.0.zip.{i:03d}", "bytes": len(blob), "sha256": sha(blob)} for i, blob in enumerate(parts, 1)]}
    return data, parts, manifest


class Response(io.BytesIO):
    def __init__(self, body, status, headers):
        super().__init__(body)
        self.status, self.headers = status, headers
    def geturl(self):
        return "https://release-assets.githubusercontent.com/test"
    def getcode(self):
        return self.status


class ReleaseTests(unittest.TestCase):
    def setUp(self):
        test_root = Path(__file__).resolve().parents[1] / "build"
        test_root.mkdir(parents=True, exist_ok=True)
        self.tmp = tempfile.TemporaryDirectory(prefix="texturequest-", dir=test_root)
        self.addCleanup(self.tmp.cleanup)
        self.folder = Path(self.tmp.name)
        self.data, self.parts, self.manifest = fixture()
        self.requests = []
    def opener(self, request, **kwargs):
        number = int(request.full_url.rsplit(".", 1)[1]) - 1
        raw = self.parts[number]
        start = int(request.get_header("Range", "bytes=0-")[6:-1])
        self.requests.append((number, start))
        return Response(raw[start:], 206 if start else 200, {"Content-Length": str(len(raw) - start), "Content-Range": f"bytes {start}-{len(raw)-1}/{len(raw)}"})
    def test_exact_append_download_and_cache_reuse(self):
        final = release.download(self.manifest, self.folder, opener=self.opener)
        self.assertEqual(final.read_bytes(), self.data)
        self.assertFalse(final.with_suffix(".zip.download").exists())
        self.assertEqual(release.download(self.manifest, self.folder, opener=lambda *_: self.fail("network on completed download")), final)
    def test_partial_resume_uses_range_and_keeps_completed_part(self):
        partial = self.folder / "Texture-Pack.zip.download"
        partial.write_bytes(self.parts[0] + self.parts[1][:31])
        release.download(self.manifest, self.folder, opener=self.opener)
        self.assertEqual(self.requests, [(1, 31)])
    def test_range_ignored_restarts_only_that_part(self):
        (self.folder / "Texture-Pack.zip.download").write_bytes(self.parts[0] + self.parts[1][:31])
        def ignored(request, **kwargs):
            self.assertEqual(request.get_header("Range"), "bytes=31-")
            return Response(self.parts[1], 200, {"Content-Length": str(len(self.parts[1]))})
        self.assertEqual(release.download(self.manifest, self.folder, opener=ignored).read_bytes(), self.data)
    def test_corrupt_completed_prefix_redownloaded(self):
        (self.folder / "Texture-Pack.zip.download").write_bytes(b"x" + self.data[1:])
        self.assertEqual(release.download(self.manifest, self.folder, opener=self.opener).read_bytes(), self.data)
        self.assertEqual(self.requests, [(0, 0), (1, 0)])
    def test_pause_retains_one_partial_without_publish(self):
        count = [0]
        def pause():
            count[0] += 1
            return count[0] > 2
        with self.assertRaises(release.Paused):
            release.download(self.manifest, self.folder, pause=pause, opener=self.opener)
        self.assertFalse((self.folder / "Texture-Pack.zip").exists())
        self.assertEqual(release.download(self.manifest, self.folder, opener=self.opener).read_bytes(), self.data)
    def test_wrong_range_is_rejected(self):
        (self.folder / "Texture-Pack.zip.download").write_bytes(self.data[:10])
        def wrong(request, **kwargs):
            return Response(self.parts[0][10:], 206, {"Content-Range": "bytes 11-20/30"})
        with self.assertRaisesRegex(ValueError, "range"):
            release.download(self.manifest, self.folder, opener=wrong)
        self.assertEqual((self.folder / "Texture-Pack.zip.download").stat().st_size, 10)
    def test_tampered_download_cannot_publish(self):
        def bad(request, **kwargs):
            raw = self.parts[0]
            return Response(b"x" + raw[1:], 200, {"Content-Length": str(len(raw))})
        with self.assertRaisesRegex(ValueError, "checksum"):
            release.download(self.manifest, self.folder, opener=bad)
        self.assertFalse((self.folder / "Texture-Pack.zip").exists())
        self.assertEqual((self.folder / "Texture-Pack.zip.download").stat().st_size, 0)
    def test_truncated_download_resumes(self):
        def truncated(request, **kwargs):
            return Response(self.parts[0][:25], 200, {"Content-Length": str(len(self.parts[0]))})
        with self.assertRaisesRegex(OSError, "interrupted"):
            release.download(self.manifest, self.folder, opener=truncated)
        self.assertEqual(release.download(self.manifest, self.folder, opener=self.opener).read_bytes(), self.data)
        self.assertEqual(self.requests[0], (0, 25))
    def test_manifest_rejects_path_url_hash_size_and_missing_inventory(self):
        self.assertEqual(release.validate_manifest(self.manifest, pinned=False), self.manifest)
        mutations = [("name", "../evil"), ("url", "https://elsewhere.example/a"), ("sha256", "bad"), ("bytes", 0)]
        for key, bad in mutations:
            changed = copy.deepcopy(self.manifest)
            changed["parts"][0][key] = bad
            with self.subTest(key=key), self.assertRaises(ValueError):
                release.validate_manifest(changed, pinned=False)
        changed = copy.deepcopy(self.manifest)
        changed["archives"] = []
        with self.assertRaises(ValueError):
            release.validate_manifest(changed, pinned=False)
        with self.assertRaises(ValueError):
            release.validate_manifest(self.manifest)
    def test_known_zip_internal_and_prepared_bytes_verified(self):
        path = self.folder / "Texture-Pack.zip"
        path.write_bytes(self.data)
        release.verify_zip(path, self.manifest)
        swapper = core.Swapper(self.folder / "state")
        ready = release.prepare_release(swapper, path, self.manifest)
        self.assertEqual(len(ready["archives"]), 2)
        self.assertEqual(release.prepare_release(swapper, path, self.manifest)["id"], ready["id"])
        Path(ready["archives"][0]["path"]).write_bytes(b"damaged")
        with self.assertRaisesRegex(ValueError, "cache changed"):
            release.prepare_release(swapper, path, self.manifest)
    def test_prepared_pointer_does_not_mutate_manifest_across_seconds(self):
        path = self.folder / "Texture-Pack.zip"
        path.write_bytes(self.data)
        swapper = core.Swapper(self.folder / "state")
        times = iter(f"2026-10-12T00:00:{i:02d}+00:00" for i in range(30))
        with patch.object(core, "_now", side_effect=lambda: next(times)):
            ready = release.prepare_release(swapper, path, self.manifest)
            self.assertEqual(swapper._prepared(ready), ready)
            self.assertEqual(core._load(swapper.state_dir / "texturequest-prepared.json"), ready)
            self.assertEqual(release.prepare_release(swapper, path, self.manifest), ready)
    def test_insufficient_preparation_space_fails_before_extraction(self):
        path = self.folder / "Texture-Pack.zip"
        path.write_bytes(self.data)
        swapper = core.Swapper(self.folder / "state")
        with patch.object(release.shutil, "disk_usage") as disk, patch.object(swapper, "prepare") as prepare:
            disk.return_value.free = 1
            with self.assertRaisesRegex(OSError, "space"):
                release.prepare_release(swapper, path, self.manifest)
            prepare.assert_not_called()
    def test_wrong_member_hash_and_internal_inventory_rejected(self):
        path = self.folder / "Texture-Pack.zip"
        path.write_bytes(self.data)
        changed = copy.deepcopy(self.manifest)
        changed["archives"][0]["sha256"] = "0" * 64
        with self.assertRaisesRegex(ValueError, "inventory"):
            release.verify_zip(path, changed)
    def test_known_zip_hash_tampering_rejected(self):
        path = self.folder / "Texture-Pack.zip"
        path.write_bytes(b"x" + self.data[1:])
        with self.assertRaisesRegex(ValueError, "checksum"):
            release.verify_zip(path, self.manifest)
    def test_explicit_install_only_and_other_game_files_retained(self):
        path = self.folder / "Texture-Pack.zip"
        path.write_bytes(self.data)
        game = self.folder / "fake-game"
        (game / "paks").mkdir(parents=True)
        (game / "EverQuest2.exe").write_bytes(b"fixture only")
        for name in ("a.vpk", "b.vpk", "untouched.vpk"):
            (game / "paks" / name).write_bytes(vpk(b"original"))
        before = {p.name: p.read_bytes() for p in (game / "paks").iterdir()}
        swapper = core.Swapper(self.folder / "state")
        ready = release.prepare_release(swapper, path, self.manifest)
        self.assertEqual(before, {p.name: p.read_bytes() for p in (game / "paks").iterdir()})
        with patch.object(core, "ensure_game_closed"):
            result = swapper.install(ready, game)
            self.assertEqual(result["status"], "installed")
            self.assertEqual((game / "paks/untouched.vpk").read_bytes(), before["untouched.vpk"])
            for row in self.manifest["archives"]:
                self.assertEqual(core._hash(game / "paks" / row["name"]), row["sha256"])
            swapper.uninstall(game)


if __name__ == "__main__":
    unittest.main()
