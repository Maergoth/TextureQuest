#pragma once
// Genuine ABI2 consumer. Platform code proves all module/table addresses first.
#include "texturequest_provider_mutation_v2.hpp"
#include <atomic>
#include <cstring>
#include <stdexcept>
namespace tqph {
class Session {
 const tqpm::Api* api_=nullptr;void* module_=nullptr;uint64_t token_=0;uint8_t nonce_[16]{};
 std::atomic<uint32_t> first_{0};std::atomic<bool> selected_{false};std::atomic<uint64_t> last_notice_{0};
 std::atomic<const void*> device_{nullptr},context_{nullptr};
public:
 enum Fault:uint32_t {shape=3001,status=3002,selection=3003,scope=3004,notice=3005,contention=3006,mutation=3007,creation=3008,native=3009,runtime=3010};
 void Close(uint32_t reason)noexcept{uint32_t zero=0;first_.compare_exchange_strong(zero,reason);if(api_&&token_)api_->close(token_);}
 bool Closed()const noexcept{return first_.load()!=0;}uint32_t First()const noexcept{return first_.load();}
 uint64_t Token()const noexcept{return token_;}const void* Device()const noexcept{return device_.load();}const void* Context()const noexcept{return context_.load();}
 const tqpm::Api* Api()const noexcept{return api_;}
 bool Register(const tqpm::Api* api,void* module,const uint8_t nonce[16],const tqpm::Callbacks& callbacks,void* cookie){
  if(api_||Closed()||!api||api->size!=80||api->abi_version!=2||api->capabilities!=2047||!module){Close(shape);return false;}
  api_=api;module_=module;std::memcpy(nonce_,nonce,16);
  tqpm::Registration r{sizeof(r),2,tqpm::capabilities,module,cookie,{},callbacks};std::memcpy(r.nonce,nonce,16);
  token_=api_->register_observer(&r);return token_&&Health(false,false,false);
 }
 bool Read(tqpm::Status& s)noexcept{
  s={};s.size=sizeof(s);return api_&&api_->read_status(&s)==1&&s.size==200&&s.abi_version==2;
 }
 bool Health(bool bound=true,bool pending=false,bool callback=false,bool ambient=false)noexcept{
  if(Closed())return false;tqpm::Status s{};
  if(!Read(s)||!s.registered||!s.healthy||s.error||s.native_bypass_gap!=1||s.capabilities!=2047||s.owner_token!=token_||
     s.observer_module!=module_||std::memcmp(s.nonce,nonce_,16)||(!ambient&&s.in_flight!=(callback?1u:0u))||(!pending&&s.pending_creations)||
     (bound&&(!selected_||!s.bound||!s.context_id||s.api_device!=device_||s.api_context!=context_||!s.protection_holding||!s.protection_verified||s.protection_device!=device_||s.protection_context!=context_))){Close(status);return false;}return true;
 }
 class Use {
  Session* s_=nullptr;uint64_t serial_=0;
 public:
  Use(Session& s,const void* target,tqpm::Use purpose,bool allow_pending=false,bool callback=false,bool ambient=false):s_(&s){
   if(s.Health(s.selected_.load(),allow_pending,callback,ambient))serial_=s.api_->begin_use(s.token_,target,purpose);
   if(!serial_)s.Close(scope);
  }
  ~Use(){End();}Use(const Use&)=delete;Use& operator=(const Use&)=delete;
  explicit operator bool()const noexcept{return serial_!=0;}uint64_t Serial()const noexcept{return serial_;}
  bool End()noexcept{if(!serial_)return true;auto n=serial_;serial_=0;if(s_->api_->end_use(s_->token_,n)!=1){s_->Close(scope);return false;}return true;}
 };
 template<class Capture>bool Select(const void* device,const void* context,Capture capture){
  if(selected_||Closed()||!device||!context){Close(selection);return false;}
  Use use(*this,context,tqpm::Use::identity_read);if(!use)return false;
  tqpm::NativeOwner pair{};pair.size=sizeof(pair);
  if(api_->read_native_owner(token_,use.Serial(),context,&pair)!=1||pair.size!=48||pair.abi_version!=2||pair.context_kind!=1||pair.retained!=1||pair.api_device!=device||pair.api_context!=context||!pair.native_device||!pair.native_context||!capture(pair)||api_->acquire_protection(token_,use.Serial(),context)!=1){Close(selection);return false;}
  if(!use.End())return false;
  device_=device;context_=context;
  if(api_->bind_context(token_,context)!=1){Close(selection);return false;}selected_=true;return Health();
 }
 bool Notice(const tqpm::Header& h)noexcept{
  if(!Health(selected_.load(),false,true)||h.size!=64||h.abi_version!=2||h.owner_token!=token_||!h.api_device||
     (selected_&&(h.api_device!=device_||(h.api_context&&h.api_context!=context_)))||(!h.sequence)||h.phase!=tqpm::Phase::before_native){Close(notice);return false;}
  uint64_t prior=last_notice_.load();if(h.sequence<=prior||!last_notice_.compare_exchange_strong(prior,h.sequence)){Close(notice);return false;}return true;
 }
};
} // namespace tqph
