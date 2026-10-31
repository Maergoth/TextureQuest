"""Add-on installer tests: invented game folders and DLLs, no native calls."""
from pathlib import Path
import hashlib
import json
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'src'))
import texturequest_graphics_proxy as g

ADDON_BUILD = b'MZ fake ReShadeRegisterAddon add-on build'
STANDARD_BUILD = b'MZ fake ReShadeRegisterAddon only limited add-on functionality'
FOREIGN = b'MZ some other proxy'
PROVIDER_170 = b'MZ fake 1.7.0 provider ReShadeRegisterAddon'


class Fixture(unittest.TestCase):
    def setUp(self):
        self.root = Path(tempfile.mkdtemp())
        self.game = self.root / 'Game ü'
        (self.game / 'paks').mkdir(parents=True)
        (self.game / 'EverQuest2.exe').write_bytes(b'exe')
        self.state = self.root / 'state'
        (self.state / 'downloads').mkdir(parents=True)
        self.runtime = self.root / 'runtime'
        self.runtime.mkdir()
        (self.runtime / 'TextureQuest.addon64').write_bytes(b'addon')
        (self.runtime / 'ReShade64.dll').write_bytes(ADDON_BUILD)
        files = [dict(name=n, bytes=(self.runtime / n).stat().st_size, sha256=hashlib.sha256((self.runtime / n).read_bytes()).hexdigest())
                 for n in ['TextureQuest.addon64', 'ReShade64.dll']]
        (self.runtime / 'RUNTIME.json').write_text(json.dumps(dict(schema='texturequest-graphics-runtime-v3', addon='TextureQuest.addon64',
            reshade='ReShade64.dll', reshade_version='6.8.0', reshade_build='addon', map_schema='TQMAP001', files=files)))
        self.map = self.state / 'graphics' / 'texturequest-map-1.2.3.bin'
        self.map.parent.mkdir(parents=True)
        self.map.write_bytes(b'map')
        self.manager = g.GraphicsManager(self.state, runtime_dir=self.runtime)
        self.patches = [patch.object(g, 'ensure_closed'), patch.object(g.GraphicsManager, '_games_open', return_value=0),
                        patch.object(g.GraphicsManager, '_exclusive_mode'),
                        patch.object(g, 'fetch_map', return_value=self.map),
                        patch.object(g.core, 'validate_game', side_effect=lambda game: Path(game) / 'EverQuest2.exe')]
        for p in self.patches:
            p.start()
        self.addCleanup(lambda: [p.stop() for p in self.patches])

    def install(self, **kwargs):
        return self.manager.install(self.game, {'release_sha256': 'a' * 64}, **kwargs)

    def record(self):
        return json.loads(next((self.state / 'graphics-addon').glob('*.json')).read_text())

    def proxy_170(self, phase, present=('d3d11.dll', 'TextureQuest.ini')):
        """A 1.7.0 graphics-proxy record and game files, shaped as that release wrote them."""
        sources = {'TextureQuest.ini': b'[INSTALL]\nBasePath=c:\\cache\n', 'd3d11.dll': PROVIDER_170}
        for name in present:
            (self.game / name).write_bytes(sources[name])
        files = [dict(path=str(self.game / n), bytes=len(d), sha256=hashlib.sha256(d).hexdigest()) for n, d in sources.items()]
        doc = dict(schema='texturequest-graphics-proxy-installation-v1', phase=phase, id='b' * 32, game=str(self.game),
                   candidate=str(self.root / 'cache' / ('b' * 32)), files=files, shortcut=dict(vk=106, modifiers=0),
                   archive_redirection=False, background_process=False, rows=1, ambiguous=0)
        doc['record_sha256'] = g.core._digest(doc)
        g.core._atomic(self.state / 'graphics-proxy' / (self.manager._key(self.game) + '.json'), doc)

    def proxy_170_phase(self):
        return json.loads((self.state / 'graphics-proxy' / (self.manager._key(self.game) + '.json')).read_text(encoding='utf-8'))['phase']


