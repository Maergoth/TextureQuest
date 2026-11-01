"""Real settings transactions on invented D-only game folders."""
from pathlib import Path
from unittest.mock import patch
import json
import tempfile
from test_graphics_proxy import Fixture
import texturequest_graphics_proxy as graphics


class SettingsRepair(Fixture):
    def setUp(self):
        root=Path(__file__).resolve().parents[1]/'build/hotkey-fixtures'
        root.mkdir(parents=True,exist_ok=True)
        temp=patch.object(tempfile,'tempdir',str(root));temp.start();self.addCleanup(temp.stop)
        super().setUp()

    def test_active_and_startup_presets_and_config_reset_custom_toggle_unchanged(self):
        main = b'[GENERAL]\r\nPresetPath=current.ini\r\nStartupPresetPath=start.ini\r\nPresetShortcutKeys=121,0,0,0\r\nPresetShortcutPaths=third.ini\r\n[INPUT]\r\nKeyOverlay=45,0,0,0\r\nKeyEffects=121,0,0,0\r\nKeyScreenshot=44,0,0,0\r\n'
        (self.game/'ReShade.ini').write_bytes(main)
        for name in ['current.ini', 'start.ini', 'third.ini']:
            (self.game/name).write_bytes(b'KeyBloom@Bloom.fx=51,0,1,0\r\nTechniques=Bloom@Bloom.fx\r\n[Bloom.fx]\r\nKeyEnable=121,0,0,0\r\nStrength=0.2\r\n')
        self.manager.base.mkdir(parents=True, exist_ok=True)
        shortcut = self.manager.base/'shortcut.txt'; shortcut.write_bytes(b'TQKEY1\n106\n0\n')
        result = self.manager.repair_shortcuts(self.game)
        self.assertEqual(result['changed'], 4)
        self.assertEqual(shortcut.read_bytes(), b'TQKEY1\n106\n0\n')
        self.assertIn(b'KeyEffects=0,0,0,0', (self.game/'ReShade.ini').read_bytes())
        self.assertIn(b'KeyOverlay=36,0,0,0', (self.game/'ReShade.ini').read_bytes())
        for name in ['current.ini', 'start.ini', 'third.ini']:
            raw=(self.game/name).read_bytes()
            self.assertIn(b'KeyBloom@Bloom.fx=0,0,0,0', raw)
            self.assertIn(b'Strength=0.2', raw)
        self.assertEqual(self.manager.repair_shortcuts(self.game)['changed'], 0)
        self.assertEqual(json.loads(Path(result['receipt']).read_bytes())['phase'], 'complete')
        self.assertTrue(any(p.read_bytes()==main for p in (self.state/'reshade-settings-history').glob('*.ini')))

    def test_outside_game_preset_refuses_before_any_settings_write(self):
        outside=self.root/'outside.ini';outside.write_bytes(b'KeyBloom=121,0,0,0\n')
        raw=('[GENERAL]\nPresetPath='+str(outside)+'\n[INPUT]\nKeyEffects=121,0,0,0\n').encode()
        (self.game/'ReShade.ini').write_bytes(raw)
        with self.assertRaisesRegex(ValueError, 'allowed directory'):
            self.manager.repair_shortcuts(self.game)
        self.assertEqual((self.game/'ReShade.ini').read_bytes(), raw)
        self.assertEqual(outside.read_bytes(), b'KeyBloom=121,0,0,0\n')

    def test_closed_game_required_before_settings_write(self):
        raw=b'[INPUT]\nKeyEffects=121,0,0,0\n'
        (self.game/'ReShade.ini').write_bytes(raw)
        with patch.object(graphics, 'ensure_closed', side_effect=ValueError('Close EQ2')):
            with self.assertRaisesRegex(ValueError, 'Close EQ2'):self.manager.repair_shortcuts(self.game)
        self.assertEqual((self.game/'ReShade.ini').read_bytes(), raw)

    def test_fresh_defaults_disable_all_known_shortcuts_but_home(self):
        self.install()
        raw=(self.game/'ReShade.ini').read_bytes()
        for name in ['KeyEffects','KeyScreenshot','KeyReload','KeyNextPreset','KeyPreviousPreset','KeyFPS','KeyFrameTime','KeyPerformanceMode']:
            self.assertIn((name+'=0,0,0,0').encode(),raw)
        self.assertIn(b'KeyOverlay=36,0,0,0',raw)

    def test_escaped_comma_in_current_and_startup_preset_paths(self):
        (self.game/'ReShade.ini').write_bytes(b'[GENERAL]\nPresetPath=current,,preset.ini\nStartupPresetPath=start,,preset.ini\n')
        for name in ['current,preset.ini', 'start,preset.ini']:
            (self.game/name).write_bytes(b'KeyBloom@Bloom.fx=121,0,0,0\n')
        self.assertEqual(self.manager.repair_shortcuts(self.game)['changed'],3)
        for name in ['current,preset.ini', 'start,preset.ini']:
            self.assertIn(b'=0,0,0,0', (self.game/name).read_bytes())

    def test_interrupted_main_settings_move_refuses_repair_and_install(self):
        original=b'[INPUT]\nKeyEffects=121,0,0,0\n'
        saved=self.game/'.ReShade.ini.tq-old-invented';saved.write_bytes(original)
        history=self.state/'reshade-settings-history';history.mkdir()
        journal=history/'invented-shortcuts.json'
        journal.write_text(json.dumps(dict(schema='texturequest-reshade-shortcuts-v1',phase='prepared',
            files=[dict(path=str(self.game/'ReShade.ini'),original_stash=str(saved))])))
        for action in [lambda:self.manager.repair_shortcuts(self.game),self.install]:
            with self.assertRaisesRegex(ValueError,'interrupted ReShade settings reset'):action()
        self.assertEqual(saved.read_bytes(),original)
        self.assertFalse((self.game/'ReShade.ini').exists())
        self.assertFalse((self.game/'dxgi.dll').exists())
        self.manager._activate_prepared.assert_not_called()

    def test_interrupted_preset_after_config_commit_refuses_further_writes(self):
        main=b'[INPUT]\nKeyOverlay=36,0,0,0\n';(self.game/'ReShade.ini').write_bytes(main)
        original=b'KeyBloom@Bloom.fx=121,0,0,0\n'
        saved=self.game/'.preset.ini.tq-old-invented';saved.write_bytes(original)
        history=self.state/'reshade-settings-history';history.mkdir()
        (history/'invented-shortcuts.json').write_text(json.dumps(dict(schema='texturequest-reshade-shortcuts-v1',
            phase='prepared',files=[dict(path=str(self.game/'ReShade.ini'),committed=True),
                                   dict(path=str(self.game/'preset.ini'),original_stash=str(saved))])))
        with self.assertRaisesRegex(ValueError,'interrupted ReShade settings reset'):
            self.manager.repair_shortcuts(self.game)
        self.assertEqual((self.game/'ReShade.ini').read_bytes(),main)
        self.assertEqual(saved.read_bytes(),original)
