"""Combined-suite admission and normal native-settings lifecycle on D fixtures."""
import json
from unittest.mock import patch
from test_effects_install import EffectsInstall
import texturequest_graphics_proxy as graphics

class SuiteInstall(EffectsInstall):
    def setUp(self):
        super().setUp()
        path=self.runtime/'RUNTIME.json'
        doc=json.loads(path.read_bytes())
        doc['schema']='texturequest-graphics-runtime-v6'
        doc['effects']=dict(schema='texturequest-effects-runtime-v3',shader='effects/TQEffects.fx',
            effect_name='TQEffects.fx',techniques=['TQAmbientOcclusion','TQContactShadows',
            'TQIndirectLighting','TQAntiAliasing','TQDepthDiagnostic'])
        path.write_text(json.dumps(doc))

    def test_suite_contract_is_exact(self):
        self.assertEqual(len(self.manager._runtime()['effects']['techniques']),5)
        path=self.runtime/'RUNTIME.json';doc=json.loads(path.read_bytes())
        doc['effects']['techniques'].pop();path.write_text(json.dumps(doc))
        with self.assertRaisesRegex(ValueError,'Unknown optional effects'):
            self.install(components=self.selected)
        self.assertFalse((self.game/'TextureQuest.addon64').exists())

    def test_settings_lifecycle_uses_normal_outside_lock_calls(self):
        with patch.object(graphics,'GameSettings') as factory:
            manager=factory.return_value
            manager.apply.return_value={'status':'unmanaged'}
            manager.restore.return_value={'status':'restored','conflicts':[]}
            def unlocked(*args,**kwargs):
                from texture_swapper import core
                with core._lock(self.state/'operation.lock'):
                    pass
                return {'status':'unmanaged','conflicts':[]}
            manager.apply.side_effect=unlocked;manager.restore.side_effect=unlocked
            self.install(components=self.selected)
            self.assertEqual(manager.apply.call_args.args[:2],(self.game,{}))
            self.assertIn('contact_shadows',manager.apply.call_args.kwargs['features'])
            self.manager.uninstall(self.game)
            manager.restore.assert_called_once_with(self.game)

    def test_later_native_edits_are_reported(self):
        with patch.object(graphics,'GameSettings') as factory:
            factory.return_value.apply.return_value={'status':'unmanaged'}
            factory.return_value.restore.return_value={'status':'restored_with_conflicts','conflicts':['changed']}
            self.install(components=self.selected)
            result=self.manager.uninstall(self.game)
            self.assertIn('Later game settings were kept',result['message'])

    def test_interrupted_native_restore_can_finish_normal_uninstall(self):
        with patch.object(graphics,'GameSettings') as factory:
            factory.return_value.apply.return_value={'status':'unmanaged'}
            factory.return_value.restore.side_effect=[ValueError('restore interrupted'),
                {'status':'restored','conflicts':[]}]
            self.install(components=self.selected)
            with self.assertRaisesRegex(ValueError,'restore interrupted'):
                self.manager.uninstall(self.game)
            self.assertEqual(self.manager._record(self.game)['phase'],'uninstalling')
            self.manager.uninstall(self.game)
            self.assertEqual(self.manager._record(self.game)['phase'],'uninstalled')
            self.assertEqual(factory.return_value.restore.call_count,2)
