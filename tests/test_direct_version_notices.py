"""Direct-archive redistribution keeps texture and application identities separate."""
import copy
import hashlib
import json
from pathlib import Path
import tempfile
import unittest

from test_version_notices import fixture
from test_release import Response
import texturequest_version_check as versions
import texturequest_latest as latest


class DirectVersions(unittest.TestCase):
    def setUp(self):
        self.old, _, _ = fixture(texture="1.2.0", manifest_version="1.2.1", tag="v1.2.1", app="1.2.1", explicit=True)
        self.manifest = {"schema": "texturequest-release-v3", "version": "1.3.0", "texture_version": "1.2.0",
                         "texture_count": self.old["texture_count"],
                         "archives": [{**{key: row[key] for key in ("name", "bytes", "sha256")},
                                       "entries": row.get("entries", 1),
                                       "url": latest.REPOSITORY + "/releases/download/v1.3.0/" + row["name"]}
                                      for row in self.old["archives"]]}
        self.requests = []
        self.refresh()

    def refresh(self):
        self.payloads = {"release-manifest.json": json.dumps(self.manifest).encode()}
        self.installer = "TextureQuest-Installer-1.3.0-windows-x64.zip"
        self.payloads[self.installer] = b"not downloaded by update check"
        self.payloads[versions.VERSION_ASSET] = json.dumps({"schema": "texturequest-version-v1", "app_version": "1.3.0",
            "texture_version": self.manifest["texture_version"], "manifest_version": self.manifest["version"],
            "manifest_sha256": hashlib.sha256(self.payloads["release-manifest.json"]).hexdigest(),
            "installer_name": self.installer}).encode()
        self.metadata = {"id": 123, "draft": False, "prerelease": False, "published_at": "2026-10-13T00:00:00Z",
                         "tag_name": "v1.3.0", "html_url": latest.REPOSITORY + "/releases/tag/v1.3.0",
                         "assets": [{"name": name, "state": "uploaded", "size": len(data),
                                     "digest": "sha256:" + hashlib.sha256(data).hexdigest(),
                                     "browser_download_url": latest.REPOSITORY + "/releases/download/v1.3.0/" + name}
                                    for name, data in self.payloads.items()]}

    def opener(self, request, **kwargs):
        self.requests.append(request.full_url)
        data = json.dumps(self.metadata).encode() if request.full_url == latest.LATEST_API else self.payloads[request.full_url.rsplit("/", 1)[1]]
        return Response(data, 200, {"Content-Length": str(len(data))})

    def test_new_delivery_format_is_app_update_with_identical_textures(self):
        result = versions.check_updates(self.old, "1.2.1", opener=self.opener)
        self.assertEqual(result["pack"]["status"], "verified")
        self.assertEqual(result["pack"]["version"], "1.2.0")
        self.assertFalse(result["pack"]["update_available"])
        self.assertTrue(result["installer"]["update_available"])
        self.assertEqual(versions.pack_identity(self.old), result["pack"]["identity"])
        self.assertFalse(any(url.endswith((".zip", ".vpk")) for url in self.requests))

    def test_latest_selection_moves_to_direct_delivery_without_redownloading_pixels(self):
        temporary_root = Path(__file__).resolve().parents[1] / "test-temp"
        temporary_root.mkdir(exist_ok=True)
        with tempfile.TemporaryDirectory(dir=temporary_root) as name:
            result = latest.fetch_latest(state_dir=name, opener=self.opener, current_manifest=self.old)
            self.assertEqual(result, self.manifest)
            self.assertEqual(versions.saved_manifest(name, self.old), self.manifest)
            self.assertEqual(versions.pack_identity(result), versions.pack_identity(self.old))

    def test_same_texture_version_with_different_archive_bytes_fails_closed(self):
        self.manifest["archives"][0]["sha256"] = "f" * 64
        self.refresh()
        result = versions.check_updates(self.old, "1.2.1", opener=self.opener)
        self.assertEqual(result["pack"]["status"], "unavailable")
        self.assertNotIn("manifest", result["pack"])
        self.assertTrue(result["installer"]["update_available"])

    def test_data_downgrade_refused_even_when_app_is_newer(self):
        self.manifest["texture_version"] = "1.1.0"
        self.refresh()
        result = versions.check_updates(self.old, "1.2.1", opener=self.opener)
        self.assertEqual(result["pack"]["status"], "unavailable")
        self.assertIn("older textures", result["pack"]["message"])
        self.assertEqual(result["installer"]["status"], "verified")

    def test_direct_app_is_current_but_does_not_invent_new_texture_version(self):
        result = versions.check_updates(self.manifest, "1.3.0", opener=self.opener)
        self.assertFalse(result["installer"]["update_available"])
        self.assertFalse(result["pack"]["update_available"])

    def test_missing_texture_version_never_uses_app_version(self):
        del self.manifest["texture_version"]
        with self.assertRaises(ValueError): versions.texture_version(self.manifest)


if __name__ == "__main__": unittest.main()
