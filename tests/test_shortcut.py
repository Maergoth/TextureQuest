from pathlib import Path
import sys,unittest
from types import SimpleNamespace
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'src'))
from texturequest_shortcut import normalize,label,from_tk,ShortcutPump
class FakeKeys:
 def __init__(self):self.pid=1;self.window=42;self.events=[];self.posts=[];self.reg=[];self.unreg=0;self.conflict=False
 def foreground(self):return self.pid
 def target(self,s):return self.window
 def register(self,s):self.reg.append(s);return not self.conflict,1409 if self.conflict else None
 def unregister(self):self.unreg+=1
 def presses(self):events=self.events;self.events=[];return events
 def post(self,w):self.posts.append(w)
class ShortcutCases(unittest.TestCase):
 def record(self):return dict(sessions=[dict(pid=1,birth100ns=2,exe='exact owned game',session='session/nonce')])
 def test_default_capture_and_modifier_names(self):
  self.assertEqual(label(),'Numpad *');self.assertEqual(from_tk(SimpleNamespace(keycode=106,state=0x10)),normalize())
  self.assertEqual(label(from_tk(SimpleNamespace(keycode=120,state=4|8|1))),'Ctrl+Alt+Shift+F9')
  for bad in [{'vk':17,'modifiers':0},{'vk':106,'modifiers':8},{'vk':True,'modifiers':0}]:
   with self.assertRaises(ValueError):normalize(bad)
 def test_only_live_owned_foreground_game_gets_one_toggle_per_press(self):
  a=FakeKeys();p=ShortcutPump(a,lambda s,e:True);r=self.record()
  self.assertTrue(p.update(r)['registered']);a.events=[(106,0)];p.update(r);self.assertEqual(a.posts,[42])
  a.pid=999;a.events=[(106,0)];self.assertFalse(p.update(r)['registered']);self.assertEqual(a.unreg,1);self.assertEqual(a.posts,[42])
 def test_change_unregisters_old_and_drops_stale_queued_event(self):
  a=FakeKeys();p=ShortcutPump(a,lambda s,e:True);r=self.record();p.update(r)
  r['shortcut']=dict(vk=119,modifiers=2);a.events=[(106,0),(119,2)];p.update(r)
  self.assertEqual(a.unreg,1);self.assertEqual(a.reg[-1],r['shortcut']);self.assertEqual(a.posts,[42])
 def test_conflict_is_visible_and_no_retry_storm_or_post(self):
  a=FakeKeys();a.conflict=True;p=ShortcutPump(a,lambda s,e:True);r=self.record()
  self.assertIsNotNone(p.update(r)['error']);a.events=[(106,0)];p.update(r)
  self.assertEqual(len(a.reg),1);self.assertFalse(a.posts)
 def test_dead_or_uninitialized_game_and_shutdown_never_post(self):
  a=FakeKeys();p=ShortcutPump(a,lambda s,e:False);self.assertFalse(p.update(self.record())['registered']);self.assertFalse(a.reg)
  p=ShortcutPump(a,lambda s,e:True);a.window=None;self.assertFalse(p.update(self.record())['registered']);self.assertFalse(a.reg)
  a.window=42;p.update(self.record());p.close();self.assertEqual(a.unreg,1)
if __name__=='__main__':unittest.main()
