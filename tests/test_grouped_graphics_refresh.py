"""Normal refresh reconciles legacy bundle flags; synthetic D metadata only."""
from pathlib import Path
import os
import tempfile
import threading
import unittest
from unittest.mock import MagicMock, patch

import texturequest_app as app
import texturequest_components as components
import texturequest_graphics_proxy as proxy

ROOT = Path(os.environ['TQ_GROUPED_GRAPHICS_TEST_ROOT']).absolute()
if ROOT.drive.casefold() != 'd:' or not ROOT.is_relative_to(Path('D:/eq2 paks')):
    raise RuntimeError('Require an owned D grouped-graphics test root')
ROOT.mkdir(parents=True, exist_ok=True)


class Value:
    def __init__(self, value=False): self.value = value
    def get(self): return self.value
    def set(self, value): self.value = value


class GroupedRefresh(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(dir=ROOT); self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name); self.game = self.root / 'synthetic-game'; self.game.mkdir()
        self.ui = app.TextureQuestApp.__new__(app.TextureQuestApp)
        ui = self.ui
        ui.state_dir = self.root / 'data'; ui.game = Value(str(self.game))
        ui.component_effects = Value(); ui.component_dlss = Value(); ui.component_graphics = Value()
        ui.component_snapshot = None; ui.component_error = ''
        ui.busy = None; ui.update_check = {}; ui.prepared = None; ui.release = {}
        ui.shortcut = dict(vk=106, modifiers=0); ui.pause = threading.Event()
        ui.graphics_manager = MagicMock(); ui.dds_store = MagicMock(); ui.upgrade = MagicMock()
        ui.downloads_in_use = lambda: False
        ui.report = MagicMock(); ui.report_overall = MagicMock(); ui.set_message = MagicMock()
        self.status = dict(unknown=False, effects_available=True, dlss_available=True)
        ui.graphics_status = MagicMock(side_effect=lambda _: dict(self.status))
        self.completed = []
        ui.begin = MagicMock(side_effect=lambda kind, message, work: self.completed.append(work()))
        self.choices = self.root / 'choices.json'; self.choices.write_bytes(b'{}')
        self.source, self.prepared = {'source': 'fixture'}, {'prepared': 'fixture'}

    def save_legacy(self, effects, dlss):
        selection = dict(components.normalize(), effects=effects, dlss=dlss)
        before = components.save(self.ui.state_dir, self.game, selection, expected_before=None)
        self.ui.load_components()
        return before

    def refresh(self, **kwargs):
        with patch.object(app, 'resource', return_value=self.choices), \
             patch('texturequest_setup.prepare_graphics', return_value=(self.source, self.prepared)) as prepare:
            result = self.ui._download_latest(auto_install=True, upgrading=True, **kwargs)
        return result, prepare

    def selected_by_worker(self, prepare):
        prepare.assert_called_once()
        return prepare.call_args.kwargs['components']

    def addon_ini(self, selection):
        manager = proxy.GraphicsManager(self.ui.state_dir, runtime_dir=self.root / 'not-executed')
        return manager._addon_ini(self.game, self.root / 'map.bin', selection, dlss_backend='aio', world_graphics=True)

    def test_legacy_effects_only_refresh_includes_both_available_bundles(self):
        before = self.save_legacy(True, False)
        self.assertTrue(self.ui.component_graphics.get())
        _, prepare = self.refresh()
        selected = self.selected_by_worker(prepare)
        self.assertTrue(selected['effects']); self.assertTrue(selected['dlss'])
        after = components.load(self.ui.state_dir, self.game)
        self.assertEqual(after['selection'], selected)
        self.assertNotEqual(after['record_sha256'], before['record_sha256'])
        self.assertIn(b'DLSS=1\r\nDLSSBackend=aio', self.addon_ini(selected))
        self.assertEqual(self.ui.dds_source, self.source)

    def test_legacy_dlss_only_refresh_uses_the_same_group_as_explicit_install(self):
        self.save_legacy(False, True)
        explicit = self.ui.selected_components(apply_graphics=True, capabilities=self.status)
        _, prepare = self.refresh()
        self.assertEqual(self.selected_by_worker(prepare), explicit)
        self.assertTrue(explicit['effects']); self.assertTrue(explicit['dlss'])

    def test_explicit_single_bundle_selection_also_reconciles(self):
        self.save_legacy(False, False)
        _, prepare = self.refresh(selection=dict(components.normalize(), effects=True))
        selected = self.selected_by_worker(prepare)
        self.assertTrue(selected['effects']); self.assertTrue(selected['dlss'])

    def test_textures_only_group_off_remains_off(self):
        before = self.save_legacy(False, False)
        _, prepare = self.refresh()
        self.assertEqual(self.selected_by_worker(prepare), components.normalize())
        self.assertEqual(components.load(self.ui.state_dir, self.game), before)
        self.assertIn(b'DLSS=0', self.addon_ini(components.normalize()))

    def test_missing_dlss_profile_never_selects_or_preflights_dlss(self):
        self.save_legacy(True, False); self.status['dlss_available'] = False
        _, prepare = self.refresh()
        selected = self.selected_by_worker(prepare)
        self.assertTrue(selected['effects']); self.assertFalse(selected['dlss'])
        manager = proxy.GraphicsManager(self.ui.state_dir, runtime_dir=self.root / 'not-executed')
        with patch.object(manager, '_preflight_renderer'), patch.object(manager, '_preflight_optional_settings'), \
             patch.object(manager, '_dlss_profile', side_effect=AssertionError('Unavailable profile must not be installed')):
            self.assertIsNone(manager._preflight_components(self.game, selected, {'renderer': {}}))
        self.assertIn(b'DLSS=0', self.addon_ini(selected))

    def test_selected_group_preserves_existing_feature_optouts_and_home(self):
        before = self.save_legacy(True, True)
        settings = self.game / 'ReShade.ini'
        raw = b'[INPUT]\r\nKeyOverlay=36,0,0,0\r\n[TextureQuest]\r\nFrameGeneration=0\r\nNeuralRendering=0\r\nManagedSrEnabled=0\r\n'
        settings.write_bytes(raw)
        _, prepare = self.refresh()
        self.assertTrue(self.selected_by_worker(prepare)['dlss'])
        self.assertEqual(settings.read_bytes(), raw)
        self.assertEqual(components.load(self.ui.state_dir, self.game), before)

    def test_read_only_loading_does_not_migrate_or_save(self):
        before = self.save_legacy(True, False)
        with patch.object(components, 'save', side_effect=AssertionError('Read-only load')):
            self.ui.load_components()
        self.assertEqual(self.ui.selected_components(), before['selection'])
        self.assertEqual(components.load(self.ui.state_dir, self.game), before)

    def test_unknown_capability_status_refuses_without_clearing_saved_selection(self):
        before = self.save_legacy(True, False)
        self.status = dict(unknown=True, message='Synthetic runtime query failure')
        result, prepare = self.refresh()
        self.assertFalse(result); prepare.assert_not_called(); self.ui.begin.assert_not_called()
        self.assertEqual(components.load(self.ui.state_dir, self.game), before)
        self.assertIn('query failure', self.ui.set_message.call_args.args[0])

    def test_stale_preference_snapshot_refuses_before_worker(self):
        self.save_legacy(True, False)
        current = components.load(self.ui.state_dir, self.game)
        changed = components.save(self.ui.state_dir, self.game, components.normalize(), expected_before=current)
        result, prepare = self.refresh()
        self.assertFalse(result); prepare.assert_not_called(); self.ui.begin.assert_not_called()
        self.assertEqual(components.load(self.ui.state_dir, self.game), changed)

    def test_incomplete_or_nonboolean_capabilities_refuse_before_save_or_worker(self):
        before = self.save_legacy(True, False)
        for status in ({'effects_available': True}, {'dlss_available': True},
                       {'effects_available': None, 'dlss_available': True},
                       {'effects_available': True, 'dlss_available': 1}):
            with self.subTest(status=status):
                self.status = status
                with patch.object(components, 'save', side_effect=AssertionError('Invalid capabilities must not save')):
                    result, prepare = self.refresh()
                self.assertFalse(result); prepare.assert_not_called(); self.ui.begin.assert_not_called()
                self.assertEqual(components.load(self.ui.state_dir, self.game), before)


if __name__ == '__main__': unittest.main(verbosity=2)
