#pragma once
// Common exact-PID/birth remote call custody, derived from the tested startup
// coordinator. Early attach has no entry patch, thread suspension, process
// creation, debugger, process kill, or game-file write path.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winternl.h>
#include <tlhelp32.h>
#include <psapi.h>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <set>
#include <stdexcept>
#include <vector>
#include <cstdio>
#include "saved_sha256.h"
#include "startup_protocol.hpp"
#include "profile_policy.hpp"
#include "startup_policy.hpp"
#include "startup_release_disposition.hpp"
#include "texturequest_provider_mutation_v2.hpp"
#include "intent_reader.hpp"
#include "provider_source_contract.hpp"
#pragma comment(lib,"psapi.lib")
#pragma comment(lib,"user32.lib")
namespace fs=std::filesystem;
static bool close_failed=false;
struct Handle { HANDLE h=nullptr;Handle()=default;explicit Handle(HANDLE p):h(p){};~Handle(){if(h&&h!=INVALID_HANDLE_VALUE&&!CloseHandle(h))close_failed=true;}Handle(const Handle&)=delete;Handle&operator=(const Handle&)=delete; };
static void Need(bool b,const char* s){if(!b)throw std::runtime_error(s);}
static std::wstring Wide(const std::string& s){for(unsigned char c:s)Need(c>=32&&c<127,"request is strict ASCII");return std::wstring(s.begin(),s.end());}
static std::string Narrow(const std::wstring& s){int n=WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,s.data(),int(s.size()),nullptr,0,nullptr,nullptr);Need(n>0,"path UTF16");std::string result(n,'\0');Need(WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,s.data(),int(s.size()),result.data(),n,nullptr,nullptr)==n,"path UTF8 conversion");return result;}
static std::wstring Canon(const std::wstring& s){std::wstring r;Need(tq::Canonical(s,r),"canonical drive path refused");return r;}
static bool DPath(const std::wstring& s){return s.size()>3&&s[0]==L'd'&&s[1]==L':'&&s[2]==L'\\';}
static std::string HexBytes(const unsigned char* p,size_t n){std::string r;for(size_t i=0;i<n;i++){r+="0123456789abcdef"[p[i]>>4];r+="0123456789abcdef"[p[i]&15];}return r;}
struct File {std::wstring path;Handle handle;std::vector<unsigned char> bytes;std::string hash;
 File(const std::wstring& p,const std::string& expected,size_t maximum=128*1024*1024,bool pinned=true):path(Canon(p)),handle(CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,nullptr)){
  Need(handle.h!=INVALID_HANDLE_VALUE,"pinned FILE open refused");BY_HANDLE_FILE_INFORMATION info{};Need(GetFileInformationByHandle(handle.h,&info)&&!(info.dwFileAttributes&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT)),"not an ordinary pinned FILE");
  wchar_t final[32768];DWORD n=GetFinalPathNameByHandleW(handle.h,final,32768,FILE_NAME_NORMALIZED|VOLUME_NAME_DOS);Need(n&&n<32768,"final FILE path unknown");std::wstring resolved(final,n);if(resolved.rfind(L"\\\\?\\",0)==0)resolved.erase(0,4);Need(Canon(resolved)==path,"FILE alias refused");
  LARGE_INTEGER size{};Need(GetFileSizeEx(handle.h,&size)&&size.QuadPart>0&&uint64_t(size.QuadPart)<=maximum,"FILE length refused");bytes.resize(size_t(size.QuadPart));DWORD got=0;Need(ReadFile(handle.h,bytes.data(),DWORD(bytes.size()),&got,nullptr)&&got==bytes.size(),"FILE read refused");char extra;Need(ReadFile(handle.h,&extra,1,&got,nullptr)&&got==0,"FILE EOF refused");hash=tq::Sha256(bytes.data(),bytes.size());Need(!pinned||(startup::Hex(expected,64)&&hash==expected),"FILE SHA256 mismatch");
 }
};
struct PE {IMAGE_NT_HEADERS64 nt{};std::vector<IMAGE_SECTION_HEADER> sections;
 explicit PE(const std::vector<unsigned char>& b){Need(b.size()>=sizeof(IMAGE_DOS_HEADER),"DOS header short");IMAGE_DOS_HEADER d;memcpy(&d,b.data(),sizeof d);Need(d.e_magic==IMAGE_DOS_SIGNATURE&&d.e_lfanew>=0&&size_t(d.e_lfanew)+sizeof(nt)<=b.size(),"PE header refused");memcpy(&nt,b.data()+d.e_lfanew,sizeof nt);Need(nt.Signature==IMAGE_NT_SIGNATURE&&nt.FileHeader.Machine==IMAGE_FILE_MACHINE_AMD64&&nt.OptionalHeader.Magic==IMAGE_NT_OPTIONAL_HDR64_MAGIC&&nt.FileHeader.NumberOfSections>0&&nt.FileHeader.NumberOfSections<=96,"AMD64 PE required");size_t at=size_t(d.e_lfanew)+4+sizeof(IMAGE_FILE_HEADER)+nt.FileHeader.SizeOfOptionalHeader;Need(at+nt.FileHeader.NumberOfSections*sizeof(IMAGE_SECTION_HEADER)<=b.size(),"section table short");sections.resize(nt.FileHeader.NumberOfSections);memcpy(sections.data(),b.data()+at,sections.size()*sizeof(IMAGE_SECTION_HEADER));}
 size_t Offset(DWORD rva,size_t n,const std::vector<unsigned char>& b)const{for(auto&s:sections)if(rva>=s.VirtualAddress&&uint64_t(rva)+n<=uint64_t(s.VirtualAddress)+s.SizeOfRawData){size_t at=s.PointerToRawData+rva-s.VirtualAddress;if(at<=b.size()&&n<=b.size()-at)return at;}throw std::runtime_error("PE RVA not FILE backed");}
 DWORD Export(const std::vector<unsigned char>& b,const char* wanted)const{auto d=nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];auto at=Offset(d.VirtualAddress,sizeof(IMAGE_EXPORT_DIRECTORY),b);IMAGE_EXPORT_DIRECTORY e;memcpy(&e,b.data()+at,sizeof e);Need(e.NumberOfNames<=4096&&e.NumberOfFunctions<=4096,"export count refused");for(DWORD i=0;i<e.NumberOfNames;i++){DWORD name;WORD ordinal;memcpy(&name,b.data()+Offset(e.AddressOfNames+i*4,4,b),4);memcpy(&ordinal,b.data()+Offset(e.AddressOfNameOrdinals+i*2,2,b),2);auto n=Offset(name,strlen(wanted)+1,b);if(memcmp(b.data()+n,wanted,strlen(wanted)+1))continue;Need(ordinal<e.NumberOfFunctions,"export ordinal refused");DWORD rva;memcpy(&rva,b.data()+Offset(e.AddressOfFunctions+ordinal*4,4,b),4);Need(!(rva>=d.VirtualAddress&&uint64_t(rva)<uint64_t(d.VirtualAddress)+d.Size),"forwarded custom export refused");Offset(rva,1,b);bool executable=false;for(auto&s:sections)if(rva>=s.VirtualAddress&&uint64_t(rva)<uint64_t(s.VirtualAddress)+s.Misc.VirtualSize&&(s.Characteristics&IMAGE_SCN_MEM_EXECUTE))executable=true;Need(executable,"export is not executable FILE section");return rva;}throw std::runtime_error("required export missing");}
};
struct Run {std::map<std::wstring,std::string> allowed_loads;std::vector<std::shared_ptr<void>> retained_files;template<class T>std::shared_ptr<T> Keep(std::shared_ptr<T> value){retained_files.push_back(value);return value;}std::wstring target,session;std::string nonce;Handle process,primary;DWORD pid=0,tid=0;uint64_t birth=0,parent_birth=0;bool resumed=false,released=false,gate_installed=false;uintptr_t entry=0;void* code=nullptr;void* block=nullptr;DWORD entry_protect=0;unsigned char original[14]{},patch[14]{};std::ofstream log;
 bool remote_unknown=false,commands_enabled=true;uintptr_t status_fn=0,command_fn=0,provider_status_fn=0;uint64_t accepted=1,host_requested=0;unsigned control_count=0;
 tqgr::StatusV3 last_status{};bool have_status=false;startup::ReleaseDisposition release_phase;
 std::wstring host_path,provider_path,bootstrap_path;uintptr_t bootstrap_base=0;DWORD bootstrap_size=0;uintptr_t host_base=0,provider_base=0;DWORD host_size=0,provider_size=0;
 static constexpr DWORD startup_timeout=15000,control_timeout=3000;

