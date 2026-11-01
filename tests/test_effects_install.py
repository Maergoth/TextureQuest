"""Optional FX deployment on invented D-only games; no renderer or model calls."""
import hashlib
import json
from pathlib import Path
import tempfile
from unittest.mock import patch

from test_graphics_proxy import Fixture, ADDON_BUILD
import texturequest_graphics_proxy as graphics
from texturequest_components import normalize
from texturequest_sideload import LoaderError


def enable_fixture(runtime):
    shader = runtime / 'effects/TQEffects.fx'
    shader.parent.mkdir()
    shader.write_bytes(b'// invented shader bytes; not executed\n')
    path = runtime / 'RUNTIME.json'
    doc = json.loads(path.read_bytes())
    doc['schema'] = 'texturequest-graphics-runtime-v6'
    doc['effects'] = dict(schema='texturequest-effects-runtime-v4', shader='effects/TQEffects.fx',
                          effect_name='TQEffects.fx', techniques=['TQAmbientOcclusion', 'TQContactShadows',
                          'TQDepthDiagnostic'])
    doc['files'].append(dict(name='effects/TQEffects.fx', bytes=shader.stat().st_size,
                             sha256=hashlib.sha256(shader.read_bytes()).hexdigest()))
    path.write_text(json.dumps(doc))


