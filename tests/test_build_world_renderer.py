"""Builder profile admission uses invented D files; no package/native execution."""
from pathlib import Path
import hashlib
import importlib.util
import json
import tempfile
import unittest
from unittest.mock import patch

import texturequest_graphics_proxy as graphics
from texturequest_sideload import LoaderError
from test_dlss_install import enable_dlss_fixture

SOURCE = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('world_renderer_build', SOURCE / 'build_installer.py')
builder = importlib.util.module_from_spec(spec)
spec.loader.exec_module(builder)


class BuilderWorldRenderer(unittest.TestCase):
    def setUp(self):
        scope = SOURCE.parent / 'comparison/sichii-renderer-001/texturequest-bridge-001/builder-001/fake-layouts'
        scope.mkdir(parents=True, exist_ok=True)
        self.temp = tempfile.TemporaryDirectory(dir=scope)
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.runtime = self.root / 'runtime'
        self.runtime.mkdir()
        rows = []
        for name in ('TextureQuest.addon64', 'ReShade64.dll'):
            raw = ('invented ' + name).encode()
            (self.runtime / name).write_bytes(raw)
            rows.append(dict(name=name, bytes=len(raw), sha256=hashlib.sha256(raw).hexdigest()))
        self.doc = dict(schema='texturequest-graphics-runtime-v4', addon='TextureQuest.addon64',
            reshade='ReShade64.dll', reshade_build='addon', reshade_version='6.8.0',
            map_schema='TQMAP002', files=rows)
        self.save()
        self.manager = graphics.GraphicsManager(self.root / 'unused-state', runtime_dir=self.runtime)

    def save(self):
        (self.runtime / 'RUNTIME.json').write_text(json.dumps(self.doc), encoding='utf-8')

    def add_renderer(self):
        raw = b'invented renderer, never loaded'
        path = self.runtime / graphics.RENDERER_FILE
        path.parent.mkdir(exist_ok=True)
        path.write_bytes(raw)
        self.doc = json.loads((self.runtime / 'RUNTIME.json').read_bytes())
        self.doc['schema'] = 'texturequest-graphics-runtime-v8'
        self.doc['renderer'] = dict(schema='texturequest-world-renderer-v1',
            id=graphics.RENDERER_PROFILE_ID, private=True, api='d3d11',
            files=[dict(name=graphics.RENDERER_FILE, bytes=len(raw), sha256=hashlib.sha256(raw).hexdigest())])
        self.save()

    def pins(self):
        return builder.graphics_runtime_pins(self.manager, self.manager._runtime(), self.runtime)

    def test_renderer_and_dlss_files_enter_the_same_exact_packaging_pin_set(self):
        enable_dlss_fixture(self.runtime)
        self.add_renderer()
        with patch.object(self.manager, '_dlss_profile', wraps=self.manager._dlss_profile) as dlss:
            pins = self.pins()
        dlss.assert_called_once_with(self.doc, full=True)
        names = {'RUNTIME.json', *(row['name'] for row in self.doc['files']),
                 *(row['name'] for row in self.doc['dlss']['files']), graphics.RENDERER_FILE}
        self.assertEqual(set(pins), names)
        for name, expected in pins.items():
            self.assertEqual(hashlib.sha256((self.runtime / name).read_bytes()).hexdigest(), expected)
        self.assertFalse((self.root / 'unused-state').exists())

    def test_legacy_core_without_optional_profiles_keeps_its_exact_pin_set(self):
        with patch.object(self.manager, '_renderer_profile', side_effect=AssertionError('not selected')):
            pins = self.pins()
        self.assertEqual(set(pins), {'RUNTIME.json', 'TextureQuest.addon64', 'ReShade64.dll'})

    def test_bad_or_missing_renderer_refuses_pin_collection_before_staging(self):
        self.add_renderer()
        path = self.runtime / graphics.RENDERER_FILE
        path.write_bytes(b'tampered')
        with self.assertRaises((ValueError, LoaderError)):
            self.pins()
        path.unlink()
        with self.assertRaises((OSError, ValueError, LoaderError)):
            self.pins()
        self.assertFalse((self.root / 'unused-state').exists())


if __name__ == '__main__':
    unittest.main()
