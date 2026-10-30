#pragma once
#include "startup_protocol.hpp"
#include "profile_policy.hpp"
#include <filesystem>
#include <memory>
namespace textureprofile {
inline bool ReadSafe(void*d,const void*s,size_t n)noexcept{__try{memcpy(d,s,n);return true;}__except(EXCEPTION_EXECUTE_HANDLER){return false;}}
inline bool WriteSafe(void*d,const void*s,size_t n)noexcept{__try{memcpy(d,s,n);return true;}__except(EXCEPTION_EXECUTE_HANDLER){return false;}}
inline void Need(bool b){if(!b)throw std::runtime_error("texture profile custody/gate refused");}
struct Hold {HANDLE h=INVALID_HANDLE_VALUE;explicit Hold(HANDLE v):h(v){}~Hold(){if(h!=INVALID_HANDLE_VALUE)CloseHandle(h);}Hold(const Hold&)=delete;};
inline std::wstring Canonical(const std::wstring& p){std::wstring r;Need(tq::Canonical(p,r));return r;}
inline std::string ASCII(const std::wstring& p){auto n=WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,p.data(),int(p.size()),nullptr,0,nullptr,nullptr);Need(n>0);std::string result(n,'\0');Need(WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,p.data(),int(p.size()),result.data(),n,nullptr,nullptr)==n);return result;}
class HeldProfile {
 std::vector<std::unique_ptr<Hold>> handles_;std::wstring base_,addon_,adjacent_;bool adjacent_absent_=false;
 void Ordinary(const std::wstring& path,bool directory){
  auto h=std::make_unique<Hold>(CreateFileW(path.c_str(),directory?FILE_READ_ATTRIBUTES|FILE_LIST_DIRECTORY:GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT|(directory?FILE_FLAG_BACKUP_SEMANTICS:0),nullptr));Need(h->h!=INVALID_HANDLE_VALUE);
  BY_HANDLE_FILE_INFORMATION i{};Need(GetFileInformationByHandle(h->h,&i)&&!(i.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT)&&bool(i.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)==directory);
  wchar_t final[32768]{};auto n=GetFinalPathNameByHandleW(h->h,final,32768,FILE_NAME_NORMALIZED|VOLUME_NAME_DOS);Need(n>4&&n<32768);std::wstring resolved(final,n);if(resolved.rfind(L"\\\\?\\",0)==0)resolved.erase(0,4);Need(Canonical(resolved)==path);handles_.push_back(std::move(h));
 }
 std::string ReadHeld(const std::wstring& path){Ordinary(path,false);auto h=handles_.back()->h;LARGE_INTEGER n{};Need(GetFileSizeEx(h,&n)&&n.QuadPart>0&&n.QuadPart<=65535);std::string s(size_t(n.QuadPart),'\0');DWORD got=0;Need(ReadFile(h,s.data(),DWORD(s.size()),&got,nullptr)&&got==s.size());return s;}
 void Chain(const std::wstring& path,bool exact_game_parent=false){Need(path.size()>3&&path[1]==L':'&&path[2]==L'\\');(void)exact_game_parent;std::filesystem::path p(path);std::vector<std::wstring> chain;while(p.wstring().size()>3){chain.push_back(Canonical(p.wstring()));p=p.parent_path();}for(auto i=chain.rbegin();i!=chain.rend();++i)Ordinary(*i,true);}
 static std::wstring Env(const wchar_t* name){wchar_t value[32768]{};SetLastError(0);auto n=GetEnvironmentVariableW(name,value,32768);Need(n<32768);if(!n){Need(GetLastError()==ERROR_ENVVAR_NOT_FOUND||GetLastError()==ERROR_SUCCESS);return {};}return {value,n};}
public:
 void Acquire(const startup::Configure& c){
  Need(handles_.empty()&&wcsnlen_s(c.base,32768)<32768&&wcsnlen_s(c.session,32768)<32768);base_=Canonical(c.base);Need(base_==Canonical(c.session)&&base_.size()<220);addon_=base_+L"\\addons-empty";
  Need(Canonical(Env(L"RESHADE_BASE_PATH_OVERRIDE"))==base_&&Env(L"RESHADE_DISABLE_INPUT_HOOK")==L"1"&&Env(L"RESHADE_DISABLE_LOADING_CHECK")==L"1"&&Env(L"TQPH_SESSION_ROOT")==base_);
  LPWCH block=GetEnvironmentStringsW();Need(block!=nullptr);std::set<std::wstring> allowed={L"RESHADE_BASE_PATH_OVERRIDE",L"RESHADE_DISABLE_INPUT_HOOK",L"RESHADE_DISABLE_LOADING_CHECK"};bool valid=true;
  for(auto p=block;*p;p+=wcslen(p)+1){std::wstring line(p);auto eq=line.find(L'=');if(eq==0)continue;auto key=line.substr(0,eq);for(auto& x:key)if(x>=L'a'&&x<=L'z')x-=32;if(key.rfind(L"RESHADE_",0)==0&&!allowed.count(key))valid=false;}FreeEnvironmentStringsW(block);Need(valid);
  Chain(base_);Ordinary(addon_,true);Need(ReadHeld(base_+L"\\reshade.ini")==Ini(ASCII(addon_)));
  wchar_t exe[32768]{};auto n=GetModuleFileNameW(nullptr,exe,32768);Need(n&&n<32768);auto executable=Canonical(std::wstring(exe,n));auto parent=Canonical(std::filesystem::path(executable).parent_path().wstring());Need(base_!=parent&&!tq::Prefix(base_.c_str(),parent));Chain(parent,true);adjacent_=parent+L"\\reshade.ini";
  // Both purposes require a real held adjacent INI. C remains read-only.
  // It does not treat a check for absent metadata as persistent file custody.
  SetLastError(0);auto attrs=GetFileAttributesW(adjacent_.c_str());if(attrs==INVALID_FILE_ATTRIBUTES){Need(GetLastError()==ERROR_FILE_NOT_FOUND);adjacent_absent_=true;}else Need(Adjacent(ReadHeld(adjacent_)));
  Need(Validate());
 }
 bool Validate()const noexcept{try{
  if(base_.empty())return false;if(adjacent_absent_){SetLastError(0);if(GetFileAttributesW(adjacent_.c_str())!=INVALID_FILE_ATTRIBUTES||GetLastError()!=ERROR_FILE_NOT_FOUND)return false;}
  WIN32_FIND_DATAW data{};auto search=FindFirstFileW((addon_+L"\\*").c_str(),&data);if(search==INVALID_HANDLE_VALUE)return false;bool empty=true;do{if(wcscmp(data.cFileName,L".")&&wcscmp(data.cFileName,L".."))empty=false;}while(FindNextFileW(search,&data));auto error=GetLastError();FindClose(search);return empty&&error==ERROR_NO_MORE_FILES;
 }catch(...){return false;}}
};
inline void Gate(const startup::Configure& c){
 auto primary=std::make_unique<Hold>(OpenThread(THREAD_QUERY_INFORMATION|SYNCHRONIZE,FALSE,c.primary_tid));Need(primary->h!=INVALID_HANDLE_VALUE&&primary->h!=nullptr&&GetProcessIdOfThread(primary->h)==c.pid&&GetThreadId(primary->h)==c.primary_tid&&WaitForSingleObject(primary->h,0)==WAIT_TIMEOUT);
 FILETIME a{},b{},d{},e{};Need(GetThreadTimes(primary->h,&a,&b,&d,&e)&&startup::Number(a)==c.primary_birth);
 startup::GateBlock block{};Need(ReadSafe(&block,reinterpret_cast<void*>(c.gate_block),sizeof(block))&&block.magic==0x31534754&&block.stage==1&&!block.release&&block.pid==c.pid&&block.birth==c.birth&&!memcmp(block.nonce,c.nonce,33));
 auto image=reinterpret_cast<unsigned char*>(GetModuleHandleW(nullptr));IMAGE_DOS_HEADER dos{};IMAGE_NT_HEADERS64 nt{};Need(ReadSafe(&dos,image,sizeof dos)&&dos.e_magic==IMAGE_DOS_SIGNATURE&&dos.e_lfanew>0&&dos.e_lfanew<4096&&ReadSafe(&nt,image+dos.e_lfanew,sizeof nt)&&nt.Signature==IMAGE_NT_SIGNATURE&&nt.FileHeader.Machine==IMAGE_FILE_MACHINE_AMD64&&c.entry==reinterpret_cast<uint64_t>(image)+nt.OptionalHeader.AddressOfEntryPoint);
 unsigned char patch[14]{};Need(ReadSafe(patch,reinterpret_cast<void*>(c.entry),14)&&!memcmp(patch,c.patch,14)&&patch[0]==0xff&&patch[1]==0x25);for(unsigned i=2;i<6;++i)Need(!patch[i]);uint64_t jump=0;memcpy(&jump,patch+6,8);Need(jump==c.gate_code);
 MEMORY_BASIC_INFORMATION m{},g{},r{};Need(VirtualQuery(reinterpret_cast<void*>(c.entry),&m,sizeof m)==sizeof m&&m.Type==MEM_IMAGE&&m.Protect==c.entry_protect&&VirtualQuery(reinterpret_cast<void*>(c.gate_code),&g,sizeof g)==sizeof g&&g.Type==MEM_PRIVATE&&g.Protect==PAGE_EXECUTE_READ&&VirtualQuery(reinterpret_cast<void*>(c.gate_block),&r,sizeof r)==sizeof r&&r.Type==MEM_PRIVATE&&r.Protect==PAGE_READWRITE);
 unsigned char actual[43]{},expected[43]={0x9c,0x50,0x48,0xb8};memcpy(expected+4,&c.gate_block,8);const unsigned char middle[]={0xc7,0x40,0x04,1,0,0,0,0xf3,0x90,0x83,0x78,0x08,1,0x75,0xf8,0x58,0x9d,0xff,0x25,0,0,0,0};memcpy(expected+12,middle,sizeof middle);memcpy(expected+35,&c.entry,8);Need(ReadSafe(actual,reinterpret_cast<void*>(c.gate_code),43)&&!memcmp(actual,expected,43));
}
} // namespace textureprofile
