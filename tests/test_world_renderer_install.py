"""New optional renderer installs use invented D-only files, never native APIs."""
from pathlib import Path
import hashlib
import json
import tempfile
from unittest.mock import patch

from test_graphics_proxy import Fixture, ADDON_BUILD
from test_dlss_install import enable_dlss_fixture
from texturequest_sideload import LoaderError
import texturequest_graphics_proxy as graphics
import texturequest_components as components


class WorldRendererInstall(Fixture):
    def setUp(self):
        base = Path(__file__).resolve().parents[2] / 'comparison/sichii-integration/installer-001/fake-layouts'
        base.mkdir(parents=True, exist_ok=True)
        local = patch.object(tempfile, 'tempdir', str(base))
        local.start(); self.addCleanup(local.stop)
        super().setUp()
        self.selected = dict(components.normalize(), effects=True)
        self.renderer_runtime()

    def renderer_runtime(self, raw=b'invented renderer, never executable'):
        path = self.runtime / graphics.RENDERER_FILE
        path.parent.mkdir(exist_ok=True)
        path.write_bytes(raw)
        doc = json.loads((self.runtime / 'RUNTIME.json').read_bytes())
        doc['schema'] = 'texturequest-graphics-runtime-v8'
        doc['renderer'] = dict(schema='texturequest-world-renderer-v1',
            id=graphics.RENDERER_PROFILE_ID, private=True, api='d3d11',
            files=[dict(name=graphics.RENDERER_FILE, bytes=len(raw), sha256=hashlib.sha256(raw).hexdigest())])
        (self.runtime / 'RUNTIME.json').write_text(json.dumps(doc))

    def legacy_effects(self, *, version='texturequest-graphics-runtime-v8'):
        path = self.runtime / 'effects/TQEffects.fx'
        path.parent.mkdir(exist_ok=True)
        path.write_bytes(b'invented old effect, never compiled')
        doc = json.loads((self.runtime / 'RUNTIME.json').read_bytes())
        doc['schema'] = version
        if version != 'texturequest-graphics-runtime-v8': doc.pop('renderer', None)
        doc['effects'] = dict(schema='texturequest-effects-runtime-v4', shader='effects/TQEffects.fx',
            effect_name='TQEffects.fx', techniques=['TQAmbientOcclusion', 'TQContactShadows', 'TQDepthDiagnostic'])
        doc['files'].append(dict(name='effects/TQEffects.fx', bytes=path.stat().st_size,
            sha256=hashlib.sha256(path.read_bytes()).hexdigest()))
        (self.runtime / 'RUNTIME.json').write_text(json.dumps(doc))

    def test_selected_renderer_only_roundtrip_owns_exact_file_without_legacy_shader(self):
        (self.game / 'paks/stock.vpk').write_bytes(b'unchanged invented stock')
        self.assertTrue(self.manager.status(self.game)['effects_available'])
        self.assertTrue(self.manager.status(self.game)['renderer_available'])
        self.manager.preflight_components(self.game, self.selected)
        self.install(components=self.selected)
        row = self.record()
        self.assertEqual(row['renderer_profile'], graphics.RENDERER_PROFILE_ID)
        self.assertEqual((self.game / graphics.RENDERER_MODULE).read_bytes(),
                         (self.runtime / graphics.RENDERER_FILE).read_bytes())
        ini = (self.game / 'TextureQuest.ini').read_bytes()
        self.assertIn(b'WorldGraphics=1', ini); self.assertIn(b'Effects=0', ini)
        self.assertIsNone(row['effects_settings'])
        self.assertFalse((self.state / 'effects').exists())
        self.assertEqual((self.game / 'paks/stock.vpk').read_bytes(), b'unchanged invented stock')
        self.manager.uninstall(self.game)
        self.assertFalse((self.game / graphics.RENDERER_MODULE).exists())

    def test_unselected_missing_optional_module_keeps_textures_available(self):
        (self.runtime / graphics.RENDERER_FILE).unlink()
        status = self.manager.status(self.game)
        self.assertFalse(status['effects_available']); self.assertTrue(status['available'])
        self.install()
        self.assertFalse((self.game / graphics.RENDERER_MODULE).exists())
        self.assertIn(b'WorldGraphics=0', (self.game / 'TextureQuest.ini').read_bytes())

    def test_renderer_selection_suppresses_bundled_legacy_effects(self):
        self.legacy_effects()
        self.install(components=self.selected)
        self.assertFalse((self.state / 'effects').exists())
        self.assertIn(b'Effects=0', (self.game / 'TextureQuest.ini').read_bytes())
        self.assertNotIn(b'TQEffects', (self.game / 'ReShade.ini').read_bytes())

    def test_old_v7_legacy_effects_still_install_without_world_renderer(self):
        self.legacy_effects(version='texturequest-graphics-runtime-v7')
        self.install(components=self.selected)
        ini = (self.game / 'TextureQuest.ini').read_bytes()
        self.assertIn(b'Effects=1', ini); self.assertIn(b'WorldGraphics=0', ini)
        self.assertFalse((self.game / graphics.RENDERER_MODULE).exists())
        self.assertIsNotNone(self.record()['effects_settings'])

    def test_normal_legacy_uninstall_then_renderer_update_restores_old_lease(self):
        self.legacy_effects(version='texturequest-graphics-runtime-v7')
        (self.game / 'dxgi.dll').write_bytes(ADDON_BUILD)
        (self.game / 'ReShade.ini').write_bytes(b'[GENERAL]\nEffectSearchPaths=.\\mine\\**\nNoReloadOnInit=1\n')
        self.install(components=self.selected)
        self.assertIn(str(self.state / 'effects').encode(), (self.game / 'ReShade.ini').read_bytes())
        self.manager.uninstall(self.game)
        self.renderer_runtime()
        self.install(components=self.selected)
        ini = (self.game / 'ReShade.ini').read_bytes()
        self.assertIn(b'EffectSearchPaths=.\\mine\\**', ini)
        self.assertNotIn(str(self.state / 'effects').encode(), ini)
        self.assertIsNone(self.record()['effects_settings'])

    def test_foreign_renderer_refuses_before_cache_activation_and_preserves_other_addons(self):
        foreign = self.game / graphics.RENDERER_MODULE
        foreign.write_bytes(b'foreign renderer')
        other = self.game / 'UserLighting.addon64'; other.write_bytes(b'unrelated')
        with self.assertRaisesRegex(ValueError, 'already exists'):
            self.manager.preflight_components(self.game, self.selected)
        with self.assertRaisesRegex(ValueError, 'already exists'):
            self.install(components=self.selected)
        self.manager._activate_prepared.assert_not_called()
        self.assertEqual(foreign.read_bytes(), b'foreign renderer')
        self.assertEqual(other.read_bytes(), b'unrelated')
        self.assertFalse((self.game / 'TextureQuest.ini').exists())

    def test_owned_current_renderer_can_preflight_new_pinned_payload_then_normal_update(self):
        self.install(components=self.selected)
        old = (self.game / graphics.RENDERER_MODULE).read_bytes()
        self.renderer_runtime(b'invented reviewed next renderer')
        self.assertTrue(self.manager.status(self.game)['refresh_required'])
        self.manager.preflight_components(self.game, self.selected)
        self.assertEqual((self.game / graphics.RENDERER_MODULE).read_bytes(), old)
        self.manager.uninstall(self.game)
        self.install(components=self.selected)
        self.assertEqual((self.game / graphics.RENDERER_MODULE).read_bytes(), b'invented reviewed next renderer')

    def test_bad_update_pin_refuses_while_old_hook_remains_installed(self):
        self.install(components=self.selected)
        old_record = self.record(); old = (self.game / graphics.RENDERER_MODULE).read_bytes()
        (self.runtime / graphics.RENDERER_FILE).write_bytes(b'bad unpinned change')
        with self.assertRaises((ValueError, LoaderError)):
            self.manager.preflight_components(self.game, self.selected)
        self.assertEqual(self.record(), old_record)
        self.assertEqual((self.game / graphics.RENDERER_MODULE).read_bytes(), old)

    def test_changed_owned_renderer_is_kept_and_selected_reinstall_refuses(self):
        self.install(components=self.selected)
        path = self.game / graphics.RENDERER_MODULE; path.write_bytes(b'later user renderer')
        result = self.manager.uninstall(self.game)
        self.assertIn(path.name, result['kept'])
        self.assertEqual(path.read_bytes(), b'later user renderer')
        with self.assertRaisesRegex(ValueError, 'already exists'):
            self.manager.preflight_components(self.game, self.selected)

    def test_disabling_effects_normally_removes_only_owned_renderer(self):
        other = self.game / 'Keep.addon64'; other.write_bytes(b'foreign')
        self.install(components=self.selected)
        self.manager.uninstall(self.game)
        self.install()
        self.assertFalse((self.game / graphics.RENDERER_MODULE).exists())
        self.assertEqual(other.read_bytes(), b'foreign')
        self.assertIn(b'WorldGraphics=0', (self.game / 'TextureQuest.ini').read_bytes())

    def test_existing_addon_path_is_respected_and_owned_file_removed_there(self):
        (self.game / 'addons').mkdir()
        (self.game / 'dxgi.dll').write_bytes(ADDON_BUILD)
        (self.game / 'ReShade.ini').write_text('[ADDON]\nAddonPath=addons\n')
        self.install(components=self.selected)
        self.assertTrue((self.game / 'addons' / graphics.RENDERER_MODULE).is_file())
        self.assertFalse((self.game / graphics.RENDERER_MODULE).exists())
        self.manager.uninstall(self.game)
        self.assertFalse((self.game / 'addons' / graphics.RENDERER_MODULE).exists())

    def test_descriptor_contract_refuses_unknown_duplicate_or_unsafe_roles(self):
        original = (self.runtime / 'RUNTIME.json').read_bytes()
        for issue in ('identity', 'extra', 'duplicate', 'traversal', 'pin'):
            doc = json.loads(original)
            if issue == 'identity': doc['renderer']['id'] = 'unknown'
            elif issue == 'extra': doc['renderer']['extra'] = True
            elif issue == 'duplicate': doc['renderer']['files'] *= 2
            elif issue == 'traversal': doc['renderer']['files'][0]['name'] = '../outside.addon64'
            else: doc['renderer']['files'][0]['sha256'] = '0' * 64
            (self.runtime / 'RUNTIME.json').write_text(json.dumps(doc))
            with self.subTest(issue=issue), self.assertRaises((ValueError, LoaderError)):
                self.install(components=self.selected)
            self.assertFalse((self.game / 'TextureQuest.addon64').exists())
            self.manager._activate_prepared.assert_not_called()

    def test_v8_renderer_and_dlss_are_independent_without_legacy_effect_loading(self):
        enable_dlss_fixture(self.runtime)
        self.renderer_runtime()
        selected = dict(self.selected, dlss=True)
        self.install(components=selected)
        self.assertIsNotNone(self.record()['dlss_profile'])
        self.assertIsNotNone(self.record()['renderer_profile'])
        self.assertFalse((self.state / 'effects').exists())
        ini = (self.game / 'TextureQuest.ini').read_bytes()
        self.assertIn(b'DLSS=1', ini); self.assertIn(b'WorldGraphics=1', ini); self.assertIn(b'Effects=0', ini)
        self.manager.uninstall(self.game)
        self.assertFalse((self.game / graphics.RENDERER_MODULE).exists())
        self.assertFalse((self.game / 'renodx-dlss5.addon64').exists())
