"""Download generated channels; assemble DDS locally from the player's originals.

No stock channel, DDS or PAK download fallback is admitted. DDSStore below is
only the local immutable cache/receipt reader also used by the graphics manager.
"""
from collections import defaultdict
from concurrent.futures import ThreadPoolExecutor
import hashlib
import json
from pathlib import Path
import re
import shutil
import stat
import threading
import zipfile
from urllib.request import urlopen

from texture_swapper import core
from texturequest_latest import _read, version_number
from texturequest_release import check_pause
from texturequest_direct import _download
from texturequest_dds_source import DDSStore, pin_file, verify_dds, _write_new, validate_pin, MAX_INDEX, REPOSITORY, API, need, sha
from texturequest_shared_textures import group_key, resolve
import texturequest_graphics_catalog as catalog
import texturequest_graphics_vpk as vpk
import texturequest_channel_codec as codec


def validate_source(source):
    need(source.get('schema') == 'texturequest-generated-channels-source-v1' and
         all(source.get(k) is False for k in ('contains_stock_channels','contains_full_dds','contains_game_archives')),
         'This installer requires generated-channel data; full DDS/PAK downloads are not supported.')
    version_number(source['texture_version'])
    validate_pin(source['index'], MAX_INDEX, r'channel-index\.json')
    chunks=source['chunks']
    need(isinstance(chunks,list) and 0<len(chunks)<=4096, 'Invalid channel chunk inventory')
    prefix=REPOSITORY+'/releases/download/v'+source['texture_version']+'/'
    for pin in [source['index'],*chunks]:
        need(pin['url']==prefix+pin['name'], 'Channel release URL differs')
    for chunk in chunks:
        validate_pin(chunk,2*1024**3,r'channels-\d{3}-[a-f0-9]{12}\.zip')
        need(type(chunk['objects']) is int and 0<chunk['objects']<=100000,'Invalid channel chunk count')
    need(len({c['name'] for c in chunks})==len(chunks) and
         sum(c['bytes'] for c in chunks)==source['compressed_download_bytes'],'Channel byte accounting')
    return source


def fetch_source(opener=urlopen, pause=None, minimum='1.3.0'):
    metadata=core._json(_read(API,8*1024**2,opener,pause))
    tag=metadata.get('tag_name','')
    need(tag.startswith('v') and version_number(tag[1:])>=version_number(minimum) and
         metadata.get('draft') is False and metadata.get('prerelease') is False and metadata.get('published_at') and
         metadata.get('html_url')==REPOSITORY+'/releases/tag/'+tag,'Unexpected texture release')
    assets=[a for a in metadata['assets'] if a.get('name')=='channel-source.json']
    need(len(assets)==1,'Release has no unique channel-source.json; update the texture data source.')
    asset=assets[0]
    pin=dict(name=asset['name'],bytes=asset['size'],sha256=str(asset['digest']).removeprefix('sha256:'),url=asset['browser_download_url'])
    validate_pin(pin,4*1024**2,r'channel-source\.json')
    need(pin['url']==REPOSITORY+'/releases/download/'+tag+'/channel-source.json' and asset.get('state')=='uploaded','Unexpected channel source')
    raw=_read(pin['url'],pin['bytes'],opener,pause)
    need(len(raw)==pin['bytes'] and sha(raw)==pin['sha256'],'Channel source checksum')
    source=validate_source(core._json(raw))
    need(source['texture_version']==tag[1:],'Channel source version differs')
    from texturequest_version import APP_VERSION
    need(version_number(APP_VERSION)>=version_number(source.get('minimum_app_version','1.9.0')),'Update TextureQuest before using this texture release.')
    return source


def valid_sha(value):
    return isinstance(value,str) and re.fullmatch('[a-f0-9]{64}',value) is not None


