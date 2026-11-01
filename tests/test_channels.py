"""Generated-channel installer: synthetic D-only VPKs and real CPU reconstruction."""
import copy
import io
import json
from pathlib import Path
import struct
import sys
import tempfile
import unittest
from unittest.mock import patch
import zipfile
from types import SimpleNamespace
import numpy as np

APP=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(APP/'src'))
import texturequest_channels as channels
import texturequest_channel_codec as codec
import texturequest_graphics_catalog as catalog
from texture_swapper import core
from texturequest_release import Paused
from texturequest_shared_textures import propose
from test_texture_map import vpk
from test_direct import Response


def header(family,w,h,mips):
    words=[0]*31
    words[0],words[1],words[2],words[3],words[6],words[18],words[26]=124,0x21007,h,w,mips,32,0x401008
    if family<4:words[19],words[20]=4,int.from_bytes({1:b'DXT1',2:b'DXT3',3:b'DXT5'}[family],'little')
    else:words[19],words[21:26]=0x41,[32,0xff0000,0xff00,0xff,0xff000000]
    return b'DDS '+struct.pack('<31I',*words)


def image(assembler,root,family,n):
    rng=np.random.default_rng(60+n)
    pixels=rng.integers(0,256,(8,8,4),np.uint8)
    pixels[...,3]=np.tile(np.arange(8,dtype=np.uint8)*32,(8,1))
    template=SimpleNamespace(fmt={1:'DXT1',2:'DXT3',3:'DXT5',4:'A8R8G8B8'}[family],header=header(family,8,8,4),trailer=b'LOCAL-ONLY-TAIL')
    if family==4:
        template.info=SimpleNamespace(bitcount=32,rmask=0xff0000,gmask=0xff00,bmask=0xff,amask=0xff000000)
        raw=assembler.codec.encode_raw_dds(template,pixels,4)
    else:raw=assembler.codec.encode_bc_dds(template,pixels,4,workdir=str(root),mip_filter='BOX')
    texture=assembler.codec.decode(raw,'fixture.dds')
    rgba=np.asarray(texture.pixels)
    if rgba.shape[2]==3:rgba=np.concatenate((rgba,np.full((8,8,1),255,np.uint8)),axis=2)
    generated=assembler.upscale.resample(rgba,2,'lanczos',(True,False))
    generated[...,:3]=rng.integers(0,256,generated[...,:3].shape,np.uint8)
    target=(assembler.codec.encode_raw_dds(texture,generated,5) if family==4 else
            assembler.codec.encode_bc_dds(texture,generated,5,workdir=str(root),mip_filter='BOX'))
    a,ap,_=catalog.decode_dds(raw);b,bp,tail=catalog.decode_dds(target)
    member=f'test/{n}.dds'
    row=dict(id=codec.sha(('test.vpk\0'+member).encode()),owner='test.vpk',member=member,category={},object_sha256=codec.sha(target),
             source=dict(layout=list(a),dds_sha256=codec.sha(raw),fingerprint=catalog.fingerprint(a,ap)),
             target=dict(layout=list(b),fingerprint=catalog.fingerprint(b,bp),mip_bytes=len(bp),mip_sha256=codec.sha(bp),tail_bytes=len(tail),tail_sha256=codec.sha(tail)))
    payload=codec.extract(b,bp)
    recipe=codec.make_recipe(row,[True,False],payload)
    return raw,target,payload,row,recipe