class EffectsInstall(Fixture):
    def setUp(self):
        base = Path(__file__).resolve().parents[1] / 'build/effects-fixtures'
        base.mkdir(parents=True, exist_ok=True)
        local = patch.object(tempfile, 'tempdir', str(base)); local.start(); self.addCleanup(local.stop)
        super().setUp()
        enable_fixture(self.runtime)
        self.selected = normalize(); self.selected['effects'] = True

    def test_opt_in_pins_shader_settings_and_native_gate_without_game_archive_write(self):
        (self.game/'paks/untouched.vpk').write_bytes(b'owned invented stock')
        self.install(components=self.selected)
        record = self.manager._record(self.game)
        self.assertEqual(record['components'], self.selected)
        self.assertEqual(record['effects_settings']['schema'], 'texturequest-effects-lease-v1')
        self.assertIn(b'Effects=1', (self.game/'TextureQuest.ini').read_bytes())
        self.assertIn(b'NoReloadOnInit=0', (self.game/'ReShade.ini').read_bytes())
        self.assertIn(b'SkipLoadingDisabledEffects=0', (self.game/'ReShade.ini').read_bytes())
        shader = next((self.state/'effects').glob('*/TQEffects.fx'))
        self.assertEqual(shader.read_bytes(), (self.runtime/'effects/TQEffects.fx').read_bytes())
        self.assertIn(str(shader.parent).encode(), (self.game/'ReShade.ini').read_bytes())
        self.assertEqual((self.game/'paks/untouched.vpk').read_bytes(), b'owned invented stock')
        self.assertTrue(self.manager.status(self.game)['components']['effects'])

    def test_default_textures_only_does_not_enable_effect_loading(self):
        self.install()
        self.assertIn(b'Effects=0', (self.game/'TextureQuest.ini').read_bytes())
        self.assertIn(b'NoReloadOnInit=1', (self.game/'ReShade.ini').read_bytes())
        self.assertFalse((self.state/'effects').exists())
        self.assertFalse(self.manager.status(self.game)['components']['effects'])

    def test_reused_reshade_without_ini_gets_owned_effects_config(self):
        (self.game/'dxgi.dll').write_bytes(ADDON_BUILD)
        self.install(components=self.selected)
        self.assertIn(b'EffectSearchPaths=', (self.game/'ReShade.ini').read_bytes())
        self.assertIn(b'Effects=1', (self.game/'TextureQuest.ini').read_bytes())
        self.assertIsNotNone(self.manager._record(self.game)['effects_settings'])
        self.assertEqual((self.game/'dxgi.dll').read_bytes(), ADDON_BUILD)

    def test_existing_user_settings_restore_and_later_unrelated_edits_survive(self):
        (self.game/'dxgi.dll').write_bytes(ADDON_BUILD)
        raw = b'[GENERAL]\nEffectSearchPaths=.\\user\\**\nNoReloadOnInit=1\nSkipLoadingDisabledEffects=1\nPresetPath=mine.ini\n[STYLE]\nFont=custom\n'
        (self.game/'ReShade.ini').write_bytes(raw)
        self.install(components=self.selected)
        self.assertIn(b'.\\user\\**', (self.game/'ReShade.ini').read_bytes())
        with (self.game/'ReShade.ini').open('ab') as out: out.write(b'[CUSTOM]\nValue=keep\n')
        result = self.manager.uninstall(self.game)
        after = (self.game/'ReShade.ini').read_bytes()
        for line in [b'EffectSearchPaths=.\\user\\**', b'NoReloadOnInit=1', b'SkipLoadingDisabledEffects=1',
                     b'PresetPath=mine.ini', b'Font=custom', b'Value=keep']:
            self.assertIn(line, after)
        self.assertNotIn(str(self.state/'effects').encode(), after)
        self.assertEqual(result['settings_conflicts'], ())
        self.assertEqual((self.game/'dxgi.dll').read_bytes(), ADDON_BUILD)

    def test_external_change_to_owned_setting_is_preserved_on_uninstall(self):
        self.install(components=self.selected)
        ini = self.game/'ReShade.ini'
        ini.write_bytes(ini.read_bytes().replace(b'NoReloadOnInit=0', b'NoReloadOnInit=9'))
        result = self.manager.uninstall(self.game)
        self.assertIn(b'NoReloadOnInit=9', ini.read_bytes())
        self.assertIn('NoReloadOnInit', result['settings_conflicts'])

    def test_bundled_shader_change_refuses_before_game_writes(self):
        (self.runtime/'effects/TQEffects.fx').write_bytes(b'changed')
        with self.assertRaisesRegex(LoaderError, 'bundled hash'): self.install(components=self.selected)
        self.assertFalse((self.game/'TextureQuest.addon64').exists())
        self.assertFalse((self.game/'ReShade.ini').exists())

    def test_missing_effects_contract_cannot_claim_opt_in_supported(self):
        path = self.runtime/'RUNTIME.json'; doc = json.loads(path.read_bytes())
        doc['schema'] = 'texturequest-graphics-runtime-v4'; del doc['effects']
        path.write_text(json.dumps(doc))
        with self.assertRaisesRegex(ValueError, 'optional effects runtime'): self.install(components=self.selected)
        self.assertFalse((self.game/'TextureQuest.addon64').exists())

    def test_interrupted_settings_install_keeps_durable_lease_for_normal_uninstall(self):
        (self.game/'dxgi.dll').write_bytes(ADDON_BUILD)
        (self.game/'ReShade.ini').write_bytes(b'[GENERAL]\nNoReloadOnInit=1\n')
        original = self.manager._repair_shortcuts
        def interrupted(*args, **kwargs):
            original(*args, **kwargs)
            raise OSError('invented interruption after settings commit')
        with patch.object(self.manager, '_repair_shortcuts', side_effect=interrupted):
            with self.assertRaisesRegex(OSError, 'invented interruption'): self.install(components=self.selected)
        record = self.manager._record(self.game)
        self.assertEqual(record['phase'], 'installing')
        self.assertIsNotNone(record['effects_settings'])
        self.assertIn(b'NoReloadOnInit=0', (self.game/'ReShade.ini').read_bytes())
        self.manager.uninstall(self.game)
        self.assertIn(b'NoReloadOnInit=1', (self.game/'ReShade.ini').read_bytes())
        self.assertFalse((self.game/'TextureQuest.addon64').exists())

    def test_game_must_be_closed_before_optional_install(self):
        with patch.object(graphics, 'ensure_closed', side_effect=ValueError('Close EQ2')):
            with self.assertRaisesRegex(ValueError, 'Close EQ2'): self.install(components=self.selected)
        self.assertFalse((self.state/'effects').exists())
        self.assertFalse((self.game/'ReShade.ini').exists())

