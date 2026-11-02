"""Real normal retirement/install on invented D-only games; never load DLLs."""
from pathlib import Path
import json
from types import SimpleNamespace, MethodType
from unittest.mock import Mock, patch

from test_graphics_proxy import Fixture
from test_dlss_install import enable_dlss_fixture
from test_aio_install import use_aio
from test_dlss_flow import Variable
import texturequest_app as app
import texturequest_components as components
import texturequest_dlss as settings
import texturequest_graphics_proxy as graphics
import texturequest_setup as setup
from texturequest_upgrade import Upgrade
from texture_swapper import core


class ControlHandoff(Fixture):
    def setUp(self):
        super().setUp()
        for name in ('texturequest_game_settings.ensure_closed',
                     'texturequest_upgrade.ensure_closed', 'texturequest_setup.ensure_closed'):
            p = patch(name); p.start(); self.addCleanup(p.stop)
        enable_dlss_fixture(self.runtime); use_aio(self.runtime)
        path = self.runtime / 'RUNTIME.json'
        value = json.loads(path.read_bytes()); value['dlss']['id'] = graphics.AIO_V7_PROFILE_ID
        path.write_text(json.dumps(value))
        self.selected = dict(components.normalize(), dlss=True)

    def install_old_v2(self):
        # The old aio2 producer used the original map. This runs that same
        # normal preparation/installation path, not a forged installed lease.
        (self.game / 'ReShade.ini').write_bytes(b'[Standalone.DLSSNR]\nManagedSrEnabled=0\n[USER]\nKeep=1\n')
        original = settings._profile_values
        with patch.object(settings, '_profile_values', side_effect=lambda p:
                (settings.AIO_V2_SCHEMA, settings.AIO_VALUES) if p == 'aio2' else original(p)):
            self.install(components=self.selected)
        lease = self.record()['dlss_consumer_settings']['journal']
        self.assertEqual(lease['schema'], settings.AIO_V2_SCHEMA)
        for name in ('NeuralRendering', 'FrameGeneration', 'SourceResolutionOverride'):
            self.assertIsNone(next(x for x in lease['changes'] if x['key'] == name)['before'])

    def assert_controls(self, nr=1, fg=1, sr=0, source=0):
        raw = (self.game / 'ReShade.ini').read_bytes()
        for name, value in (('NeuralRendering', nr), ('FrameGeneration', fg),
                            ('ManagedSrEnabled', sr), ('SourceResolutionOverride', source)):
            self.assertIn((name + '=' + str(value)).encode(), raw)

    def ui(self):
        ui = SimpleNamespace(state_dir=self.state, game=Variable(str(self.game)), busy=None,
            component_effects=Variable(False), component_dlss=Variable(False), component_graphics=Variable(False),
            component_snapshot=None, component_error='', prepared={'distribution': 'generated-channels-only'},
            release_checked=True, graphics_selected=Mock(return_value=True),
            graphics_status=lambda game: dict(self.manager.status(game), unknown=False),
            graphics_manager=self.manager, loader_status=Mock(return_value={}), loader_in_use=Mock(return_value=False),
            upgrade_pending=Mock(return_value=False), _download_latest=Mock(), dds_store=Mock(),
            set_message=Mock(), refresh_controls=Mock(), report=Mock(), shortcut={'vk': 106, 'modifiers': 0}, upgrade=Mock())
        for name in ('load_components', 'selected_components', 'save_components'):
            setattr(ui, name, MethodType(getattr(app.TextureQuestApp, name), ui))
        ui.begin = Mock(side_effect=lambda kind, message, action: action())
        components.save(self.state, self.game, self.selected, expected_before=None)
        ui.load_components()
        return ui

    def test_direct_apply_transfers_old_owned_controls_to_new_v3(self):
        self.install_old_v2(); self.assert_controls()
        ui = self.ui()
        with patch.object(self.manager, 'uninstall', wraps=self.manager.uninstall) as remove:
            self.assertTrue(app.TextureQuestApp.manage_graphics(ui))
        self.assertTrue(remove.call_args.kwargs['preserve_dlss_controls'])
        self.assert_controls()
        lease = self.record()['dlss_consumer_settings']['journal']
        self.assertEqual(lease['schema'], settings.AIO_V3_SCHEMA)
        self.assertFalse({(x['section'], x['key']) for x in lease['changes']} & settings.AIO_CONTROL_KEYS)
        self.assertTrue(self.record()['components']['effects'])
        ui.dds_store.verify.assert_called_once_with(ui.prepared, full=True)

    def test_prepare_first_upgrade_keeps_later_edits_between_retirement_and_install(self):
        self.install_old_v2()
        upgrade = Upgrade(self.state, self.manager)
        upgrade._stock = Mock(); upgrade.complete = Mock()
        prepared = {'distribution': 'generated-channels-only'}
        store = Mock(); order = []
        def prepare(*args, **kwargs):
            core._save(self.state / 'dds-prepared.json', prepared)
            order.append('prepare'); return prepared
        store.prepare.side_effect = prepare
        store.verify.side_effect = lambda *a, **k: order.append('verify')
        original = self.manager.uninstall
        def retire(*args, **kwargs):
            self.assertIn('verify', order)
            self.assertTrue(kwargs['preserve_dlss_controls'])
            result = original(*args, **kwargs)
            self.assert_controls()
            ini = self.game / 'ReShade.ini'
            ini.write_bytes(ini.read_bytes().replace(b'NeuralRendering=1', b'NeuralRendering=0')
                            .replace(b'SourceResolutionOverride=0', b'SourceResolutionOverride=2'))
            order.append('retire'); return result
        with patch.object(self.manager, 'uninstall', side_effect=retire):
            setup.prepare_graphics(self.game, Mock(return_value={}), {}, store, self.manager, upgrade,
                minimum='1.3.0', binding={'vk': 106, 'modifiers': 0}, in_use=lambda: False,
                auto_install=True, components=self.selected)
        self.assertEqual(order[:3], ['prepare', 'verify', 'retire'])
        self.assert_controls(nr=0, source=2)
        self.assertEqual(self.record()['dlss_consumer_settings']['journal']['schema'], settings.AIO_V3_SCHEMA)
        upgrade.complete.assert_called_once()

    def test_true_uninstall_still_restores_original_old_v2_ownership(self):
        self.install_old_v2()
        self.manager.uninstall(self.game)
        raw = (self.game / 'ReShade.ini').read_bytes()
        for name in ('NeuralRendering=', 'FrameGeneration=', 'SourceResolutionOverride='):
            self.assertNotIn(name.encode(), raw)
        self.assertIn(b'ManagedSrEnabled=0', raw)
        self.assertEqual(self.record()['phase'], 'uninstalled')

    def test_replacement_keeps_originally_created_ini_even_without_control_lease(self):
        self.install()
        self.assertIsNone(self.record()['dlss_consumer_settings'])
        before = (self.game / 'ReShade.ini').read_bytes()
        row = next(row for row in self.record()['files'] if Path(row['path']).name == 'ReShade.ini')
        self.assertEqual(graphics._sha(before), row['sha256'])
        result = self.manager.uninstall(self.game, preserve_dlss_controls=True)
        self.assertIn('ReShade.ini', result['kept'])
        self.assertEqual((self.game / 'ReShade.ini').read_bytes(), before)

    def test_settings_commit_failure_rolls_back_before_hook_retirement(self):
        from texturequest_direct import FileCustody
        self.install_old_v2()
        before = (self.game / 'ReShade.ini').read_bytes(); record = self.manager._record(self.game)
        original = FileCustody.rename_new
        def refuse_new_output(held, target, directories):
            if held.path.suffix == '.tmp' and Path(target).name == 'ReShade.ini':
                raise OSError('invented output commit refusal')
            return original(held, target, directories)
        with patch.object(FileCustody, 'rename_new', new=refuse_new_output), \
             self.assertRaisesRegex(OSError, 'invented output commit refusal'):
            self.manager.uninstall(self.game, preserve_dlss_controls=True)
        self.assertEqual((self.game / 'ReShade.ini').read_bytes(), before)
        self.assertEqual(self.manager._record(self.game), record)
        self.assertTrue((self.game / 'TextureQuest.addon64').is_file())
