"""Graphics-only controls and real installer using invented DDS/game/DLL files."""
from contextlib import ExitStack
import hashlib
import json
from pathlib import Path
import sys
import tkinter as tk
import unittest
from unittest.mock import MagicMock, patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'src'))
import texturequest_app as app
import texturequest_graphics_proxy as proxy
from test_loose_dds import Fixture
from test_direct import fixture as legacy_manifest


class GraphicsOnlyUI(Fixture):
    def setUp(self):
        super().setUp()
        self.state = self.root / 'data'
        self.runtime = self.root / 'runtime'
        self.runtime.mkdir()
        files = []
        for name, raw in [('TextureQuest.addon64', b'not executed'), ('ReShade64.dll', b'not executed ReShadeRegisterAddon')]:
            (self.runtime / name).write_bytes(raw)
            files.append(dict(name=name, bytes=len(raw), sha256=hashlib.sha256(raw).hexdigest()))
        (self.runtime / 'RUNTIME.json').write_text(json.dumps(dict(schema=proxy.RUNTIME_SCHEMA,
            addon='TextureQuest.addon64', reshade='ReShade64.dll', reshade_build='addon', map_schema='TQMAP002', files=files)))
        self.manager = proxy.GraphicsManager(self.state, runtime_dir=self.runtime)
        self.tk = tk.Tk()
        self.tk.withdraw()
        self.stack = ExitStack()
        self.addCleanup(self.stack.close)
        self.stack.enter_context(patch.object(app, 'load_manifest', return_value=legacy_manifest()[1]))
        self.stack.enter_context(patch.object(app, 'GraphicsManager', return_value=self.manager))
        loader = MagicMock()
        loader.status.return_value = dict(installed=False, running=False, active_sessions=0, available=False, message='')
        loader.cache_in_use.return_value = False
        self.stack.enter_context(patch.object(app, 'LoaderManager', return_value=loader))
        self.stack.enter_context(patch.object(proxy, 'ensure_closed'))
        self.ui = app.TextureQuestApp(self.tk, self.state, self.game, startup_check=False)
        self.addCleanup(self.close_window)

    def close_window(self):
        for callback in self.tk.tk.call('after', 'info'):
            self.tk.after_cancel(callback)
        self.tk.destroy()

    def prepared(self):
        import texturequest_shared_textures as shared
        return self.ui.dds_store.prepare(self.source, self.game, shared.propose(self.index), opener=self.opener)

    def test_only_graphics_option_and_no_pak_install_handler(self):
        self.assertEqual([r[1] for r in app.INSTALL_MODE_INFO], ['graphics'])
        self.assertEqual(self.ui.install_mode.get(), 'graphics')
        self.assertEqual([w.cget('text') for w in self.ui.mode_controls], ['Graphics hook'])
        with patch.object(self.ui, 'manage_graphics', return_value='graphics') as run:
            self.assertEqual(self.ui.install(), 'graphics')
            self.assertEqual(self.ui.install_selected(), 'graphics')
        self.assertEqual(run.call_count, 2)

    def test_real_install_uses_dds_root_and_preserves_game_archive(self):
        prepared = self.prepared()
        old = (self.game/'paks/test.vpk').read_bytes()
        self.manager.install(self.game, prepared)
        ini = (self.game/'TextureQuest.ini').read_text(encoding='utf-8')
        self.assertIn(str(self.state/'dds/objects'), ini)
        self.assertNotIn(str(self.state/'downloads'), ini)
        self.assertEqual((self.game/'paks/test.vpk').read_bytes(), old)
        self.assertEqual((self.state/'graphics/texturequest-map.bin').read_bytes()[:8], b'TQMAP002')
        record = self.manager._record(self.game)
        self.assertEqual(record['payload'], 'loose-dds')
        self.manager.uninstall(self.game)
        self.assertEqual((self.game/'paks/test.vpk').read_bytes(), old)
        self.assertTrue((Path(prepared['object_root'])/self.ui.dds_store.object_rows(prepared)[0]['entry']).exists())

    def test_preparation_for_different_game_refused(self):
        prepared = self.prepared()
        prepared['game'] = str(self.root/'different-game')
        with self.assertRaisesRegex(ValueError, 'different game folder'):
            self.manager.install(self.game, prepared)
        self.assertFalse((self.game/'TextureQuest.addon64').exists())

    def test_corrupt_dds_refuses_install_before_game_writes(self):
        prepared = self.prepared()
        path = Path(prepared['object_root']) / self.ui.dds_store.object_rows(prepared)[0]['entry']
        raw = path.read_bytes(); path.write_bytes(raw[:-1]+b'X')
        with self.assertRaisesRegex(ValueError, 'checksum'):
            self.manager.install(self.game, prepared)
        self.assertFalse((self.game/'TextureQuest.addon64').exists())

    def test_failed_activation_keeps_previous_active_map(self):
        prepared = self.prepared()
        self.manager.install(self.game, prepared)
        active = self.state/'graphics/texturequest-map.bin'
        before = active.read_bytes()
        with patch.object(proxy.os, 'replace', side_effect=OSError('simulated commit failure')):
            with self.assertRaisesRegex(OSError, 'commit failure'):
                self.manager.activate_prepared(self.game, prepared)
        self.assertEqual(active.read_bytes(), before)
        self.ui.dds_store.verify(prepared)


if __name__ == '__main__':
    unittest.main()
