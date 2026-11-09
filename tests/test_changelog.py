"""Canonical admission and the small standalone viewer; no app startup or network."""
import hashlib
import importlib.util
import json
from pathlib import Path
import sys
import tempfile
import tkinter as tk
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'src'))
import texturequest_changelog as notes
import texturequest_app as app_module
spec = importlib.util.spec_from_file_location('changelog_build', ROOT / 'build_installer.py')
builder = importlib.util.module_from_spec(spec)
spec.loader.exec_module(builder)


class Changelog(unittest.TestCase):
    def test_canonical_staging_pins_bytes_and_refuses_missing_current_entry(self):
        base = ROOT / 'build' / 'tmp'
        base.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(prefix='changelog-', dir=base) as directory:
            source, staged = Path(directory) / 'CHANGELOG.md', Path(directory) / 'bundled.md'
            raw = '# Changelog\r\n\r\n## 1.13.13 — App update\r\n- Current.\r\n\r\n## 1.13.12\r\n- History.\r\n'.encode()
            source.write_bytes(raw)
            digest = builder.stage_changelog(source, staged, '1.13.13')
            self.assertEqual(staged.read_bytes(), raw)
            self.assertEqual(digest, hashlib.sha256(raw).hexdigest())
            self.assertEqual(notes.load_changelog(staged, '1.13.13', digest), (raw.decode(), digest))
            manifest = json.loads((ROOT / 'src/release-manifest.json').read_text())
            pin = builder.build_pin('1.13.13', manifest, 'a' * 64, digest)
            self.assertIn('CHANGELOG_SHA256 = "' + digest + '"', pin)
            staged.write_bytes(raw + b'changed')
            with self.assertRaisesRegex(ValueError, 'build pin'):
                notes.load_changelog(staged, '1.13.13', digest)
            source.write_bytes(b'# Changelog\n## 1.13.130\n- Wrong version.\n')
            with self.assertRaisesRegex(ValueError, 'no entry'):
                builder.stage_changelog(source, staged, '1.13.13')

    def test_bounded_plaintext_and_only_verified_target_release_link(self):
        for raw in (b'', b'x' * (notes.MAX_BYTES + 1), b'## 1.13.13\n\x00'):
            with self.assertRaises(ValueError):
                notes.validate_changelog(raw, '1.13.13')
        self.assertIn('<script>', notes.validate_changelog(b'## 1.13.13\n<script>plain text</script>', '1.13.13'))
        target = dict(status='verified', update_available=True, version='1.14.0',
                      release_url='https://github.com/Maergoth/TextureQuest/releases/tag/v1.14.0')
        self.assertEqual(notes.target_release(target), ('1.14.0', target['release_url']))
        for change in ({'status': 'unavailable'}, {'update_available': False},
                       {'release_url': 'https://example.com/notes'}, {'version': 'bad\nversion'}):
            self.assertIsNone(notes.target_release(dict(target, **change)))

    def test_header_action_works_while_busy_and_reuses_window_without_network(self):
        app = object.__new__(app_module.TextureQuestApp)
        app.root, app.app_version, app.busy = object(), '1.13.13', 'install'
        app.update_check = {}
        class Dialog:
            def winfo_exists(self): return True
            def lift(self): self.lifted = True
        dialog = Dialog()
        with patch.object(app_module, 'load_changelog', return_value=('## 1.13.13', 'a' * 64)), \
             patch.object(app_module, 'show_changelog', return_value=dialog) as show, \
             patch.object(notes.webbrowser, 'open') as browser:
            self.assertIs(app.open_changelog(), dialog)
            self.assertIs(app.open_changelog(), dialog)
            show.assert_called_once()
            self.assertTrue(dialog.lifted)
            browser.assert_not_called()

    def test_standalone_viewer_readonly_scrollable_nonmodal_with_separate_update_context(self):
        root = tk.Tk()
        root.withdraw()
        try:
            target = dict(status='verified', update_available=True, version='1.14.0',
                          release_url='https://github.com/Maergoth/TextureQuest/releases/tag/v1.14.0')
            text = '## 1.13.13\nCurrent.\n\n## 1.13.12\nHistory.\n<script>plain text</script>'
            with patch.object(notes.webbrowser, 'open') as browser:
                dialog = notes.show_changelog(root, '1.13.13', text, target)
                self.assertEqual(dialog.changelog_text.get('1.0', 'end-1c'), text)
                self.assertEqual(dialog.changelog_text.cget('state'), 'disabled')
                dialog.changelog_text.insert('1.0', 'forbidden')
                self.assertEqual(dialog.changelog_text.get('1.0', 'end-1c'), text)
                self.assertTrue(dialog.changelog_text.cget('yscrollcommand'))
                self.assertIsNone(root.grab_current())
                self.assertIn('1.13.13 (installed)', dialog.title())
                widgets, labels = list(dialog.winfo_children()), []
                while widgets:
                    widget = widgets.pop()
                    widgets.extend(widget.winfo_children())
                    if 'text' in widget.keys(): labels.append(widget.cget('text'))
                self.assertTrue(any('These bundled notes describe the installed app' in value for value in labels))
                self.assertIn('App 1.14.0 release notes', labels)
                self.assertIn('Develop changelog on GitHub', labels)
                browser.assert_not_called()
                dialog.destroy()
        finally:
            root.destroy()


if __name__ == '__main__': unittest.main()
