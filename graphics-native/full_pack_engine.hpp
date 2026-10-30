#pragma once
#include "full_pack_port.hpp"
#include "resource_registry.hpp"
#include "full_catalog.hpp"
#include <unordered_map>

namespace tqgx {
using namespace tqab;
// Released textures are canonical, supplied by the existing named file layer.
// Upscaled mode forwards untouched draws. Only Original comparison needs a
// native draw lease or a temporary alternate SRV.
class FullPackEngine {
 struct Source {const Row* row=nullptr;std::shared_ptr<tqph::Resources::Lease> life;uint64_t epoch=0;};
 struct Bound {uint32_t stage=0,slot=0;uint64_t view=0;std::shared_ptr<tqab::ViewAdmission> life;};
 struct Wanted {const Row* row=nullptr;uint64_t last=0;std::weak_ptr<tqph::Resources::Lease> life;uint64_t epoch=0;};
 struct Shadow {const Row* row=nullptr;std::shared_ptr<Texture> texture;std::map<uint32_t,std::shared_ptr<View>> views;uint64_t bytes=0,last=0;};
 struct Deferred {std::shared_ptr<void> fence;std::vector<std::shared_ptr<View>> views;std::vector<std::shared_ptr<Texture>> textures;uint64_t bytes=0;};
 std::shared_ptr<const Catalog> catalog_;std::shared_ptr<FullPackPort> port_;tqph::Resources& registry_;
 std::mutex mutex_;std::unordered_map<uint64_t,Source> sources_;std::map<uint32_t,Bound> bound_;std::map<std::string,Wanted> wanted_;
 std::unordered_map<std::string,Shadow> shadows_;std::vector<Deferred> retired_;
 std::atomic<bool> original_{false},dual_{false},failed_{false};bool scan_=true;
 uint64_t frame_=0,ack_=0,replaced_=0,gpu_bytes_=0,misses_=0,gpu_textures_=0;static constexpr uint64_t Budget=256*MiB;
 std::vector<uint32_t> Formats(uint32_t family){switch(family){case 1:return{71,72};case 2:return{74,75};case 3:return{77,78};case 4:return{87,91};case 5:return{88,93};case 6:return{61};case 7:return{31};}return{};}
 uint32_t Storage(uint32_t family){switch(family){case 1:return 70;case 2:return 73;case 3:return 76;case 4:return 90;case 5:return 92;case 6:return 61;case 7:return 31;}return 0;}
 void Fault()noexcept{failed_=true;port_->Poison();}
 void Want(const std::shared_ptr<tqab::ViewAdmission>&life){if(!life)return;auto source=sources_.find(life->Resource());if(source!=sources_.end()&&source->second.life->epoch.Valid(source->second.epoch))wanted_[source->second.row->stock_sha256]={source->second.row,frame_,source->second.life,source->second.epoch};}
 Deferred Take(std::unordered_map<std::string,Shadow>::iterator i){
  Deferred batch;batch.bytes=i->second.bytes;for(auto&v:i->second.views)batch.views.push_back(std::move(v.second));batch.textures.push_back(std::move(i->second.texture));shadows_.erase(i);return batch;
 }
 void Fence(Deferred batch){
  // No metadata mutex spans native context acquisition or object release.
  batch.fence=port_->Fence();bool okay=bool(batch.fence);{std::lock_guard lock(mutex_);retired_.push_back(std::move(batch));}require(okay,"graphics retirement fence");
 }
 void Reap(){
  std::vector<Deferred> pending;{std::lock_guard lock(mutex_);pending.swap(retired_);}
  for(auto&i:pending){auto state=i.fence?port_->FenceReady(i.fence):-1;
   if(state==1){port_->ForgetTargets(i.views,i.textures);{std::lock_guard lock(mutex_);require(gpu_bytes_>=i.bytes&&gpu_textures_,"graphics GPU accounting");gpu_bytes_-=i.bytes;--gpu_textures_;}i.views.clear();i.textures.clear();}
   else {std::lock_guard lock(mutex_);retired_.push_back(std::move(i));if(state<0)failed_=true;}
  }require(!failed_,"graphics retirement outcome UNKNOWN");
 }
public:
 FullPackEngine(std::shared_ptr<const Catalog> catalog,std::shared_ptr<FullPackPort> port,tqph::Resources&registry,bool dual):catalog_(std::move(catalog)),port_(std::move(port)),registry_(registry),dual_(dual){catalog_->PayloadEnabled(dual);}
 bool WantsTracking()const noexcept{return !failed_&&(original_||dual_);}
 void Admit(const Row* row,const std::shared_ptr<tqph::Resources::Lease>& life){std::lock_guard lock(mutex_);require(row&&life&&!life->epoch.Retired(),"graphics settled creation lifetime");sources_[life->handle]={row,life,life->epoch.Value()};}
 void Bind(uint32_t stages,uint32_t first,const uint64_t* handles,size_t count){
  if(!WantsTracking()||!handles||first>=128||count>128-first||!stages||(stages&~31u))return;
  std::lock_guard lock(mutex_);for(uint32_t stage=1;stage<=16;stage<<=1)if(stages&stage)for(size_t i=0;i<count;++i){uint32_t key=stage*128+first+uint32_t(i);bound_.erase(key);auto life=registry_.Binding(handles[i]);if(life){Want(life);bound_.emplace(key,Bound{stage,first+uint32_t(i),handles[i],std::move(life)});}}
 }
 void Present(uint64_t request){
  if(failed_)return;try{
   Reap();if(catalog_->PayloadFailed())throw std::runtime_error("graphics disk payload failed verification");
   std::map<std::string,const Row*> wanted;std::vector<Deferred> evicted;
   {std::lock_guard lock(mutex_);++frame_;if((request>>1)>ack_){original_=(request&1)!=0;ack_=request>>1;scan_=true;catalog_->PayloadEnabled(original_||dual_);}
    if(!original_&&!dual_){while(!shadows_.empty())evicted.push_back(Take(shadows_.begin()));bound_.clear();wanted_.clear();}
    else {for(auto i=bound_.begin();i!=bound_.end();){if(!i->second.life||!i->second.life->Valid(i->second.view)){i=bound_.erase(i);continue;}Want(i->second.life);++i;}
     for(auto i=wanted_.begin();i!=wanted_.end();){auto life=i->second.life.lock();if(!life||!life->epoch.Valid(i->second.epoch)||frame_-i->second.last>120){i=wanted_.erase(i);continue;}wanted.emplace(i->first,i->second.row);++i;}}
    if(frame_%120==0)for(auto i=sources_.begin();i!=sources_.end();)if(!i->second.life->epoch.Valid(i->second.epoch))i=sources_.erase(i);else ++i;
   }
   for(auto&batch:evicted)Fence(std::move(batch));
   unsigned created=0;for(auto& item:wanted){
    {std::lock_guard lock(mutex_);auto existing=shadows_.find(item.first);if(existing!=shadows_.end()){existing->second.last=frame_;continue;}}
    if(created>=2)break;auto bytes=catalog_->Payload(*item.second);if(!bytes){std::lock_guard lock(mutex_);++misses_;continue;}uint64_t cost=bytes->size();bool admitted=false;Deferred victim;
    {std::lock_guard lock(mutex_);if(cost<=Budget-gpu_bytes_&&gpu_textures_<2048){gpu_bytes_+=cost;++gpu_textures_;admitted=true;}else{auto which=shadows_.end();for(auto i=shadows_.begin();i!=shadows_.end();++i)if(!wanted.count(i->first)&&(which==shadows_.end()||i->second.last<which->second.last))which=i;if(which!=shadows_.end())victim=Take(which);}}
    if(!victim.textures.empty())Fence(std::move(victim));if(!admitted)continue;
    Shadow shadow{item.second,{}, {},cost,frame_};
    try{shadow.texture=port_->CreateStock(item.second->stock,*bytes,Storage(item.second->stock.family));require(bool(shadow.texture),"graphics comparison allocation");
     for(auto format:Formats(item.second->stock.family)){auto view=port_->CreateView(shadow.texture,{format,0,item.second->stock.mips,1});require(bool(view),"graphics comparison view");shadow.views.emplace(format,std::move(view));}}
    catch(...){Deferred keep;keep.bytes=cost;if(shadow.texture)keep.textures.push_back(shadow.texture);for(auto&v:shadow.views)keep.views.push_back(v.second);{std::lock_guard lock(mutex_);retired_.push_back(std::move(keep));}throw;}
    {std::lock_guard lock(mutex_);shadows_.emplace(item.first,std::move(shadow));}++created;
   }
  }catch(...){Fault();}
 }
 bool Draw(reshade::api::command_list* command,bool indexed,uint32_t count,uint32_t instances,uint32_t first,int32_t offset,uint32_t first_instance)noexcept{
  // Crucial full-pack fast path: no context lock, health call, COM query,
  // allocation, descriptor getter, or extra draw in canonical Upscaled mode.
  if(!original_||failed_||!count||instances!=1||first_instance)return false;
  bool invoked=false;try{
   {std::lock_guard lock(mutex_);if(!scan_){bool possible=false;for(const auto&item:bound_){const auto&b=item.second;if(!b.life||!b.life->Valid(b.view))continue;auto source=sources_.find(b.life->Resource());if(source!=sources_.end()&&source->second.life->epoch.Valid(source->second.epoch)&&shadows_.count(source->second.row->stock_sha256)){possible=true;break;}}if(!possible)return false;}}
   auto context=port_->CaptureContext(command);auto lease=port_->LockContext(context);if(!lease)return false;
   std::lock_guard lock(mutex_);if(!original_||failed_)return false;
   if(scan_){auto all=port_->AllBound();bound_.clear();for(auto&b:all){auto life=registry_.Binding(b.view);if(life){Want(life);bound_.emplace(b.stage*128+b.slot,Bound{b.stage,b.slot,b.view,std::move(life)});}}scan_=false;}
   std::vector<BindingWitness> work;
   for(auto& item:bound_){auto&b=item.second;if(!b.life||!b.life->Valid(b.view))continue;auto s=sources_.find(b.life->Resource());if(s==sources_.end()||!s->second.life->epoch.Valid(s->second.epoch))continue;
    Want(b.life);auto shadow=shadows_.find(s->second.row->stock_sha256);if(shadow==shadows_.end())continue;ViewDesc desc;
    if(!port_->DescribeBound(b.stage,b.slot,b.view,s->second.life->canonical,desc))continue;
    if(desc.first_mip||desc.layers!=1||(desc.mips!=s->second.row->upload.mips&&desc.mips!=UINT32_MAX))continue;
    auto view=shadow->second.views.find(desc.format);if(view==shadow->second.views.end())continue;shadow->second.last=frame_;work.push_back({b.stage,b.slot,b.view,view->second->handle});
   }
   if(work.empty())return false;require(port_->ArmBindings(context,work),"graphics exact active binding set");size_t attempted=0;bool restored=true;
   try{for(auto&b:work){++attempted;require(port_->Bind(context,b.stage,b.slot,b.stock),"graphics alternate bind");}invoked=true;port_->ForwardDraw(context,indexed,count,instances,first,offset,first_instance);++replaced_;}
   catch(...){for(size_t i=0;i<attempted;++i){auto&b=work[i];try{restored=port_->Bind(context,b.stage,b.slot,b.canonical)&&restored;}catch(...){restored=false;}}Fault();return invoked||!restored;}
   for(auto&b:work)try{restored=port_->Bind(context,b.stage,b.slot,b.canonical)&&restored;}catch(...){restored=false;}if(!restored){Fault();return true;}return true;
  }catch(...){Fault();return invoked;}
 }
 Status Snapshot(){std::lock_guard lock(mutex_);Status s;s.pairs=sources_.size();s.views=shadows_.size();s.bindings=bound_.size();s.frame=frame_;s.ack_sequence=ack_;s.original=original_;s.replaced_draws=replaced_;s.gpu_bytes=gpu_bytes_;s.unsafe=s.stopping=failed_;s.bindings_restored=!failed_;s.draw_scoped=true;return s;}
};
} // namespace tqgx
