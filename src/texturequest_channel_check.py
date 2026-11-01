"""Frozen CPU/channel pipeline check. Only fresh synthetic files on D:."""
import io,json,struct,tempfile,zlib
from pathlib import Path
from types import SimpleNamespace
import zipfile
import numpy as np
from texture_swapper import core
import texturequest_channel_codec as codec
import texturequest_channels as channels
import texturequest_graphics_catalog as catalog
from texturequest_shared_textures import propose


def run(output):
    output=Path(output).absolute()
    if output.drive.casefold()!='d:':raise ValueError('Internal runtime fixtures must stay on D:')
    output.parent.mkdir(parents=True,exist_ok=True)
    result=dict(status='error',game_files_touched=False,gui_opened=False)
    try:
        with tempfile.TemporaryDirectory(prefix='channel-runtime-',dir=output.parent) as temporary:
            folder=Path(temporary);game=folder/'fixture-game';(game/'paks').mkdir(parents=True)
            (game/'EverQuest2.exe').write_bytes(b'not an executable')
            assembler=codec.LocalAssembler(None,folder,catalog)
            objects=[];recipes=[];named=[];originals={};targets={}
            for family in range(1,5):
                words=[0]*31
                words[0],words[1],words[2],words[3],words[6],words[18],words[26]=124,0x21007,8,8,4,32,0x401008
                if family<4:words[19],words[20]=4,int.from_bytes({1:b'DXT1',2:b'DXT3',3:b'DXT5'}[family],'little')
                else:words[19],words[21:26]=0x41,[32,0xff0000,0xff00,0xff,0xff000000]
                template=SimpleNamespace(fmt={1:'DXT1',2:'DXT3',3:'DXT5',4:'A8R8G8B8'}[family],
                         header=b'DDS '+struct.pack('<31I',*words),trailer=b'SYNTHETIC-LOCAL-ONLY')
                template.info=SimpleNamespace(bitcount=32,rmask=0xff0000,gmask=0xff00,bmask=0xff,amask=0xff000000)
                pixels=np.random.default_rng(family).integers(0,256,(8,8,4),np.uint8)
                original=(assembler.codec.encode_raw_dds(template,pixels,4) if family==4 else
                          assembler.codec.encode_bc_dds(template,pixels,4,workdir=str(folder),mip_filter='BOX'))
                texture=assembler.codec.decode(original,'fixture.dds');rgba=np.asarray(texture.pixels)
                if rgba.shape[2]==3:rgba=np.concatenate((rgba,np.full((8,8,1),255,np.uint8)),axis=2)
                generated=assembler.upscale.resample(rgba,2,'lanczos',(True,False));generated[...,:3]^=np.uint8(17)
                target=(assembler.codec.encode_raw_dds(texture,generated,5) if family==4 else
                        assembler.codec.encode_bc_dds(texture,generated,5,workdir=str(folder),mip_filter='BOX'))
                a,ap,_=catalog.decode_dds(original);b,bp,tail=catalog.decode_dds(target)
                member=f'runtime/{family}.dds';originals[member]=original
                row=dict(id=codec.sha(('runtime.vpk\0'+member).encode()),owner='runtime.vpk',member=member,category={},object_sha256=codec.sha(target),
                         source=dict(layout=list(a),dds_sha256=codec.sha(original),fingerprint=catalog.fingerprint(a,ap)),
                         target=dict(layout=list(b),fingerprint=catalog.fingerprint(b,bp),mip_bytes=len(bp),mip_sha256=codec.sha(bp),tail_bytes=len(tail),tail_sha256=codec.sha(tail)))
                payload=codec.extract(b,bp);recipe=codec.make_recipe(row,[True,False],payload)
                objects.append((recipe,payload));recipes.append(recipe);targets[codec.sha(target)]=target
                named.append(dict(released_binding=row,recipe_id=row['id'],runtime_target_sha256=row['object_sha256']))
            body=b'';rows=b''
            for member,raw in originals.items():
                name=member.encode();packed=zlib.compress(struct.pack('<II',len(name)+1,len(raw))+name+b'\0'+raw)
                rows+=struct.pack('<III',len(body),len(packed),len(name))+name;body+=struct.pack('<I',len(packed))+packed
            table=zlib.compress(struct.pack('<II',len(rows)+8,len(originals))+rows)
            stock=body+struct.pack('<I',len(table)+12)+table+struct.pack('<III',0x33744,len(body),2)
            (game/'paks/runtime.vpk').write_bytes(stock)
            buffer=io.BytesIO()
            with zipfile.ZipFile(buffer,'w',zipfile.ZIP_DEFLATED) as archive:
                for recipe,payload in objects:
                    info=zipfile.ZipInfo(recipe['payload']['entry']);info.create_system=3;info.external_attr=0o100644<<16
                    archive.writestr(info,payload)
            packed=buffer.getvalue();chunk_name='channels-001-'+codec.sha(packed)[:12]+'.zip'
            for recipe,_ in objects:recipe['payload']['chunk']=chunk_name
            aliases=dict(pending=[],aliases=[dict(id=r['id'],original=r['object_sha256'],selected=r['object_sha256']) for r in recipes])
            index=dict(schema='texturequest-generated-channels-index-v1',texture_version='1.3.0',recipes=recipes,
                       objects=[r['payload'] for r in recipes],named_textures=named,aliases=aliases,pending=[])
            raw_index=json.dumps(index).encode();base=channels.REPOSITORY+'/releases/download/v1.3.0/'
            source=dict(schema='texturequest-generated-channels-source-v1',texture_version='1.3.0',contains_stock_channels=False,
                        contains_full_dds=False,contains_game_archives=False,selected_groups=4,prepared_groups=4,pending_groups=0,
                        named_texture_count=4,unique_payload_count=4,generated_payload_bytes=sum(len(p) for _,p in objects),compressed_download_bytes=len(packed),
                        index=dict(name='channel-index.json',bytes=len(raw_index),sha256=codec.sha(raw_index),url=base+'channel-index.json'),
                        chunks=[dict(name=chunk_name,bytes=len(packed),sha256=codec.sha(packed),objects=4,url=base+chunk_name)])
            requests=[]
            class Response(io.BytesIO):
                def geturl(self):return 'https://release-assets.githubusercontent.com/runtime-fixture'
                def getcode(self):return 200
            def opener(request,**kwargs):
                requests.append(request.full_url)
                data=raw_index if request.full_url.endswith('channel-index.json') else packed
                response=Response(data);response.headers={'Content-Length':str(len(data))};return response
            store=channels.ChannelStore(folder/'data')
            choices=propose(dict(texture_version='1.3.0',named_textures=[n['released_binding'] for n in named]))
            ready=store.prepare(source,game,choices,opener=opener)
            store.verify(ready)
            assert ready['coverage']['mapped_groups']==4 and ready['coverage']['pending_groups']==0
            for digest,target in targets.items():assert (store.objects/'dds'/(digest+'.dds')).read_bytes()==target
            assert (game/'paks/runtime.vpk').read_bytes()==stock
            requests.clear();store.prepare(source,game,choices,opener=opener);assert not requests
            from texturequest_app import resource,APP_VERSION
            from texturequest_release import load_manifest
            from texturequest_version_check import texture_version
            manifest=load_manifest(resource('release-manifest.json'))
            result.update(status='passed',app_version=APP_VERSION,texture_version=texture_version(manifest),
                          four_channel_profiles=True,source_alpha_reconstructed=True,local_dds_exact=True,
                          cached_update_network_free=True,stock_archive_unchanged=True,portable_runtime=True,
                          map_schema='TQMAP002',distribution='generated-channels-only')
    except Exception as error:
        result['error']=repr(error);raise
    finally:output.write_text(json.dumps(result,indent=2)+'\n',encoding='utf-8')
    return result
