"""One new concrete Instructions regression case; no Tk/browser launch."""
import ast
from pathlib import Path
import sys,unittest
from unittest.mock import patch
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'src'))
import texturequest_app as module

class InstructionsPreserved(unittest.TestCase):
    def test_actual_instructions_menu_callback_works_without_app_update_browser_fallback(self):
        tree=ast.parse(Path(module.__file__).read_text(encoding='utf-8'))
        app=next(n for n in tree.body if isinstance(n,ast.ClassDef) and n.name=='TextureQuestApp')
        build=next(n for n in app.body if isinstance(n,ast.FunctionDef) and n.name=='build')
        call=next(n for n in ast.walk(build) if isinstance(n,ast.Call) and any(k.arg=='label' and isinstance(k.value,ast.Constant) and k.value.value=='Instructions' for k in n.keywords))
        callback=next(k.value for k in call.keywords if k.arg=='command')
        with patch.object(module.webbrowser,'open') as browser:
            command=eval(compile(ast.fix_missing_locations(ast.Expression(callback)),'actual-instructions-menu','eval'),vars(module))
            command();browser.assert_called_once_with('https://github.com/Maergoth/TextureQuest#download-and-install')
        update=next(n for n in app.body if isinstance(n,ast.FunctionDef) and n.name=='open_app_update')
        self.assertFalse(any(isinstance(n,ast.Name) and n.id=='webbrowser' for n in ast.walk(update)))
if __name__=='__main__':unittest.main()
