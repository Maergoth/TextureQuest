"""New private DLSS graph/lease cases; invented D-only files, never native calls."""
from pathlib import Path
import hashlib
import json
import tempfile
import unittest
from unittest.mock import patch, Mock
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'src'))
from test_graphics_proxy import Fixture, ADDON_BUILD
import texturequest_graphics_proxy as graphics
import texturequest_components as components
import texturequest_dlss as settings
from texturequest_effects import prepare_effects_config, restore_effects_config


def enable_dlss_fixture(runtime):
    shader = runtime / 'effects/TQEffects.fx'
    shader.parent.mkdir(exist_ok=True)
    shader.write_bytes(b'// invented TQ suite, never executed\n')
    doc = json.loads((runtime / 'RUNTIME.json').read_bytes())
    doc['schema'] = 'texturequest-graphics-runtime-v7'
    doc['effects'] = dict(schema='texturequest-effects-runtime-v2', shader='effects/TQEffects.fx',
                         effect_name='TQEffects.fx', techniques=['TQAmbientOcclusion', 'TQContactShadows',
                         'TQIndirectLighting', 'TQBloom', 'TQAntiAliasing', 'TQDepthDiagnostic'])
    doc['files'].append(dict(name='effects/TQEffects.fx', bytes=shader.stat().st_size,
                             sha256=hashlib.sha256(shader.read_bytes()).hexdigest()))
    rows = []
    for role, name in graphics.DLSS_ROLES.items():
        path = runtime / name
        path.parent.mkdir(parents=True, exist_ok=True)
        raw = ('invented nonexecuted role ' + role).encode()
        path.write_bytes(raw)
        rows.append(dict(role=role, name=name, bytes=len(raw), sha256=hashlib.sha256(raw).hexdigest()))
    doc['dlss'] = dict(schema='texturequest-dlss-runtime-v1', id=graphics.DLSS_PROFILE_ID,
                       private=True, api='d3d11', files=rows)
    (runtime / 'RUNTIME.json').write_text(json.dumps(doc))


class DLSSSettings(unittest.TestCase):
    def test_scalar_and_macro_transform_roundtrip_preserves_bom_comments_unicode_tail(self):
        raw = b'\xef\xbb\xbf; comment\r\n[GENERAL]\r\nPreprocessorDefinitions=OTHER=7,WITH_COMMA=a,,b,DLSS5_MV_PROVIDER=0\r\nPresetPath=mine.ini\r\n[RenoDX.DLSS5]\r\nEnableHooks = 9 ; keep\r\nNRStyle=2\r\n[USER]\r\nName=\xc3\xbc'
        after, journal = settings.prepare_dlss_config(raw)
        self.assertIn(b'OTHER=7,WITH_COMMA=a,,b,DLSS5_MV_PROVIDER=3', after)
        self.assertIn(b'EnableHooks = 2 ; keep', after)
        for (_, key), value in settings.VALUES.items():
            if value is not None and key != 'EnableHooks': self.assertIn((key + '=' + value).encode(), after)
        self.assertEqual(settings.restore_dlss_config(after, json.loads(json.dumps(journal))), (raw, ()))

    def test_new_sections_empty_file_and_unterminated_tail_restore_exactly(self):
        for raw in (b'', b'; text', b'[GENERAL]\nPresetPath=mine', b'[OTHER]\r\nvalue=2\r\n'):
            with self.subTest(raw=raw):
                after, journal = settings.prepare_dlss_config(raw)
                self.assertEqual(settings.restore_dlss_config(after, journal), (raw, ()))

    def test_changed_owned_values_and_unrelated_edits_survive(self):
        raw = b'[GENERAL]\nPreprocessorDefinitions=OTHER=1\n[RenoDX.DLSS5]\nNRStyle=1\n'
        after, journal = settings.prepare_dlss_config(raw)
        edited = after.replace(b'NRStyle=0', b'NRStyle=7') + b'[USER]\nKeep=9\n'
        restored, conflicts = settings.restore_dlss_config(edited, journal)
        self.assertIn(b'NRStyle=7', restored)
        self.assertIn(b'Keep=9', restored)
        self.assertIn('RenoDX.DLSS5.NRStyle', conflicts)
        self.assertIn(b'PreprocessorDefinitions=OTHER=1', restored)

    def test_duplicate_macro_or_affected_key_refuses(self):
        for raw in (b'[GENERAL]\nPreprocessorDefinitions=DLSS5_MV_PROVIDER=0,DLSS5_MV_PROVIDER=1\n',
                    b'[RenoDX.DLSS5]\nNRStyle=1\nNRStyle=2\n'):
            with self.subTest(raw=raw), self.assertRaisesRegex(ValueError, 'Duplicate'):
                settings.prepare_dlss_config(raw)

    def test_journal_tamper_refuses_and_settings_are_idempotent(self):
        after, journal = settings.prepare_dlss_config(b'[GENERAL]\nValue=keep\n')
        again, lease = settings.prepare_dlss_config(after)
        self.assertEqual(again, after); self.assertEqual(lease['changes'], [])
        journal['changes'][0]['after'] = 'PreprocessorDefinitions=UNOWNED=1\n'
        with self.assertRaisesRegex(ValueError, 'checksum'): settings.restore_dlss_config(after, journal)

    def test_composed_effect_dlss_path_and_consumer_restore_in_reverse(self):
        raw = b'[GENERAL]\nEffectSearchPaths=.\\user\\**\nNoReloadOnInit=1\nPresetPath=mine.ini\n'
        first, effects = prepare_effects_config(raw, 'D:/data/effects/own')
        second, dlss = prepare_effects_config(first, 'D:/data/dlss/own')
        third, consumer = settings.prepare_dlss_config(second)
        value, conflicts = settings.restore_dlss_config(third, consumer); self.assertFalse(conflicts)
        value, conflicts = restore_effects_config(value, dlss); self.assertFalse(conflicts)
        value, conflicts = restore_effects_config(value, effects); self.assertFalse(conflicts)
        self.assertEqual(value, raw)


