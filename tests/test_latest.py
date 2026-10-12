"""Published release discovery and cache safety; no real network/game access."""
import copy
import hashlib
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "src"))
import texturequest_latest as latest
import texturequest_release as release
from test_release import Response


class LatestTests(unittest.TestCase):
    def setUp(self):
        public = json.loads((Path(__file__).resolve().parents[1] / "release-manifest.json").read_text())
        base = public.get("base", public)
        row = copy.deepcopy(base["archives"][0])
        self.manifest = {"schema": "texturequest-release-v2", "version": "1.1.1", "texture_count": 23282,
                         "base": base, "archives": copy.deepcopy(base["archives"]),
                         "update": {"version": "1.1.0", "archive": {"name": "TextureQuest-Update-1.1.0.zip", "bytes": 10, "sha256": "a" * 64},
                                    "archives": [row], "parts": [{"name": "TextureQuest-Update-1.1.0.zip.001", "bytes": 10, "sha256": "a" * 64,
                                    "url": latest.REPOSITORY + "/releases/download/v1.1.0/TextureQuest-Update-1.1.0.zip.001"}]}}
        self.raw = json.dumps(self.manifest).encode()
        self.url = latest.REPOSITORY + "/releases/download/v1.1.1/release-manifest.json"
        self.metadata = {"id": 123, "draft": False, "prerelease": False, "published_at": "2026-10-12T00:00:00Z",
                         "tag_name": "v1.1.1", "html_url": latest.REPOSITORY + "/releases/tag/v1.1.1",
                         "assets": [{"name": "release-manifest.json", "state": "uploaded", "size": len(self.raw),
                                     "digest": "sha256:" + hashlib.sha256(self.raw).hexdigest(), "browser_download_url": self.url}]}
        self.requests = []

    def opener(self, request, **kwargs):
        self.requests.append(request.full_url)
        data = json.dumps(self.metadata).encode() if request.full_url == latest.LATEST_API else self.raw
        return Response(data, 200, {"Content-Length": str(len(data))})

    def test_latest_verified_from_api_and_asset_digest(self):
        self.assertEqual(latest.fetch_latest(opener=self.opener), self.manifest)
        self.assertEqual(self.requests, [latest.LATEST_API, self.url])

    def test_installer_only_release_retains_older_exact_texture_package(self):
        self.assertEqual(release.validate_manifest(self.manifest)["update"]["version"], "1.1.0")
        for mutate in (lambda m: m["update"].update(version="1.2.0"),
                       lambda m: m["update"]["parts"][0].update(url="https://example.org/part"),
                       lambda m: m["update"]["archive"].update(name="TextureQuest-Update-1.1.1.zip")):
            broken = copy.deepcopy(self.manifest); mutate(broken)
            with self.assertRaises(ValueError):
                release.validate_manifest(broken)

    def test_draft_prerelease_unpublished_foreign_duplicate_digest_and_size_rejected(self):
        original = copy.deepcopy(self.metadata)
        for mutate in (lambda m: m.update(draft=True), lambda m: m.update(prerelease=True),
                       lambda m: m.update(published_at=None), lambda m: m.update(html_url="https://example.org/release"),
                       lambda m: m["assets"].append(copy.deepcopy(m["assets"][0])),
                       lambda m: m["assets"][0].update(browser_download_url="https://example.org/file"),
                       lambda m: m["assets"][0].update(digest="sha256:" + "0" * 64),
                       lambda m: m["assets"][0].update(size=len(self.raw) - 1)):
            self.metadata = copy.deepcopy(original); mutate(self.metadata)
            with self.subTest(metadata=self.metadata), self.assertRaises(ValueError):
                latest.fetch_latest(opener=self.opener)

    def test_manifest_tamper_and_version_rebinding_rejected(self):
        self.raw += b" "
        self.metadata["assets"][0]["size"] = len(self.raw)
        with self.assertRaisesRegex(ValueError, "checksum"):
            latest.fetch_latest(opener=self.opener)
        self.manifest["version"] = "1.1.2"
        self.raw = json.dumps(self.manifest).encode()
        self.metadata["assets"][0].update(size=len(self.raw), digest="sha256:" + hashlib.sha256(self.raw).hexdigest())
        with self.assertRaisesRegex(ValueError, "version differs"):
            latest.fetch_latest(opener=self.opener)

    def test_downgrade_and_unsupported_schema_rejected(self):
        with self.assertRaisesRegex(ValueError, "older"):
            latest.fetch_latest(minimum_version="1.1.2", opener=self.opener)
        self.manifest["schema"] = "texturequest-release-v99"
        self.raw = json.dumps(self.manifest).encode()
        self.metadata["assets"][0].update(size=len(self.raw), digest="sha256:" + hashlib.sha256(self.raw).hexdigest())
        with self.assertRaisesRegex(ValueError, "Unsupported"):
            latest.fetch_latest(opener=self.opener)

    def test_validated_receipt_is_saved_without_game_action(self):
        testroot = Path(__file__).resolve().parents[1] / "build"
        testroot.mkdir(exist_ok=True)
        with tempfile.TemporaryDirectory(dir=testroot) as folder:
            latest.fetch_latest(folder, opener=self.opener)
            saved = json.loads((Path(folder) / "latest-release.json").read_text())
            self.assertEqual(saved["manifest"], self.manifest)
            self.assertEqual(saved["manifest_sha256"], self.metadata["assets"][0]["digest"][7:])
            self.assertEqual(list(Path(folder).iterdir()), [Path(folder) / "latest-release.json"])

    def test_pause_retains_existing_latest_receipt(self):
        testroot = Path(__file__).resolve().parents[1] / "build"
        testroot.mkdir(exist_ok=True)
        with tempfile.TemporaryDirectory(dir=testroot) as folder:
            saved = Path(folder) / "latest-release.json"; saved.write_bytes(b"prior receipt")
            with self.assertRaises(release.Paused):
                latest.fetch_latest(folder, pause=lambda: True, opener=self.opener)
            self.assertEqual(saved.read_bytes(), b"prior receipt")
            self.assertEqual(self.requests, [])


if __name__ == "__main__":
    unittest.main()