def fixture(root,families=(4,3)):
    game=root/'Game';(game/'paks').mkdir(parents=True);(game/'EverQuest2.exe').write_bytes(b'not executed')
    assembler=codec.LocalAssembler(None,root,catalog)
    examples=[image(assembler,root,f,n) for n,f in enumerate(families)]
    originals={r[3]['member']:r[0] for r in examples}
    (game/'paks/test.vpk').write_bytes(vpk(originals))
    chunk=io.BytesIO()
    with zipfile.ZipFile(chunk,'w',zipfile.ZIP_DEFLATED) as archive:
        for _,_,payload,_,_ in examples:
            info=zipfile.ZipInfo('channels/'+codec.sha(payload)+'.tqc');info.create_system=3;info.external_attr=0o100644<<16
            archive.writestr(info,payload)
    packed=chunk.getvalue();name='channels-001-'+codec.sha(packed)[:12]+'.zip'
    objects=[];recipes=[];named=[];aliases=[]
    for original,target,payload,row,recipe in examples:
        obj=dict(sha256=codec.sha(payload),bytes=len(payload),entry='channels/'+codec.sha(payload)+'.tqc',chunk=name)
        recipe['payload']=obj;objects.append(obj);recipes.append(recipe)
        named.append(dict(released_binding=row,recipe_id=recipe['id'],runtime_target_sha256=recipe['object_sha256']))
        aliases.append(dict(id=row['id'],original=row['object_sha256'],selected=row['object_sha256']))
    index=dict(schema='texturequest-generated-channels-index-v1',texture_version='1.3.0',recipes=recipes,objects=objects,
               named_textures=named,aliases=dict(aliases=aliases,pending=[]),pending=[])
    raw_index=json.dumps(index).encode()
    prefix=channels.REPOSITORY+'/releases/download/v1.3.0/'
    source=dict(schema='texturequest-generated-channels-source-v1',texture_version='1.3.0',
                contains_stock_channels=False,contains_full_dds=False,contains_game_archives=False,selected_groups=len(recipes),
                prepared_groups=len(recipes),pending_groups=0,named_texture_count=len(named),unique_payload_count=len(objects),
                generated_payload_bytes=sum(o['bytes'] for o in objects),compressed_download_bytes=len(packed),
                index=dict(name='channel-index.json',bytes=len(raw_index),sha256=codec.sha(raw_index),url=prefix+'channel-index.json'),
                chunks=[dict(name=name,bytes=len(packed),sha256=codec.sha(packed),objects=len(objects),url=prefix+name)])
    choices=propose(dict(texture_version='1.3.0',named_textures=[n['released_binding'] for n in named]))
    requests=[]
    def opener(request,**kwargs):
        requests.append(request.full_url)
        raw=raw_index if request.full_url.endswith('channel-index.json') else packed
        start=int(request.headers.get('Range','bytes=0-').split('=')[1].split('-')[0])
        return Response(raw,start=start,status=206 if start else 200)
    return game,source,index,examples,choices,opener,requests


