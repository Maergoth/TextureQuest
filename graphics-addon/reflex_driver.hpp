#pragma once
#include "reflex_policy.hpp"
#include <windows.h>
#include <unknwn.h>
#include <string>
#include <cwchar>
namespace tq::reflex {
// Driver implementation only: never distribute a proxy/static NVAPI library,
// change process DLL search state, create a native device or call from DllMain.
struct NvApiLoader {
 virtual ~NvApiLoader()=default;
 virtual HMODULE Load(std::wstring& expected)=0;
 virtual bool Matches(HMODULE module,const std::wstring& expected)=0;
 virtual FARPROC Resolve(HMODULE module,const char* name)=0;
 virtual bool Free(HMODULE module)=0;
};
class WindowsNvApiLoader final:public NvApiLoader {
public:
 HMODULE Load(std::wstring& path)override{
  wchar_t system[32768]{};const UINT length=GetSystemDirectoryW(system,32768);
  if(!length||length>=32768)return nullptr;
  path=std::wstring(system,length)+L"\\nvapi64.dll";
  return LoadLibraryExW(path.c_str(),nullptr,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_SYSTEM32);
 }
 bool Matches(HMODULE module,const std::wstring& path)override{
  wchar_t actual[32768]{};const DWORD count=GetModuleFileNameW(module,actual,32768);
  return count&&count<32768&&_wcsicmp(path.c_str(),actual)==0;
 }
 FARPROC Resolve(HMODULE module,const char* name)override{return GetProcAddress(module,name);}
 bool Free(HMODULE module)override{return FreeLibrary(module)!=0;}
};
class SystemNvApi final:public Api {
 WindowsNvApiLoader Windows;NvApiLoader& Loader;
 HMODULE Module=nullptr;bool Initialized=false;
 using Query=void*(__cdecl*)(uint32_t);
 using Initialize=NvStatus(__cdecl*)();using Unload=NvStatus(__cdecl*)();
 using GetStatus=NvStatus(__cdecl*)(IUnknown*,SleepStatus*);
 using SetMode=NvStatus(__cdecl*)(IUnknown*,SleepMode*);
 Initialize Init=nullptr;Unload End=nullptr;GetStatus GetFn=nullptr;SetMode SetFn=nullptr;
 NvStatus Refuse(){return LibraryMissing;} // Policy owns cleanup even when Open fails.
public:
 SystemNvApi():Loader(Windows){}
 explicit SystemNvApi(NvApiLoader& injected):Loader(injected){}
 NvStatus Open()override{
  if(Initialized)return Ok;if(Module)return Error;
  std::wstring path;Module=Loader.Load(path);if(!Module)return LibraryMissing;
  if(!Loader.Matches(Module,path))return Refuse();
  const auto query=reinterpret_cast<Query>(Loader.Resolve(Module,"nvapi_QueryInterface"));
  if(!query)return Refuse();
  Init=reinterpret_cast<Initialize>(query(InitializeId));End=reinterpret_cast<Unload>(query(UnloadId));
  GetFn=reinterpret_cast<GetStatus>(query(GetSleepStatusId));SetFn=reinterpret_cast<SetMode>(query(SetSleepModeId));
  if(!Init||!End||!GetFn||!SetFn)return Refuse();
  const auto code=Init();if(code!=Ok)return code;Initialized=true;return Ok;
 }
 NvStatus Get(void* native,SleepStatus& status)override{
  if(!Initialized||!native||!GetFn)return Error;
  return GetFn(static_cast<IUnknown*>(native),&status);
 }
 NvStatus Set(void* native,const SleepMode& request)override{
  if(!Initialized||!native||!SetFn)return Error;
  SleepMode owned=request;return SetFn(static_cast<IUnknown*>(native),&owned);
 }
 NvStatus Close()override{
  // Logical use-close after a successful Initialize. ReShade's destroy_device
  // and AddonUninit both precede native device Release. Actual A/B testing found
  // Unload before that Release unsafe even while the HMODULE remains loaded.
  // Keep one successful initialization + HMODULE per addon instance to process
  // exit; Open reuses them. This intentionally is not balanced native Unload.
  if(Initialized)return Ok;
  // Open failures before successful initialization retain explicit cleanup
  // custody and the existing Free failure/retry behavior.
  if(Module){if(!Loader.Free(Module))return Error;Module=nullptr;}
  Init=nullptr;End=nullptr;GetFn=nullptr;SetFn=nullptr;return Ok;
 }
 // Explicit controller Shutdown/Destroy owns cleanup outside loader lock.
 // There are deliberately no native cleanup calls in this object's destructor.
};
class Controller {
 SystemNvApi System;
 Policy Logic;
public:
 Controller():Logic(System){}
 explicit Controller(Api& injected):Logic(injected){}
 Controller(const Controller&)=delete;Controller&operator=(const Controller&)=delete;
 void Service(void* owner,IUnknown* native,HWND window,bool requested,bool allowed,uint64_t now){Logic.Service(owner,native,reinterpret_cast<uintptr_t>(window),requested,allowed,now);}
 void Destroy(void* owner){Logic.Destroy(owner);}
 void Shutdown(){Logic.Shutdown();}
 reflex::Snapshot Snapshot()const{return Logic.GetSnapshot();}
};
}
