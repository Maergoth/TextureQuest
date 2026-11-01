import copy,json
from pathlib import Path
import sys
import unittest
from unittest.mock import patch

sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'src'))
from texture_swapper import core
import texturequest_release as release
import texturequest_version_check as versions


class ChannelRelease(unittest.TestCase):
    def setUp(self):
        self.manifest=json.loads((Path(__file__).resolve().parents[1]/'src/release-manifest.json').read_bytes())

    def test_manifest_has_no_pak_download_inventory(self):
        self.assertEqual(release.validate_manifest(self.manifest),self.manifest)
        self.assertEqual(versions.texture_version(self.manifest),'1.3.0')
        self.assertEqual(self.manifest['archives'],[])
        self.assertTrue(versions.pack_identity(self.manifest))

    def test_invalid_boundary_and_foreign_source_are_refused(self):
        for field in ('archives','url','sha'):
            bad=copy.deepcopy(self.manifest)
            if field=='archives':bad['archives']=[{'name':'stock.vpk'}]
            elif field=='url':bad['channel_source']['url']='https://example.com/channel-source.json'
            else:bad['channel_source']['sha256']='?'
            with self.assertRaises(ValueError):release.validate_manifest(bad)

    def test_app_update_is_available_even_if_an_old_client_cannot_read_new_pack_schema(self):
        # Discovery treats installer and texture-format errors independently.
        with patch.object(versions,'_metadata',return_value={}),patch.object(versions,'_version_asset',return_value={}), \
             patch.object(versions,'_installer',return_value=dict(status='verified',version='1.9.0')), \
             patch.object(versions,'_pack',side_effect=ValueError('Unsupported TextureQuest release manifest')):
            result=versions.check_updates(self.manifest,'1.7.2')
        self.assertTrue(result['installer']['update_available'])
        self.assertEqual(result['pack']['status'],'unavailable')


if __name__=='__main__':unittest.main()
