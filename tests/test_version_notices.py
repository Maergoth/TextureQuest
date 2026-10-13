"""Independent application/texture checks, offline safety and build identity."""
import copy
import hashlib
import importlib.util
import json
from pathlib import Path
import sys
import subprocess
import tempfile
import unittest
from unittest.mock import patch
from urllib.error import HTTPError, URLError

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "src"))
import texturequest_version_check as versions
import texturequest_latest as latest
from texture_swapper import core
from test_release import Response


def fixture(texture="1.1.0", manifest_version="1.1.1", tag="v1.1.1", app="1.1.1", explicit=False):
    manifest = json.loads((ROOT / "release-manifest.json").read_text(encoding="utf-8"))
    manifest["version"] = manifest_version
    package = manifest["update"]
    package["version"] = texture
    package["archive"]["name"] = f"TextureQuest-Update-{texture}.zip"
    for index, part in enumerate(package["parts"], 1):
        part["name"] = f"TextureQuest-Update-{texture}.zip.{index:03d}"
        part["url"] = latest.REPOSITORY + f"/releases/download/v{texture}/" + part["name"]
    if texture != "1.1.0":
        package["archives"][0]["sha256"] = hashlib.sha256(texture.encode()).hexdigest()
        changed = {r["name"]: r for r in package["archives"]}
        manifest["archives"] = [copy.deepcopy(changed.get(r["name"], r)) for r in manifest["base"]["archives"]]
    payloads = {"release-manifest.json": json.dumps(manifest).encode()}
    installer = f"TextureQuest-Installer-{app}-windows-x64.zip"
    payloads[installer] = b"not downloaded by a version check"
    if explicit:
        payloads[versions.VERSION_ASSET] = json.dumps({"schema": "texturequest-version-v1", "app_version": app,
            "texture_version": texture, "manifest_version": manifest_version,
            "manifest_sha256": hashlib.sha256(payloads["release-manifest.json"]).hexdigest(),
            "installer_name": installer}).encode()
    metadata = {"id": 123, "draft": False, "prerelease": False, "published_at": "2026-10-13T00:00:00Z",
                "tag_name": tag, "html_url": latest.REPOSITORY + "/releases/tag/" + tag,
                "assets": [{"name": name, "state": "uploaded", "size": len(data),
                            "digest": "sha256:" + hashlib.sha256(data).hexdigest(),
                            "browser_download_url": latest.REPOSITORY + "/releases/download/" + tag + "/" + name}
                           for name, data in payloads.items()]}
    return manifest, metadata, payloads


