"""Bundled helper integration on tiny invented VPK/VPL/DDS inputs, no native calls.

Only catalog selection is a fixture leaf: the real catalog is fully read and
pinned, then projected to one invented member. All path/prepare/merge/READY/
bindings/catalog/install code runs normally. Never reads production game data.
"""
from contextlib import contextmanager
import hashlib,json,os,shutil,struct,sys,tempfile,unittest,zlib
from pathlib import Path
from unittest.mock import patch
ROOT=Path(os.environ.get('TQ_TEST_SOURCE',str(Path(__file__).resolve().parents[1]))).resolve()
sys.path.insert(0,str(ROOT/'src'))
import texturequest_graphics_proxy as g
import texturequest_sideload as loader
from texture_swapper import core


def sha(b):return hashlib.sha256(b).hexdigest()
def dds(n,value):
    w=[0]*31;w[0],w[1],w[2],w[3],w[6],w[18]=124,0x21007,n,n,1,32
    w[19:26]=[0x41,0,32,0xff0000,0xff00,0xff,0xff000000];w[26]=0x1000
    return b'DDS '+struct.pack('<31I',*w)+bytes([value,4,7,255])*(n*n)
def vpk(data):
    name=b'fixture.dds'
    packed=zlib.compress(struct.pack('<II',len(name)+1,len(data))+name+b'\0'+data)
    entry=struct.pack('<III',0,len(packed),len(name))+name
    index=zlib.compress(struct.pack('<II',len(entry)+8,1)+entry)
    body=struct.pack('<I',len(packed))+packed
    return body+struct.pack('<I',len(index)+12)+index+struct.pack('<III',0x33744,len(body),2),len(packed)
def vpl(source,csize):
    names=b'paks/fixture.vpk\0';ns=zlib.compress(names);aux=zlib.compress(b'')
    w=[60,80,84,84,84+len(ns),1,1,len(names),len(ns),len(aux),0,0x80000002,52]
    return struct.pack('<13I',*w)+struct.pack('<II',int(source.stat().st_mtime),source.stat().st_size)+struct.pack('<5I',0,0,0,0,csize)+bytes(4)+ns+aux


class BundledPreparation(unittest.TestCase):
    def setUp(self):
        base=Path(r'D:\eq2 paks\work\tmp');base.mkdir(exist_ok=True)
        self.tmp=tempfile.TemporaryDirectory(dir=base,prefix='gprep-');self.home=Path(self.tmp.name)
        self.game=self.home/'game';(self.game/'paks').mkdir(parents=True)
        (self.game/'EverQuest2.exe').write_bytes(b'inert fixture, never executed')
        self.data=self.home/'data';(self.data/'downloads').mkdir(parents=True)
        self.bundle=self.home/'resources';self.bundle.mkdir()
        shutil.copytree(ROOT/'graphics-runtime',self.bundle/'graphics-runtime')
        shutil.copytree(ROOT/'sideload-runtime/sideload',self.bundle/'sideload')
        shutil.copy2(ROOT/'sideload-runtime/texturequest-sideload-runtime.json',self.bundle/'texturequest-sideload-runtime.json')
        self.old,self.new=dds(4,10),dds(8,99)
        raw,csize=vpk(self.old);self.stock=self.game/'paks/fixture.vpk';self.stock.write_bytes(raw)
        self.vpl=vpl(self.stock,csize);(self.game/'assetslib.vpl').write_bytes(self.vpl)
        target,_=vpk(self.new);(self.data/'downloads/fixture.vpk').write_bytes(target)
        self.archive=dict(name='fixture.vpk',bytes=len(target),sha256=sha(target),entries=1)
        release=dict(schema='texturequest-release-v3',version='1.3.7',texture_version='1.2.3',texture_count=1,archives=[self.archive])
        doc=dict(schema='texturequest-direct-prepared-v1',release=release,release_sha256=core._digest(release),
            archives=[dict(self.archive,path=str(self.data/'downloads/fixture.vpk'))],total_bytes=len(target),id=core._digest([self.archive])[:32])
        core._save(self.data/'direct-prepared.json',doc);self.prepared=doc
        self.m=g.GraphicsManager(self.data,runtime_dir=self.bundle/'graphics-runtime')
        self.stack=__import__('contextlib').ExitStack()
        def closed():
            assert self.game.resolve().is_relative_to(self.home.resolve())
            assert (self.game/'EverQuest2.exe').read_bytes()==b'inert fixture, never executed'
        self.stack.enter_context(patch.object(g,'ensure_closed',side_effect=closed))
        self.stack.enter_context(patch.object(self.m,'_games_open',return_value=0))
        actual_preparer=loader._preparer
        @contextmanager
        def fixture_preparer(runtime):
            with actual_preparer(runtime) as helper:
                _,_,pins=helper.load_catalog()  # Genuine bundled catalog byte verification.
                catalog={'fixture.vpk':{'fixture.dds':dict(source_dds=dict(bytes=len(self.old),sha256=sha(self.old)),replacement_dds=dict(bytes=len(self.new),sha256=sha(self.new)))}}
                with patch.object(helper,'load_catalog',return_value=([self.archive],catalog,pins)):
                    yield helper
        self.stack.enter_context(patch.object(loader,'_preparer',fixture_preparer))
        self.stack.enter_context(patch.object(loader,'_spawn_watcher',side_effect=AssertionError('No watcher may start')))

    def tearDown(self):
        self.stack.close()
        assert self.home.resolve().is_relative_to(Path(r'D:\eq2 paks\work\tmp').resolve())
        self.tmp.cleanup()

    def test_fresh_install_real_prepare_ready_binding_and_graphics_catalog(self):
        if os.environ.get('TQ_EXPECT_170_BUG'):
            with self.assertRaisesRegex(ValueError,'output must be inside the loader component'):
                self.m.install(self.game,self.prepared)
            self.assertFalse((self.game/'d3d11.dll').exists());return
        result=self.m.install(self.game,self.prepared)
        self.assertTrue(result['installed']);self.assertFalse(result['running'])
        record=self.m._record(self.game);render=Path(record['candidate'])
        reference=json.loads((render/'PREPARATION.json').read_bytes());epoch=Path(reference['preparation_root'])
        package=epoch.parent.parent
        self.assertEqual(package.parent,self.data/'sideload/runtimes')
        self.assertEqual(epoch.name,'graphics-'+record['id'])
        self.assertEqual(json.loads((epoch/'OWNER.json').read_bytes()),reference)
        ready=json.loads((epoch/'pack/READY.json').read_bytes())
        self.assertEqual(ready['applied'],1);self.assertFalse(ready['unavailable'])
        self.assertEqual(ready['prepared']['path'],str(self.data/'direct-prepared.json'))
        self.assertTrue((epoch/'BINDINGS.bin').is_file())
        self.assertEqual((render/'addons/catalog.cfg').read_bytes()[:8],b'TQGXCFG2')
        self.assertEqual((render/'textures/payloads.bin').read_bytes(),self.new[128:])
        self.assertEqual(self.stock.read_bytes(),vpk(self.old)[0]);self.assertEqual((self.game/'assetslib.vpl').read_bytes(),self.vpl)
        self.m.uninstall(self.game)
        self.assertFalse((self.game/'d3d11.dll').exists());self.assertTrue((epoch/'pack/READY.json').is_file())
        self.assertEqual(self.stock.read_bytes(),vpk(self.old)[0])

if __name__=='__main__':unittest.main(verbosity=2)