class InstallTests(Fixture):
    def test_fresh_install_adds_reshade_and_addon(self):
        self.install()
        names = sorted(Path(f['path']).name for f in self.record()['files'])
        self.assertEqual(names, ['ReShade.ini', 'TextureQuest.addon64', 'TextureQuest.ini', 'dxgi.dll'])
        self.assertTrue(self.record()['reshade_owned'])
        self.assertEqual((self.game / 'dxgi.dll').read_bytes(), ADDON_BUILD)

    def test_existing_addon_build_gets_addon_only(self):
        (self.game / 'dxgi.dll').write_bytes(ADDON_BUILD)
        self.install()
        names = sorted(Path(f['path']).name for f in self.record()['files'])
        self.assertEqual(names, ['TextureQuest.addon64', 'TextureQuest.ini'])
        self.assertFalse(self.record()['reshade_owned'])

    def test_standard_reshade_refused(self):
        (self.game / 'dxgi.dll').write_bytes(STANDARD_BUILD)
        (self.game / 'ReShade.ini').write_text('[GENERAL]\nPresetPath=mine.ini\n')
        self.proxy_170('installed')
        with self.assertRaisesRegex(ValueError, 'dxgi.dll') as caught:
            self.install()
        self.assertIs(type(caught.exception), ValueError)
        self.assertEqual((self.game / 'dxgi.dll').read_bytes(), STANDARD_BUILD)
        self.assertEqual((self.game / 'ReShade.ini').read_text(), '[GENERAL]\nPresetPath=mine.ini\n')
        self.assertEqual(sorted(p.name for p in self.game.glob('TextureQuest.*')), ['TextureQuest.ini'])
        self.assertEqual((self.game / 'TextureQuest.ini').read_bytes(), b'[INSTALL]\nBasePath=c:\\cache\n')
        self.assertEqual(self.proxy_170_phase(), 'installed')
        self.assertFalse((self.state / 'graphics-addon').exists())
        self.assertFalse((self.state / 'graphics' / 'shortcut.txt').exists())
        g.fetch_map.assert_not_called()

    def test_foreign_proxy_refused(self):
        (self.game / 'd3d11.dll').write_bytes(FOREIGN)
        with self.assertRaisesRegex(ValueError, 'd3d11.dll'):
            self.install()
        self.assertFalse((self.game / 'TextureQuest.addon64').exists())

    def test_addon_path_respected(self):
        (self.game / 'dxgi.dll').write_bytes(ADDON_BUILD)
        (self.game / 'addons').mkdir()
        (self.game / 'ReShade.ini').write_text('[ADDON]\nAddonPath=.\\addons\n')
        self.install()
        self.assertTrue((self.game / 'addons' / 'TextureQuest.addon64').is_file())
        self.assertTrue((self.game / 'addons' / 'TextureQuest.ini').is_file())

    def test_unicode_game_path_round_trips_config(self):
        self.install()
        text = (self.game / 'TextureQuest.ini').read_text(encoding='utf-8')
        self.assertIn(str(self.game / 'paks'), text)
        self.assertIn(str(self.map), text)

    def test_legacy_proxy_uninstalled_first(self):
        with patch.object(g.GraphicsManager, '_uninstall_proxy_170') as legacy:
            legacy.side_effect = lambda game: None
            self.install()
            legacy.assert_called_once()


