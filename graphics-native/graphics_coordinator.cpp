#define NOMINMAX
#include "remote_runtime.hpp"
#include "native/background_protocol.h"
#include "protocol.hpp"

static std::wstring DecodePath(const std::string& hex){
 Need(!hex.empty()&&!(hex.size()%2)&&hex.size()<=65536,"encoded path length");std::string bytes;bytes.reserve(hex.size()/2);
 auto nibble=[](char c){Need((c>='0'&&c<='9')||(c>='a'&&c<='f'),"encoded path hex");return c<='9'?c-'0':c-'a'+10;};
 for(size_t i=0;i<hex.size();i+=2)bytes.push_back(char((nibble(hex[i])<<4)|nibble(hex[i+1])));
 int n=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,bytes.data(),int(bytes.size()),nullptr,0);Need(n>0,"encoded UTF8 path");std::wstring text(n,L'\0');Need(MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,bytes.data(),int(bytes.size()),text.data(),n)==n&&text.find(L'\0')==std::wstring::npos,"encoded path text");return Canon(text);
}
static std::string JsonString(const std::string& text){std::string out="\"";for(unsigned char c:text){if(c=='\\'||c=='\"'){out+='\\';out+=char(c);}else if(c<32){char buf[7];sprintf_s(buf,"\\u%04x",c);out+=buf;}else out+=char(c);}return out+'\"';}
static std::string Utf8(const std::wstring& text){int n=WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,text.data(),int(text.size()),nullptr,0,nullptr,nullptr);Need(n>0,"UTF16 path text");std::string out(n,'\0');Need(WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,text.data(),int(text.size()),out.data(),n,nullptr,nullptr)==n,"UTF16 conversion");return out;}
struct WindowSearch {DWORD pid=0;HWND best=nullptr;uint64_t area=0;};
static BOOL CALLBACK FindWindow(HWND window,LPARAM pointer){auto&s=*reinterpret_cast<WindowSearch*>(pointer);DWORD pid=0;GetWindowThreadProcessId(window,&pid);RECT r{};if(pid!=s.pid||!IsWindowVisible(window)||GetWindow(window,GW_OWNER)||!GetClientRect(window,&r)||r.right<320||r.bottom<240)return TRUE;uint64_t area=uint64_t(r.right)*r.bottom;if(area>s.area){s.area=area;s.best=window;}return TRUE;}
static void SaveStartup(Run&run,const std::string& state,const tqgr::StatusV3*status,const bg::Status*memory){
 std::string data="{\"schema\":\"texturequest-graphics-startup-v1\",\"state\":"+JsonString(state)+",\"pid\":"+std::to_string(run.pid)+",\"birth100ns\":"+std::to_string(run.birth)+",\"nonce\":"+JsonString(run.nonce)+",\"game_files_changed\":false,\"entry_patched\":false";
 if(status)data+=",\"host_error\":"+std::to_string(status->error)+",\"main_ready\":"+std::to_string(status->main_ready)+",\"frames\":"+std::to_string(status->frames)+",\"rows\":"+std::to_string(status->canonical_upscaled_rows)+",\"native_bypass_gap\":1";
 if(memory)data+=",\"file_layer_phase\":"+std::to_string(memory->phase)+",\"file_redirects\":"+std::to_string(memory->succeeded);
 data+="}\n";NewFile(fs::path(run.session)/L"STARTUP.json",std::vector<unsigned char>(data.begin(),data.end()));
}
static int Attach(const wchar_t* request_path){
 Run run;Handle mapping;bg::Status* memory_status=nullptr;
 try{
  File request(Canon(request_path),"",65535,false);auto fields=policy::Fields(std::string(request.bytes.begin(),request.bytes.end()),"TQGXREQ1\n",65535);
  policy::Keys(fields,{"nonce","pid","birth100ns","target","target_sha256","session","provider","provider_sha256","host","host_sha256","bootstrap","bootstrap_sha256","memory","memory_sha256","manifest","manifest_sha256","bindings","bindings_sha256","package","game","catalog","catalog_sha256"});
  run.nonce=fields.at("nonce");Need(startup::Hex(run.nonce,32),"graphics nonce");run.pid=DWORD(policy::Decimal(fields.at("pid"),UINT32_MAX));run.birth=policy::Decimal(fields.at("birth100ns"));run.target=DecodePath(fields.at("target"));run.session=DecodePath(fields.at("session"));auto game=DecodePath(fields.at("game"));
  Need(fs::path(run.target).filename()==L"everquest2.exe"&&fs::path(run.target).parent_path()==fs::path(game)&&!tq::Prefix(run.session.c_str(),game)&&run.session!=game,"exact EQ2 target/outside-game session");
  run.process.h=OpenProcess(PROCESS_QUERY_INFORMATION|PROCESS_VM_READ|PROCESS_VM_WRITE|PROCESS_VM_OPERATION|PROCESS_CREATE_THREAD|SYNCHRONIZE,FALSE,run.pid);Need(run.process.h!=nullptr,"exact game process access");run.released=run.resumed=true;run.Identity();FILETIME now{};GetSystemTimeAsFileTime(&now);Need(startup::Number(now)>=run.birth&&startup::Number(now)-run.birth<=20000000ull,"late attachment refused; wait for the next ordinary launch");
  auto parent=run.Keep(std::make_shared<Directory>(fs::path(run.session).parent_path().wstring()));Need(CreateDirectoryW(run.session.c_str(),nullptr),"new graphics session only");auto held_session=run.Keep(std::make_shared<Directory>(run.session));
  run.log.open(fs::path(run.session)/L"graphics-host-coordinator.log",std::ios::out|std::ios::binary);Need(bool(run.log),"new graphics session journal");run.Log("TQGX full-pack EARLY_ATTACH; no entry patch/suspend/resume/kill/game-file writes");
  auto exe=run.Keep(std::make_shared<File>(run.target,fields.at("target_sha256")));PE exe_pe(exe->bytes);
  std::map<std::string,std::shared_ptr<File>> roles;
  for(auto name:{"provider","host","bootstrap","memory","manifest","bindings","catalog"}){auto p=DecodePath(fields.at(name));Need(p!=run.target&&!tq::Prefix(p.c_str(),game),"runtime input must be outside game");auto f=run.Keep(std::make_shared<File>(p,fields.at(std::string(name)+"_sha256")));roles.emplace(name,f);}
  auto package=DecodePath(fields.at("package"));Need(package!=game&&!tq::Prefix(package.c_str(),game),"private package outside game");
  for(auto proxy:{L"dxgi.dll",L"d3d11.dll",L"d3d9.dll",L"ReShade64.dll"}){SetLastError(0);auto attributes=GetFileAttributesW((fs::path(game)/proxy).c_str());Need(attributes==INVALID_FILE_ATTRIBUTES&&GetLastError()==ERROR_FILE_NOT_FOUND,"competing game-folder graphics proxy; files left untouched");}
  std::map<std::string,std::shared_ptr<File>> copies;
  for(auto name:{"provider","host","bootstrap","memory"}){auto source=roles.at(name);auto destination=fs::path(run.session)/fs::path(source->path).filename();NewFile(destination,source->bytes);auto f=run.Keep(std::make_shared<File>(destination.wstring(),source->hash));run.allowed_loads.emplace(f->path,f->hash);copies.emplace(name,f);}
  auto addon=run.session+L"\\addons-empty";Need(CreateDirectoryW(addon.c_str(),nullptr),"new empty addon folder");auto ini=textureprofile::Ini(Utf8(addon));NewFile(fs::path(run.session)/L"ReShade.ini",std::vector<unsigned char>(ini.begin(),ini.end()));
  auto memory_dll=copies.at("memory");PE memory_pe(memory_dll->bytes);auto memory_base=run.Load(*memory_dll);auto mapping_name=L"Local\\TQBG-"+std::wstring(run.nonce.begin(),run.nonce.end());mapping.h=CreateFileMappingW(INVALID_HANDLE_VALUE,nullptr,PAGE_READWRITE,0,sizeof(bg::Status),mapping_name.c_str());Need(mapping.h&&GetLastError()!=ERROR_ALREADY_EXISTS,"new file-layer status mapping");memory_status=static_cast<bg::Status*>(MapViewOfFile(mapping.h,FILE_MAP_READ|FILE_MAP_WRITE,0,0,sizeof(bg::Status)));Need(memory_status!=nullptr,"file-layer status mapping view");bg::Prepare(*memory_status,run.pid,run.birth,roles.at("manifest")->hash);
  auto memory_config=std::make_unique<bg::Config>();memset(memory_config.get(),0,sizeof(*memory_config));memcpy(memory_config->magic,"TQBGCF01",8);memory_config->size=sizeof(*memory_config);memory_config->pid=run.pid;memory_config->birth100ns=run.birth;
  wcscpy_s(memory_config->manifest,roles.at("manifest")->path.c_str());wcscpy_s(memory_config->bindings,roles.at("bindings")->path.c_str());wcscpy_s(memory_config->package,package.c_str());wcscpy_s(memory_config->game,game.c_str());wcscpy_s(memory_config->mapping,mapping_name.c_str());memcpy(memory_config->hash,roles.at("manifest")->hash.c_str(),65);memcpy(memory_config->bindings_hash,roles.at("bindings")->hash.c_str(),65);
  Need(run.Call(memory_base+memory_pe.Export(memory_dll->bytes,"TextureQuestInitialize"),run.Buffer(memory_config.get(),sizeof(*memory_config),"full-pack-file-layer-configuration"))==0,"file-layer initializer refused; no retry");
  auto bootstrap=copies.at("bootstrap");PE bootstrap_pe(bootstrap->bytes);auto bootstrap_base=run.Load(*bootstrap);run.bootstrap_path=bootstrap->path;run.bootstrap_base=bootstrap_base;run.bootstrap_size=bootstrap_pe.nt.OptionalHeader.SizeOfImage;
  auto setup=std::make_unique<startup::Configure>();setup->pid=run.pid;setup->birth=run.birth;setup->reserved=1;memcpy(setup->nonce,run.nonce.c_str(),33);wcscpy_s(setup->base,run.session.c_str());wcscpy_s(setup->session,run.session.c_str());
  Need(run.Call(bootstrap_base+bootstrap_pe.Export(bootstrap->bytes,"TQGX_PrepareAttachV4"),run.Buffer(setup.get(),sizeof(*setup),"early-attach-texture-only-profile"))==0,"early-attach texture profile refused");
  auto provider=copies.at("provider");PE provider_pe(provider->bytes);auto provider_base=run.Load(*provider);run.provider_path=provider->path;run.provider_base=provider_base;run.provider_size=provider_pe.nt.OptionalHeader.SizeOfImage;
  auto host=copies.at("host");PE host_pe(host->bytes);auto host_base=run.Load(*host);run.host_path=host->path;run.host_base=host_base;run.host_size=host_pe.nt.OptionalHeader.SizeOfImage;
  tqgr::InitV3 init{};memcpy(init.magic,"TQPHIN03",8);init.bytes=sizeof(init);init.version=3;init.pid=run.pid;init.flags=3;init.birth100ns=run.birth;init.provider=provider_base;init.bootstrap=bootstrap_base;init.profile_receipt=bootstrap_base+bootstrap_pe.Export(bootstrap->bytes,"TQPH_GetProfileV3");init.catalog_address=reinterpret_cast<uint64_t>(run.Buffer(roles.at("catalog")->bytes.data(),roles.at("catalog")->bytes.size(),"full-pack-metadata-only"));init.catalog_bytes=uint32_t(roles.at("catalog")->bytes.size());memcpy(init.nonce,run.nonce.c_str(),33);memcpy(init.catalog_sha256,roles.at("catalog")->hash.c_str(),65);
  Need(run.Call(host_base+host_pe.Export(host->bytes,"TQPH_InitializeV3"),run.Buffer(&init,sizeof(init),"full-pack-host-initialization"))==0,"full-pack host initialization refused");
  auto status_fn=host_base+host_pe.Export(host->bytes,"TQPH_GetStatusV3");auto command_fn=host_base+host_pe.Export(host->bytes,"TQPH_CommandV3");tqgr::StatusV3 status{};auto status_memory=run.Buffer(&status,sizeof(status),"startup-status-only");bool bound=false,ready=false;auto deadline=GetTickCount64()+15000;
  while(GetTickCount64()<deadline&&WaitForSingleObject(run.process.h,200)==WAIT_TIMEOUT){
   if(!bound){WindowSearch search{run.pid};EnumWindows(FindWindow,reinterpret_cast<LPARAM>(&search));if(!search.best)continue;
    Need(run.Call(status_fn,status_memory)==0,"startup host status");run.Read(reinterpret_cast<uintptr_t>(status_memory),&status,sizeof(status));if(!status.input_ready){Need(!status.error&&!status.stopping,"startup input fault");continue;}
    tqgr::CommandV3 command{};memcpy(command.magic,"TQPHCM03",8);command.bytes=sizeof(command);command.version=3;command.pid=run.pid;command.birth100ns=run.birth;command.sequence=1;command.mode=0;command.main_hwnd=reinterpret_cast<uint64_t>(search.best);memcpy(command.nonce,run.nonce.c_str(),33);Need(run.Call(command_fn,run.Buffer(&command,sizeof(command),"one-main-Upscaled-binding"))==0,"exact main window binding");bound=true;
   }
   Need(run.Call(status_fn,status_memory)==0,"startup status query");run.Read(reinterpret_cast<uintptr_t>(status_memory),&status,sizeof(status));Need(startup::StatusIdentity(status,run.pid,run.birth,run.nonce)&&!status.error&&!status.stopping,"startup ownership or health");if(status.frames&&status.acknowledged>=1&&status.main_ready){ready=true;break;}
  }
  SaveStartup(run,ready?"main_present_observed":"restart_needed",&status,memory_status);
  run.Log("startup_complete; NO periodic remote status calls; foreground hotkey owns quality requests");
  while(WaitForSingleObject(run.process.h,500)==WAIT_TIMEOUT){}run.Log("natural_game_exit; owned remote leases expire with process");if(memory_status)UnmapViewOfFile(memory_status);return ready?0:3;
 }catch(const std::exception&e){if(run.log){try{run.Log(std::string("REFUSED=")+e.what()+"; NO suspend/resume/kill/retry");SaveStartup(run,"refused",nullptr,memory_status);}catch(...){}}std::fprintf(stderr,"REFUSED: %s Win32=%lu\n",e.what(),GetLastError());if(run.process.h&&run.remote_unknown)while(WaitForSingleObject(run.process.h,500)==WAIT_TIMEOUT){}if(memory_status)UnmapViewOfFile(memory_status);return 2;}
}
int wmain(int argc,wchar_t**argv){if(argc!=3||wcscmp(argv[1],L"--attach"))return 2;return Attach(argv[2]);}
