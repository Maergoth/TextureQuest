#pragma once
#include "proxy_files.hpp"
#include "proxy_wire.hpp"
#include <bcrypt.h>
#include <shellapi.h>
namespace {
struct ProxyState {
 std::map<std::string,std::string> fields;std::vector<std::shared_ptr<File>> files;std::vector<HMODULE> modules;
 std::wstring base,session;std::string nonce;Handle mapping;uint64_t observed=UINT64_MAX;
};
ProxyState&P(){static auto*p=new ProxyState;return *p;}
std::wstring PXModule(HMODULE m){wchar_t b[32768]{};auto n=GetModuleFileNameW(m,b,32768);Need(n&&n<32768,"proxy module path");return Canon(std::wstring(b,n));}
std::wstring PXDecode(const std::string&v){Need(!v.empty()&&!(v.size()%2)&&v.size()<=65536,"encoded proxy path length");std::string b;b.reserve(v.size()/2);auto nibble=[](char c){Need((c>='0'&&c<='9')||(c>='a'&&c<='f'),"encoded proxy path hex");return c<='9'?c-'0':c-'a'+10;};for(size_t i=0;i<v.size();i+=2)b.push_back(char((nibble(v[i])<<4)|nibble(v[i+1])));auto n=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,b.data(),int(b.size()),nullptr,0);Need(n>0,"proxy UTF8 path");std::wstring w(n,L'\0');Need(MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,b.data(),int(b.size()),w.data(),n)==n&&w.find(L'\0')==std::wstring::npos,"proxy path framing");return Canon(w);}
void PXNew(const std::wstring&path,const std::string&text){Handle h(CreateFileW(path.c_str(),GENERIC_WRITE,FILE_SHARE_READ,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr));Need(h.h!=INVALID_HANDLE_VALUE,"new proxy record");DWORD n=0;Need(WriteFile(h.h,text.data(),DWORD(text.size()),&n,nullptr)&&n==text.size(),"proxy record write");}
std::shared_ptr<File> PXFile(const std::string&role){auto&p=P();auto f=std::make_shared<File>(PXDecode(p.fields.at(role)),p.fields.at(role+"_sha256"));p.files.push_back(f);return f;}
HMODULE PXLoad(const std::shared_ptr<File>&f){auto h=LoadLibraryExW(f->path.c_str(),nullptr,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);Need(h&&PXModule(h)==f->path,"normal in-process module load");P().modules.push_back(h);return h;}
template<class T>T PXFunction(HMODULE m,const char*name){auto f=GetProcAddress(m,name);Need(f&&Image(reinterpret_cast<void*>(f),m,true),"proxy export owner");return reinterpret_cast<T>(f);}
bool PXKeyValid(unsigned vk,unsigned mods){return mods<=7&&((vk>=0x30&&vk<=0x39)||(vk>=0x41&&vk<=0x5a)||(vk>=0x60&&vk<=0x6b)||(vk>=0x6d&&vk<=0x6f)||(vk>=0x70&&vk<=0x87)||vk==0x20||(vk>=0x21&&vk<=0x28)||vk==0x2d||vk==0x2e);}
std::pair<unsigned,unsigned> PXKey(){
 auto&p=P();unsigned vk=106,mods=0;Handle file(CreateFileW((p.base+L"\\shortcut.txt").c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,nullptr));
 if(file.h!=INVALID_HANDLE_VALUE){char b[513]{};DWORD n=0;BY_HANDLE_FILE_INFORMATION info{};if(GetFileInformationByHandle(file.h,&info)&&!(info.dwFileAttributes&(FILE_ATTRIBUTE_REPARSE_POINT|FILE_ATTRIBUTE_DIRECTORY))&&ReadFile(file.h,b,512,&n,nullptr)&&n&&n<512){unsigned a,c;char tail;if(sscanf_s(b,"TQKEY1\n%u\n%u\n%c",&a,&c,&tail,1)==2&&PXKeyValid(a,c)){vk=a;mods=c;}}}
 return {vk,mods};
}
DWORD WINAPI ProxyInput(void*)noexcept{
 try{auto&h=H();std::wstring name=L"TextureQuestProxyInput-";for(unsigned i=0;i<32;++i)name+=wchar_t(h.init.nonce[i]);WNDCLASSEXW c{};c.cbSize=sizeof c;c.lpfnWndProc=Window;c.hInstance=self_module;c.lpszClassName=name.c_str();require(RegisterClassExW(&c)!=0,"proxy input class");HWND w=CreateWindowExW(0,name.c_str(),L"",0,0,0,0,0,HWND_MESSAGE,nullptr,self_module,nullptr);require(w,"proxy input window");h.input_ready=true;
  bool registered=false;std::pair<unsigned,unsigned> active{};uint64_t next=0;
  while(!h.stopping){DWORD foreground=0;GetWindowThreadProcessId(GetForegroundWindow(),&foreground);auto key=PXKey();bool want=foreground==h.pid;
   if(registered&&(!want||active!=key)){require(UnregisterHotKey(w,0x5451)!=FALSE,"release own proxy shortcut");registered=false;MSG old{};while(PeekMessageW(&old,w,WM_HOTKEY,WM_HOTKEY,PM_REMOVE)){};}
   if(want&&!registered&&GetTickCount64()>=next){if(RegisterHotKey(w,0x5451,key.second|MOD_NOREPEAT,key.first)){registered=true;active=key;}next=GetTickCount64()+1000;}
   MSG m{};while(PeekMessageW(&m,nullptr,0,0,PM_REMOVE)){TranslateMessage(&m);DispatchMessageW(&m);}Sleep(50);
  }if(registered)UnregisterHotKey(w,0x5451);h.input_ready=false;return 0;
 }catch(...){Fail(4602);return 1;}
}
void ProxyObserve()noexcept{
 try{auto&p=P();auto&h=H();auto seq=h.control->Load();if(p.observed==seq)return;auto core=std::atomic_load(&h.full);if(!core)return;auto state=core->Snapshot();if(state.ack_sequence!=(seq>>1))return;
  std::string text="{\"schema\":\"texturequest-graphics-only-present-v1\",\"pid\":"+std::to_string(h.pid)+",\"birth100ns\":"+std::to_string(h.birth)+",\"sequence\":"+std::to_string(seq>>1)+",\"original\":"+(state.original?"true":"false")+",\"frames\":"+std::to_string(state.frame)+",\"matched_resources\":"+std::to_string(state.pairs)+",\"replacement_views\":"+std::to_string(state.views)+",\"replaced_draws\":"+std::to_string(state.replaced_draws)+",\"host_error\":"+std::to_string(h.error.load())+",\"archive_redirection\":false}\n";
  PXNew(p.session+L"\\present-"+std::to_wstring(seq>>1)+L".json",text);p.observed=seq;
 }catch(...){/* Keep rendering and selected pixels; failed optional log is not a GPU operation. */}
}
}
extern "C" __declspec(dllexport) const char* NAME="TextureQuest";
extern "C" __declspec(dllexport) const char* DESCRIPTION="Released textures with on-demand Original comparison";
bool PXMainClient()noexcept{int n=0;auto args=CommandLineToArgvW(GetCommandLineW(),&n);if(!args)return false;bool main=true;for(int i=1;i<n;++i)if(!wcsncmp(args[i],L"--type=",7)||!wcscmp(args[i],L"--type"))main=false;LocalFree(args);return main;}
extern "C" __declspec(dllexport) bool AddonInit(HMODULE addon,HMODULE provider)noexcept{
 if(!PXMainClient())return false; // EQ2 browser/CEF children are not game render targets.
 try{Need(addon==self_module&&!proxy_startup,"one normal proxy add-on startup");proxy_startup=true;auto&p=P();auto folder=fs::path(PXModule(addon)).parent_path();auto cfg=std::make_shared<File>((folder/L"TextureQuestProxy.cfg").wstring(),"",65535,false);p.files.push_back(cfg);
  p.fields=policy::Fields(std::string(cfg->bytes.begin(),cfg->bytes.end()),"TQGFXP1\n",65535);
  policy::Keys(p.fields,{"installation","game","base","host_sha256","provider_sha256","bootstrap","bootstrap_sha256","catalog","catalog_sha256","base_ini_sha256","game_ini_sha256"});
  Need(startup::Hex(p.fields.at("installation"),32),"proxy installation identity");auto game=PXDecode(p.fields.at("game"));p.base=PXDecode(p.fields.at("base"));Need(PXModule(nullptr)==game+L"\\everquest2.exe"&&folder==fs::path(p.base)/L"addons"&&PXModule(provider)==game+L"\\d3d11.dll","normal proxy/addon owner paths");
  auto host_file=std::make_shared<File>(PXModule(addon),p.fields.at("host_sha256"));auto provider_file=std::make_shared<File>(PXModule(provider),p.fields.at("provider_sha256"));p.files.push_back(host_file);p.files.push_back(provider_file);
  auto game_ini=std::make_shared<File>(game+L"\\TextureQuest.ini",p.fields.at("game_ini_sha256"),65535);auto base_ini=std::make_shared<File>(p.base+L"\\ReShade.ini",p.fields.at("base_ini_sha256"),65535);p.files.push_back(game_ini);p.files.push_back(base_ini);
  auto bootstrap_file=PXFile("bootstrap"),catalog=PXFile("catalog");
  Need(fs::path(bootstrap_file->path).parent_path()==folder,"proxy companion sibling files");
  unsigned char nonce[16];Need(BCryptGenRandom(nullptr,nonce,sizeof nonce,BCRYPT_USE_SYSTEM_PREFERRED_RNG)==0,"proxy session nonce");p.nonce=HexBytes(nonce,sizeof nonce);p.session=p.base+L"\\sessions\\"+std::wstring(p.nonce.begin(),p.nonce.end());Need(p.session.size()<220,"proxy session path bound");
  Need(CreateDirectoryW((p.base+L"\\sessions").c_str(),nullptr)||GetLastError()==ERROR_ALREADY_EXISTS,"proxy sessions folder");Need(CreateDirectoryW(p.session.c_str(),nullptr),"new proxy session");uint64_t birth=0;Need(startup::Birth(GetCurrentProcess(),birth),"proxy process birth");
  auto bootstrap=PXLoad(bootstrap_file);auto profile=std::make_unique<ProxyProfileWire>();profile->pid=GetCurrentProcessId();profile->birth=birth;memcpy(profile->nonce,p.nonce.c_str(),33);wcscpy_s(profile->base,p.base.c_str());wcscpy_s(profile->game,game.c_str());wcscpy_s(profile->session,p.session.c_str());memcpy(profile->base_ini,p.fields.at("base_ini_sha256").c_str(),65);memcpy(profile->game_ini,p.fields.at("game_ini_sha256").c_str(),65);
  Need(PXFunction<DWORD(WINAPI*)(const void*)>(bootstrap,"TQPX_PrepareProxyV1")(profile.get())==0,"normal proxy profile");
  tqgr::InitV3 init{};memcpy(init.magic,"TQPHIN03",8);init.bytes=sizeof(init);init.version=3;init.pid=GetCurrentProcessId();init.flags=3;init.birth100ns=birth;init.provider=reinterpret_cast<uint64_t>(provider);init.bootstrap=reinterpret_cast<uint64_t>(bootstrap);init.profile_receipt=reinterpret_cast<uint64_t>(PXFunction<DWORD(WINAPI*)(void*)>(bootstrap,"TQPH_GetProfileV3"));init.catalog_address=reinterpret_cast<uint64_t>(catalog->bytes.data());init.catalog_bytes=uint32_t(catalog->bytes.size());memcpy(init.nonce,p.nonce.c_str(),33);memcpy(init.catalog_sha256,catalog->hash.c_str(),65);
  Need(TQPH_InitializeV3(&init)==0,"normal proxy graphics initialization");Need(H().control->Request(1,false),"initial proxy Upscaled request");PXNew(p.session+L"\\ADDON-READY.json","{\"normal_addon\":true,\"remote_thread_injection\":false,\"archive_redirection\":false,\"pid\":"+std::to_string(GetCurrentProcessId())+",\"birth100ns\":"+std::to_string(birth)+"}\n");return true;
 }catch(const std::exception&e){try{if(!P().session.empty())PXNew(P().session+L"\\REFUSED.txt",e.what());}catch(...){}Fail(4701);return false;}catch(...){Fail(4702);return false;}
}
