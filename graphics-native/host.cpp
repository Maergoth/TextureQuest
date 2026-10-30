// Private experimental wrapper-observed DEFAULT host. Source only until sealed CPU scope.
#define NOMINMAX
#include "protocol.hpp"
#include "native_port.hpp"
#include "creation_ingress.hpp"
#include "full_pack_engine.hpp"
#include "pending_authority.hpp"
#include "swapchain_lifecycle.hpp"
#include "startup_protocol.hpp"
#include <array>
#include <filesystem>
namespace api=reshade::api;
using namespace tqab;using namespace tqgr;
static_assert(RESHADE_API_VERSION==20&&sizeof(void*)==8);
static HMODULE self_module=nullptr;
static bool proxy_startup=false;
namespace {static void ProxyObserve()noexcept;static DWORD WINAPI ProxyInput(void*)noexcept;}
namespace {
struct Host {
 Once once;tqph::SwapchainLifecycle swap_lifecycle;std::mutex mutex;bool selecting=false;InitV3 init{};Parsed parsed;tqph::Session session;tqph::Resources resources{session};
 std::shared_ptr<tqgx::FullPackEngine> full;
 std::shared_ptr<Control> control=std::make_shared<Control>();std::shared_ptr<Adapter> adapter;std::shared_ptr<NativePort> port;const tqpc::Api* creation_api=nullptr;std::shared_ptr<tqph::CreationIngress> creation_ingress;
 std::atomic<api::device*> device{nullptr};std::atomic<api::command_list*> context{nullptr};std::atomic<api::command_queue*> queue{nullptr};std::atomic<api::swapchain*> swapchain{nullptr};
 std::atomic<uint32_t> error{0};std::atomic<bool> registered{false},input_ready{false},main_ready{false},stopping{false};std::atomic<uint64_t> main_hwnd{0};
 std::atomic<DWORD> render_thread{0};uint32_t pid=0;uint64_t birth=0;HANDLE thread=nullptr;HMODULE bootstrap=nullptr;DWORD(WINAPI* profile)(void*)=nullptr;
};
Host& H(){static auto* h=new Host;return *h;} // All published modules/owners retained to natural exit.
bool WriteSafe(void*d,const void*s,size_t n)noexcept{__try{memcpy(d,s,n);return true;}__except(EXCEPTION_EXECUTE_HANDLER){return false;}}
uint64_t Birth(){FILETIME a{},b{},c{},d{};if(!GetProcessTimes(GetCurrentProcess(),&a,&b,&c,&d))return 0;return(uint64_t(a.dwHighDateTime)<<32)|a.dwLowDateTime;}
void Fail(uint32_t code)noexcept{auto&h=H();uint32_t zero=0;h.error.compare_exchange_strong(zero,code);h.stopping=true;h.main_ready=false;h.swap_lifecycle.Close();h.session.Close(code);if(auto a=std::atomic_load(&h.adapter))a->NativeFault();}
bool OwnedWindow(uint64_t hwnd){DWORD pid=0;return hwnd&&IsWindow(reinterpret_cast<HWND>(hwnd))&&GetWindowThreadProcessId(reinterpret_cast<HWND>(hwnd),&pid)&&pid==H().pid;}
bool DModule(HMODULE m){wchar_t path[32768]{};auto n=GetModuleFileNameW(m,path,32768);if(!n||n>=32768)return false;auto f=CreateFileW(path,GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,nullptr);if(f==INVALID_HANDLE_VALUE)return false;BY_HANDLE_FILE_INFORMATION i{};wchar_t final[32768]{};auto size=GetFinalPathNameByHandleW(f,final,32768,FILE_NAME_NORMALIZED|VOLUME_NAME_DOS);bool good=GetFileInformationByHandle(f,&i)&&!(i.dwFileAttributes&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT))&&size>7&&size<32768;CloseHandle(f);return good;}
std::wstring ModuleParent(HMODULE m){wchar_t path[32768]{};auto n=GetModuleFileNameW(m,path,32768);require(n&&n<32768,"module path");std::wstring canonical;require(tq::Canonical(path,canonical),"canonical module path");return std::filesystem::path(canonical).parent_path().wstring();}
bool Image(const void* p,HMODULE m,bool code){MEMORY_BASIC_INFORMATION i{};if(!p||VirtualQuery(p,&i,sizeof i)!=sizeof i||i.AllocationBase!=m||i.Type!=MEM_IMAGE||i.State!=MEM_COMMIT||(i.Protect&(PAGE_GUARD|PAGE_NOACCESS)))return false;return !code||(i.Protect&(PAGE_EXECUTE|PAGE_EXECUTE_READ|PAGE_EXECUTE_READWRITE|PAGE_EXECUTE_WRITECOPY));}
bool Pin(HMODULE m){HMODULE v=nullptr;return GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_PIN,reinterpret_cast<LPCWSTR>(m),&v)&&v==m;}
bool Profile(){auto&h=H();startup::ProfileStatus s{};if(!h.profile||h.profile(&s)||memcmp(s.magic,"TQPHPR03",8)||s.bytes!=sizeof s||s.version!=3||s.pid!=h.pid||s.birth!=h.birth||memcmp(s.nonce,h.init.nonce,33)||s.error||s.ready!=1||s.held!=1||s.unknown||s.reserved)return false;for(auto b:s.pad)if(b)return false;return true;}
std::shared_ptr<void> ModuleLease(){return std::shared_ptr<void>(self_module,[](void*){});}
// Ordinary SDK create/init notifications retain their original returns. The
// authenticated sideband owns exact capture/native-result/final settlement.
bool Create(api::device*,api::resource_desc&,api::subresource_data*,api::resource_usage)noexcept{return false;}
void Resource(api::device*,const api::resource_desc&,const api::subresource_data*,api::resource_usage,api::resource)noexcept{}
uint32_t TQPM_CALL CaptureCreation(void*,const tqpc::Info* input,void** ticket)noexcept{try{tqpc::Info i{};require(input&&SafeRead(&i,input,sizeof i),"creation capture info fence");auto ingress=std::atomic_load(&H().creation_ingress);require(bool(ingress),"creation ingress initialized");return ingress->Capture(i,ticket);}catch(...){Fail(tqph::Session::creation);return 2;}}
uint32_t TQPM_CALL NativeCreation(void*,void* ticket,const tqpc::NativeResult* input)noexcept{try{tqpc::NativeResult r{};tqpc::Info i{};require(input&&SafeRead(&r,input,sizeof r)&&r.info&&SafeRead(&i,r.info,sizeof i),"creation result fence");r.info=&i;auto ingress=std::atomic_load(&H().creation_ingress);require(bool(ingress),"creation result ingress");return ingress->Native(ticket,r);}catch(...){Fail(tqph::Session::creation);return 0;}}
uint32_t TQPM_CALL SettleCreation(void*,void* ticket,const tqpc::Settlement* input)noexcept{try{tqpc::Settlement r{};tqpc::Info i{};require(input&&SafeRead(&r,input,sizeof r)&&r.info&&SafeRead(&i,r.info,sizeof i),"creation settlement fence");r.info=&i;auto ingress=std::atomic_load(&H().creation_ingress);require(bool(ingress),"creation settlement ingress");return ingress->Settle(ticket,r);}catch(...){Fail(tqph::Session::creation);return 0;}}
void ResourceView(api::device*d,api::resource r,api::resource_usage usage,const api::resource_view_desc&,api::resource_view v)noexcept{try{auto&h=H();if(h.stopping||d!=h.session.Device()||usage!=api::resource_usage::shader_resource)return;auto ingress=std::atomic_load(&h.creation_ingress);require(ingress&&ingress->View(r,v),"captured view lifetime");}catch(...){Fail(tqph::Session::creation);}}
bool Observe(const tqpm::Header* borrowed,const void* object,bool view,const tqpm::Boundary* boundary=nullptr)noexcept{
 try{auto&h=H();tqpm::Header head{};if(!borrowed||!SafeRead(&head,borrowed,sizeof head)||!h.session.Notice(head))return false;
  auto ingress=std::atomic_load(&h.creation_ingress);if(!h.session.Device())return true;require(bool(ingress),"selected mutation ingress");
  return h.resources.Mutation(head.api_device==h.session.Device(),[&](std::vector<uint64_t>& ids){
   if(boundary&&(boundary->kind==tqpm::BoundaryKind::offer||boundary->kind==tqpm::BoundaryKind::reclaim)){
    if(!boundary->resource_array||!boundary->count||boundary->count>64)return false;
    for(uint32_t i=0;i<boundary->count;++i){const void* r=nullptr;if(!SafeRead(&r,static_cast<const void*const*>(boundary->resource_array)+i,sizeof r))return false;uint64_t id=0;if(!ingress->Canonical(r,false,id,true))return false;ids.push_back(id);}return true;
   }
   uint64_t id=0;if(!ingress->Canonical(object,view,id,true))return false;ids.push_back(id);return true;
  },[&](uint64_t){return true;});
 }catch(...){Fail(tqph::Session::mutation);return false;}
}
uint32_t TQPM_CALL DiscardResource(void*,const tqpm::Header*h,ID3D11Resource*r){return Observe(h,r,false)?1u:0u;}
uint32_t TQPM_CALL DiscardView(void*,const tqpm::Header*h,ID3D11View*v){return Observe(h,v,true)?1u:0u;}
uint32_t TQPM_CALL DiscardView1(void*,const tqpm::Header*h,ID3D11View*v,const D3D11_RECT*,uint32_t){return Observe(h,v,true)?1u:0u;}
uint32_t TQPM_CALL Swap(void*,const tqpm::Header*,ID3DDeviceContextState*,ID3DDeviceContextState*const*){Fail(tqph::Session::mutation);return 0;}
uint32_t TQPM_CALL Boundary(void*,const tqpm::Header*h,const tqpm::Boundary*b){tqpm::Boundary copy{};if(!b||!SafeRead(&copy,b,sizeof copy)||copy.size!=48||copy.abi_version!=2||uint32_t(copy.kind)<1||uint32_t(copy.kind)>10||uint32_t(copy.origin)<1||uint32_t(copy.origin)>4){Fail(tqph::Session::notice);return 0;}bool view=copy.kind==tqpm::BoundaryKind::mips||copy.kind==tqpm::BoundaryKind::clear;return Observe(h,copy.destination,view,&copy)?1u:0u;}
void SdkNotice(api::device* actual,tqdefault::MutationTarget kind,uint64_t key=0)noexcept{try{auto&h=H();auto selected=h.session.Device();if(!selected)return;require(actual!=nullptr,"SDK actual owner");if(actual!=selected)return;if(kind==tqdefault::MutationTarget::device_wide){Fail(tqph::Session::mutation);return;}require(h.resources.SdkDirty(key,kind==tqdefault::MutationTarget::view),"SDK opaque native lifetime retirement");}catch(...){Fail(tqph::Session::mutation);}}
// A genuine destruction notification carries only an opaque dying-object
// handle. Never send it through NativePort, COM identity or a native read scope.
void SdkDestruction(api::device* actual,bool view,uint64_t key)noexcept{try{auto&h=H();
 bool accepted=tqph::ObserveSdkDestruction(h.resources,h.session.Device(),actual,view,[&]{return key;},[&](uint64_t){return true;});if(!accepted)Fail(tqph::Session::mutation);
 }catch(...){Fail(tqph::Session::mutation);}}
