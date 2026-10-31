"""Graphics hook shortcut: validation, labels and capture from Tk key events."""

DEFAULT={'vk':0x6a,'modifiers':0}
NAMES={0x20:'Space',0x21:'Page Up',0x22:'Page Down',0x23:'End',0x24:'Home',
       0x25:'Left',0x26:'Up',0x27:'Right',0x28:'Down',0x2d:'Insert',0x2e:'Delete',
       0x6a:'Numpad *',0x6b:'Numpad +',0x6d:'Numpad -',0x6e:'Numpad .',0x6f:'Numpad /'}
NAMES.update({n:chr(n) for n in range(0x30,0x3a)})
NAMES.update({n:chr(n) for n in range(0x41,0x5b)})
NAMES.update({0x60+n:'Numpad '+str(n) for n in range(10)})
NAMES.update({0x70+n:'F'+str(n+1) for n in range(24)})

def normalize(value=None):
    value=DEFAULT if value is None else value
    if (type(value) is not dict or type(value.get('vk')) is not int or value['vk'] not in NAMES or
            type(value.get('modifiers')) is not int or not 0<=value['modifiers']<=7):
        raise ValueError('Choose a function, number, letter, navigation or numpad key, optionally with Ctrl, Alt or Shift.')
    return dict(vk=value['vk'],modifiers=value['modifiers'])

def label(value=None):
    value=normalize(value);parts=[]
    for bit,text in [(2,'Ctrl'),(1,'Alt'),(4,'Shift')]:
        if value['modifiers']&bit:parts.append(text)
    return '+'.join(parts+[NAMES[value['vk']]])

def from_tk(event):
    return normalize(dict(vk=event.keycode,modifiers=(2 if event.state&4 else 0)|
                          (1 if event.state&(8|0x20000) else 0)|(4 if event.state&1 else 0)))

