#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <tlhelp32.h>
#include <psapi.h>
#include <bcrypt.h>
#include <shellapi.h>
#include <set>
#include <sstream>
#include <memory>
#include "file_io.h"
#include "background_protocol.h"
#include "bindings.h"
#include "census.h"
#include "guards.h"
#include "process_detector.h"
#pragma comment(lib,"bcrypt.lib")
#pragma comment(lib,"psapi.lib")
#pragma comment(lib,"shell32.lib")
using Event=bg::ProcessEvent;
struct ProcessSnapshotPort {
    using Entry=PROCESSENTRY32W;
    static constexpr DWORD NoMore=ERROR_NO_MORE_FILES;
    HANDLE snapshot=INVALID_HANDLE_VALUE;DWORD error=ERROR_SUCCESS;
    bool Open(){snapshot=CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS,0);if(snapshot==INVALID_HANDLE_VALUE){error=GetLastError();return false;}return true;}
    bool First(Entry& row){SetLastError(ERROR_SUCCESS);if(Process32FirstW(snapshot,&row))return true;error=GetLastError();return false;}
    bool Next(Entry& row){SetLastError(ERROR_SUCCESS);if(Process32NextW(snapshot,&row))return true;error=GetLastError();return false;}
    DWORD LastError()const{return error;}
    bool Close(){HANDLE held=snapshot;snapshot=INVALID_HANDLE_VALUE;if(!CloseHandle(held)){error=GetLastError();return false;}return true;}
};
static bool SnapshotEvents(std::vector<Event>& events,bg::ScanFailure& failure){ProcessSnapshotPort port;return bg::ProcessRows(port,events,failure);}
static std::string DetectorMessage(const bg::ScanFailure& failure){return std::string("process snapshot refused at ")+failure.stage+"; Win32="+std::to_string(failure.error)+"; close Win32="+std::to_string(failure.close_error)+"; no partial candidates attached";}
struct Session{DWORD pid=0;uint64_t birth=0;std::string state,message;HANDLE process=nullptr,mapping=nullptr;bg::Status* view=nullptr;void* remote_config=nullptr;void* remote_path=nullptr;HANDLE outstanding=nullptr;};
struct Settings{std::wstring game,exe,package,manifest,bindings,status_dir,dll,stop;std::string hash,binding_hash;};
struct Persistence{std::string nonce,owner_hash,run_id;std::wstring run_dir;DWORD error=0;std::string stage;};static Persistence persistence;
static void FirstIO(const char* stage,DWORD error){if(!persistence.error){persistence.error=error?error:ERROR_WRITE_FAULT;persistence.stage=stage;}}
static bool NewFile(const std::wstring& path,const std::string& data){
    HANDLE h=CreateFileW(path.c_str(),GENERIC_WRITE,FILE_SHARE_READ,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);if(h==INVALID_HANDLE_VALUE){FirstIO("CreateNew",GetLastError());return false;}
    DWORD written=0;bool okay=WriteFile(h,data.data(),static_cast<DWORD>(data.size()),&written,nullptr)!=FALSE;if(!okay)FirstIO("WriteFile",GetLastError());else if(written!=data.size()){okay=false;FirstIO("WriteLength",ERROR_WRITE_FAULT);}
    if(okay&&!FlushFileBuffers(h)){FirstIO("FlushFileBuffers",GetLastError());okay=false;}if(!CloseHandle(h)){FirstIO("CloseHandle",GetLastError());okay=false;}return okay;
}
static void SaveIO(){if(persistence.error&&!persistence.run_dir.empty()){std::string data="{\"schema\":\"tq-background-status-io-error-v1\",\"stage\":\""+persistence.stage+"\",\"win32_error\":"+std::to_string(persistence.error)+"}\n";NewFile(persistence.run_dir+L"\\status-error.json",data);}}
static bool ReadSmall(const std::wstring& path,size_t cap,std::string& data,HANDLE* retained=nullptr){
    HANDLE h=CreateFileW(path.c_str(),GENERIC_READ,retained?FILE_SHARE_READ:(FILE_SHARE_READ|FILE_SHARE_DELETE),nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);if(h==INVALID_HANDLE_VALUE)return false;
    LARGE_INTEGER size{};bool okay=GetFileSizeEx(h,&size)&&size.QuadPart>=0&&static_cast<uint64_t>(size.QuadPart)<=cap;
    if(okay){data.resize(static_cast<size_t>(size.QuadPart));DWORD n=0;okay=ReadFile(h,data.data(),static_cast<DWORD>(data.size()),&n,nullptr)&&n==data.size();if(okay){char extra;okay=ReadFile(h,&extra,1,&n,nullptr)&&n==0;}}
    if(retained&&okay)*retained=h;else if(!CloseHandle(h))okay=false;return okay;
}
static std::string RandomHex(){unsigned char bytes[16];if(BCryptGenRandom(nullptr,bytes,sizeof(bytes),BCRYPT_USE_SYSTEM_PREFERRED_RNG)!=0)return{};std::string result;for(unsigned char b:bytes){result+="0123456789abcdef"[b>>4];result+="0123456789abcdef"[b&15];}return result;}
static bool StartPersistence(const Settings& cfg,HANDLE& stop){
    persistence.nonce=std::string(cfg.stop.begin()+14,cfg.stop.end());auto expected=bg::Owner(cfg.game,persistence.nonce,cfg.hash,cfg.binding_hash);if(expected.empty())return false;persistence.owner_hash=tq::Sha256(expected.data(),expected.size());
    std::string actual;HANDLE owner=nullptr;if(!ReadSmall(cfg.status_dir+L"\\OWNER.bin",1024*1024,actual,&owner)||actual.size()!=expected.size()||std::memcmp(actual.data(),expected.data(),expected.size()))return false;
    // The exact OWNER handle is intentionally retained through watcher exit.
    SetLastError(ERROR_SUCCESS);stop=CreateEventW(nullptr,TRUE,FALSE,cfg.stop.c_str());DWORD event_error=GetLastError();if(!stop)return false;if(event_error==ERROR_ALREADY_EXISTS){CloseHandle(stop);stop=nullptr;return false;}
    DWORD legacy=GetFileAttributesW((cfg.status_dir+L"\\status.pending").c_str());DWORD legacy_error=GetLastError();bool legacy_pending=legacy!=INVALID_FILE_ATTRIBUTES||legacy_error!=ERROR_FILE_NOT_FOUND;
    std::wstring latest=cfg.status_dir+L"\\status.json";DWORD attributes=GetFileAttributesW(latest.c_str());DWORD latest_error=GetLastError();bool prior_present=attributes!=INVALID_FILE_ATTRIBUTES;
    if(!prior_present&&latest_error!=ERROR_FILE_NOT_FOUND)return false;std::string prior;bool prior_owned=!prior_present||(ReadSmall(latest,1024*1024,prior)&&bg::OwnedLatest(prior,persistence.nonce,persistence.owner_hash));
    if(!bg::StartAllowed(true,true,legacy_pending,prior_present,prior_owned))return false;
    std::wstring history=cfg.status_dir+L"\\history";if(!CreateDirectoryW(history.c_str(),nullptr)){DWORD error=GetLastError();DWORD attrs=GetFileAttributesW(history.c_str());if(error!=ERROR_ALREADY_EXISTS||attrs==INVALID_FILE_ATTRIBUTES||!(attrs&FILE_ATTRIBUTE_DIRECTORY))return false;}
    persistence.run_id=RandomHex();if(!bg::Hex(persistence.run_id,32))return false;persistence.run_dir=history+L"\\run-"+std::wstring(persistence.run_id.begin(),persistence.run_id.end());if(!CreateDirectoryW(persistence.run_dir.c_str(),nullptr))return false;
    if(prior_present&&!NewFile(persistence.run_dir+L"\\previous-status.json",prior)){SaveIO();return false;}return true;
}
static bool Image(HANDLE process,std::wstring& path){wchar_t text[tq::kPathChars];DWORD n=static_cast<DWORD>(tq::kPathChars);if(!QueryFullProcessImageNameW(process,0,text,&n))return false;return tq::Canonical(std::wstring(text,n),path);}
static bool Amd64(const std::wstring& path){HANDLE h=CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr);if(h==INVALID_HANDLE_VALUE)return false;IMAGE_DOS_HEADER d{};DWORD n=0;bool okay=ReadFile(h,&d,sizeof(d),&n,nullptr)&&n==sizeof(d)&&d.e_magic==IMAGE_DOS_SIGNATURE&&d.e_lfanew>=sizeof(d)&&d.e_lfanew<1024*1024;LARGE_INTEGER p{};p.QuadPart=d.e_lfanew;DWORD sig=0;IMAGE_FILE_HEADER f{};if(okay)okay=SetFilePointerEx(h,p,nullptr,FILE_BEGIN)&&ReadFile(h,&sig,4,&n,nullptr)&&n==4&&sig==IMAGE_NT_SIGNATURE&&ReadFile(h,&f,sizeof(f),&n,nullptr)&&n==sizeof(f)&&f.Machine==IMAGE_FILE_MACHINE_AMD64;if(!CloseHandle(h))okay=false;return okay;}
static uintptr_t RemoteModule(DWORD pid,const std::wstring& path,DWORD size=0){HANDLE snapshot=CreateToolhelp32Snapshot(TH32CS_SNAPMODULE|TH32CS_SNAPMODULE32,pid);if(snapshot==INVALID_HANDLE_VALUE)return 0;MODULEENTRY32W m{};m.dwSize=sizeof(m);uintptr_t found=0;if(Module32FirstW(snapshot,&m))do{std::wstring canonical;if(tq::Canonical(m.szExePath,canonical)&&tq::Equal(canonical.c_str(),path.c_str())&&(!size||m.modBaseSize==size)){if(found){found=0;break;}found=reinterpret_cast<uintptr_t>(m.modBaseAddr);}}while(Module32NextW(snapshot,&m));if(!CloseHandle(snapshot))return 0;return found;}
static uintptr_t RemoteLoader(DWORD pid){
    FARPROC local=GetProcAddress(GetModuleHandleW(L"kernel32.dll"),"LoadLibraryW");HMODULE owner=nullptr;if(!local||!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,reinterpret_cast<LPCWSTR>(local),&owner))return 0;
    MODULEINFO info{};wchar_t file[tq::kPathChars];DWORD n=GetModuleFileNameW(owner,file,static_cast<DWORD>(tq::kPathChars));std::wstring path;if(!n||n>=tq::kPathChars||!GetModuleInformation(GetCurrentProcess(),owner,&info,sizeof(info))||!tq::Canonical(file,path))return 0;
    uintptr_t offset=reinterpret_cast<uintptr_t>(local)-reinterpret_cast<uintptr_t>(owner);if(offset>=info.SizeOfImage)return 0;uintptr_t remote=RemoteModule(pid,path,info.SizeOfImage);return remote?remote+offset:0;
}
static DWORD ExportRva(const std::vector<unsigned char>& bytes,const char* wanted){
    auto fits=[&](size_t at,size_t n){return at<=bytes.size()&&n<=bytes.size()-at;};
    if(!fits(0,sizeof(IMAGE_DOS_HEADER)))return 0;IMAGE_DOS_HEADER d;std::memcpy(&d,bytes.data(),sizeof(d));if(d.e_magic!=IMAGE_DOS_SIGNATURE||d.e_lfanew<0||!fits(d.e_lfanew,sizeof(IMAGE_NT_HEADERS64)))return 0;
    IMAGE_NT_HEADERS64 nt;std::memcpy(&nt,bytes.data()+d.e_lfanew,sizeof(nt));if(nt.Signature!=IMAGE_NT_SIGNATURE||nt.FileHeader.Machine!=IMAGE_FILE_MACHINE_AMD64||nt.OptionalHeader.Magic!=IMAGE_NT_OPTIONAL_HDR64_MAGIC||nt.FileHeader.NumberOfSections>96)return 0;
    size_t sections=d.e_lfanew+4+sizeof(IMAGE_FILE_HEADER)+nt.FileHeader.SizeOfOptionalHeader;
    auto offset=[&](DWORD rva,size_t n)->size_t{if(rva<nt.OptionalHeader.SizeOfHeaders&&fits(rva,n))return rva;for(unsigned i=0;i<nt.FileHeader.NumberOfSections;++i){if(!fits(sections+i*sizeof(IMAGE_SECTION_HEADER),sizeof(IMAGE_SECTION_HEADER)))return SIZE_MAX;IMAGE_SECTION_HEADER s;std::memcpy(&s,bytes.data()+sections+i*sizeof(s),sizeof(s));if(rva>=s.VirtualAddress&&static_cast<uint64_t>(rva)+n<=static_cast<uint64_t>(s.VirtualAddress)+s.SizeOfRawData&&fits(s.PointerToRawData+rva-s.VirtualAddress,n))return s.PointerToRawData+rva-s.VirtualAddress;}return SIZE_MAX;};
    auto dir=nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];size_t e=offset(dir.VirtualAddress,sizeof(IMAGE_EXPORT_DIRECTORY));if(e==SIZE_MAX)return 0;IMAGE_EXPORT_DIRECTORY exp;std::memcpy(&exp,bytes.data()+e,sizeof(exp));if(exp.NumberOfNames>4096||exp.NumberOfFunctions>4096)return 0;
    for(DWORD i=0;i<exp.NumberOfNames;++i){size_t a=offset(exp.AddressOfNames+i*4,4),b=offset(exp.AddressOfNameOrdinals+i*2,2);if(a==SIZE_MAX||b==SIZE_MAX)return 0;DWORD name;WORD ordinal;std::memcpy(&name,bytes.data()+a,4);std::memcpy(&ordinal,bytes.data()+b,2);size_t c=offset(name,std::strlen(wanted)+1);if(c!=SIZE_MAX&&!std::memcmp(bytes.data()+c,wanted,std::strlen(wanted)+1)){if(ordinal>=exp.NumberOfFunctions)return 0;size_t f=offset(exp.AddressOfFunctions+ordinal*4,4);if(f==SIZE_MAX)return 0;DWORD rva;std::memcpy(&rva,bytes.data()+f,4);if(rva>=dir.VirtualAddress&&rva<static_cast<uint64_t>(dir.VirtualAddress)+dir.Size)return 0;return offset(rva,1)==SIZE_MAX?0:rva;}}
    return 0;
}
static std::string Escape(const std::string& s){std::string r;for(unsigned char c:s){if(c=='"'||c=='\\'){r+='\\';r+=c;}else if(c>=32&&c<127)r+=c;else r+=' ';}return r;}
struct StatusPublisher{
    const std::wstring& pending;const std::wstring& final;
    bg::LatestKind Lookup(){DWORD attrs=GetFileAttributesW(final.c_str());if(attrs!=INVALID_FILE_ATTRIBUTES){if(!(attrs&FILE_ATTRIBUTE_DIRECTORY))return bg::latest_file;FirstIO("StatusDestinationType",ERROR_ACCESS_DENIED);return bg::latest_unknown;}DWORD error=GetLastError();if(error==ERROR_FILE_NOT_FOUND)return bg::latest_absent;FirstIO("StatusDestinationQuery",error);return bg::latest_unknown;}
    bool Replace(){if(ReplaceFileW(final.c_str(),pending.c_str(),nullptr,0,nullptr,nullptr))return true;FirstIO("ReplaceFileW",GetLastError());return false;}
    bool MoveNew(){if(MoveFileExW(pending.c_str(),final.c_str(),MOVEFILE_WRITE_THROUGH))return true;FirstIO("MoveFileExWNew",GetLastError());return false;}
};
static bool WriteStatus(const Settings& cfg,uint64_t self_birth,const std::string& state,const std::string& message,const std::vector<Session>& sessions){
    std::ostringstream out;out<<bg::StatusPrefix(persistence.nonce,persistence.owner_hash)<<persistence.run_id<<"\",\"watcher_pid\":"<<GetCurrentProcessId()<<",\"watcher_birth100ns\":"<<self_birth<<",\"state\":\""<<state<<"\",\"message\":\""<<Escape(message)<<"\",\"pre_attach_closed_read_history\":\"UNKNOWN\",\"sessions\":[";
    bool first=true;for(const auto& s:sessions){if(!first)out<<',';first=false;bool observation=s.birth==0;auto view=observation?nullptr:s.view;int64_t redirects=view?InterlockedCompareExchange64(reinterpret_cast<volatile LONG64*>(&view->succeeded),0,0):0;LONG phase=view?InterlockedCompareExchange(reinterpret_cast<volatile LONG*>(&view->phase),0,0):0;auto evidence=bg::Evidence(s.birth,redirects,phase==bg::active);out<<"{\"pid\":"<<s.pid<<",\"birth100ns\":"<<s.birth<<",\"state\":\""<<s.state<<"\",\"observation_only\":"<<(evidence.observation_only?"true":"false")<<",\"redirects\":"<<evidence.redirects<<",\"private_index_active\":"<<(evidence.private_active?"true":"false")<<",\"message\":\""<<Escape(s.message)<<"\",\"pre_attach_closed_read_history\":\"UNKNOWN\"}";}out<<"]}\n";
    std::string data=out.str();if(data.size()>1024*1024){FirstIO("StatusSize",ERROR_BUFFER_OVERFLOW);return false;}std::wstring pending=persistence.run_dir+L"\\status.pending",final=cfg.status_dir+L"\\status.json";if(!NewFile(pending,data))return false;StatusPublisher publisher{pending,final};return bg::PublishStatus(publisher);
}
static bool RemoteWrite(HANDLE p,const void* bytes,size_t size,void*& remote){remote=VirtualAllocEx(p,nullptr,size,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE);if(!remote)return false;SIZE_T written=0;return WriteProcessMemory(p,remote,bytes,size,&written)&&written==size;}
static bool CallRemote(Session& s,uintptr_t entry,void* argument,DWORD& result){
    s.outstanding=CreateRemoteThread(s.process,nullptr,0,reinterpret_cast<LPTHREAD_START_ROUTINE>(entry),argument,0,nullptr);if(!s.outstanding)return false;
    if(WaitForSingleObject(s.outstanding,10000)!=WAIT_OBJECT_0)return false;
    if(!GetExitCodeThread(s.outstanding,&result)||!CloseHandle(s.outstanding))return false;s.outstanding=nullptr;return true;
}
static void Attach(Session& s,const Settings& cfg,const tq::Manifest& manifest,HANDLE anchor,DWORD init_rva){
    if(!bg::CanAttachBirth(s.birth)){s.state="restart_needed";s.message="observation-only unknown birth; never attach";return;}
    s.state="attaching";s.message="fresh main process; preattach closed reads UNKNOWN";
    s.process=OpenProcess(PROCESS_QUERY_INFORMATION|PROCESS_VM_READ|PROCESS_VM_WRITE|PROCESS_VM_OPERATION|PROCESS_CREATE_THREAD|PROCESS_DUP_HANDLE|SYNCHRONIZE,FALSE,s.pid);
    std::wstring image;uint64_t birth=0;if(!s.process||!Image(s.process,image)||!tq::Equal(image.c_str(),cfg.exe.c_str())||!bg::Birth(s.process,birth)||birth!=s.birth||!bg::Fresh(birth)){s.state="restart_needed";s.message="exact identity/age/access refused";return;}
    std::string why;if(!bg::Census(s.process,s.pid,anchor,manifest,why)||!bg::Fresh(birth)){s.state="restart_needed";s.message=why;return;}
    uintptr_t loader=RemoteLoader(s.pid);if(!loader){s.state="restart_needed";s.message="remote LoadLibrary owner correspondence unavailable";return;}
    unsigned char random[16];if(BCryptGenRandom(nullptr,random,sizeof(random),BCRYPT_USE_SYSTEM_PREFERRED_RNG)!=0){s.state="error";s.message="nonce unavailable";return;}
    const wchar_t hex[]=L"0123456789abcdef";std::wstring name=L"Local\\TQBG-";for(unsigned char c:random){name+=hex[c>>4];name+=hex[c&15];}
    s.mapping=CreateFileMappingW(INVALID_HANDLE_VALUE,nullptr,PAGE_READWRITE,0,sizeof(bg::Status),name.c_str());if(!s.mapping||GetLastError()==ERROR_ALREADY_EXISTS){s.state="error";s.message="fresh mapping refused";return;}
    s.view=static_cast<bg::Status*>(MapViewOfFile(s.mapping,FILE_MAP_READ|FILE_MAP_WRITE,0,0,sizeof(bg::Status)));if(!s.view){s.state="error";s.message="status mapping refused";return;}bg::Prepare(*s.view,s.pid,s.birth,cfg.hash);
    if(!RemoteWrite(s.process,cfg.dll.c_str(),(cfg.dll.size()+1)*sizeof(wchar_t),s.remote_path)){s.state="error";s.message="DLL path copy failed; no retry";return;}
    DWORD loaded=0;if(!CallRemote(s,loader,s.remote_path,loaded)){s.state="error";s.message="LoadLibrary thread outcome UNKNOWN; remote memory retained, no retry";return;}
    uintptr_t module=RemoteModule(s.pid,cfg.dll);if(!module){s.state="error";s.message="exact loaded DLL module not found; no retry";return;}
    auto config=std::make_unique<bg::Config>();std::memset(config.get(),0,sizeof(*config));std::memcpy(config->magic,"TQBGCF01",8);config->size=sizeof(*config);config->pid=s.pid;config->birth100ns=s.birth;
    wcscpy_s(config->manifest,cfg.manifest.c_str());wcscpy_s(config->bindings,cfg.bindings.c_str());wcscpy_s(config->package,cfg.package.c_str());wcscpy_s(config->game,cfg.game.c_str());wcscpy_s(config->mapping,name.c_str());std::memcpy(config->hash,cfg.hash.c_str(),65);std::memcpy(config->bindings_hash,cfg.binding_hash.c_str(),65);
    if(!RemoteWrite(s.process,config.get(),sizeof(*config),s.remote_config)){s.state="error";s.message="configuration copy failed; no retry";return;}
    DWORD result=0;if(!CallRemote(s,module+init_rva,s.remote_config,result)){s.state="error";s.message="initializer outcome UNKNOWN; remote buffers retained, no retry";return;}
    if(result!=0){s.state="restart_needed";s.message=s.view->message[0]?s.view->message:"initializer refused; no retry";return;}
    s.state="awaiting_index";s.message="installed; first private index still required; closed-read history UNKNOWN";
}
static int Run(int argc,wchar_t** argv){
    Settings cfg;std::wstring hash,binding_hash;
    for(int i=1;i<argc;i+=2){if(i+1>=argc)return 2;std::wstring key=argv[i];auto p=key==L"--game-dir"?&cfg.game:key==L"--manifest"?&cfg.manifest:key==L"--manifest-sha256"?&hash:key==L"--bindings"?&cfg.bindings:key==L"--bindings-sha256"?&binding_hash:key==L"--status-dir"?&cfg.status_dir:key==L"--stop-event"?&cfg.stop:nullptr;if(!p||!p->empty())return 2;*p=argv[i+1];}
    if(cfg.game.empty()||cfg.manifest.empty()||cfg.bindings.empty()||cfg.status_dir.empty()||cfg.stop.size()!=46||cfg.stop.find(L"Local\\TQWatch-")!=0||!tq::HashText(hash,cfg.hash)||!tq::HashText(binding_hash,cfg.binding_hash))return 2;
    for(size_t i=14;i<cfg.stop.size();++i)if(!((cfg.stop[i]>=L'0'&&cfg.stop[i]<=L'9')||(cfg.stop[i]>=L'a'&&cfg.stop[i]<=L'f')))return 2;
    std::wstring canonical;if(!tq::Canonical(cfg.game,canonical))return 2;cfg.game=canonical;cfg.exe=cfg.game+L"\\EverQuest2.exe";
    wchar_t self[tq::kPathChars];DWORD n=GetModuleFileNameW(nullptr,self,static_cast<DWORD>(tq::kPathChars));if(!n||n>=tq::kPathChars)return 2;std::wstring executable(self,n),data_root;if(!tq::Canonical(executable.substr(0,executable.find_last_of(L"\\/")),cfg.package)||!tq::ComponentDataRoot(cfg.package,data_root)||!tq::DisjointTrees(cfg.game,data_root))return 2;cfg.dll=cfg.package+L"\\TextureQuestBackground.dll";
    if(!tq::UnderPackage(cfg.manifest,cfg.package,canonical))return 2;cfg.manifest=canonical;if(!tq::UnderPackage(cfg.bindings,cfg.package,canonical))return 2;cfg.bindings=canonical;if(!tq::UnderPackage(cfg.status_dir,cfg.package,canonical))return 2;cfg.status_dir=canonical;
    DWORD attrs=GetFileAttributesW(cfg.status_dir.c_str());if(attrs==INVALID_FILE_ATTRIBUTES||!(attrs&FILE_ATTRIBUTE_DIRECTORY))return 2;
    std::vector<unsigned char> bytes,bound,dll_bytes;DWORD error=0;tq::Manifest manifest;std::string why;std::vector<bg::Binding> bindings;
    if(!tq::ReadManifestFile(cfg.manifest,cfg.hash,bytes,error)||!tq::Parse(bytes.data(),bytes.size(),cfg.game,cfg.package,manifest,why)||!tq::ReadManifestFile(cfg.bindings,cfg.binding_hash,bound,error)||!bg::ParseBindings(bound.data(),bound.size(),manifest,bindings)||!Amd64(cfg.exe))return 3;
    HANDLE dll=CreateFileW(cfg.dll.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr);if(dll==INVALID_HANDLE_VALUE)return 3;LARGE_INTEGER size{};if(!GetFileSizeEx(dll,&size)||size.QuadPart<=0||size.QuadPart>4*1024*1024)return 3;dll_bytes.resize(static_cast<size_t>(size.QuadPart));DWORD got=0;if(!ReadFile(dll,dll_bytes.data(),static_cast<DWORD>(dll_bytes.size()),&got,nullptr)||got!=dll_bytes.size())return 3;DWORD init_rva=ExportRva(dll_bytes,"TextureQuestInitialize");if(!init_rva)return 3;
    HANDLE anchor=CreateFileW(cfg.manifest.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr);if(anchor==INVALID_HANDLE_VALUE)return 3;
    uint64_t self_birth=0;if(!bg::Birth(GetCurrentProcess(),self_birth))return 3;HANDLE stop=nullptr;if(!StartPersistence(cfg,stop))return 3;
    std::set<std::pair<DWORD,uint64_t>> ignored,seen;std::set<DWORD> unknown_baseline;
    std::vector<Event> baseline;bg::ScanFailure detector_failure;
    if(!SnapshotEvents(baseline,detector_failure)){std::vector<Session> none;WriteStatus(cfg,self_birth,"error",DetectorMessage(detector_failure),none);SaveIO();return 5;}
    for(const auto& process:baseline){HANDLE h=OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,FALSE,process.pid);std::wstring image;uint64_t birth=0;if(h&&Image(h,image)&&bg::Birth(h,birth)){if(tq::Equal(image.c_str(),cfg.exe.c_str()))ignored.insert({process.pid,birth});}else unknown_baseline.insert(process.pid);if(h&&!CloseHandle(h)){bg::ScanFailure failed{"BaselineCloseHandle",GetLastError()};std::vector<Session> none;WriteStatus(cfg,self_birth,"error",DetectorMessage(failed),none);SaveIO();return 5;}}
    std::vector<Session> sessions;std::string last;if(!WriteStatus(cfg,self_birth,"armed","baseline ignored; 100ms readonly process snapshots; future main clients only; preattach closed-read history UNKNOWN",sessions)){SaveIO();return 5;}
    bool stopping=false,io_failure=false;std::string detector_message;
    while(!stopping){DWORD wait=WaitForSingleObject(stop,100);if(wait==WAIT_OBJECT_0){stopping=true;break;}if(wait!=WAIT_TIMEOUT){bg::ScanFailure failed{"StopWait",GetLastError()};detector_message=DetectorMessage(failed);break;}
        std::vector<Event> events;if(!SnapshotEvents(events,detector_failure)){detector_message=DetectorMessage(detector_failure);break;}
        for(auto event:events){if(unknown_baseline.count(event.pid)||seen.count({event.pid,0}))continue;HANDLE h=OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,FALSE,event.pid);std::wstring image;uint64_t birth=0;bool known=h&&Image(h,image)&&bg::Birth(h,birth);if(h&&!CloseHandle(h))known=false;auto action=bg::ClassifyEvent(true,known,known&&tq::Equal(image.c_str(),cfg.exe.c_str()));
            if(action==bg::uncertain_event){if(!seen.count({event.pid,0})){seen.insert({event.pid,0});Session uncertain;uncertain.pid=event.pid;uncertain.state="restart_needed";uncertain.message="matching EXE name but exact path/birth/access unknown; no attach";sessions.push_back(std::move(uncertain));}continue;}
            if(action==bg::ignore_event||ignored.count({event.pid,birth})||seen.count({event.pid,birth}))continue;seen.insert({event.pid,birth});
            bool parent_known=false,parent_game=false;uint64_t parent_birth=0;HANDLE parent=OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,FALSE,event.parent);if(parent){std::wstring parent_image;parent_known=Image(parent,parent_image)&&bg::Birth(parent,parent_birth);parent_game=parent_known&&tq::Equal(parent_image.c_str(),cfg.exe.c_str());if(!CloseHandle(parent))parent_known=false;}
            auto role=bg::Parent(parent_known,parent_game,parent_birth,birth);if(role==bg::game_child)continue;
            Session s;s.pid=event.pid;s.birth=birth;if(role==bg::unknown_parent){s.state="restart_needed";s.message="parent identity unknown/reused; main role not proved";}else if(!bg::Fresh(birth)){s.state="restart_needed";s.message="process snapshot candidate aged beyond2s";}else{s.state="attaching";s.message="known non-game parent; fresh exact main candidate";}
            sessions.push_back(std::move(s));if(sessions.back().state=="attaching"){if(!WriteStatus(cfg,self_birth,"attaching",sessions.back().message,sessions)){io_failure=true;break;}Attach(sessions.back(),cfg,manifest,anchor,init_rva);}
        }
        if(io_failure)break;
        std::ostringstream signature;std::string overall="armed",message="watching future clients";
        for(auto& s:sessions){if(s.view){LONG state=InterlockedCompareExchange(reinterpret_cast<volatile LONG*>(&s.view->state),0,0);if(state==2)s.state="attached";else if(state==3)s.state="restart_needed";else if(state==4)s.state="error";else if(state==1)s.state="awaiting_index";char text[193];std::memcpy(text,s.view->message,192);text[192]=0;if(*text)s.message=text;signature<<InterlockedCompareExchange64(reinterpret_cast<volatile LONG64*>(&s.view->selected),0,0)<<':'<<state<<';';}if(s.process&&WaitForSingleObject(s.process,0)==WAIT_OBJECT_0)s.state="exited";signature<<s.pid<<s.state<<s.message;if(s.state=="attached")overall="attached";else if(overall!="attached"&&(s.state=="restart_needed"||s.state=="error"))overall=s.state;else if(overall=="armed"&&s.state=="awaiting_index")overall="attaching";}
        if(signature.str()!=last){last=signature.str();if(!WriteStatus(cfg,self_birth,overall,message,sessions))break;}
    }
    WriteStatus(cfg,self_birth,stopping?"stopped":"error",stopping?"future watching stopped; existing DLLs stay until game exit":detector_message.empty()?"watcher wait/status failure; no process killed":detector_message,sessions);SaveIO();
    // Process exit reclaims only watcher-side views/handles. Remote buffers and
    // loaded DLLs are deliberately not freed/unloaded on unknown or stop.
    return stopping?0:5;
}
int WINAPI wWinMain(HINSTANCE,HINSTANCE,PWSTR,int){int argc=0;wchar_t** argv=CommandLineToArgvW(GetCommandLineW(),&argc);if(!argv)return 2;int result=10;try{result=Run(argc,argv);}catch(...){result=10;}LocalFree(argv);return result;}
