from pathlib import Path
import contextlib,hashlib,io,json,sys,tempfile,unittest
from types import SimpleNamespace
from unittest.mock import MagicMock,patch
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'src'))
import texturequest_graphics_control as c

class EpochIntegration(unittest.TestCase):
 def test_progress_redirect_is_not_recursive(self):
  target=io.StringIO();redirected=io.StringIO()
  with patch.object(sys,'__stdout__',target),contextlib.redirect_stdout(redirected):c.emit('one provider')
  self.assertEqual(target.getvalue(),'one provider\n');self.assertEqual(redirected.getvalue(),'')
 def test_reused_pack_has_new_owned_bindings_parent_and_original_download_identity(self):
  root=Path(__file__).resolve().parents[1]/'build';root.mkdir(exist_ok=True)
  with tempfile.TemporaryDirectory(dir=root) as td:
   root=Path(td);state=root/'app-data';component=state/'sideload/runtimes'/('a'*64)
   fake=MagicMock();fake._runtime.return_value=dict(portable_pending=True,bundle={})
   fake._materialize.return_value=dict(files={},preparation_root=str(component/'background-epochs'))
   fake._record.return_value=None
   manager=SimpleNamespace(state_dir=state,cache_base=root/'graphics')
   def prepare(runtime,game,prepared,output,progress):
    self.assertEqual(prepared,state/'direct-prepared.json')
    self.assertTrue(output.parent.is_dir());self.assertTrue(output.is_relative_to(component))
    # Reuse does not create output/pack; the new bindings live in its parent.
    (output.parent/'BINDINGS.bin').write_bytes(b'new bindings')
    ready=root/'old-ready.json';ready.write_text(json.dumps(dict(providers=[])))
    return dict(ready_json=c._pin(ready),applied=1)
   def graphics(ready,output,api,report,**kw):
    self.assertEqual(kw['direction'],'upscaled_canonical');output.mkdir()
    raw=b'metadata-only-index';(output/'catalog.bin').write_bytes(raw)
    value=dict(rows=1,gaps=[],ambiguous=[],index=dict(bytes=len(raw),sha256=hashlib.sha256(raw).hexdigest()))
    (output/'CATALOG.json').write_text(json.dumps(value));return value
   with patch.object(c,'memory_manager',return_value=fake),patch.object(c,'_prepare',side_effect=prepare),patch.object(c.catalog,'prepare',side_effect=graphics):
    result=c.build_epoch(manager,root/'game',{},False,root/'graphics/epoch',report=lambda _:None)
   self.assertEqual(result['rows'],1);self.assertTrue(Path(result['config']['path']).is_file())

if __name__=='__main__':unittest.main()
