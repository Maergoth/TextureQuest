// Included once after the genuine mutation boundary definitions, in the same TU.
#include "texturequest_provider_creation.hpp"
#include <limits>
#include <mutex>
namespace {
tqpc::Registration creation_observer{};std::atomic<bool> creation_registered{false};
std::atomic_flag creation_once=ATOMIC_FLAG_INIT;tqpc::CallSlots creation_slots;
std::mutex creation_slots_mutex;
thread_local tqpc::Frame* creation_current=nullptr;
std::atomic<uint64_t> creation_counts[13]{};
std::atomic<uint32_t> creation_first{0};tqpc::Diagnostics creation_witness{};
bool CreationAuthorized(uint64_t token,const void* caller)noexcept{return creation_registered.load()&&token==core.Snapshot().owner_token&&Module(caller)==creation_observer.observer_module;}
void CreationFault(tqpc::Frame* f,uint32_t reason)noexcept{
 uint32_t zero=0;if(creation_first.compare_exchange_strong(zero,0xffffffffu)){creation_witness.first_reason=reason;
  // Known bits: call1, parent2, thread4, factory8, phase16. An early frame knows only its phase.
  if(f){creation_witness.first_call=f->info.call;creation_witness.first_parent=f->info.parent;creation_witness.first_thread=f->info.thread;creation_witness.first_factory=uint32_t(f->info.factory);creation_witness.first_phase=uint32_t(f->phase);creation_witness.first_known=f->info.size==sizeof(tqpc::Info)&&f->info.abi_version==tqpc::version?31u:16u;}
  creation_first.store(reason,std::memory_order_release);}
 creation_counts[12].fetch_add(1);core.Close(tqpm::Error::creation_overlap,f?f->info.api_device:nullptr);
}
uint32_t CreationCapture(tqpc::Frame& f)noexcept{__try{return creation_observer.callbacks.capture(creation_observer.cookie,&f.info,&f.ticket);}__except(EXCEPTION_EXECUTE_HANDLER){return 2;}}
uint32_t CreationNative(tqpc::Frame& f,const tqpc::NativeResult& r)noexcept{__try{return creation_observer.callbacks.native_result(creation_observer.cookie,f.ticket,&r);}__except(EXCEPTION_EXECUTE_HANDLER){return 0;}}
uint32_t CreationSettle(tqpc::Frame& f,const tqpc::Settlement& r)noexcept{__try{return creation_observer.callbacks.settle(creation_observer.cookie,f.ticket,&r);}__except(EXCEPTION_EXECUTE_HANDLER){return 0;}}
uint64_t TQPM_CALL RegisterCreation(const tqpc::Registration* input)noexcept{
 tqpc::Registration r{};auto s=core.Snapshot();if(!Read(&r,input,sizeof r)||r.size!=80||r.abi_version!=1||r.required_capabilities!=15||!s.registered||!core.CreationRegistrationOpen()||r.observer_module!=observer.observer_module||Module(_ReturnAddress())!=r.observer_module||std::memcmp(r.nonce,s.nonce,16)||r.callbacks.size!=32||r.callbacks.abi_version!=1)return 0;
 if(Module(reinterpret_cast<const void*>(r.callbacks.capture))!=r.observer_module||Module(reinterpret_cast<const void*>(r.callbacks.native_result))!=r.observer_module||Module(reinterpret_cast<const void*>(r.callbacks.settle))!=r.observer_module||creation_once.test_and_set())return 0;
 creation_observer=r;creation_registered.store(true,std::memory_order_release);return s.owner_token;
}
uint64_t TQPM_CALL CreationIdentity(uint64_t token,uint64_t call)noexcept{
 auto f=creation_current;if(!f||!f->matched||f->phase!=tqpc::Phase::native_result||f->info.call!=call||!CreationAuthorized(token,_ReturnAddress()))return 0;
 return core.BeginUse(reinterpret_cast<uintptr_t>(creation_observer.observer_module),token,f->info.api_device,tqpm::Use::identity_read);
}
uint32_t TQPM_CALL CreationDiagnostics(uint64_t token,tqpc::Diagnostics* output)noexcept{
 uint32_t size=0;if(!CreationAuthorized(token,_ReturnAddress())||!Read(&size,output,sizeof size)||size!=184)return 0;
 tqpc::Diagnostics d{};d.size=184;d.abi_version=1;d.owner_token=token;d.observer_module=creation_observer.observer_module;std::memcpy(d.nonce,creation_observer.nonce,16);
 d.active=creation_counts[0].load();d.started=creation_counts[1].load();d.settled=creation_counts[2].load();d.failed=creation_counts[3].load();d.cancelled=creation_counts[4].load();d.eligible=creation_counts[5].load();d.nonmatching=creation_counts[6].load();d.matching=creation_counts[7].load();d.queued=creation_counts[8].load();d.admitted=creation_counts[9].load();d.dirty_cancelled=creation_counts[10].load();d.capacity_faults=creation_counts[11].load();d.faults=creation_counts[12].load();
 const auto reason=creation_first.load(std::memory_order_acquire);if(reason&&reason!=0xffffffffu){d.first_call=creation_witness.first_call;d.first_parent=creation_witness.first_parent;d.first_thread=creation_witness.first_thread;d.first_factory=creation_witness.first_factory;d.first_phase=creation_witness.first_phase;d.first_reason=creation_witness.first_reason;d.first_known=creation_witness.first_known;}
 return Read(output,&d,sizeof d)?1u:0u;
}
uint32_t TQPM_CALL CreationHostEvent(uint64_t token,tqpc::HostEvent event)noexcept{if(!CreationAuthorized(token,_ReturnAddress()))return 0;auto n=uint32_t(event);if(n<1||n>4)return 0;creation_counts[n==1?8:n==2?9:n==3?10:11].fetch_add(1);return 1;}
void TQPM_CALL CreationClose(uint64_t token,uint32_t reason)noexcept{if(CreationAuthorized(token,_ReturnAddress()))CreationFault(creation_current,reason?reason:100);}
const tqpc::Api creation_api{56,1,15,RegisterCreation,CreationIdentity,CreationDiagnostics,CreationHostEvent,CreationClose};
}
namespace tqpc {
void Begin(Frame& f,const void* device,Factory factory)noexcept{
 f.exceptions=std::uncaught_exceptions();auto s=core.Snapshot();f.observed=s.registered&&s.healthy&&s.bound&&s.api_device==device;
 if(!f.observed)return;if(!creation_registered.load()){CreationFault(&f,101);f.observed=false;return;}
 f.parent=creation_current;f.info={128,1,creation_slots.Next(),f.parent?f.parent->info.call:0,GetCurrentThreadId(),s.owner_token,s.context_id,device,factory,factory==Factory::sdk_resource?0u:1u,{},nullptr,0,0};
 unsigned depth=0;for(auto p=f.parent;p;p=p->parent)++depth;
 if(!f.info.call||depth>=32){CreationFault(&f,102);f.observed=false;return;}
 creation_current=&f;creation_counts[0].fetch_add(1);creation_counts[1].fetch_add(1);
}
void Prepare(Frame& f,const Descriptor& d,const Subresource* initial)noexcept{
 if(!f.observed)return;if(f.prepared||creation_current!=&f){CreationFault(&f,103);return;}f.prepared=true;f.info.descriptor=d;f.info.initial=initial;
 // Exact ordinary nonshared BC 2D class; format/layout/content matching remains host authority.
 if(!initial||!d.width||!d.height||!d.mips||d.mips>15||d.layers!=1||d.samples!=1||d.quality||d.cpu||d.misc||d.texture_layout||d.bind!=D3D11_BIND_SHADER_RESOURCE||(d.usage!=D3D11_USAGE_DEFAULT&&d.usage!=D3D11_USAGE_IMMUTABLE)||d.format<70||d.format>78)return;
 f.info.initial_count=d.mips;creation_counts[5].fetch_add(1);
 {std::lock_guard lock(creation_slots_mutex);f.reserved=creation_slots.Reserve(f.info.call);}
 if(!f.reserved){creation_counts[11].fetch_add(1);CreationFault(&f,104);return;}
 auto result=CreationCapture(f);if(result>1||((result==1)!=(f.ticket!=nullptr))){CreationFault(&f,105);return;}f.matched=result==1;creation_counts[f.matched?7:6].fetch_add(1);
}
void Result(Frame& f,int32_t hr,void* resource)noexcept{
 if(!f.observed)return;if(creation_current!=&f||f.native_returned){CreationFault(&f,106);return;}f.native_returned=true;f.outcome=hr>=0?Outcome::succeeded:Outcome::failed;f.phase=Phase::native_result;
 if(f.matched){NativeResult r{32,1,&f.info,hr,1,resource};if(CreationNative(f,r)!=1)CreationFault(&f,107);}
}
void Finish(Frame& f)noexcept{
 if(!f.observed)return;if(f.settled||creation_current!=&f){CreationFault(&f,108);return;}f.settled=true;f.phase=Phase::settle;
 if(std::uncaught_exceptions()!=f.exceptions||(f.native_called&&!f.native_returned)){f.outcome=Outcome::unknown;CreationFault(&f,109);}
 if(f.matched&&f.outcome==Outcome::succeeded&&f.init_seen!=f.info.expected_init)CreationFault(&f,110);
 if(f.matched){Settlement s{24,1,&f.info,f.outcome,f.init_seen};if(CreationSettle(f,s)!=1)CreationFault(&f,111);}
 if(f.reserved){bool released=false;{std::lock_guard lock(creation_slots_mutex);released=creation_slots.Release(f.info.call);}if(!released)CreationFault(&f,112);}
 creation_counts[2].fetch_add(1);if(f.native_returned&&f.outcome==Outcome::failed)creation_counts[3].fetch_add(1);if(f.outcome!=Outcome::succeeded)creation_counts[4].fetch_add(1);
 creation_counts[0].fetch_sub(1);creation_current=f.parent;
}
}
extern "C" __declspec(dllexport) const tqpc::Api* TQPM_CALL TQPC_GetInterfaceV1(uint32_t abi,uint32_t size)noexcept{return abi==1&&size==56?&creation_api:nullptr;}
