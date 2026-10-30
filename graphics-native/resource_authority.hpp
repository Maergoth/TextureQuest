#pragma once
#include "provider_session.hpp"
#include "pending_authority.hpp"
#include "default_policy.hpp"
#include <memory>
#include <vector>
namespace tqph {
// Registry mutex is NEVER waited on by a provider callback, admission or draw.
class Resources {
public:
 struct Creation {tqdefault::PendingTicket ticket;tqdefault::Stamp stamp;bool consumed=false;};
 struct Lease {uint64_t handle=0,canonical=0;std::shared_ptr<void> native;std::atomic<bool> retired{false},selected{false},destroyed{false};};
private:
 // Opaque associations contain no COM owner. Capture them only while the view
 // and resource are live; tombstones retain history without releasing objects.
 struct ViewLifetime {uint64_t handle=0,resource=0;bool destroyed=false;};
 Session& session_;mutable std::mutex mutex_;tqdefault::PendingAuthority pending_;
 std::vector<std::unique_ptr<Creation>> creates_;std::vector<std::shared_ptr<Lease>> rows_;std::vector<ViewLifetime> views_;
 std::unique_lock<std::mutex> Lock(){std::unique_lock<std::mutex> l(mutex_,std::try_to_lock);if(!l.owns_lock())session_.Close(Session::contention);return l;}
public:
 explicit Resources(Session& s):session_(s){}
 void Refuse()noexcept{session_.Close(Session::mutation);}
 Creation* Begin(const tqdefault::Stamp& stamp){auto lock=Lock();if(!lock.owns_lock()||session_.Closed())return nullptr;
  if(creates_.size()>=128){session_.Close(Session::creation);return nullptr;}auto c=std::make_unique<Creation>();c->stamp=stamp;
  if(!pending_.Begin(c->ticket)){session_.Close(Session::creation);return nullptr;}auto p=c.get();creates_.push_back(std::move(c));return p;
 }
 bool Consume(Creation* c,const tqdefault::Stamp& stamp){auto lock=Lock();if(!lock.owns_lock()||session_.Closed())return false;
  bool known=false;for(auto& v:creates_)known|=v.get()==c;
  if(!known||c->consumed||!(c->stamp==stamp)||!pending_.Consume(c->ticket)){session_.Close(Session::creation);return false;}c->consumed=true;return true;
 }
 std::shared_ptr<Lease> Commit(Creation* c,uint64_t h,uint64_t canonical,std::shared_ptr<void> native){auto lock=Lock();if(!lock.owns_lock()||session_.Closed())return {};
  if(!h||!canonical||!native||rows_.size()>=64||!pending_.CanCommit(c->ticket)){session_.Close(Session::creation);return {};}
  for(auto& r:rows_)if(r->handle==h||r->canonical==canonical){session_.Close(Session::creation);return {};}
  auto r=std::make_shared<Lease>();r->handle=h;r->canonical=canonical;r->native=std::move(native);rows_.push_back(r);
  if(!pending_.Commit(c->ticket)){session_.Close(Session::creation);return {};}return r;
 }
 std::shared_ptr<Lease> Find(uint64_t h){auto lock=Lock();if(!lock.owns_lock()||session_.Closed())return {};for(auto& r:rows_)if(r->handle==h&&!r->destroyed)return r;return {};}
 bool Valid(const std::shared_ptr<Lease>& r,uint64_t h)const noexcept{return r&&r->handle==h&&!r->retired&&!r->destroyed&&!session_.Closed();}
 bool Select(const std::shared_ptr<Lease>& r)noexcept{if(!Valid(r,r?r->handle:0))return false;r->selected=true;return Valid(r,r->handle);}
 void Retire(const std::shared_ptr<Lease>& r)noexcept{if(r)r->retired=true;}
 // Called after NativePort's live DescribeView identity/owner validation. Only
 // already committed DEFAULT resources acquire a destruction association.
 bool RememberView(uint64_t view,uint64_t resource){auto lock=Lock();if(!lock.owns_lock()||session_.Closed())return false;
  if(pending_.Observe(true)){session_.Close(Session::mutation);return false;}
  bool known=false;for(auto& r:rows_)if(r->handle==resource&&!r->destroyed)known=true;if(!known)return true;
  if(!view){session_.Close(Session::creation);return false;}
  for(auto& v:views_)if(v.handle==view&&!v.destroyed){if(v.resource==resource)return true;session_.Close(Session::creation);return false;}
  if(views_.size()>=256){session_.Close(Session::creation);return false;}views_.push_back({view,resource,false});return true;
 }
 // Destruction is NOT a valid pre-write identity query. No native pointer is
 // accepted here: even the opaque integer accessor runs only after pending.
 // Keep all existing native owners; never reset/release them in this callback.
 template<class Opaque,class Notify>bool Destruction(bool view,Opaque opaque,Notify notify){
  uint64_t dirty=0;
  {auto lock=Lock();if(!lock.owns_lock()||session_.Closed())return false;
   if(pending_.Observe(true)){session_.Close(Session::mutation);return false;}
   const uint64_t key=opaque();
   if(view){for(auto& v:views_)if(v.handle==key&&!v.destroyed){v.destroyed=true;dirty=v.resource;break;}}
   else for(auto& r:rows_)if(r->handle==key&&!r->destroyed){r->destroyed=true;dirty=r->handle;for(auto& v:views_)if(v.resource==dirty)v.destroyed=true;break;}
   if(dirty)for(auto& r:rows_)if(r->handle==dirty)r->retired=true;
  } // Registry admission ends before any nonblocking Adapter/Engine notice.
  if(dirty&&!notify(dirty)){session_.Close(Session::contention);return false;}return !session_.Closed();
 }
 // Pending observation is intentionally BEFORE individual destination resolution.
 template<class Resolve,class Notify>bool Mutation(bool same_device,Resolve resolve,Notify notify){
  if(!same_device)return true;std::vector<uint64_t> dirty;
  {auto lock=Lock();if(!lock.owns_lock()||session_.Closed())return false;
   if(pending_.Observe(true)){session_.Close(Session::mutation);return false;}
   std::vector<uint64_t> canonical;if(!resolve(canonical)){session_.Close(Session::mutation);return false;}
   for(auto c:canonical)for(auto& r:rows_)if(r->canonical==c){r->retired=true;dirty.push_back(r->handle);}
  } // No registry-to-Engine blocking chain. Native MT is never acquired here.
  for(auto h:dirty)if(!notify(h)){session_.Close(Session::contention);return false;}return !session_.Closed();
 }
};
// Shared early SDK pre-write policy. API owner is supplied by the genuine SDK
// callback. Canonical native reads occur only after pending-before-key admission.
template<class Resolve,class Notify>bool ObserveSdk(Resources& rows,const void* selected,const void* actual,bool device_wide,Resolve resolve,Notify notify){
 if(!selected)return true;if(!actual){rows.Refuse();return false;}if(actual!=selected)return true;
 return rows.Mutation(true,[&](std::vector<uint64_t>& ids){return !device_wide&&resolve(ids);},notify);
}
// Shared production destruction frontend: selected API-owner equality only.
// Foreign/unknown handles cannot acquire canonical or selected-object authority.
template<class Opaque,class Notify>bool ObserveSdkDestruction(Resources& rows,const void* selected,const void* actual,bool view,Opaque opaque,Notify notify){
 if(!selected)return true;if(!actual){rows.Refuse();return false;}if(actual!=selected)return true;
 return rows.Destruction(view,opaque,notify);
}
} // namespace tqph
