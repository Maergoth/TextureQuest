// Source-staged private profile bootstrap. No device, probe, detour or fallback.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "profile_guard.hpp"
#include <atomic>
namespace {
struct State {std::atomic<bool> attempted{false};std::atomic<uint32_t> error{0};startup::ProfileStatus status;textureprofile::HeldProfile profile;std::unique_ptr<startup::Configure> config;};
State& S(){static auto*s=new State;return *s;} // Natural-exit file/directory custody.
}
extern "C" __declspec(dllexport) DWORD WINAPI TQPH_PrepareV3(const void* memory)noexcept{
 auto& s=S();if(s.attempted.exchange(true))return ERROR_ALREADY_INITIALIZED;
 try{s.config=std::make_unique<startup::Configure>();textureprofile::Need(memory&&textureprofile::ReadSafe(s.config.get(),memory,sizeof(*s.config)));auto& c=*s.config;uint64_t birth=0;
  textureprofile::Need(c.magic==0x33504854&&c.bytes==sizeof(c)&&!c.reserved&&c.pid==GetCurrentProcessId()&&startup::Birth(GetCurrentProcess(),birth)&&birth==c.birth&&startup::Hex(c.nonce,32)&&!c.zero[0]&&!c.zero[1]);
  textureprofile::Gate(c);s.profile.Acquire(c);textureprofile::Gate(c);
  memcpy(s.status.magic,"TQPHPR03",8);s.status.pid=c.pid;s.status.birth=c.birth;memcpy(s.status.nonce,c.nonce,33);s.status.held=1;s.status.ready=1;return 0;
 }catch(...){s.error=ERROR_INVALID_DATA;return ERROR_INVALID_DATA;}
}
// Future ordinary EXE starts use an early attach. It never edits entry bytes,
// suspends a thread, or changes parent/global environment. Missing the early
// device-creation window is reported by the controller as restart-needed.
extern "C" __declspec(dllexport) DWORD WINAPI TQGX_PrepareAttachV4(const void* memory)noexcept{
 auto&s=S();if(s.attempted.exchange(true))return ERROR_ALREADY_INITIALIZED;
 try{s.config=std::make_unique<startup::Configure>();textureprofile::Need(memory&&textureprofile::ReadSafe(s.config.get(),memory,sizeof(*s.config)));auto&c=*s.config;uint64_t birth=0;
  textureprofile::Need(c.magic==0x33504854&&c.bytes==sizeof(c)&&c.reserved==1&&c.pid==GetCurrentProcessId()&&startup::Birth(GetCurrentProcess(),birth)&&birth==c.birth&&startup::Hex(c.nonce,32)&&!c.primary_tid&&!c.primary_birth&&!c.gate_block&&!c.gate_code&&!c.entry&&!c.entry_protect);
  for(auto b:c.patch)textureprofile::Need(!b);for(auto b:c.zero)textureprofile::Need(!b);
  textureprofile::Need(!GetModuleHandleW(L"ReShade64.dll"));auto base=textureprofile::Canonical(c.base);textureprofile::Need(base==textureprofile::Canonical(c.session));
  LPWCH environment=GetEnvironmentStringsW();textureprofile::Need(environment!=nullptr);bool clean=true;for(auto p=environment;*p;p+=wcslen(p)+1){std::wstring line(p);auto eq=line.find(L'=');auto key=line.substr(0,eq);for(auto&ch:key)if(ch>=L'a'&&ch<=L'z')ch-=32;if(key.rfind(L"RESHADE_",0)==0||key.rfind(L"TQPH_",0)==0)clean=false;}FreeEnvironmentStringsW(environment);textureprofile::Need(clean);
  textureprofile::Need(SetEnvironmentVariableW(L"RESHADE_BASE_PATH_OVERRIDE",base.c_str())&&SetEnvironmentVariableW(L"RESHADE_DISABLE_INPUT_HOOK",L"1")&&SetEnvironmentVariableW(L"RESHADE_DISABLE_LOADING_CHECK",L"1")&&SetEnvironmentVariableW(L"TQPH_SESSION_ROOT",base.c_str()));
  s.profile.Acquire(c);memcpy(s.status.magic,"TQPHPR03",8);s.status.pid=c.pid;s.status.birth=c.birth;memcpy(s.status.nonce,c.nonce,33);s.status.held=s.status.ready=1;return 0;
 }catch(...){s.error=ERROR_INVALID_DATA;return ERROR_INVALID_DATA;}
}
extern "C" __declspec(dllexport) DWORD WINAPI TQPH_GetProfileV3(void* output)noexcept{
 auto& s=S();auto status=s.status;if(!s.profile.Validate()){uint32_t zero=0;s.error.compare_exchange_strong(zero,ERROR_INVALID_DATA);}status.error=s.error;status.ready=status.ready&&!status.error;
 return output&&textureprofile::WriteSafe(output,&status,sizeof status)?0:ERROR_NOACCESS;
}
BOOL WINAPI DllMain(HINSTANCE h,DWORD reason,void*){if(reason==DLL_PROCESS_ATTACH)DisableThreadLibraryCalls(h);return TRUE;}
