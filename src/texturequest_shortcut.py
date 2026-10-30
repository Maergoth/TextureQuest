"""Configurable, foreground-only shortcut for the owned graphics input window."""
import ctypes
from ctypes import wintypes
from pathlib import Path
import re
import time

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

class WindowsKeys:
    ID=0x5447
    def __init__(self):
        self.u=ctypes.WinDLL('user32',use_last_error=True)
        self.u.GetForegroundWindow.restype=wintypes.HWND
        self.u.GetWindowThreadProcessId.argtypes=[wintypes.HWND,ctypes.POINTER(wintypes.DWORD)]
        self.u.GetWindowThreadProcessId.restype=wintypes.DWORD
        self.u.FindWindowExW.argtypes=[wintypes.HWND,wintypes.HWND,wintypes.LPCWSTR,wintypes.LPCWSTR]
        self.u.FindWindowExW.restype=wintypes.HWND
        self.u.RegisterHotKey.argtypes=[wintypes.HWND,ctypes.c_int,wintypes.UINT,wintypes.UINT]
        self.u.RegisterHotKey.restype=wintypes.BOOL
        self.u.UnregisterHotKey.argtypes=[wintypes.HWND,ctypes.c_int];self.u.UnregisterHotKey.restype=wintypes.BOOL
        self.u.PeekMessageW.argtypes=[ctypes.POINTER(wintypes.MSG),wintypes.HWND,wintypes.UINT,wintypes.UINT,wintypes.UINT]
        self.u.PeekMessageW.restype=wintypes.BOOL
        self.u.PostMessageW.argtypes=[wintypes.HWND,wintypes.UINT,wintypes.WPARAM,wintypes.LPARAM]
        self.u.PostMessageW.restype=wintypes.BOOL
        # Establish this watcher thread's message queue before registration.
        message=wintypes.MSG();self.u.PeekMessageW(ctypes.byref(message),None,0,0,0)
    def foreground(self):
        pid=wintypes.DWORD();self.u.GetWindowThreadProcessId(self.u.GetForegroundWindow(),ctypes.byref(pid));return pid.value
    def target(self,session):
        nonce=Path(session.get('session','')).name
        if not re.fullmatch('[a-f0-9]{32}',nonce):return None
        window=self.u.FindWindowExW(wintypes.HWND(-3),None,'TextureQuestPrivateProviderHostV3-'+nonce,None)
        pid=wintypes.DWORD()
        return window if window and self.u.GetWindowThreadProcessId(window,ctypes.byref(pid)) and pid.value==session['pid'] else None
    def register(self,value):
        ok=self.u.RegisterHotKey(None,self.ID,value['modifiers']|0x4000,value['vk'])
        return bool(ok),None if ok else ctypes.get_last_error()
    def unregister(self):
        if not self.u.UnregisterHotKey(None,self.ID):raise OSError(ctypes.get_last_error(),'Cannot release the owned texture shortcut')
    def presses(self):
        message=wintypes.MSG();result=[]
        while self.u.PeekMessageW(ctypes.byref(message),None,0x0312,0x0312,1):
            if message.wParam==self.ID:result.append(((message.lParam>>16)&0xffff,message.lParam&7))
        return result
    def post(self,window):
        # The genuine host additionally checks its own PID/birth and foreground
        # before requesting a quality change. No key injection or graphics call.
        if not self.u.PostMessageW(window,0x0312,0x5451,0):
            raise OSError(ctypes.get_last_error(),'The owned graphics input window rejected the shortcut')

class ShortcutPump:
    def __init__(self,api,alive):
        self.api,self.alive=api,alive;self.registered=None;self.attempt=None;self.retry_at=0
    def close(self):
        if self.registered is not None:self.api.unregister();self.registered=None
    def update(self,record):
        spec=normalize(record.get('shortcut'));foreground=self.api.foreground()
        session=next((s for s in record['sessions'] if s['pid']==foreground and self.alive(s,s['exe'])),None)
        target=self.api.target(session) if session else None
        desired=(spec['vk'],spec['modifiers'],session['pid'],session['birth100ns']) if target else None
        if self.registered is not None and desired!=self.registered:self.close()
        error=None
        if desired and self.registered is None:
            if self.attempt!=desired or time.monotonic()>=self.retry_at:
                ok,error=self.api.register(spec);self.attempt=desired;self.retry_at=time.monotonic()+1
                if ok:self.registered=desired
            else:error='in_use'
        for pressed in self.api.presses():
            if (pressed==(spec['vk'],spec['modifiers']) and target and desired==self.registered and self.api.foreground()==session['pid'] and
                    self.alive(session,session['exe']) and self.api.target(session)==target):
                self.api.post(target)
        return dict(binding=spec,label=label(spec),registered=self.registered is not None,
                    error=('Shortcut is in use. Choose another key in TextureQuest.' if error else None))
