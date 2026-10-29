#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <tlhelp32.h>
#include <vector>
#include "detours.h"
#include "file_io.h"
#include "background_protocol.h"
#include "census.h"
#include "bindings.h"
#include "guards.h"
using OpenType=HANDLE(WINAPI*)(LPCWSTR,DWORD,DWORD,LPSECURITY_ATTRIBUTES,DWORD,DWORD,HANDLE);
static OpenType original_open=CreateFileW;
static tq::Manifest* routes=nullptr;static bg::Status* status=nullptr;
static const tq::Route* index_route=nullptr;
static LONG initialized=0;static SRWLOCK epoch_lock=SRWLOCK_INIT;
static thread_local bool inside=false;static thread_local wchar_t name_copy[tq::kPathChars],full[tq::kPathChars],key[tq::kPathChars];
struct ErrorPort{uint32_t Get()const noexcept{return GetLastError();}void Set(uint32_t n)const noexcept{SetLastError(n);}};
struct Atom{void Add(int64_t* p,int64_t n)const noexcept{InterlockedAdd64(reinterpret_cast<volatile LONG64*>(p),n);}void Set(int32_t* p,int32_t n)const noexcept{InterlockedExchange(reinterpret_cast<volatile LONG*>(p),n);}};
static void Publish(LONG state,const char* message,LONG error=0)noexcept{
    if(!status)return;size_t n=std::strlen(message);if(n>=sizeof(status->message))n=sizeof(status->message)-1;std::memcpy(status->message,message,n);status->message[n]=0;status->error=error;InterlockedExchange(reinterpret_cast<volatile LONG*>(&status->state),state);
}
static bool Copy(LPCWSTR source)noexcept{if(!source)return false;__try{for(size_t i=0;i<tq::kPathChars;++i){name_copy[i]=source[i];if(!source[i])return true;}}__except(EXCEPTION_EXECUTE_HANDLER){return false;}return false;}
static const tq::Route* Lookup(LPCWSTR source){
    if(!Copy(source)||!tq::Normalize(name_copy,routes->game_root.c_str(),key))return nullptr;
    bool absolute=(name_copy[1]==L':'||(name_copy[0]==L'\\'&&name_copy[1]==L'\\'));
    const wchar_t* path=name_copy;if(!absolute){DWORD n=GetFullPathNameW(name_copy,static_cast<DWORD>(tq::kPathChars),full,nullptr);if(!n||n>=tq::kPathChars)return nullptr;path=full;}
    if(!tq::Normalize(path,nullptr,key))return nullptr;
    for(const auto& row:routes->rows)if(tq::Equal(row.original.c_str(),key))return &row;return nullptr;
}
static HANDLE WINAPI Hook(LPCWSTR filename,DWORD access,DWORD share,LPSECURITY_ATTRIBUTES security,DWORD creation,DWORD flags,HANDLE templ){
    DWORD entry=GetLastError();
    if(inside){
        // A mapped recursive open must not introduce a stock/private mixed epoch.
        if(routes&&Copy(filename)&&tq::Normalize(name_copy,routes->game_root.c_str(),key))for(const auto& r:routes->rows)if(tq::Equal(r.original.c_str(),key)){SetLastError(ERROR_BUSY);return INVALID_HANDLE_VALUE;}
        SetLastError(entry);return original_open(filename,access,share,security,creation,flags,templ);
    }
    inside=true;const tq::Route* row=routes?Lookup(filename):nullptr;bool selected=false,locked=false,first=false;const wchar_t* chosen=nullptr;
    if(row){
        bool eligible=tq::ReadOnlyOpen(access,creation,flags);
        bool writing=(access&(GENERIC_WRITE|GENERIC_ALL|DELETE|WRITE_DAC|WRITE_OWNER|FILE_WRITE_DATA|FILE_APPEND_DATA))!=0||creation!=OPEN_EXISTING||(flags&FILE_FLAG_DELETE_ON_CLOSE)!=0;
        LONG phase=InterlockedCompareExchange(reinterpret_cast<volatile LONG*>(&status->phase),0,0);
        if(phase==bg::pending){AcquireSRWLockExclusive(&epoch_lock);locked=true;phase=status->phase;}
        bool index=row==index_route;
        int32_t next=phase;bg::Action action=bg::Decide(next,index,eligible);
        if(next!=phase){InterlockedExchange(reinterpret_cast<volatile LONG*>(&status->phase),next);Publish(3,"restart_needed: stock asset open observed before private index");}
        if(action!=bg::original){
            if(!eligible&&!writing){if(locked)ReleaseSRWLockExclusive(&epoch_lock);inside=false;SetLastError(ERROR_NOT_SUPPORTED);return INVALID_HANDLE_VALUE;}
            if(eligible){selected=true;chosen=row->replacement.c_str();first=action==bg::first_index;}
        }
    }
    SetLastError(entry);HANDLE result=tq::Forward(filename,chosen,access,share,security,creation,flags,templ,original_open,ErrorPort{});DWORD returned=GetLastError();
    if(first){int32_t phase=status->phase;bg::FinishIndex(phase,result!=INVALID_HANDLE_VALUE);InterlockedExchange(reinterpret_cast<volatile LONG*>(&status->phase),phase);Publish(phase==bg::active?2:3,phase==bg::active?"attached: private index replacement opened; preattach closed reads UNKNOWN":"restart_needed: private index open failed",result==INVALID_HANDLE_VALUE?returned:0);}
    if(locked)ReleaseSRWLockExclusive(&epoch_lock);
    if(status)bg::Count(*status,selected,result!=INVALID_HANDLE_VALUE,returned,Atom{});
    inside=false;SetLastError(returned);return result;
}
static bool CopyConfig(const void* source,bg::Config* dest)noexcept{__try{std::memcpy(dest,source,sizeof(*dest));return true;}__except(EXCEPTION_EXECUTE_HANDLER){return false;}}
struct ThreadPort{
    using Entry=THREADENTRY32;HANDLE snapshot;static constexpr DWORD NoMore=ERROR_NO_MORE_FILES;
    bool First(Entry& e)const{return Thread32First(snapshot,&e)!=FALSE;}bool Next(Entry& e)const{return Thread32Next(snapshot,&e)!=FALSE;}DWORD LastError()const{return GetLastError();}
};
static DWORD Initialize(void* input){
    if(InterlockedCompareExchange(&initialized,1,0)!=0)return ERROR_ALREADY_INITIALIZED;
    auto config=new bg::Config;if(!CopyConfig(input,config))return ERROR_INVALID_DATA;
    if(std::memcmp(config->magic,"TQBGCF01",8)||config->size!=sizeof(*config)||config->pid!=GetCurrentProcessId()||config->hash[64]||std::strlen(config->hash)!=64)return ERROR_INVALID_DATA;
    for(const wchar_t* s:{config->manifest,config->bindings,config->package,config->game})if(wcsnlen_s(s,tq::kPathChars)==tq::kPathChars)return ERROR_INVALID_DATA;
    if(config->bindings_hash[64]||std::strlen(config->bindings_hash)!=64)return ERROR_INVALID_DATA;
    if(wcsnlen_s(config->mapping,96)==96)return ERROR_INVALID_DATA;
    uint64_t birth=0;if(!bg::Birth(GetCurrentProcess(),birth)||birth!=config->birth100ns||!bg::Fresh(birth))return ERROR_TIMEOUT;
    HANDLE mapping=OpenFileMappingW(FILE_MAP_READ|FILE_MAP_WRITE,FALSE,config->mapping);if(!mapping)return GetLastError();
    status=static_cast<bg::Status*>(MapViewOfFile(mapping,FILE_MAP_READ|FILE_MAP_WRITE,0,0,sizeof(bg::Status)));if(!status||!bg::Matches(*status,*config))return ERROR_INVALID_DATA;
    Publish(0,"attaching; preattach closed-read history UNKNOWN");
    std::string hash=config->hash;std::vector<unsigned char> bytes,binding_bytes;DWORD error=0;std::wstring manifest_path,bindings_path,package,game;
    if(!tq::Canonical(config->package,package)||!tq::Canonical(config->game,game)||!tq::UnderPackage(config->manifest,package,manifest_path)||!tq::ReadManifestFile(manifest_path,hash,bytes,error)){Publish(4,"manifest refused",error);return ERROR_INVALID_DATA;}
    auto candidate=new tq::Manifest;std::string why;if(!tq::Parse(bytes.data(),bytes.size(),game,package,*candidate,why)){Publish(4,"manifest parser refused");return ERROR_INVALID_DATA;}
    std::vector<bg::Binding> bindings;
    if(!tq::UnderPackage(config->bindings,package,bindings_path)||!tq::ReadManifestFile(bindings_path,config->bindings_hash,binding_bytes,error)||!bg::ParseBindings(binding_bytes.data(),binding_bytes.size(),*candidate,bindings)){Publish(3,"refresh_required: BINDINGS refused",error);return ERROR_INVALID_DATA;}
    wchar_t executable[tq::kPathChars];DWORD n=GetModuleFileNameW(nullptr,executable,static_cast<DWORD>(tq::kPathChars));if(!n||n>=tq::kPathChars||!tq::Equal(executable,(game+L"\\EverQuest2.exe").c_str())){Publish(3,"restart_needed: exact client identity refused");return ERROR_INVALID_DATA;}
    HANDLE anchor=original_open(manifest_path.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr);if(anchor==INVALID_HANDLE_VALUE)return GetLastError();
    // Our own known pagefile status view is not a FILE-backed stock asset; close
    // its view during census, then reopen it before any transaction.
    if(!UnmapViewOfFile(status))return GetLastError();status=nullptr;
    bool clear=bg::Census(GetCurrentProcess(),GetCurrentProcessId(),anchor,*candidate,why);
    status=static_cast<bg::Status*>(MapViewOfFile(mapping,FILE_MAP_READ|FILE_MAP_WRITE,0,0,sizeof(bg::Status)));if(!status)return GetLastError();
    if(!clear||!bg::Fresh(birth)){Publish(3,clear?"restart_needed: process aged beyond2s":why.c_str());return ERROR_BUSY;}
    auto leases=new std::vector<HANDLE>;leases->reserve(2+bindings.size());leases->push_back(anchor);
    HANDLE binding_lease=original_open(bindings_path.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr);if(binding_lease==INVALID_HANDLE_VALUE)return GetLastError();leases->push_back(binding_lease);
    for(const auto& binding:bindings){
        HANDLE h=original_open(binding.path.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);if(h==INVALID_HANDLE_VALUE){Publish(3,"refresh_required: readonly source/original lease failed",GetLastError());return ERROR_SHARING_VIOLATION;}leases->push_back(h);
        BY_HANDLE_FILE_INFORMATION info{};if(GetFileType(h)!=FILE_TYPE_DISK||!GetFileInformationByHandle(h,&info)||(info.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)){Publish(3,"restart_needed: lease is not ordinary disk FILE");return ERROR_INVALID_DATA;}
        uint64_t bytes=(static_cast<uint64_t>(info.nFileSizeHigh)<<32)|info.nFileSizeLow,stamp=bg::Number(info.ftLastWriteTime);
        if(!bg::FreshBinding(bytes,stamp,binding)){Publish(3,"refresh_required: prepared asset size/mtime changed");return ERROR_FILE_INVALID;}
    }
    std::vector<HANDLE> threads;threads.reserve(256);HANDLE snap=CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD,0);if(snap==INVALID_HANDLE_VALUE)return GetLastError();
    bool complete=bg::ThreadRows(ThreadPort{snap},[&](const THREADENTRY32& item){
        if(item.th32OwnerProcessID==GetCurrentProcessId()&&item.th32ThreadID!=GetCurrentThreadId()){
            if(threads.size()>=256)return false;
            HANDLE h=OpenThread(THREAD_SUSPEND_RESUME|THREAD_GET_CONTEXT|THREAD_SET_CONTEXT|THREAD_QUERY_INFORMATION,FALSE,item.th32ThreadID);if(!h)return false;threads.push_back(h);if(GetProcessIdOfThread(h)!=GetCurrentProcessId())return false;
        }return true;
    });
    bool snapshot_closed=CloseHandle(snap)!=FALSE;
    if(!complete||!snapshot_closed||!bg::Fresh(birth)){Publish(3,"restart_needed: thread row coverage/identity/census/age refused");return ERROR_BUSY;}
    HMODULE pin=nullptr;if(!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_PIN,reinterpret_cast<LPCWSTR>(&Initialize),&pin))return GetLastError();
    routes=candidate;for(const auto& row:routes->rows)if(tq::Equal(row.original.c_str(),(game+L"\\assetslib.vpl").c_str()))index_route=&row;
    if(!index_route){Publish(3,"refresh_required: full index route missing");return ERROR_INVALID_DATA;}
    status->routes=static_cast<uint32_t>(routes->rows.size());DetourSetRetainRegions(TRUE);
    LONG result=DetourTransactionBegin();if(result!=NO_ERROR){Publish(4,"Detours Begin refused",result);return result;}
    try{for(HANDLE h:threads){result=DetourUpdateThread(h);if(result!=NO_ERROR)break;}if(result==NO_ERROR)result=DetourAttach(reinterpret_cast<PVOID*>(&original_open),reinterpret_cast<PVOID>(Hook));LONG committed=DetourTransactionCommit();if(result==NO_ERROR)result=committed;}
    catch(...){LONG aborted=DetourTransactionAbort();Publish(4,aborted==NO_ERROR?"Detours exception; standard Abort returned0":"Detours exception; cleanup UNKNOWN",ERROR_UNHANDLED_EXCEPTION);return ERROR_UNHANDLED_EXCEPTION;}
    for(HANDLE h:threads)if(!CloseHandle(h)){Publish(4,"thread HANDLE close unknown",GetLastError());return ERROR_INVALID_HANDLE;}
    if(result!=NO_ERROR){Publish(4,"standard Detours transaction refused; native cleanup not independently proved",result);return result;}
    if(InterlockedCompareExchange(reinterpret_cast<volatile LONG*>(&status->phase),0,0)==bg::pending)Publish(1,"awaiting_index: hook installed; closed-read prehistory UNKNOWN");return 0;
}
extern "C" __declspec(dllexport) DWORD WINAPI TextureQuestInitialize(void* config){try{return Initialize(config);}catch(...){Publish(4,"initializer exception",ERROR_UNHANDLED_EXCEPTION);return ERROR_UNHANDLED_EXCEPTION;}}
BOOL WINAPI DllMain(HINSTANCE,DWORD,LPVOID){return TRUE;}
