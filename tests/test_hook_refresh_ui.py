"""New-version hook reinstall using a current, already prepared channel cache."""
from unittest.mock import patch
from test_loose_dds_ui import GraphicsOnlyUI
import texturequest_app as app
from texture_swapper import core


class HookRefreshUI(GraphicsOnlyUI):
    def test_new_version_reinstalls_same_dll_and_hides_tutorial_with_cached_textures(self):
        from test_channels import fixture
        game, source, index, examples, choices, opener, requests = fixture(self.root/'channel-current')
        self.game = game; self.ui.game.set(str(game))
        prepared = self.ui.dds_store.prepare(source, game, choices, opener=opener)
        self.manager.install(game, prepared)
        self.ui.upgrade.complete(game, prepared)
        old = self.manager._record(game); old['app_version'] = '1.9.2'; self.manager._save(game, old)
        config = game/'ReShade.ini'
        config.write_bytes(config.read_bytes().replace(b'TutorialProgress=4', b'TutorialProgress=0'))
        dll = (game/'TextureQuest.addon64').read_bytes()
        pak = (game/'paks/test.vpk').read_bytes()
        self.ui.prepared = prepared; self.ui.release_checked = True
        self.ui.refresh_controls()
        self.assertTrue(self.ui.upgrade_pending())
        requests.clear()
        prepare = self.ui.dds_store.prepare
        with patch.object(app, 'fetch_dds_source', return_value=source), \
             patch.object(self.ui.dds_store, 'prepare', side_effect=lambda *a, **kw: prepare(*a, **kw, opener=opener)), \
             patch.object(self.ui, 'begin', side_effect=lambda kind,message,action: action()), \
             patch.object(self.manager, 'install', wraps=self.manager.install) as install:
            self.assertTrue(self.ui.manage_graphics())
        install.assert_called_once()
        self.assertEqual(self.manager._record(game)['app_version'], '1.9.3')
        self.assertNotEqual(self.manager._record(game)['id'], old['id'])
        self.assertEqual((game/'TextureQuest.addon64').read_bytes(), dll)
        self.assertEqual((game/'paks/test.vpk').read_bytes(), pak)
        self.assertIn(b'TutorialProgress=4', config.read_bytes())
        self.assertFalse(self.ui.upgrade_pending())
        self.assertEqual(requests, [])

    def test_no_extra_reinstall_action_is_added(self):
        labels = [self.ui.menu.entrycget(i, 'label') for i in range(self.ui.menu.index('end')+1)
                  if self.ui.menu.type(i) == 'command']
        self.assertNotIn('Reinstall graphics hook', labels)

    def test_auto_refresh_cannot_downgrade_a_newer_hook(self):
        status = self.ui.graphics_status(str(self.game))
        status['installed_newer_app'] = True
        with patch.object(self.ui, 'graphics_status', return_value=status), \
             patch.object(self.ui, 'begin', side_effect=lambda kind,message,action: action()), \
             patch.object(app, 'fetch_dds_source') as fetch:
            with self.assertRaisesRegex(ValueError, 'newer TextureQuest app'):
                self.ui._download_latest(auto_install=True, upgrading=True)
        fetch.assert_not_called()