void SdkCommand(api::command_list*c,tqdefault::MutationTarget kind,uint64_t key=0)noexcept{try{SdkNotice(c?c->get_device():nullptr,kind,key);}catch(...){Fail(tqph::Session::mutation);}}
bool Update(api::device*d,const api::subresource_data&,api::resource r,uint32_t,const api::subresource_box*){SdkNotice(d,tqdefault::MutationTarget::resource,r.handle);return false;}
bool UpdateCommand(api::command_list*c,const api::subresource_data&,api::resource r,uint32_t,const api::subresource_box*){SdkCommand(c,tqdefault::MutationTarget::resource,r.handle);return false;}
void Map(api::device*d,api::resource r,uint32_t,const api::subresource_box*,api::map_access,api::subresource_data*){SdkNotice(d,tqdefault::MutationTarget::resource,r.handle);}
void Unmap(api::device*d,api::resource r,uint32_t){SdkNotice(d,tqdefault::MutationTarget::resource,r.handle);}
bool CopyResource(api::command_list*c,api::resource,api::resource d){SdkCommand(c,tqdefault::MutationTarget::resource,d.handle);return false;}
bool CopyTexture(api::command_list*c,api::resource,uint32_t,const api::subresource_box*,api::resource d,uint32_t,const api::subresource_box*,api::filter_mode){SdkCommand(c,tqdefault::MutationTarget::resource,d.handle);return false;}
bool CopyBuffer(api::command_list*c,api::resource,uint64_t,uint32_t,uint32_t,api::resource d,uint32_t,const api::subresource_box*){SdkCommand(c,tqdefault::MutationTarget::resource,d.handle);return false;}
bool Resolve(api::command_list*c,api::resource,uint32_t,const api::subresource_box*,api::resource d,uint32_t,uint32_t,uint32_t,uint32_t,api::format){SdkCommand(c,tqdefault::MutationTarget::resource,d.handle);return false;}
bool Mips(api::command_list*c,api::resource_view v){SdkCommand(c,tqdefault::MutationTarget::view,v.handle);return false;}
void Secondary(api::command_list*c,api::command_list*){SdkCommand(c,tqdefault::MutationTarget::device_wide);}
void Reset(api::command_list*c){SdkCommand(c,tqdefault::MutationTarget::device_wide);}
void DestroyResource(api::device*d,api::resource r){SdkDestruction(d,false,r.handle);}
void DestroyView(api::device*d,api::resource_view v){SdkDestruction(d,true,v.handle);}
bool Indirect(api::command_list*c,api::indirect_command,api::resource,uint64_t,uint32_t,uint32_t){SdkCommand(c,tqdefault::MutationTarget::device_wide);return false;}
void Device(api::device*d)noexcept{if(!d)return;auto&h=H();if(!Profile())Fail(tqph::Session::runtime);if(h.device&&d!=h.device){} /* Foreign devices receive no selected authority. */}
void Push(api::command_list*c,api::shader_stage stages,api::pipeline_layout,uint32_t param,const api::descriptor_table_update&update)noexcept{
 try{auto core=std::atomic_load(&H().full);if(!core||!core->WantsTracking()||c!=H().context||param!=1||update.type!=api::descriptor_type::shader_resource_view||update.array_offset||!update.descriptors||update.binding>=128||update.count>128-update.binding)return;
  std::vector<uint64_t> handles(update.count);auto data=static_cast<const api::resource_view*>(update.descriptors);for(size_t i=0;i<handles.size();++i){api::resource_view value{};require(SafeRead(&value,data+i,sizeof value),"full graphics descriptor fence");handles[i]=value.handle;}core->Bind(uint32_t(stages),update.binding,handles.data(),handles.size());
 }catch(...){Fail(tqph::Session::mutation);}
}
void Context(api::command_list*)noexcept{} // Kind is obtained only from NativeOwner after producer ContextKind.
void Present(api::command_queue*q,api::swapchain*s,const api::rect*src,const api::rect*dst,uint32_t n,const api::rect*dirty)noexcept{
 try{auto&h=H();if(h.stopping||!q||!s)return;auto hwnd=h.main_hwnd.load();if(proxy_startup&&!hwnd){auto candidate=reinterpret_cast<uint64_t>(s->get_hwnd());RECT rect{};if(OwnedWindow(candidate)&&IsWindowVisible(reinterpret_cast<HWND>(candidate))&&!GetWindow(reinterpret_cast<HWND>(candidate),GW_OWNER)&&GetClientRect(reinterpret_cast<HWND>(candidate),&rect)&&rect.right>=320&&rect.bottom>=240){uint64_t zero=0;h.main_hwnd.compare_exchange_strong(zero,candidate);hwnd=h.main_hwnd.load();}}if(!hwnd||!OwnedWindow(hwnd)||reinterpret_cast<uint64_t>(s->get_hwnd())!=hwnd)return;
  if(!Profile()||s->get_device()!=q->get_device()){Fail(tqph::Session::selection);return;}
  auto a=std::atomic_load(&h.adapter);if(a&&!h.swap_lifecycle.Ready())return;if(!a){
   {std::unique_lock lock(h.mutex,std::try_to_lock);require(lock.owns_lock()&&!h.selecting&&!std::atomic_load(&h.adapter)&&hwnd==h.main_hwnd.load(),"single main selection claim");h.selecting=true;}
   // No metadata lock spans provider/native identity, protection, or Engine setup.
   auto d=q->get_device();auto c=q->get_immediate_command_list();require(d&&c,"actual immediate API pair");
   h.render_thread=GetCurrentThreadId();auto p=std::make_shared<NativePort>(h.session,h.resources,d,c,ModuleLease());a=std::make_shared<Adapter>(h.parsed.catalog,p,h.control,Witness{false,true,true},true);a->InitDevice(d);a->InitContext(c);require(a->AdmitPresent(q,s)&&Entries::Publish(a),"adapter exact publication");{std::lock_guard lock(h.mutex);require(h.selecting&&hwnd==h.main_hwnd.load(),"stable claimed main owner");require(h.swap_lifecycle.Select(reinterpret_cast<uintptr_t>(s)),"one selected swapchain lifetime");h.device=d;h.context=c;h.queue=q;h.swapchain=s;std::atomic_store(&h.port,p);std::atomic_store(&h.adapter,a);h.selecting=false;}
   auto core=std::make_shared<tqgx::FullPackEngine>(h.parsed.catalog,p,h.resources,h.parsed.dual);auto ingress=std::atomic_load(&h.creation_ingress);require(bool(ingress),"full graphics ingress");ingress->Receiver([core](const Row* row,const std::shared_ptr<tqph::Resources::Lease>&life){core->Admit(row,life);});std::atomic_store(&h.full,core);
  }
  if(q!=h.queue||s!=h.swapchain||h.render_thread.load()!=GetCurrentThreadId()){Fail(tqph::Session::selection);return;}auto ingress=std::atomic_load(&h.creation_ingress);auto selected_port=std::atomic_load(&h.port);require(ingress&&selected_port,"selected creation drain");ingress->Drain(*selected_port,*a,h.device);tqpm::Status health{};require(h.session.Read(health),"provider status shape");if(!health.healthy||health.error){Fail(tqph::Session::status);return;}if(health.pending_creations)return;
  if(!h.session.Health(true,false,false,true)){Fail(tqph::Session::status);return;}auto core=std::atomic_load(&h.full);require(bool(core),"full graphics renderer");core->Present(h.control->Load());if(core->Snapshot().unsafe){Fail(tqph::Session::native);return;}if(!a->Faulted())h.main_ready=true;if(proxy_startup)ProxyObserve();
 }catch(...){Fail(tqph::Session::selection);}
}
bool Draw(api::command_list*c,uint32_t n,uint32_t i,uint32_t first,uint32_t fi)noexcept{auto&h=H();if(h.stopping||!h.swap_lifecycle.Ready()||!h.main_ready||!h.input_ready||c!=h.context)return false;auto core=std::atomic_load(&h.full);return core?core->Draw(c,false,n,i,first,0,fi):false;}
bool Indexed(api::command_list*c,uint32_t n,uint32_t i,uint32_t first,int32_t offset,uint32_t fi)noexcept{auto&h=H();if(h.stopping||!h.swap_lifecycle.Ready()||!h.main_ready||!h.input_ready||c!=h.context)return false;auto core=std::atomic_load(&h.full);return core?core->Draw(c,true,n,i,first,offset,fi):false;}
void DestroyDevice(api::device*d)noexcept{if(d==H().device)Fail(3811);}
void DestroyContext(api::command_list*c)noexcept{if(c==H().context)Fail(3812);}
void DestroyQueue(api::command_queue*q)noexcept{if(q==H().queue)Fail(3813);}
void DestroySwap(api::swapchain*s,bool resize)noexcept{auto&h=H();if(s!=h.swapchain||h.stopping)return;if(!resize){Fail(3814);return;}if(!h.swap_lifecycle.BeginResize(reinterpret_cast<uintptr_t>(s))){Fail(3815);return;}h.main_ready=false;}
void InitSwap(api::swapchain*s,bool resize)noexcept{try{auto&h=H();if(s!=h.swapchain||h.stopping)return;const auto hwnd=h.main_hwnd.load();const bool continuity=resize&&OwnedWindow(hwnd)&&reinterpret_cast<uint64_t>(s->get_hwnd())==hwnd&&s->get_device()==h.device&&h.queue&&h.context&&h.render_thread.load()==GetCurrentThreadId()&&Profile();if(!h.swap_lifecycle.Resume(reinterpret_cast<uintptr_t>(s),continuity))Fail(3816);}catch(...){Fail(3816);}}
void Runtime(api::effect_runtime*)noexcept{Fail(tqph::Session::runtime);}
void Toggle()noexcept{auto&h=H();DWORD foreground=0;GetWindowThreadProcessId(GetForegroundWindow(),&foreground);if(h.stopping||h.session.Closed()||!h.registered||!Foreground(h.pid,h.birth,foreground,Birth()))return;for(unsigned i=0;i<8;++i){auto v=h.control->Load();if(h.control->Request((v>>1)+1,(v&1)==0))return;}}
LRESULT CALLBACK Window(HWND w,UINT message,WPARAM key,LPARAM l){if(message==WM_HOTKEY&&key==0x5451){Toggle();return 0;}return DefWindowProcW(w,message,key,l);}
DWORD WINAPI Input(void*)noexcept{if(proxy_startup)return ProxyInput(nullptr);try{auto&h=H();std::wstring name=L"TextureQuestPrivateProviderHostV3-";for(unsigned i=0;i<32;++i)name+=wchar_t(h.init.nonce[i]);WNDCLASSEXW c{};c.cbSize=sizeof c;c.lpfnWndProc=Window;c.hInstance=self_module;c.lpszClassName=name.c_str();require(RegisterClassExW(&c)!=0,"input class");auto window=CreateWindowExW(0,name.c_str(),L"",0,0,0,0,0,HWND_MESSAGE,nullptr,self_module,nullptr);require(window,"owned graphics input window");h.input_ready=true;MSG m{};int r;while((r=GetMessageW(&m,nullptr,0,0))>0){TranslateMessage(&m);DispatchMessageW(&m);}h.input_ready=false;Fail(3601);}catch(...){Fail(3602);}return 0;}
void RegisterEvents(){
#define E(name,fn) reshade::register_event<reshade::addon_event::name>(fn)
 E(init_device,Device);E(init_command_list,Context);E(create_resource,Create);E(init_resource,Resource);E(present,Present);E(draw,Draw);E(draw_indexed,Indexed);E(init_effect_runtime,Runtime);
 E(init_swapchain,InitSwap);E(destroy_device,DestroyDevice);E(destroy_command_list,DestroyContext);E(destroy_command_queue,DestroyQueue);E(destroy_swapchain,DestroySwap);
 E(init_resource_view,ResourceView);E(push_descriptors,Push);E(update_texture_region,Update);E(update_texture_region_command,UpdateCommand);E(map_texture_region,Map);E(unmap_texture_region,Unmap);E(copy_resource,CopyResource);E(copy_texture_region,CopyTexture);E(copy_buffer_to_texture,CopyBuffer);E(resolve_texture_region,Resolve);E(generate_mipmaps,Mips);E(execute_secondary_command_list,Secondary);E(reset_command_list,Reset);E(destroy_resource_view,DestroyView);E(destroy_resource,DestroyResource);E(draw_or_dispatch_indirect,Indirect);
#undef E
}
}
extern "C" __declspec(dllexport) DWORD WINAPI TQPH_InitializeV3(const void* memory)noexcept{
 auto&h=H();if(!h.once.Enter())return ERROR_ALREADY_INITIALIZED;try{h.pid=GetCurrentProcessId();h.birth=Birth();InitV3 value{};require(memory&&SafeRead(&value,memory,sizeof value)&&InitValid(value,h.pid,h.birth),"new init wire");h.init=value;
  auto provider=reinterpret_cast<HMODULE>(value.provider);h.bootstrap=reinterpret_cast<HMODULE>(value.bootstrap);require(provider!=self_module&&h.bootstrap!=self_module&&provider!=h.bootstrap&&DModule(self_module)&&DModule(provider)&&DModule(h.bootstrap)&&Pin(self_module)&&Pin(provider)&&Pin(h.bootstrap),"exact pinned runtime modules");
  require(ModuleParent(self_module)==ModuleParent(h.bootstrap)&&ModuleParent(self_module)!=ModuleParent(nullptr)&&(proxy_startup?ModuleParent(provider)==ModuleParent(nullptr):ModuleParent(self_module)==ModuleParent(provider)),"coherent verified proxy or sibling runtime ownership");
  h.profile=reinterpret_cast<DWORD(WINAPI*)(void*)>(GetProcAddress(h.bootstrap,"TQPH_GetProfileV3"));require(reinterpret_cast<uint64_t>(h.profile)==value.profile_receipt&&Image(reinterpret_cast<void*>(h.profile),h.bootstrap,true)&&Profile(),"owned held texture-only profile");
  auto get=reinterpret_cast<const tqpm::Api*(TQPM_CALL*)(uint32_t,uint32_t)>(GetProcAddress(provider,"TQPM_GetInterfaceV2"));require(Image(reinterpret_cast<void*>(get),provider,true),"genuine ABI2 export");auto table=get(2,80);tqpm::Api copy{};require(Image(table,provider,false)&&SafeRead(&copy,table,sizeof copy)&&copy.size==80&&copy.abi_version==2&&copy.capabilities==2047,"ABI2 table");
  std::array<const void*,8> entries={reinterpret_cast<const void*>(copy.register_observer),reinterpret_cast<const void*>(copy.bind_context),reinterpret_cast<const void*>(copy.read_status),reinterpret_cast<const void*>(copy.begin_use),reinterpret_cast<const void*>(copy.end_use),reinterpret_cast<const void*>(copy.close),reinterpret_cast<const void*>(copy.read_native_owner),reinterpret_cast<const void*>(copy.acquire_protection)};for(auto p:entries)require(Image(p,provider,true),"all eight exact table code owners");
  uint8_t nonce[16]{};auto nibble=[](char c){return c<='9'?c-'0':c-'a'+10;};for(unsigned i=0;i<16;++i)nonce[i]=uint8_t(nibble(value.nonce[i*2])*16+nibble(value.nonce[i*2+1]));tqpm::Callbacks callbacks{48,2,DiscardResource,DiscardView,DiscardView1,Swap,Boundary};require(h.session.Register(table,self_module,nonce,callbacks,&h),"register before any device");
  auto get_creation=reinterpret_cast<const tqpc::Api*(TQPM_CALL*)(uint32_t,uint32_t)>(GetProcAddress(provider,"TQPC_GetInterfaceV1"));require(Image(reinterpret_cast<void*>(get_creation),provider,true),"genuine creation export");auto creation=get_creation(1,56);tqpc::Api creation_copy{};require(Image(creation,provider,false)&&SafeRead(&creation_copy,creation,sizeof creation_copy)&&creation_copy.size==56&&creation_copy.abi_version==1&&creation_copy.capabilities==15,"creation ABI1 table");
  std::array<const void*,5> creation_entries={reinterpret_cast<const void*>(creation_copy.register_observer),reinterpret_cast<const void*>(creation_copy.begin_identity),reinterpret_cast<const void*>(creation_copy.read_diagnostics),reinterpret_cast<const void*>(creation_copy.host_event),reinterpret_cast<const void*>(creation_copy.close)};for(auto p:creation_entries)require(Image(p,provider,true),"creation table exact code owner");tqpc::Registration registration{80,1,15,self_module,&h,{},tqpc::Callbacks{32,1,CaptureCreation,NativeCreation,SettleCreation}};memcpy(registration.nonce,nonce,16);require(creation->register_observer(&registration)==h.session.Token(),"once genuine creation registration before devices");h.creation_api=creation;
  Bytes bytes(value.catalog_bytes);require(SafeRead(bytes.data(),reinterpret_cast<void*>(value.catalog_address),bytes.size()),"catalog memory");h.parsed=Parse(bytes,value.catalog_sha256);require(h.parsed.canonical_stock&& !h.parsed.canonical_upscaled&&!h.parsed.dual,"graphics-only stock canonical catalog");std::atomic_store(&h.creation_ingress,std::make_shared<tqph::CreationIngress>(h.session,h.resources,h.creation_api,h.parsed.catalog));if(value.main_hwnd)require(OwnedWindow(value.main_hwnd),"owned initial main window");h.main_hwnd=value.main_hwnd;
  require(reshade::register_addon(self_module,provider),"exact API20 addon");RegisterEvents();h.registered=true;h.thread=CreateThread(nullptr,0,Input,nullptr,0,nullptr);require(h.thread!=nullptr,"input thread");return 0;
 }catch(...){Fail(3701);return ERROR_INVALID_DATA;}
}
extern "C" __declspec(dllexport) DWORD WINAPI TQPH_CommandV3(const void* memory)noexcept{try{auto&h=H();CommandV3 c{};if(!h.registered||h.stopping||h.session.Closed()||!memory||!SafeRead(&c,memory,sizeof c)||!CommandValid(c,h.init)||!Profile()||!h.session.Health(bool(std::atomic_load(&h.port)),false,false,true))return ERROR_NOT_READY;if(c.main_hwnd&&!OwnedWindow(c.main_hwnd))return ERROR_INVALID_WINDOW_HANDLE;std::unique_lock lock(h.mutex,std::try_to_lock);if(!lock.owns_lock()||h.selecting||(c.main_hwnd&&h.swapchain&&c.main_hwnd!=h.main_hwnd))return ERROR_BUSY;if(!h.control->Request(c.sequence,c.mode==1))return ERROR_INVALID_DATA;if(c.main_hwnd)h.main_hwnd=c.main_hwnd;return 0;}catch(...){Fail(3702);return ERROR_INVALID_DATA;}}
extern "C" __declspec(dllexport) DWORD WINAPI TQPH_GetCreationStatusV1(void* output)noexcept{tqpc::Diagnostics d{};d.size=184;auto&h=H();if(!h.creation_api||h.creation_api->read_diagnostics(h.session.Token(),&d)!=1)return ERROR_NOT_READY;return output&&WriteSafe(output,&d,sizeof d)?0:ERROR_NOACCESS;}
extern "C" __declspec(dllexport) DWORD WINAPI TQPH_GetProviderStatusV2(void* output)noexcept{tqpm::Status p{};if(!H().session.Read(p))return ERROR_NOT_READY;return output&&WriteSafe(output,&p,sizeof p)?0:ERROR_NOACCESS;}
extern "C" __declspec(dllexport) DWORD WINAPI TQPH_GetStatusV3(void* output)noexcept{try{auto&h=H();StatusV3 s{};memcpy(s.magic,"TQPHST03",8);s.bytes=sizeof s;s.version=3;s.pid=h.pid;s.birth100ns=h.birth;memcpy(s.nonce,h.init.nonce,33);s.error=h.error;if(!s.error)s.error=h.session.First();s.attempted=h.once.Tried();s.registered=h.registered;s.input_ready=h.input_ready;s.main_ready=h.main_ready;s.stopping=h.stopping;s.requested=h.control->Load()>>1;s.canonical_stock_rows=h.parsed.canonical_stock;s.canonical_upscaled_rows=h.parsed.canonical_upscaled;s.protection_previous=0xffffffffu;s.private_experimental=1;s.native_bypass_gap=1;s.profile_ready=Profile()?1u:0u;
 tqpm::Status p{};if(h.session.Read(p)){s.provider_error=p.error;s.protection_enabled=p.protection_holding&&p.protection_verified;s.group_guard_ready=s.protection_enabled;s.first_close_sequence=p.first_close_sequence;s.first_close_context=p.first_close_context_id;s.first_close_caller=p.first_close_caller;s.first_close_object=reinterpret_cast<uint64_t>(p.first_close_object);s.first_close_last_allocated=p.first_close_last_allocated_sequence;s.first_close_known_flags=p.first_close_known_flags;s.first_close_operation=p.first_close_operation;s.first_close_origin=p.first_close_origin;if(!p.healthy||p.error||p.in_flight||p.pending_creations)s.main_ready=0;}
 if(auto core=std::atomic_load(&h.full)){auto state=core->Snapshot();s.user_original=state.original;s.acknowledged=state.ack_sequence;s.frames=state.frame;s.pairs=state.pairs;s.replaced_draws=state.replaced_draws;s.stopping|=state.stopping||state.unsafe;s.comparison_ready=s.registered&&s.input_ready&&s.main_ready&&s.group_guard_ready&&s.profile_ready&&state.pairs&&state.views&&state.bindings&&!s.stopping;}

 return output&&WriteSafe(output,&s,sizeof s)?0:ERROR_NOACCESS;}catch(...){return ERROR_INVALID_DATA;}}
BOOL WINAPI DllMain(HINSTANCE m,DWORD reason,void*){if(reason==DLL_PROCESS_ATTACH){self_module=m;DisableThreadLibraryCalls(m);}return TRUE;}

#include "proxy_addon.hpp"