class DLSSInstall(Fixture):
    def setUp(self):
        base = Path(__file__).resolve().parents[1] / 'build/dlss-fixtures-001'
        base.mkdir(parents=True, exist_ok=True)
        local = patch.object(tempfile, 'tempdir', str(base)); local.start(); self.addCleanup(local.stop)
        super().setUp()
        enable_dlss_fixture(self.runtime)
        self.selected = dict(components.normalize(), dlss=True)

    def test_selected_installs_only_fixed_modules_and_private_shaders_with_two_leases(self):
        (self.game / 'paks/stock.vpk').write_bytes(b'never change')
        self.assertTrue(self.manager.status(self.game)['dlss_available'])
        self.manager.preflight_components(self.game, self.selected)
        self.install(components=self.selected)
        record = self.record()
        self.assertEqual(record['dlss_profile'], graphics.DLSS_PROFILE_ID)
        self.assertEqual(record['components'], self.selected)
        self.assertIsNotNone(record['dlss_settings']); self.assertIsNotNone(record['dlss_consumer_settings'])
        for role in graphics.DLSS_MODULES:
            name = Path(graphics.DLSS_ROLES[role]).name
            self.assertEqual((self.game / name).read_bytes(), (self.runtime / graphics.DLSS_ROLES[role]).read_bytes())
        self.assertEqual(len(list((self.state / 'dlss').glob('**/*.*'))), 7)
        ini = (self.game / 'ReShade.ini').read_bytes()
        self.assertIn(b'DLSS5_MV_PROVIDER=3', ini); self.assertIn(b'EnableHooks=2', ini)
        self.assertIn(b'DLSS=1', (self.game / 'TextureQuest.ini').read_bytes())
        self.assertEqual((self.game / 'paks/stock.vpk').read_bytes(), b'never change')

    def test_unselected_missing_optional_payload_does_not_disable_textures(self):
        (self.runtime / graphics.DLSS_ROLES['nr']).unlink()
        status = self.manager.status(self.game)
        self.assertFalse(status['dlss_available']); self.assertTrue(status['available'])
        self.install()
        self.assertFalse((self.state / 'dlss').exists())
        self.assertIn(b'DLSS=0', (self.game / 'TextureQuest.ini').read_bytes())
        self.assertFalse((self.game / 'nvngx_dlssnr.dll').exists())

    def test_missing_tampered_duplicate_and_traversal_roles_refuse_before_writes(self):
        original = (self.runtime / 'RUNTIME.json').read_bytes()
        for issue in ('missing', 'tampered', 'duplicate', 'traversal'):
            doc = json.loads(original)
            if issue == 'missing': doc['dlss']['files'].pop()
            elif issue == 'tampered': doc['dlss']['files'][0]['sha256'] = '0' * 64
            elif issue == 'duplicate': doc['dlss']['files'][1] = doc['dlss']['files'][0]
            else: doc['dlss']['files'][0]['name'] = '../outside.addon64'
            (self.runtime / 'RUNTIME.json').write_text(json.dumps(doc))
            with self.subTest(issue=issue), self.assertRaises((ValueError, RuntimeError)):
                self.install(components=self.selected)
            self.assertFalse((self.game / 'TextureQuest.addon64').exists())
            self.manager._activate_prepared.assert_not_called()
        (self.runtime / 'RUNTIME.json').write_bytes(original)

    def test_foreign_module_consumer_and_external_addonpath_preserved(self):
        for name in ('nvngx_dlss.dll', 'other-dlss.addon64', 'OptiScaler.dll'):
            (self.game / name).write_bytes(b'foreign')
            with self.subTest(name=name), self.assertRaisesRegex(ValueError, 'exists|Another DLSS'):
                self.manager.preflight_components(self.game, self.selected)
            self.assertEqual((self.game / name).read_bytes(), b'foreign')
            (self.game / name).unlink()
        (self.game / 'addons').mkdir()
        (self.game / 'ReShade.ini').write_bytes(b'[ADDON]\nAddonPath=addons\n')
        with self.assertRaisesRegex(ValueError, 'AddonPath'):
            self.manager.preflight_components(self.game, self.selected)
        self.assertEqual((self.game / 'ReShade.ini').read_bytes(), b'[ADDON]\nAddonPath=addons\n')

    def test_availability_cache_avoids_large_rehash_but_install_forces_verification(self):
        self.manager.status(self.game)
        real = graphics._pin
        optional = []
        def count(path, expected=None):
            if '/dlss/' in Path(path).as_posix(): optional.append(Path(path).name)
            return real(path, expected)
        with patch.object(graphics, '_pin', side_effect=count):
            self.manager.status(self.game); self.assertEqual(optional, [])
            self.manager.preflight_components(self.game, self.selected)
            self.assertEqual(len(optional), 11)
        path = self.runtime / graphics.DLSS_ROLES['nr']
        path.write_bytes(b'tampered')
        self.assertFalse(self.manager.status(self.game)['dlss_available'])

    def test_combined_settings_restore_preserves_user_cfg_and_edits_then_reinstall(self):
        (self.game / 'dxgi.dll').write_bytes(ADDON_BUILD)
        raw = b'\xef\xbb\xbf[GENERAL]\r\nEffectSearchPaths=.\\user\\**\r\nNoReloadOnInit=1\r\nSkipLoadingDisabledEffects=1\r\nPresetPath=mine.ini\r\n[RenoDX.DLSS5]\r\nNRStyle=1\r\n'
        (self.game / 'ReShade.ini').write_bytes(raw)
        cfg = self.game / 'dlss5-feed.cfg'; cfg.write_bytes(b'user runtime config')
        self.selected['effects'] = True
        self.install(components=self.selected)
        ini = self.game / 'ReShade.ini'; ini.write_bytes(ini.read_bytes() + b'[USER]\r\nKeep=9\r\n')
        result = self.manager.uninstall(self.game)
        after = ini.read_bytes()
        self.assertFalse(result['settings_conflicts'])
        self.assertIn(b'NRStyle=1', after); self.assertIn(b'EffectSearchPaths=.\\user\\**', after)
        self.assertNotIn(b'DLSS5_MV_PROVIDER', after); self.assertIn(b'Keep=9', after)
        self.assertNotIn(str(self.state / 'effects').encode(), after)
        self.assertNotIn(str(self.state / 'dlss').encode(), after)
        self.assertEqual(cfg.read_bytes(), b'user runtime config')
        self.install(components=self.selected)
        self.assertTrue(self.manager.status(self.game)['components']['dlss'])

    def test_interrupted_settings_commit_has_recoverable_leases(self):
        (self.game / 'dxgi.dll').write_bytes(ADDON_BUILD)
        raw = b'[GENERAL]\nNoReloadOnInit=1\n'
        (self.game / 'ReShade.ini').write_bytes(raw)
        real = self.manager._repair_shortcuts
        def interrupted(*args, **kwargs):
            real(*args, **kwargs); raise OSError('invented post-settings interruption')
        with patch.object(self.manager, '_repair_shortcuts', side_effect=interrupted):
            with self.assertRaisesRegex(OSError, 'invented post-settings'): self.install(components=self.selected)
        self.assertEqual(self.record()['phase'], 'installing')
        self.assertIsNotNone(self.record()['dlss_consumer_settings'])
        self.manager.uninstall(self.game)
        self.assertIn(b'NoReloadOnInit=1', (self.game / 'ReShade.ini').read_bytes())
        self.assertNotIn(b'DLSS5_MV_PROVIDER', (self.game / 'ReShade.ini').read_bytes())

    def test_changed_owned_module_is_kept_and_refuses_reinstall(self):
        self.install(components=self.selected)
        path = self.game / 'renodx-dlss5.addon64'; path.write_bytes(b'external updated consumer')
        result = self.manager.uninstall(self.game)
        self.assertIn(path.name, result['kept'])
        self.assertEqual(path.read_bytes(), b'external updated consumer')
        with self.assertRaisesRegex(ValueError, 'already exists'):
            self.manager.preflight_components(self.game, self.selected)

    def test_preflight_refuses_ambiguous_settings_without_writes(self):
        for raw in (b'[GENERAL]\nEffectSearchPaths=one\nEffectSearchPaths=two\n',
                    b'[GENERAL]\nPreprocessorDefinitions=DLSS5_MV_PROVIDER=1,DLSS5_MV_PROVIDER=2\n',
                    b'[RenoDX.DLSS5]\nNRPasses=1\nNRPasses=2\n'):
            (self.game / 'ReShade.ini').write_bytes(raw)
            with self.subTest(raw=raw), self.assertRaisesRegex(ValueError, 'Duplicate'):
                self.manager.preflight_components(self.game, self.selected)
            self.assertEqual((self.game / 'ReShade.ini').read_bytes(), raw)
            self.assertFalse((self.state / 'dlss').exists())
            self.assertFalse((self.game / 'TextureQuest.addon64').exists())


if __name__ == '__main__': unittest.main()