class ChannelTests(unittest.TestCase):
    def setUp(self):
        (APP/'build').mkdir(exist_ok=True)
        self.temp=tempfile.TemporaryDirectory(dir=APP/'build');self.addCleanup(self.temp.cleanup)
        self.root=Path(self.temp.name)
        self.game,self.source,self.index,self.examples,self.choices,self.opener,self.requests=fixture(self.root)
        self.store=channels.ChannelStore(self.root/'data')

    def prepare(self,**kwargs):
        return self.store.prepare(self.source,self.game,self.choices,opener=self.opener,**kwargs)

    def test_download_contains_only_generated_channels_and_local_outputs_are_exact(self):
        before=(self.game/'paks/test.vpk').read_bytes()
        ready=self.prepare()
        self.assertEqual(ready['coverage']['mapped_groups'],2)
        self.assertEqual(ready['coverage']['pending_groups'],0)
        self.store.verify(ready)
        self.assertEqual((self.game/'paks/test.vpk').read_bytes(),before)
        for original,target,payload,row,recipe in self.examples:
            self.assertNotIn(b'LOCAL-ONLY-TAIL',payload)
            self.assertEqual((self.store.objects/'dds'/(codec.sha(target)+'.dds')).read_bytes(),target)
        self.assertTrue(all(url.endswith('.zip') or url.endswith('channel-index.json') for url in self.requests))
        self.assertFalse(any('.vpk' in url or '/dds-' in url for url in self.requests))

    def test_verified_cache_avoids_download_and_reconstruction(self):
        self.prepare();self.requests.clear()
        with patch.object(codec.LocalAssembler,'assemble',side_effect=AssertionError('Must reuse verified cache')):
            ready=self.prepare()
        self.assertFalse(self.requests)
        self.assertEqual(ready['coverage']['mapped_groups'],2)

    def test_patch_changes_one_source_reports_original_fallback_without_silent_drop(self):
        self.prepare()
        members={e[3]['member']:e[0] for e in self.examples}
        member=self.examples[0][3]['member'];members[member]=members[member][:128]+bytes(len(members[member])-128)
        (self.game/'paks/test.vpk').write_bytes(vpk(members))
        ready=self.prepare()
        self.assertEqual(ready['coverage']['selected_groups'],2)
        self.assertEqual(ready['coverage']['mapped_groups'],1)
        self.assertEqual(ready['coverage']['pending_groups'],1)

    def test_missing_sources_do_not_download_stock_or_create_prepared_receipt(self):
        (self.game/'paks/test.vpk').unlink()
        with self.assertRaisesRegex(ValueError,'No compatible local game sources'):
            self.prepare()
        self.assertEqual(len(self.requests),1)
        self.assertFalse((self.store.state_dir/'dds-prepared.json').exists())

    def test_changed_source_descriptor_preserves_previous_receipt(self):
        self.prepare();pointer=self.store.state_dir/'dds-prepared.json';before=pointer.read_bytes()
        self.source['index']['sha256']='0'*64
        with self.assertRaisesRegex(ValueError,'without a version change'):self.prepare()
        self.assertEqual(pointer.read_bytes(),before)

    def test_game_open_refuses_before_network_or_receipt_change(self):
        with self.assertRaisesRegex(ValueError,'Close EQ2'):self.prepare(in_use=lambda:True)
        self.assertFalse(self.requests)

    def test_corrupt_cache_is_preserved_and_refused(self):
        prepared=self.prepare()
        obj=self.store.object_rows(prepared)[0]
        path=self.store.objects/obj['entry']
        bad=path.read_bytes()[:-1]+b'?';path.write_bytes(bad)
        with self.assertRaisesRegex(ValueError,'checksum'):self.prepare()
        self.assertEqual(path.read_bytes(),bad)

    def test_hardlinked_cache_is_refused(self):
        import os
        prepared=self.prepare()
        obj=self.store.object_rows(prepared)[0];path=self.store.objects/obj['entry']
        os.link(path,self.root/'extra-link.dds')
        with self.assertRaisesRegex(ValueError,'Hard-linked'):
            self.prepare()

    def test_tampered_transport_fails_before_any_reconstruction(self):
        normal=self.opener
        def bad(request,**kwargs):
            response=normal(request,**kwargs)
            if request.full_url.endswith('.zip'):
                raw=response.getvalue();return Response(raw[:-1]+bytes([raw[-1]^1]),status=200)
            return response
        with self.assertRaises(ValueError):
            self.store.prepare(self.source,self.game,self.choices,opener=bad)
        self.assertFalse(list(self.store.objects.glob('dds/*.dds')))
        self.assertFalse((self.store.state_dir/'dds-prepared.json').exists())

    def test_non_rgb_and_wrong_alias_policies_refuse(self):
        for changed in ('ownership','alias'):
            index=copy.deepcopy(self.index)
            if changed=='ownership':index['recipes'][0]['generated_channels']=['A']
            else:index['aliases']['aliases'][0]['selected']='0'*64
            with self.assertRaises(ValueError):channels.validate_index(index,self.source,self.choices)

    def test_cancel_preserves_completed_objects_and_resumes_without_download(self):
        count=0
        real=codec.LocalAssembler.assemble
        def assembled(*args):
            nonlocal count
            result=real(*args);count+=1
            return result
        with patch.object(codec.LocalAssembler,'assemble',autospec=True,side_effect=assembled):
            with self.assertRaises(Paused):
                self.prepare(pause=lambda:count==1)
        self.assertFalse((self.store.state_dir/'dds-prepared.json').exists())
        self.assertEqual(len(list(self.store.objects.glob('dds/*.dds'))),1)
        self.requests.clear();ready=self.prepare()
        self.assertEqual(ready['coverage']['mapped_groups'],2)
        self.assertFalse(self.requests)


class CodecProfiles(unittest.TestCase):
    def test_all_four_ported_profiles_reconstruct_exact_pixels_and_local_tail(self):
        (APP/'build').mkdir(exist_ok=True)
        with tempfile.TemporaryDirectory(dir=APP/'build') as temporary:
            root=Path(temporary);assembler=codec.LocalAssembler(None,root,catalog)
            for family in range(1,5):
                with self.subTest(family=family):
                    original,target,payload,_,recipe=image(assembler,root,family,family)
                    self.assertEqual(assembler.assemble(original,payload,recipe),target)


if __name__=='__main__':unittest.main()