 void Log(const std::string& s){log<<s<<"\n";log.flush();Need(bool(log),"session journal write failed");}
 void Identity(){wchar_t p[32768];DWORD n=32768;uint64_t b=0;Need(process.h&&WaitForSingleObject(process.h,0)==WAIT_TIMEOUT&&QueryFullProcessImageNameW(process.h,0,p,&n)&&Canon(std::wstring(p,n))==target&&startup::Birth(process.h,b)&&b==birth&&GetProcessId(process.h)==pid,"owned child path/PID/birth changed");}
 void Read(uintptr_t p,void* b,size_t n){SIZE_T got=0;Need(ReadProcessMemory(process.h,reinterpret_cast<void*>(p),b,n,&got)&&got==n,"exact owned remote read failed");}
 void Write(void* p,const void* b,size_t n){SIZE_T got=0;Need(WriteProcessMemory(process.h,p,b,n,&got)&&got==n,"exact owned remote write failed");}
 void* Buffer(const void* b,size_t n,const char* purpose="retained-argument"){
  Need(n&&n<=128*1024*1024,"remote lease size refused");void* p=VirtualAllocEx(process.h,nullptr,n,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE);Need(p!=nullptr,"remote buffer allocation failed");
  Log("remote_lease address="+std::to_string(reinterpret_cast<uintptr_t>(p))+" bytes="+std::to_string(n)+" purpose="+purpose+" lifetime=natural-child-exit");Write(p,b,n);return p;
 }
 DWORD Call(uintptr_t fn,void* arg,DWORD timeout=startup_timeout){
  Need(!remote_unknown,"remote call forbidden after UNKNOWN completion");Identity();if(bootstrap_base)Need(Module(bootstrap_path,bootstrap_size)==bootstrap_base,"exact bootstrap changed before call");if(provider_base)Need(Module(provider_path,provider_size)==provider_base,"exact provider changed before call");if(host_base)Need(Module(host_path,host_size)==host_base,"exact host changed before call");DWORD thread_id=0;Handle thread(CreateRemoteThread(process.h,nullptr,0,reinterpret_cast<LPTHREAD_START_ROUTINE>(fn),arg,0,&thread_id));Need(thread.h!=nullptr,"remote thread create failed");
  Log("remote_call tid="+std::to_string(thread_id)+" function="+std::to_string(fn)+" argument="+std::to_string(reinterpret_cast<uintptr_t>(arg))+" timeout_ms="+std::to_string(timeout));
  DWORD wait=WaitForSingleObject(thread.h,timeout),result=0;
  if(wait!=WAIT_OBJECT_0||!GetExitCodeThread(thread.h,&result)){remote_unknown=true;throw std::runtime_error("remote call outcome UNKNOWN; all buffers retained; no retry");}
  Log("remote_call_completed tid="+std::to_string(thread_id)+" result="+std::to_string(result));return result;
 }
 uintptr_t Module(const std::wstring& path,DWORD expected_size=0){for(unsigned attempt=0;attempt<80;attempt++){Handle snap(CreateToolhelp32Snapshot(TH32CS_SNAPMODULE|TH32CS_SNAPMODULE32,pid));if(snap.h==INVALID_HANDLE_VALUE){DWORD e=GetLastError();if((e==ERROR_BAD_LENGTH||e==ERROR_PARTIAL_COPY)&&attempt<79){Sleep(10);continue;}throw std::runtime_error("owned child module snapshot refused");}MODULEENTRY32W m{};m.dwSize=sizeof m;Need(Module32FirstW(snap.h,&m),"owned child first module unknown");uintptr_t result=0;do{if(_wcsicmp(m.szModule,fs::path(path).filename().c_str())==0){Need(Canon(m.szExePath)==path,"module basename owner conflict");Need(!result&&(!expected_size||m.modBaseSize==expected_size),"module identity/size ambiguous");result=reinterpret_cast<uintptr_t>(m.modBaseAddr);}m.dwSize=sizeof m;}while(Module32NextW(snap.h,&m));Need(GetLastError()==ERROR_NO_MORE_FILES,"module enumeration incomplete");return result;}throw std::runtime_error("unreachable module snapshot");}
 uintptr_t Loader(){auto local=GetProcAddress(GetModuleHandleW(L"kernel32.dll"),"LoadLibraryW");HMODULE owner=nullptr;Need(local&&GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,reinterpret_cast<LPCWSTR>(local),&owner),"local LoadLibrary owner unknown");wchar_t path[32768];DWORD n=GetModuleFileNameW(owner,path,32768);MODULEINFO info{};Need(n&&n<32768&&GetModuleInformation(GetCurrentProcess(),owner,&info,sizeof info),"local loader owner metadata unknown");uintptr_t offset=reinterpret_cast<uintptr_t>(local)-reinterpret_cast<uintptr_t>(owner);Need(offset<info.SizeOfImage,"local loader offset outside owner");auto canonical=Canon(std::wstring(path,n));uintptr_t remote=Module(canonical,info.SizeOfImage);Need(remote!=0,"remote actual loader-owner correspondence absent");Log("loader_owner="+Narrow(canonical)+" offset="+std::to_string(offset));return remote+offset;}
 uintptr_t Load(File& file){Need(allowed_loads.count(file.path)&&allowed_loads.at(file.path)==file.hash&&fs::path(file.path).parent_path()==fs::path(session),"exact pinned session module allowlist");Identity();PE pe(file.bytes);Need(!Module(file.path),"DLL already loaded before owned load");void* p=Buffer(file.path.c_str(),(file.path.size()+1)*2,"DLL-path");Call(Loader(),p);// Path lease is retained too, including every failure branch.
 uintptr_t base=Module(file.path,pe.nt.OptionalHeader.SizeOfImage);Need(base!=0,"exact loaded DLL owner absent");Log("loaded="+Narrow(file.path)+" sha256="+file.hash);return base;}

};
static void NewFile(const fs::path& p,const std::vector<unsigned char>& bytes){Handle h(CreateFileW(p.c_str(),GENERIC_WRITE,FILE_SHARE_READ,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr));Need(h.h!=INVALID_HANDLE_VALUE,"new D session FILE already exists/refused");DWORD n=0;Need(WriteFile(h.h,bytes.data(),DWORD(bytes.size()),&n,nullptr)&&n==bytes.size()&&FlushFileBuffers(h.h),"new session FILE write failed");}
struct Directory {
 Handle handle;
 explicit Directory(const std::wstring& path,bool /*exact_game_parent*/=false):handle(CreateFileW(path.c_str(),FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE_POINT,nullptr)){
  Need((!path.empty())&&handle.h!=INVALID_HANDLE_VALUE,"physical purpose directory open refused");BY_HANDLE_FILE_INFORMATION info{};Need(GetFileInformationByHandle(handle.h,&info)&&(info.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)&&!(info.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT),"ordinary purpose directory required");
  wchar_t final[32768];DWORD n=GetFinalPathNameByHandleW(handle.h,final,32768,FILE_NAME_NORMALIZED|VOLUME_NAME_DOS);Need(n&&n<32768,"D directory final path unknown");std::wstring resolved(final,n);if(resolved.rfind(L"\\\\?\\",0)==0)resolved.erase(0,4);Need(Canon(resolved)==path,"D directory alias refused");
 }
};