class VersionTests(unittest.TestCase):
    def setUp(self):
        self.current, _, _ = fixture()
        self.manifest, self.metadata, self.payloads = fixture()
        self.requests = []
        self.failures = {}

    def opener(self, request, **kwargs):
        url = request.full_url
        self.requests.append(url)
        name = url.rsplit("/", 1)[1]
        if name in self.failures:
            raise self.failures[name]
        data = json.dumps(self.metadata).encode() if url == latest.LATEST_API else self.payloads[name]
        return Response(data, 200, {"Content-Length": str(len(data))})

    def check(self, app="1.1.1", folder=None):
        return versions.check_updates(self.current, app, folder, opener=self.opener)

    def test_legacy_published_release_reports_separate_versions(self):
        result = self.check()
        self.assertEqual(result["installer"]["version"], "1.1.1")
        self.assertEqual(result["pack"]["version"], "1.1.0")
        self.assertFalse(result["installer"]["update_available"])
        self.assertFalse(result["pack"]["update_available"])
        self.assertFalse(any(url.endswith(".zip") for url in self.requests))

    def test_app_only_release_can_bind_exact_unchanged_old_manifest(self):
        self.manifest, self.metadata, self.payloads = fixture(tag="v1.2.1", app="1.2.1", explicit=True)
        result = self.check()
        self.assertTrue(result["installer"]["update_available"])
        self.assertFalse(result["pack"]["update_available"])
        self.assertEqual(versions.pack_identity(self.current), result["pack"]["identity"])
        self.assertEqual(result["pack"]["manifest"], self.current)
        self.assertEqual(latest.fetch_latest(opener=self.opener, current_manifest=self.current), self.current)

    def test_recommended_app_only_wrapper_remains_compatible_with_shipped_checker(self):
        self.current, _, _ = fixture(texture="1.2.0", manifest_version="1.2.0", tag="v1.2.0", app="1.2.0")
        self.manifest = copy.deepcopy(self.current)
        self.manifest["version"] = "1.2.1"
        _, self.metadata, self.payloads = fixture(texture="1.2.0", manifest_version="1.2.1", tag="v1.2.1", app="1.2.1", explicit=True)
        self.replace_manifest()
        result = self.check(app="1.2.0")
        self.assertTrue(result["installer"]["update_available"])
        self.assertFalse(result["pack"]["update_available"])
        self.assertEqual(result["pack"]["version"], "1.2.0")
        self.assertEqual(result["pack"]["identity"], versions.pack_identity(self.current))
        self.assertEqual(result["pack"]["manifest"]["base"], self.current["base"])
        self.assertEqual(result["pack"]["manifest"]["update"], self.current["update"])
        # Execute the exact shipped source from the preserved source/tag commit,
        # using an offline response fixture, with no GitHub or game access.
        source = subprocess.check_output(["git", "show", "daef02c41a654ee8a35a61e9b44eb992f2d03bc5:src/texturequest_latest.py"],
                                         cwd=ROOT, text=True, encoding="utf-8")
        namespace = {"__name__": "legacy_latest_fixture"}
        exec(compile(source, "<shipped-latest-checker>", "exec"), namespace)
        legacy = namespace["fetch_latest"](minimum_version="1.2.0", opener=self.opener)
        self.assertEqual(legacy, self.manifest)
        from texturequest_release import verify_prepared
        prepared = {"archives": copy.deepcopy(self.current["archives"])}
        self.assertIs(verify_prepared(prepared, self.manifest), prepared)
        self.assertEqual(len(prepared["archives"]), 310)

    def test_pack_only_release_does_not_invent_app_update_from_tag(self):
        self.manifest, self.metadata, self.payloads = fixture(texture="1.2.0", manifest_version="1.2.0", tag="v1.2.0", app="1.1.1")
        result = self.check()
        self.assertFalse(result["installer"]["update_available"])
        self.assertTrue(result["pack"]["update_available"])

    def test_texture_download_floor_uses_data_version_not_prior_app_envelope(self):
        self.current["version"] = "1.3.0"  # Previous app-only publication, textures still 1.1.0.
        self.manifest, self.metadata, self.payloads = fixture(texture="1.2.0", manifest_version="1.2.0", tag="v1.2.0", app="1.3.0")
        selected = latest.fetch_latest(minimum_version="1.3.0", opener=self.opener, current_manifest=self.current)
        self.assertEqual(versions.texture_version(selected), "1.2.0")

    def test_both_updates_and_local_newer_app(self):
        self.manifest, self.metadata, self.payloads = fixture(texture="1.2.0", manifest_version="1.2.0", tag="v1.2.0", app="1.2.0")
        result = self.check()
        self.assertTrue(result["installer"]["update_available"])
        self.assertTrue(result["pack"]["update_available"])
        result = self.check(app="1.3.0")
        self.assertTrue(result["installer"]["local_newer"])
        self.assertFalse(result["installer"]["update_available"])

    def test_older_textures_keep_saved_release_but_app_check_still_works(self):
        self.current, _, _ = fixture(texture="1.2.0", manifest_version="1.2.0")
        result = self.check()
        self.assertEqual(result["pack"]["status"], "unavailable")
        self.assertIn("older", result["pack"]["message"])
        self.assertEqual(result["installer"]["status"], "verified")

    def test_same_texture_version_different_bytes_is_not_current(self):
        self.manifest["update"]["archives"][0]["sha256"] = "f" * 64
        changed = {r["name"]: r for r in self.manifest["update"]["archives"]}
        self.manifest["archives"] = [changed.get(r["name"], r) for r in self.manifest["base"]["archives"]]
        self.replace_manifest()
        result = self.check()
        self.assertEqual(result["pack"]["status"], "unavailable")
        self.assertIn("without a texture version", result["pack"]["message"])
        self.assertNotIn("manifest", result["pack"])

    def replace_manifest(self):
        raw = json.dumps(self.manifest).encode()
        self.payloads["release-manifest.json"] = raw
        row = next(r for r in self.metadata["assets"] if r["name"] == "release-manifest.json")
        row.update(size=len(raw), digest="sha256:" + hashlib.sha256(raw).hexdigest())

    def test_manifest_network_error_does_not_hide_app_update(self):
        self.manifest, self.metadata, self.payloads = fixture(tag="v1.2.1", app="1.2.1", explicit=True)
        self.failures["release-manifest.json"] = URLError("offline")
        result = self.check()
        self.assertTrue(result["installer"]["update_available"])
        self.assertEqual(result["pack"]["status"], "unavailable")
        self.assertIn("Could not reach", result["pack"]["message"])

    def test_invalid_or_ambiguous_installer_does_not_hide_compatible_pack(self):
        row = next(r for r in self.metadata["assets"] if r["name"].endswith(".zip"))
        for change in ("foreign", "duplicate", "bad-checksum", "bool-size"):
            original = copy.deepcopy(self.metadata)
            if change == "foreign":
                row["browser_download_url"] = "https://example.org/installer.zip"
            elif change == "duplicate":
                self.metadata["assets"].append(copy.deepcopy(row))
            elif change == "bad-checksum":
                row["digest"] = "md5:" + "a" * 32
            else:
                row["size"] = True
            with self.subTest(change=change):
                result = self.check()
                self.assertEqual(result["installer"]["status"], "unavailable")
                self.assertEqual(result["pack"]["status"], "verified")
            self.metadata = original
            row = next(r for r in self.metadata["assets"] if r["name"].endswith(".zip"))

    def test_draft_prerelease_or_foreign_release_never_reports_current(self):
        for mutation in ({"draft": True}, {"prerelease": True}, {"tag_name": "v1.3.0-beta"},
                         {"html_url": "https://example.org/release"}, {"assets": None}):
            original = copy.deepcopy(self.metadata)
            self.metadata.update(mutation)
            with self.subTest(mutation=mutation):
                result = self.check()
                self.assertEqual(result["pack"]["status"], "unavailable")
                self.assertEqual(result["installer"]["status"], "unavailable")
            self.metadata = original

    def test_offline_and_rate_limit_keep_cache_and_never_claim_current(self):
        testroot = ROOT / "build"; testroot.mkdir(exist_ok=True)
        with tempfile.TemporaryDirectory(dir=testroot) as folder:
            saved = Path(folder) / "latest-release.json"; saved.write_bytes(b"old verified cache")
            for error in (URLError("offline"), HTTPError(latest.LATEST_API, 403, "rate limit", {}, None),
                          HTTPError(latest.LATEST_API, 429, "rate limit", {}, None)):
                self.failures["latest"] = error
                result = self.check(folder=folder)
                self.assertEqual(result["pack"]["status"], "unavailable")
                self.assertEqual(result["installer"]["status"], "unavailable")
                self.assertEqual(saved.read_bytes(), b"old verified cache")

    def test_corrupt_explicit_mapping_and_manifest_pin_are_rejected(self):
        self.manifest, self.metadata, self.payloads = fixture(tag="v1.2.1", app="1.2.1", explicit=True)
        for key, value in (("schema", "unknown"), ("app_version", "1.2.1-beta"), ("manifest_sha256", "0" * 64),
                           ("texture_version", "1.2.0"), ("installer_name", "other.zip")):
            original = self.payloads[versions.VERSION_ASSET]
            data = json.loads(original); data[key] = value
            raw = json.dumps(data).encode(); self.payloads[versions.VERSION_ASSET] = raw
            row = next(r for r in self.metadata["assets"] if r["name"] == versions.VERSION_ASSET)
            row.update(size=len(raw), digest="sha256:" + hashlib.sha256(raw).hexdigest())
            with self.subTest(key=key):
                result = self.check()
                self.assertEqual(result["pack"]["status"], "unavailable")
            self.payloads[versions.VERSION_ASSET] = original
            row.update(size=len(original), digest="sha256:" + hashlib.sha256(original).hexdigest())

    def test_failed_download_discovery_does_not_replace_prior_cache(self):
        testroot = ROOT / "build"; testroot.mkdir(exist_ok=True)
        self.current, _, _ = fixture(texture="1.2.0", manifest_version="1.2.0")
        with tempfile.TemporaryDirectory(dir=testroot) as folder:
            saved = Path(folder) / "latest-release.json"; saved.write_bytes(b"verified newer cache")
            with self.assertRaisesRegex(ValueError, "older"):
                latest.fetch_latest(folder, opener=self.opener, current_manifest=self.current)
            self.assertEqual(saved.read_bytes(), b"verified newer cache")

    def test_foreign_https_redirect_is_rejected(self):
        response = Response(b"{}", 200, {})
        response.geturl = lambda: "https://example.org/metadata"
        with self.assertRaisesRegex(ValueError, "invalid response"):
            latest._read(latest.LATEST_API, 20, lambda *a, **k: response, None)

    def test_browser_upgrade_action_never_accepts_foreign_or_unverified_url(self):
        for value in ({"status": "unavailable", "release_url": latest.REPOSITORY + "/releases/tag/v1.2.0"},
                      {"status": "verified", "release_url": "https://example.org/v1.2.0"},
                      {"status": "verified", "release_url": latest.REPOSITORY + "/releases/tag/v1.2.0/other"}):
            with self.assertRaises(ValueError):
                versions.installer_page(value)

    def test_saved_newer_manifest_remains_selected_without_claiming_live_success(self):
        manifest, metadata, payloads = fixture(texture="1.2.0", manifest_version="1.2.0", tag="v1.2.0", app="1.2.0")
        testroot = ROOT / "build"; testroot.mkdir(exist_ok=True)
        with tempfile.TemporaryDirectory(dir=testroot) as folder:
            pack = {"manifest": manifest, "version": "1.2.0", "manifest_url": metadata["assets"][0]["browser_download_url"],
                    "manifest_sha256": metadata["assets"][0]["digest"][7:]}
            latest.save_receipt(folder, metadata, pack)
            self.assertEqual(versions.saved_manifest(folder, self.current), manifest)
            saved = Path(folder) / "latest-release.json"
            record = json.loads(saved.read_text()); record["manifest"]["texture_count"] = 1
            saved.write_text(json.dumps(record))
            with self.assertRaisesRegex(ValueError, "checksum changed"):
                versions.saved_manifest(folder, self.current)

    def test_cache_save_error_preserves_independent_app_result(self):
        with patch("texturequest_latest.save_receipt", side_effect=PermissionError("locked")):
            result = self.check(folder=ROOT / "build")
        self.assertEqual(result["installer"]["status"], "verified")
        self.assertEqual(result["pack"]["status"], "unavailable")
        self.assertIn("Could not save", result["pack"]["message"])

    def test_pack_identity_ignores_display_metadata_but_detects_bytes(self):
        same = copy.deepcopy(self.current)
        same["version"] = "1.2.1"
        same["archives"].reverse()
        for row in same["archives"]:
            row.pop("entries", None); row.pop("member", None)
        self.assertEqual(versions.pack_identity(same), versions.pack_identity(self.current))
        same["archives"][0]["sha256"] = "0" * 64
        self.assertNotEqual(versions.pack_identity(same), versions.pack_identity(self.current))