class Proxy170Tests(Fixture):
    def test_installed_record_migrates(self):
        self.proxy_170('installed')
        self.install()
        self.assertEqual(self.proxy_170_phase(), 'uninstalled')
        self.assertFalse((self.game / 'd3d11.dll').exists())
        self.assertEqual((self.game / 'dxgi.dll').read_bytes(), ADDON_BUILD)
        self.assertTrue(self.record()['reshade_owned'])

    def test_failed_record_with_only_ini_migrates(self):
        self.proxy_170('failed', present=('TextureQuest.ini',))
        self.install()
        self.assertEqual(self.proxy_170_phase(), 'uninstalled')
        self.assertTrue(self.record()['reshade_owned'])

    def test_changed_file_kept_and_install_refuses(self):
        self.proxy_170('installed')
        (self.game / 'TextureQuest.ini').write_bytes(b'edited')
        with self.assertRaisesRegex(ValueError, 'TextureQuest.ini'):
            self.install()
        self.assertEqual((self.game / 'TextureQuest.ini').read_bytes(), b'edited')
        self.assertFalse((self.game / 'd3d11.dll').exists())
        self.assertEqual(self.proxy_170_phase(), 'uninstalled')

    def test_unrecorded_provider_refused_as_foreign(self):
        (self.game / 'd3d11.dll').write_bytes(PROVIDER_170)
        with patch.object(g, 'PROVIDER_170_SHA256', hashlib.sha256(PROVIDER_170).hexdigest()):
            with self.assertRaisesRegex(ValueError, 'd3d11.dll'):
                self.install()
        self.assertFalse((self.game / 'TextureQuest.addon64').exists())

    def test_live_install_status_and_uninstall(self):
        self.proxy_170('installed')
        status = self.manager.status(self.game)
        self.assertTrue(status['installed'])
        self.assertIn('1.7.0', status['message'])
        self.assertEqual(self.manager.installed_game(), str(self.game))
        result = self.manager.uninstall(self.game)
        self.assertEqual(result['message'], 'Graphics hook removed.')
        self.assertFalse(result['installed'])
        self.assertFalse((self.game / 'd3d11.dll').exists())
        self.assertFalse((self.game / 'TextureQuest.ini').exists())
        self.assertEqual(self.proxy_170_phase(), 'uninstalled')


class UninstallTests(Fixture):
    def test_uninstall_removes_owned_files(self):
        self.install()
        self.manager.uninstall(self.game)
        for name in ['dxgi.dll', 'ReShade.ini', 'TextureQuest.addon64', 'TextureQuest.ini']:
            self.assertFalse((self.game / name).exists(), name)

    def test_patcher_removed_files_uninstall_cleanly(self):
        self.install()
        (self.game / 'dxgi.dll').unlink()
        self.manager.uninstall(self.game)
        self.assertFalse((self.game / 'TextureQuest.addon64').exists())

    def test_changed_file_kept(self):
        self.install()
        (self.game / 'TextureQuest.ini').write_text('edited', encoding='utf-8')
        result = self.manager.uninstall(self.game)
        self.assertTrue((self.game / 'TextureQuest.ini').exists())
        self.assertIn('TextureQuest.ini', result['message'])

    def test_changed_owned_reshade_kept(self):
        self.install()
        (self.game / 'dxgi.dll').write_bytes(b'MZ updated ReShade')
        result = self.manager.uninstall(self.game)
        self.assertEqual((self.game / 'dxgi.dll').read_bytes(), b'MZ updated ReShade')
        self.assertIn('dxgi.dll', result['message'])
        self.assertIn('dxgi.dll', self.record()['kept'])
        self.assertFalse((self.game / 'TextureQuest.addon64').exists())

    def test_existing_reshade_left_alone(self):
        (self.game / 'dxgi.dll').write_bytes(ADDON_BUILD)
        self.install()
        self.manager.uninstall(self.game)
        self.assertTrue((self.game / 'dxgi.dll').exists())


class ShortcutTests(Fixture):
    def test_shortcut_rewritten_without_game_changes(self):
        self.install()
        before = {p.name: p.read_bytes() for p in self.game.iterdir() if p.is_file()}
        self.manager.set_shortcut(self.game, {'vk': 0x6b, 'modifiers': 2})
        after = {p.name: p.read_bytes() for p in self.game.iterdir() if p.is_file()}
        self.assertEqual(before, after)
        self.assertEqual((self.state / 'graphics' / 'shortcut.txt').read_text(), 'TQKEY1\n107\n2\n')


if __name__ == '__main__':
    unittest.main()