def validate_index(index, source, choices):
    need(index.get('schema')=='texturequest-generated-channels-index-v1' and
         index.get('texture_version')==source['texture_version'],'Unsupported channel index')
    recipes=index['recipes'];objects=index['objects'];named=index['named_textures']
    need(isinstance(recipes,list) and 0<len(recipes)==source['selected_groups']==source['prepared_groups']<=100000 and
         source['pending_groups']==0 and not index['pending'],'Channel recipe cardinality')
    need(len(named)==source['named_texture_count']<=200000 and len(objects)==source['unique_payload_count']<=100000,'Channel index cardinality')
    by_id={r['id']:r for r in recipes}; pins={o['sha256']:o for o in objects}; chunks={c['name']:c for c in source['chunks']}
    need(len(by_id)==len(recipes) and len(pins)==len(objects),'Duplicate channel identity')
    for obj in objects:
        need(valid_sha(obj['sha256']) and obj['entry']=='channels/'+obj['sha256']+'.tqc' and obj['chunk'] in chunks and
             type(obj['bytes']) is int and 24<obj['bytes']<=catalog.MAX_DDS,'Invalid channel object')
    groups=set()
    for row in recipes:
        core._name(row['owner']);core._member(row['member'])
        need(row['id']==sha((row['owner'].lower()+'\0'+row['member']).encode()) and valid_sha(row['object_sha256']), 'Channel recipe identity')
        source_layout=row['source']['layout'];target_layout=row['target']['layout']
        for layout in (source_layout,target_layout):
            need(type(layout) is list and len(layout)==5 and all(type(v) is int for v in layout) and
                 0<layout[0]<=4096 and 0<layout[1]<=4096 and 0<layout[2]<=13 and layout[3] in codec.PROFILES and layout[4]==1,'Unsupported channel geometry')
            catalog.mip_levels(tuple(layout))
        need(source_layout[3:]==target_layout[3:] and row['profile']==codec.PROFILES[target_layout[3]] and
             row['generated_channels']==['R','G','B'] and row['local_game_channels']==['A'],'Channel ownership/profile differs')
        need(row['payload']==pins.get(row['payload']['sha256']) and valid_sha(row['source']['dds_sha256']) and
             valid_sha(row['source']['fingerprint']),'Channel payload/source identity')
        need(type(row['wrap']) is list and len(row['wrap'])==2 and all(type(v) is bool for v in row['wrap']),'Invalid reconstruction wraps')
        need(row.get('local_alpha_policy','EXACT_LEGACY_V1') in ('EXACT_LEGACY_V1','SOURCE_REFERENCE_LANCZOS4_BOX_V1'),'Unsupported alpha policy')
        if row.get('local_alpha_policy')=='SOURCE_REFERENCE_LANCZOS4_BOX_V1':
            need(valid_sha(row.get('reference_dds_sha256')),'Missing prior output identity')
        target=row['target']; size=sum(v[4] for v in catalog.mip_levels(tuple(target_layout)))
        need(target['mip_bytes']==size and 0<=target['tail_bytes']<=4096 and
             all(valid_sha(target[k]) for k in ('fingerprint','mip_sha256','tail_sha256')),'Invalid target layout/mip/tail')
        key=group_key(row);need(key not in groups,'Duplicate source group');groups.add(key)
    need(set(pins)=={r['payload']['sha256'] for r in recipes},'Unreferenced payload')
    need(sum(o['bytes'] for o in objects)==source['generated_payload_bytes'],'Channel payload accounting')
    for name,chunk in chunks.items():need(sum(o['chunk']==name for o in objects)==chunk['objects'],'Chunk cardinality differs')
    bindings=[];ids=set()
    for named_row in named:
        before=named_row['released_binding'];recipe=by_id[named_row['recipe_id']]
        core._name(before['owner']);core._member(before['member'])
        need(before['id']==sha((before['owner'].lower()+'\0'+before['member']).encode()) and before['id'] not in ids,'Named identity differs')
        ids.add(before['id'])
        need(group_key(before)==group_key(recipe) and named_row['runtime_target_sha256']==recipe['object_sha256'],'Named recipe differs')
        bindings.append(before)
    # Independently retain the established correction-first shared selection.
    selected,aliases=resolve(dict(named_textures=bindings,texture_version=source['texture_version']),choices)
    need({r['id'] for r in selected}==set(by_id),'Published shared selection differs from correction policy')
    need(index['aliases']['pending']==[] and len(index['aliases']['aliases'])==len(bindings),'Alias cardinality')
    expected_aliases={r['id']:(by_id[n['recipe_id']]['object_sha256'],r['object_sha256']) for n,r in zip(named,bindings)}
    actual_aliases={r['id']:(r['selected'],r['original']) for r in index['aliases']['aliases']}
    need(len(actual_aliases)==len(bindings) and actual_aliases==expected_aliases,'Alias selection differs')
    return index


def project(index, ready):
    by_id={r['id']:r for r in index['recipes']}
    objects={r['object_sha256']:dict(sha256=r['object_sha256'],entry='dds/'+r['object_sha256']+'.dds',
             bytes=128+r['target']['mip_bytes']+r['target']['tail_bytes']) for r in ready}
    rows=[dict(n['released_binding'],object_sha256=by_id[n['recipe_id']]['object_sha256'],target=by_id[n['recipe_id']]['target'])
          for n in index['named_textures']]
    return dict(texture_version=index['texture_version'],named_textures=rows,objects=list(objects.values()))


