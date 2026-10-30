#pragma once
#include "provider_session.hpp"
#include "engine.hpp"
#include "default_policy.hpp"
#include "texturequest_creation_policy.hpp"
#include <mutex>
#include <unordered_map>
namespace tqph {
// These are generation observations, not permanent COM ownership. A draw
// acquires live strong references under its context lease before rebinding.
class Resources {
public:
 struct ViewLifetime;
 struct Lease {private:friend class Resources;friend struct ViewLifetime;const Resources* authority_=nullptr;
 public:uint64_t handle=0,canonical=0,generation=0;std::shared_ptr<void> native;
  tqpc::ResourceEpoch epoch;std::atomic<bool> selected{false},destroyed{false},engine_dirty{false};};
 struct ViewLifetime final:tqab::ViewAdmission {
 private:friend class Resources;const Resources* authority_=nullptr;Session* session_=nullptr;std::shared_ptr<Lease> row_;
 public:uint64_t handle=0,resource=0,generation=0;std::shared_ptr<void> native;std::atomic<bool> destroyed{false},published{false};
  bool Valid(uint64_t h)const noexcept override{return authority_&&session_&&!session_->Closed()&&h&&h==handle&&native&&row_&&row_->authority_==authority_&&row_->generation==generation&&row_->handle==resource&&!destroyed&&!row_->destroyed&&!row_->epoch.Retired();}
  uint64_t Resource()const noexcept override{return resource;}
 };
private:
 Session& session_;mutable std::mutex mutex_;uint64_t generation_=0;
 std::unordered_map<uint64_t,std::shared_ptr<Lease>> rows_,canonical_;
 std::unordered_map<uint64_t,std::shared_ptr<ViewLifetime>> views_;
public:
 explicit Resources(Session&s):session_(s){}
 void Refuse()noexcept{session_.Close(Session::mutation);}
 std::shared_ptr<Lease> Register(uint64_t h,uint64_t canonical,std::shared_ptr<void> native,bool* capacity=nullptr){
  if(capacity)*capacity=false;if(!h||!canonical||!native)return {};auto r=std::make_shared<Lease>();r->handle=h;r->canonical=canonical;r->native=std::move(native);
  std::lock_guard lock(mutex_);if(rows_.size()>=100000){if(capacity)*capacity=true;return {};}if(generation_==UINT64_MAX||rows_.count(h)||canonical_.count(canonical))return {};
  r->authority_=this;r->generation=++generation_;rows_.emplace(h,r);canonical_.emplace(canonical,r);return r;
 }
 std::shared_ptr<Lease> Find(uint64_t h){std::lock_guard lock(mutex_);auto i=rows_.find(h);return i==rows_.end()?nullptr:i->second;}
 std::shared_ptr<Lease> FindCanonical(uint64_t h){std::lock_guard lock(mutex_);auto i=canonical_.find(h);return i==canonical_.end()?nullptr:i->second;}
 bool Valid(const std::shared_ptr<Lease>&r,uint64_t h,uint64_t generation,uint64_t epoch)const noexcept{return r&&r->authority_==this&&generation&&r->handle==h&&r->generation==generation&&!r->destroyed&&r->epoch.Valid(epoch)&&!session_.Closed();}
 bool Select(const std::shared_ptr<Lease>&r,uint64_t generation,uint64_t epoch)noexcept{if(!Valid(r,r?r->handle:0,generation,epoch))return false;r->selected=true;return Valid(r,r->handle,generation,epoch);}
 void Retire(const std::shared_ptr<Lease>&r)noexcept{if(r){r->epoch.Dirty();r->engine_dirty=true;}}
 std::vector<std::shared_ptr<Lease>> Rows(){std::lock_guard lock(mutex_);std::vector<std::shared_ptr<Lease>> result;for(auto&r:rows_)result.push_back(r.second);return result;}
 std::vector<std::shared_ptr<ViewLifetime>> Views(){std::lock_guard lock(mutex_);std::vector<std::shared_ptr<ViewLifetime>> result;for(auto&v:views_)result.push_back(v.second);return result;}
 std::shared_ptr<tqab::ViewAdmission> Binding(uint64_t h){std::lock_guard lock(mutex_);auto i=views_.find(h);return i!=views_.end()&&i->second->Valid(h)?i->second:nullptr;}
 bool RememberView(uint64_t view,uint64_t resource,std::shared_ptr<void> observed={}){
  std::lock_guard lock(mutex_);auto i=rows_.find(resource);if(i==rows_.end()||i->second->destroyed||i->second->epoch.Retired())return true;
  if(!view||!observed||views_.size()>=400000)return false;auto old=views_.find(view);if(old!=views_.end())return old->second->resource==resource&&old->second->generation==i->second->generation;
  auto v=std::make_shared<ViewLifetime>();v->authority_=this;v->session_=&session_;v->row_=i->second;v->generation=i->second->generation;v->handle=view;v->resource=resource;v->native=std::move(observed);views_.emplace(view,std::move(v));return true;
 }
 template<class Opaque,class Notify>bool Destruction(bool view,Opaque opaque,Notify notify){
  const auto key=opaque();std::shared_ptr<Lease> dirty;
  {std::lock_guard lock(mutex_);if(view){auto i=views_.find(key);if(i!=views_.end()){i->second->destroyed=true;views_.erase(i);}}
   else {auto i=rows_.find(key);if(i!=rows_.end()){dirty=i->second;dirty->destroyed=true;dirty->epoch.Dirty(true);canonical_.erase(dirty->canonical);rows_.erase(i);
     for(auto v=views_.begin();v!=views_.end();)if(v->second->resource==key&&v->second->generation==dirty->generation){v->second->destroyed=true;v=views_.erase(v);}else ++v;}}}
  if(dirty){dirty->engine_dirty=true;return notify(dirty->handle);}return true;
 }
 template<class Resolve,class Notify>bool Mutation(bool selected,Resolve resolve,Notify notify){
  if(!selected)return true;std::vector<uint64_t> ids;if(!resolve(ids)){Refuse();return false;}
  for(auto id:ids){auto row=FindCanonical(id);if(row){Retire(row);if(!notify(row->handle))return false;}}return !session_.Closed();
 }
 bool SdkDirty(uint64_t key,bool view){std::shared_ptr<Lease> row;{std::lock_guard lock(mutex_);if(view){auto v=views_.find(key);if(v!=views_.end()){auto r=rows_.find(v->second->resource);if(r!=rows_.end()&&r->second->generation==v->second->generation)row=r->second;}}else {auto r=rows_.find(key);if(r!=rows_.end())row=r->second;}}Retire(row);return !session_.Closed();}
};
template<class Resolve,class Notify>bool ObserveSdk(Resources&rows,const void*selected,const void*actual,bool wide,Resolve resolve,Notify notify){if(!selected)return true;if(!actual){rows.Refuse();return false;}if(actual!=selected)return true;if(wide){rows.Refuse();return false;}return rows.Mutation(true,resolve,notify);}
template<class Opaque,class Notify>bool ObserveSdkDestruction(Resources&rows,const void*selected,const void*actual,bool view,Opaque opaque,Notify notify){if(!selected)return true;if(!actual){rows.Refuse();return false;}if(actual!=selected)return true;return rows.Destruction(view,opaque,notify);}
}