class BuildVersionTests(unittest.TestCase):
    def test_application_build_version_is_independent_of_texture_manifest(self):
        spec = importlib.util.spec_from_file_location("fixture_builder", ROOT / "build_installer.py")
        builder = importlib.util.module_from_spec(spec); spec.loader.exec_module(builder)
        manifest, _, _ = fixture()
        metadata = builder.version_metadata("1.2.1", manifest, "a" * 64)
        self.assertEqual(metadata["app_version"], "1.2.1")
        self.assertEqual(metadata["texture_version"], "1.1.0")
        self.assertEqual(metadata["manifest_version"], "1.1.1")
        constants = {}; exec(builder.build_pin("1.2.1", manifest, "a" * 64), constants)
        self.assertEqual(constants["APP_VERSION"], "1.2.1")
        self.assertEqual(constants["TEXTURE_VERSION"], "1.1.0")
        self.assertEqual(constants["VERSION"], "1.2.1")
        with self.assertRaises(ValueError):
            builder.version_metadata("1.2.1-beta", manifest, "a" * 64)
        with self.assertRaises(ValueError):
            builder.version_metadata("1.2.1", manifest, "not a checksum")

    def test_frozen_runtime_displays_actual_build_pin_not_source_default(self):
        testroot = ROOT / "build"; testroot.mkdir(exist_ok=True)
        with tempfile.TemporaryDirectory(dir=testroot) as folder:
            folder = Path(folder)
            raw = (ROOT / "src/release-manifest.json").read_bytes()
            manifest = json.loads(raw)
            pin = (f'APP_VERSION = "7.2.0"\nTEXTURE_VERSION = "{versions.texture_version(manifest)}"\n'
                   f'MANIFEST_SHA256 = "{hashlib.sha256(raw).hexdigest()}"\n')
            (folder / "texturequest_build_pin.py").write_text(pin, encoding="utf-8")
            script = ('import sys, tkinter as tk\nfrom pathlib import Path\n'
                      f'sys.path.insert(0, {str(ROOT / "src")!r})\nsys.path.insert(0, {str(folder)!r})\nsys.frozen = True\n'
                      'from texturequest_app import TextureQuestApp, VERSION\n'
                      f'root = tk.Tk(); root.withdraw(); app = TextureQuestApp(root, {str(folder / "data")!r})\n'
                      'assert app.app_version == VERSION == "7.2.0"\n'
                      'assert app.version_label.cget("text") == "App 7.2.0"\nroot.destroy()\n')
            result = subprocess.run([sys.executable, "-B", "-c", script], capture_output=True, text=True, timeout=30)
            self.assertEqual(result.returncode, 0, result.stderr)


if __name__ == "__main__":
    unittest.main()