def read_sources(game, recipes, index, pause):
    """Read bounded originals with writes denied; close handles before decoding."""
    needed={r['id']:r for r in recipes}; candidates=defaultdict(list)
    for n in index['named_textures']:
        row=n['released_binding'];recipe=needed.get(n['recipe_id'])
        if recipe and row['source']['dds_sha256']==recipe['source']['dds_sha256']:
            candidates[row['owner']].append((recipe['id'],row['member']))
    found={}
    for owner,rows in sorted(candidates.items()):
        check_pause(pause)
        if all(key in found for key,_ in rows):continue
        path=core._plain(game/'paks'/core._name(owner),game)
        if not path.is_file():continue
        with catalog.held(path) as (stream,identity):
            entries={e.member:e for e in vpk.index(stream,identity[2]).entries}
            for key,member in rows:
                check_pause(pause)
                if key in found or member not in entries:continue
                raw=vpk.member_data(stream,entries[member])
                if sha(raw)==needed[key]['source']['dds_sha256']:found[key]=raw
    return found


class ChannelStore(DDSStore):
    def summary(self):
        value = super().summary()
        return value if value and value.get('distribution') == 'generated-channels-only' else None

    def saved(self):
        value = super().saved()
        return value if value and value.get('distribution') == 'generated-channels-only' else None

    def prepare(self,source,game,choices,*,opener=urlopen,pause=None,progress=lambda _:None,overall=lambda *_:None,in_use=lambda:False):
        from texturequest_loose_map import build
        source=validate_source(source);game=core._plain(core.validate_game(game).parent)
        self.base=core._plain(self.base,self.state_dir)
        self.objects=core._plain(self.objects,self.base)
        self.base.mkdir(parents=True,exist_ok=True)
        with core._lock(self.state_dir/'operation.lock'),core._lock(self.state_dir/'sideload/helper-owner.lock'):
            need(not in_use(),'Close EQ2 before preparing texture updates.')
            pointer=core._plain(self.state_dir/'dds-prepared.json',self.state_dir)
            if pointer.exists():
                previous=core._load(pointer)
                need(previous.get('schema')=='texturequest-loose-dds-prepared-v1','Unknown preparation is preserved')
                need(version_number(source['texture_version'])>=version_number(previous['texture_version']),'Texture downgrade refused')
                if source['texture_version']==previous['texture_version']:
                    need(previous['source_sha256']==core._digest(source),'Published source changed without a version change')
            metadata=core._plain(self.base/'metadata',self.base);metadata.mkdir(exist_ok=True)
            index_path=core._plain(metadata/(source['index']['sha256']+'.json'),metadata)
            if not index_path.exists():
                raw=_read(source['index']['url'],source['index']['bytes'],opener,pause)
                need(len(raw)==source['index']['bytes'] and sha(raw)==source['index']['sha256'],'Channel index checksum')
                _write_new(index_path,raw)
            pin_file(index_path,source['index'])
            index=validate_index(core._json(index_path.read_bytes()),source,choices)
            self.objects.mkdir(exist_ok=True);core._plain(self.objects/'dds',self.objects).mkdir(exist_ok=True)
            downloads=core._plain(self.base/'downloads',self.base);downloads.mkdir(exist_ok=True)
            scratch=core._plain(self.base/'scratch',self.base);scratch.mkdir(exist_ok=True)
            assembler=codec.LocalAssembler(None,scratch,catalog)
            recipes=index['recipes'];ready=[];pending=[]
            total=len(recipes)
            chunks={c['name']:c for c in source['chunks']}
            chunk_rows=defaultdict(list)
            for row in recipes:chunk_rows[row['payload']['chunk']].append(row)
            cancelled=threading.Event()
            def stopped():
                return cancelled.is_set() or bool(pause and pause())
            def open_transfer(request, **kwargs):
                check_pause(stopped)
                return opener(request, **kwargs)
            def transfer(chunk):
                check_pause(stopped)
                download=dict(chunk,entries=chunk['objects']);download.pop('objects')
                return _download(download,downloads,
                    notify=lambda m,done=0,total=1:overall('Downloading custom channels',done,total,len(ready),len(recipes)),
                    pause=stopped,opener=open_transfer)
            def finish_transfer(active):
                future,name,usable,originals=active
                path=future.result()
                check_pause(pause)
                need(not in_use(),'Close EQ2 before building texture updates.')
                expected={o['entry']:o for o in index['objects'] if o['chunk']==name}
                with catalog.held(path),zipfile.ZipFile(path) as archive:
                    infos=archive.infolist()
                    need(len(infos)==len(expected) and {i.filename for i in infos}==set(expected),'Channel ZIP membership differs')
                    for info in infos:
                        obj=expected[info.filename]
                        need(info.file_size==obj['bytes'] and not info.flag_bits&1 and stat.S_IFMT(info.external_attr>>16)==stat.S_IFREG,'Channel ZIP entry type/length differs')
                    for row in usable:
                        check_pause(pause)
                        payload=archive.read(row['payload']['entry'])
                        raw=assembler.assemble(originals.pop(row['id']),payload,row)
                        obj=dict(sha256=row['object_sha256'],bytes=len(raw))
                        verify_dds(raw,obj,[row['target']])
                        destination=core._plain(self.objects/'dds'/(row['object_sha256']+'.dds'),self.objects)
                        _write_new(destination,raw);ready.append(row)
                        overall('Building local textures',len(ready),total,len(ready),total)
                path.unlink() # Only our verified transport ZIP; local DDS survive interruption.
            # One transport can run while the next chunk's local outputs are
            # checked. No whole-cache census precedes the first needed ZIP.
            # A failed scan cancels the owned worker before either lock exits.
            workers=ThreadPoolExecutor(max_workers=1)
            active=None;checked=0
            try:
                for name,chunk in chunks.items():
                    check_pause(pause)
                    required=[]
                    for row in chunk_rows[name]:
                        check_pause(pause)
                        if active is not None and active[0].done():active[0].result()
                        path=core._plain(self.objects/'dds'/(row['object_sha256']+'.dds'),self.objects)
                        if path.exists():
                            obj=dict(sha256=row['object_sha256'],bytes=128+row['target']['mip_bytes']+row['target']['tail_bytes'])
                            pin_file(path,obj);ready.append(row)
                        else:required.append(row)
                        checked+=1
                    overall('Checking saved textures',checked,total,checked,total)
                    if not required:continue
                    if active is not None:
                        finish_transfer(active)
                        active=None
                    progress('Reading local game channels for '+name+'; remaining differences are still being checked.')
                    originals=read_sources(game,required,index,pause)
                    usable=[r for r in required if r['id'] in originals]
                    pending.extend(dict(id=r['id'],owner=r['owner'],member=r['member'],reason='current-source-unmatched')
                                   for r in required if r['id'] not in originals)
                    if not usable:continue
                    check_pause(pause)
                    need(not in_use(),'Close EQ2 before building texture updates.')
                    output_bytes=sum(128+r['target']['mip_bytes']+r['target']['tail_bytes'] for r in usable)
                    need(shutil.disk_usage(self.base).free>=chunk['bytes']+output_bytes+64*core.CHUNK,'Not enough space for this download and local reconstruction.')
                    progress('Downloading missing custom channels: '+name)
                    active=(workers.submit(transfer,chunk),name,usable,originals)
                if active is not None:finish_transfer(active)
            finally:
                cancelled.set()
                workers.shutdown(wait=True,cancel_futures=True)
            check_pause(pause);need(not in_use(),'Close EQ2 before activating texture updates.')
            need(ready,'No compatible local game sources were found. Patch EQ2 to stock, then try again.')
            local=project(index,ready)
            raw,coverage=build(game,local,ready,index['aliases'],self.objects,core._digest(source),progress,pause)
            coverage['pending'].extend(pending)
            coverage['selected_groups']=len(recipes)
            coverage['pending_groups']=len(recipes)-coverage['mapped_groups']
            coverage['distribution']='generated-channels-only'
            maps=core._plain(self.base/'maps',self.base);maps.mkdir(exist_ok=True)
            map_path=core._plain(maps/(sha(raw)+'.bin'),maps)
            if not map_path.exists():_write_new(map_path,raw)
            objects=sorted(local['objects'],key=lambda o:o['sha256'])
            value=dict(schema='texturequest-loose-dds-prepared-v1',texture_version=source['texture_version'],source_sha256=core._digest(source),
                       index_sha256=source['index']['sha256'],choices_sha256=core._digest(choices),game=str(game),object_root=str(self.objects),
                       objects=self._metadata(objects),object_count=len(objects),object_bytes=sum(o['bytes'] for o in objects),
                       named_textures=source['named_texture_count'],distribution='generated-channels-only',
                       map=dict(path=str(map_path),bytes=len(raw),sha256=sha(raw)),
                       coverage=dict(mapped_groups=coverage['mapped_groups'],selected_groups=len(recipes),pending_groups=coverage['pending_groups'],report=self._metadata(coverage)))
            self.verify(value,full=False)
            if pointer.exists():
                history=core._plain(self.base/'receipts',self.base);history.mkdir(exist_ok=True)
                old=pointer.read_bytes();saved=history/(sha(old)+'.json')
                if not saved.exists():_write_new(saved,old)
            core._save(pointer,value)
            progress('Textures ready.' if not coverage['pending_groups'] else str(coverage['pending_groups'])+' changed or missing source groups stay original; details saved in the coverage report.')
            return value
