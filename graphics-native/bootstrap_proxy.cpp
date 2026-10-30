#define NOMINMAX
#include "proxy_files.hpp"
#include "proxy_wire.hpp"
#include <atomic>
namespace {
struct State {std::atomic<bool> attempted{false};startup::ProfileStatus status{};std::vector<std::unique_ptr<File>> files;};
State&S(){static auto*s=new State;return *s;}
}
extern "C" __declspec(dllexport) DWORD WINAPI TQPX_PrepareProxyV1(const void* input)noexcept{
 auto&s=S();if(s.attempted.exchange(true))return ERROR_ALREADY_INITIALIZED;
 try{auto p=std::make_unique<ProxyProfileWire>();Need(input&&ProxyCopy(p.get(),input,sizeof(*p))&&p->bytes==sizeof(*p),"proxy profile framing");Need(p->nonce[32]==0&&strnlen_s(p->nonce,33)==32&&p->base_ini[64]==0&&strnlen_s(p->base_ini,65)==64&&p->game_ini[64]==0&&strnlen_s(p->game_ini,65)==64&&wcsnlen_s(p->base,32768)<32768&&wcsnlen_s(p->game,32768)<32768&&wcsnlen_s(p->session,32768)<32768,"bounded proxy wire strings");uint64_t birth=0;Need(p->pid==GetCurrentProcessId()&&startup::Birth(GetCurrentProcess(),birth)&&birth==p->birth&&startup::Hex(p->nonce,32),"proxy exact current process");
  auto base=Canon(p->base),game=Canon(p->game),session=Canon(p->session);wchar_t exe[32768]{};auto n=GetModuleFileNameW(nullptr,exe,32768);Need(n&&n<32768&&Canon(std::wstring(exe,n))==game+L"\\everquest2.exe"&&base!=game&&!tq::Prefix(base.c_str(),game)&&tq::Prefix(session.c_str(),base),"proxy folder authority");
  s.files.push_back(std::make_unique<File>(base+L"\\ReShade.ini",p->base_ini,65535));s.files.push_back(std::make_unique<File>(game+L"\\TextureQuest.ini",p->game_ini,65535));
  auto base_text=std::string(s.files[0]->bytes.begin(),s.files[0]->bytes.end()),game_text=std::string(s.files[1]->bytes.begin(),s.files[1]->bytes.end());
  Need(base_text==textureprofile::Ini(Narrow(base+L"\\addons"))&&game_text=="[INSTALL]\nBasePath="+Narrow(base)+"\n","exact texture-only proxy INI profile");
  memcpy(s.status.magic,"TQPHPR03",8);s.status.pid=p->pid;s.status.birth=birth;memcpy(s.status.nonce,p->nonce,33);s.status.ready=s.status.held=1;return 0;
 }catch(...){s.status.error=ERROR_INVALID_DATA;return ERROR_INVALID_DATA;}
}
extern "C" __declspec(dllexport) DWORD WINAPI TQPH_GetProfileV3(void*out)noexcept{
 auto s=S().status;return out&&ProxyCopy(out,&s,sizeof s)?0:ERROR_NOACCESS;
}
BOOL WINAPI DllMain(HMODULE m,DWORD r,void*){if(r==DLL_PROCESS_ATTACH)DisableThreadLibraryCalls(m);return TRUE;}
