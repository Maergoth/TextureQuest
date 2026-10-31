from pathlib import Path
import sys,unittest
from types import SimpleNamespace
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'src'))
from texturequest_shortcut import normalize,label,from_tk
class ShortcutCases(unittest.TestCase):
 def test_default_capture_and_modifier_names(self):
  self.assertEqual(label(),'Numpad *');self.assertEqual(from_tk(SimpleNamespace(keycode=106,state=0x10)),normalize())
  self.assertEqual(label(from_tk(SimpleNamespace(keycode=120,state=4|8|1))),'Ctrl+Alt+Shift+F9')
  for bad in [{'vk':17,'modifiers':0},{'vk':106,'modifiers':8},{'vk':True,'modifiers':0}]:
   with self.assertRaises(ValueError):normalize(bad)
if __name__=='__main__':unittest.main()
