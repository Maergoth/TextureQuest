#pragma once
#include "native_port.hpp"
#include "texturequest_provider_creation_v1.hpp"
#include <deque>
namespace tqph {
class CreationIngress {
 using Bytes=tqab::Bytes;using Layout=tqab::Layout;using Catalog=tqab::Catalog;
 struct Bundle {tqpc::Info info{};Layout layout;uint32_t format=0;bool immutable=false,native_seen=false,skipped=false;const tqab::Row* match=nullptr;Bytes bytes;std::shared_ptr<Resources::Lease> row;uint64_t epoch=0;};
 Session& session_;Resources& resources_;const tqpc::Api* api_;std::shared_ptr<const Catalog> catalog_;
 std::function<void(const tqab::Row*,const std::shared_ptr<Resources::Lease>&)> receiver_;
 std::mutex mutex_;std::map<uint64_t,std::shared_ptr<Bundle>> active_;std::deque<std::shared_ptr<Bundle>> ready_;size_t bytes_=0,transient_=0;
#if defined(TQPC_TEST_LEAVES)
 std::function<std::shared_ptr<Resources::Lease>(const tqpc::Info&,void*)> test_native_;
#endif
 bool Same(const tqpc::Info&i,const Bundle&b){return i.size==128&&i.abi_version==1&&!i.reserved&&i.call==b.info.call&&i.parent==b.info.parent&&i.thread==b.info.thread&&i.thread==GetCurrentThreadId()&&i.owner_token==session_.Token()&&i.device_generation==b.info.device_generation&&i.api_device==b.info.api_device&&i.factory==b.info.factory&&i.expected_init==b.info.expected_init&&i.initial_count==b.info.initial_count&&std::memcmp(&i.descriptor,&b.info.descriptor,sizeof i.descriptor)==0;}
 std::shared_ptr<Bundle> Get(void*ticket,const tqpc::Info&i){std::lock_guard lock(mutex_);auto it=active_.find(i.call);return it!=active_.end()&&it->second.get()==ticket?it->second:nullptr;}
 void Telemetry(tqpc::HostEvent e){tqab::require(api_->host_event(session_.Token(),e)==1,"authenticated creation telemetry");}
 void Done(const std::shared_ptr<Bundle>&b){std::lock_guard lock(mutex_);tqab::require(bytes_>=b->bytes.size(),"capture bytes accounting");bytes_-=b->bytes.size();}
 std::shared_ptr<Resources::Lease> NativeIdentity(const tqpc::Info&i,void* pointer,bool&capacity){
  using Microsoft::WRL::ComPtr;const auto serial=api_->begin_identity(session_.Token(),i.call);tqab::require(serial!=0,"current native-result creation permit");
  struct End {Session&s;uint64_t n;~End(){if(s.Api()->end_use(s.Token(),n)!=1)s.Close(Session::scope);}}end{session_,serial};
  tqpm::NativeOwner selected{};selected.size=48;tqab::require(session_.Api()->read_native_owner(session_.Token(),serial,i.api_device,&selected)==1&&selected.size==48&&selected.abi_version==2&&selected.context_kind==0&&selected.retained==1&&selected.api_device==i.api_device&&!selected.api_context&&selected.native_device&&!selected.native_context,"recorded native device for exact creation");
  ComPtr<ID3D11Texture2D> texture;tqab::require(pointer&&reinterpret_cast<IUnknown*>(pointer)->QueryInterface(IID_PPV_ARGS(&texture))==S_OK,"live native2D creation");
  ComPtr<ID3D11Device> owner;texture->GetDevice(&owner);tqpm::NativeOwner actual{};actual.size=48;ID3D11Device* raw=owner.Get();
  if(session_.Api()->read_native_owner(session_.Token(),serial,owner.Get(),&actual)==1){tqab::require(actual.size==48&&actual.abi_version==2&&actual.context_kind==0&&actual.retained==1&&actual.api_device==i.api_device&&!actual.api_context&&actual.native_device&&!actual.native_context,"typed device alias");raw=static_cast<ID3D11Device*>(actual.native_device);}
  ComPtr<IUnknown> a,b;tqab::require(raw&&raw->QueryInterface(IID_PPV_ARGS(&a))==S_OK&&static_cast<ID3D11Device*>(selected.native_device)->QueryInterface(IID_PPV_ARGS(&b))==S_OK&&a.Get()==b.Get(),"exact retained native device");
  D3D11_TEXTURE2D_DESC d{};texture->GetDesc(&d);auto&e=i.descriptor;
  tqab::require(d.Width==e.width&&d.Height==e.height&&d.MipLevels==e.mips&&d.ArraySize==e.layers&&uint32_t(d.Format)==e.format&&d.SampleDesc.Count==e.samples&&d.SampleDesc.Quality==e.quality&&uint32_t(d.Usage)==e.usage&&d.BindFlags==e.bind&&d.CPUAccessFlags==e.cpu&&d.MiscFlags==e.misc,"full final native descriptor");
  ComPtr<ID3D11Texture2D1> t1;auto hr=texture.As(&t1);if(hr==S_OK){D3D11_TEXTURE2D_DESC1 d1{};t1->GetDesc1(&d1);tqab::require(uint32_t(d1.TextureLayout)==e.texture_layout,"final native texture layout");}else tqab::require(hr==E_NOINTERFACE&&e.texture_layout==0,"known texture layout");
  ComPtr<IUnknown> canonical,original;tqab::require(texture.As(&canonical)==S_OK&&reinterpret_cast<IUnknown*>(pointer)->QueryInterface(IID_PPV_ARGS(&original))==S_OK&&canonical.Get()==original.Get(),"canonical resource generation");
  auto keep=std::make_shared<ComPtr<ID3D11Texture2D>>(texture);return resources_.Register(reinterpret_cast<uint64_t>(pointer),reinterpret_cast<uint64_t>(canonical.Get()),keep,&capacity);
 }
public:
 CreationIngress(Session&s,Resources&r,const tqpc::Api*api,std::shared_ptr<const Catalog>catalog):session_(s),resources_(r),api_(api),catalog_(std::move(catalog)){}

#if defined(TQPC_TEST_LEAVES)
 void TestNative(std::function<std::shared_ptr<Resources::Lease>(const tqpc::Info&,void*)> leaf){test_native_=std::move(leaf);}
 size_t TestQueued(){std::lock_guard lock(mutex_);return ready_.size();}
 size_t TestActive(){std::lock_guard lock(mutex_);return active_.size();}
#endif
 bool Canonical(const void* pointer,bool view,uint64_t&id,bool callback){
  using Microsoft::WRL::ComPtr;Session::Use use(session_,session_.Device(),tqpm::Use::identity_read,false,callback);if(!use||!pointer)return false;
  ComPtr<ID3D11Resource> resource;if(view){ComPtr<ID3D11View> v;if(reinterpret_cast<IUnknown*>(const_cast<void*>(pointer))->QueryInterface(IID_PPV_ARGS(&v))!=S_OK)return false;v->GetResource(&resource);}else if(reinterpret_cast<IUnknown*>(const_cast<void*>(pointer))->QueryInterface(IID_PPV_ARGS(&resource))!=S_OK)return false;
  if(!resource)return false;ComPtr<ID3D11Device> owner;resource->GetDevice(&owner);tqpm::NativeOwner selected{};selected.size=48;
  if(session_.Api()->read_native_owner(session_.Token(),use.Serial(),session_.Device(),&selected)!=1||selected.size!=48||selected.abi_version!=2||selected.context_kind!=0||selected.retained!=1||selected.api_device!=session_.Device()||selected.api_context||!selected.native_device||selected.native_context)return false;
  ID3D11Device* raw=owner.Get();tqpm::NativeOwner actual{};actual.size=48;if(session_.Api()->read_native_owner(session_.Token(),use.Serial(),owner.Get(),&actual)==1){if(actual.size!=48||actual.abi_version!=2||actual.context_kind!=0||actual.retained!=1||actual.api_device!=session_.Device()||actual.api_context||!actual.native_device||actual.native_context)return false;raw=static_cast<ID3D11Device*>(actual.native_device);}
  ComPtr<IUnknown>a,b,canonical;if(!raw||raw->QueryInterface(IID_PPV_ARGS(&a))!=S_OK||static_cast<ID3D11Device*>(selected.native_device)->QueryInterface(IID_PPV_ARGS(&b))!=S_OK||a.Get()!=b.Get()||resource.As(&canonical)!=S_OK)return false;id=reinterpret_cast<uint64_t>(canonical.Get());return true;
 }
 bool View(reshade::api::resource resource,reshade::api::resource_view view){
  // A texture IUnknown query is live metadata, not a device/context unwrap.
  // Identity already belongs to a retained fully validated creation generation.
  Microsoft::WRL::ComPtr<IUnknown> canonical;if(!resource.handle||reinterpret_cast<IUnknown*>(resource.handle)->QueryInterface(IID_PPV_ARGS(&canonical))!=S_OK)return false;
  auto row=resources_.FindCanonical(reinterpret_cast<uint64_t>(canonical.Get()));if(!row||row->destroyed||row->epoch.Retired())return true;
  // The genuine SDK callback observes a live app-owned view. Do not keep
  // every game view alive; a selected draw later obtains its own strong getter
  // reference under the protected context lease, before any temporary bind.
  auto keep=std::make_shared<uint64_t>(view.handle);return resources_.RememberView(view.handle,row->handle,keep);
 }
 void Receiver(std::function<void(const tqab::Row*,const std::shared_ptr<Resources::Lease>&)> value){tqab::require(!receiver_&&bool(value),"one full-pack receiver");receiver_=std::move(value);}
 uint32_t Capture(const tqpc::Info&i,void** ticket){
  using namespace tqab;tqab::require(ticket&&i.size==128&&i.abi_version==1&&!i.reserved&&i.call&&i.thread==GetCurrentThreadId()&&i.owner_token==session_.Token()&&i.api_device==session_.Device()&&i.device_generation,"exact creation capture frame");*ticket=nullptr;
  auto&d=i.descriptor;Layout l{d.width,d.height,d.mips,Family(d.format),1};if(!l.family||!catalog_->HasUploadLayout(l))return 0;
  if(!(i.initial&&i.initial_count==l.mips&&l.mips<=15&&d.layers==1&&d.samples==1&&!d.quality&&!d.cpu&&!d.misc&&!d.texture_layout&&d.bind==D3D11_BIND_SHADER_RESOURCE&&(d.usage==D3D11_USAGE_DEFAULT||d.usage==D3D11_USAGE_IMMUTABLE)))return 0;
  const auto needed=ByteCount(l);bool transient_capacity=false;{std::lock_guard lock(mutex_);transient_capacity=needed>MaxCpu-transient_;if(!transient_capacity)transient_+=needed;}
  if(transient_capacity){Telemetry(tqpc::HostEvent::capacity_fault);return 0;}
  struct Transient {CreationIngress&owner;size_t bytes;~Transient(){std::lock_guard lock(owner.mutex_);owner.transient_-=bytes;}}transient{*this,needed};
  auto levels=Levels(l);std::vector<Span> spans;for(size_t n=0;n<levels.size();++n){tqpc::Subresource v{};tqab::require(SafeRead(&v,i.initial+n,sizeof v),"creation subresource metadata");auto limit=uint64_t(v.row_pitch)*(levels[n].rows-1)+levels[n].row;tqab::require(limit<=MaxCpu,"creation source pitch bound");spans.push_back({v.data,v.row_pitch,v.slice_pitch,size_t(limit)});}
  auto bytes=Pack(l,spans.data(),spans.size(),SafeRead);bool ambiguous=false;auto row=catalog_->Find(l,Fingerprint(l,bytes),ambiguous);if(ambiguous||!row)return 0; // ambiguous Original comparisons keep the canonical released upscale
  auto b=std::make_shared<Bundle>();b->info=i;b->info.initial=nullptr;b->layout=l;b->format=d.format;b->immutable=d.usage==D3D11_USAGE_IMMUTABLE;b->match=row; // whole capture fingerprint verified above; no queued pixel copy
  bool capacity=false;{std::lock_guard lock(mutex_);capacity=active_.size()+ready_.size()>=100000||b->bytes.size()>MaxCpu-bytes_;if(!capacity){tqab::require(active_.emplace(i.call,b).second,"unique host creation frame");bytes_+=b->bytes.size();}}
  if(capacity){Telemetry(tqpc::HostEvent::capacity_fault);return 0;}
  *ticket=b.get();return 1;
 }
 uint32_t Native(void*ticket,const tqpc::NativeResult&r){tqab::require(r.size==32&&r.abi_version==1&&r.info&&r.known==1,"creation native result shape");auto b=Get(ticket,*r.info);tqab::require(b&&Same(*r.info,*b)&&!b->native_seen,"exact native-result frame");b->native_seen=true;
  if(r.hresult<0){tqab::require(!r.resource,"failed native has no published resource");return 1;}bool capacity=false;
#if defined(TQPC_TEST_LEAVES)
  tqab::require(bool(test_native_),"explicit invented native leaf");b->row=test_native_(*r.info,r.resource);
#else
  b->row=NativeIdentity(*r.info,r.resource,capacity);
#endif
  if(!b->row){if(capacity){Telemetry(tqpc::HostEvent::capacity_fault);b->skipped=true;return 1;}throw std::runtime_error("matched native lifetime identity conflict");}b->epoch=b->row->epoch.Value();return 1;
 }
 uint32_t Settle(void*ticket,const tqpc::Settlement&s){tqab::require(s.size==24&&s.abi_version==1&&s.info,"creation settlement shape");auto b=Get(ticket,*s.info);tqab::require(b&&Same(*s.info,*b),"exact final creation frame");
  bool ready=false;if(s.outcome==tqpc::Outcome::succeeded&&!b->skipped){tqab::require(b->native_seen&&b->row&&s.init_seen==s.info->expected_init,"complete successful factory lifecycle");ready=b->row->epoch.Ready(b->epoch);if(!ready)tqab::require(b->row->epoch.Retired(),"stale clean ready misuse");}
  else if(b->row)resources_.Retire(b->row);
  {std::lock_guard lock(mutex_);tqab::require(active_.erase(s.info->call)==1,"one final frame reclaim");if(ready)ready_.push_back(b);}
  // Settled creation identity is now maintained by observed generation and
  // destruction epochs. Retaining every source COM object would prevent EQ2
  // from unloading its own textures. Release this temporary native-result
  // owner outside the ingress mutex; a destruction callback can retire it.
  if(b->row)b->row->native.reset();
  if(ready)Telemetry(tqpc::HostEvent::queued);else{if(b->row&&b->row->epoch.Retired())Telemetry(tqpc::HostEvent::dirty_cancelled);Done(b);}return 1;
 }
 void Drain(tqab::AdapterPort&,tqab::Adapter&,reshade::api::device*){
  tqab::require(bool(receiver_),"full-pack creation receiver initialized");
  for(unsigned n=0;n<4096;++n){std::shared_ptr<Bundle>b;{std::lock_guard lock(mutex_);if(ready_.empty())break;b=ready_.front();ready_.pop_front();}
   if(b->row->epoch.Retired()||!b->row->epoch.Admit(b->epoch)){Telemetry(tqpc::HostEvent::dirty_cancelled);Done(b);continue;}
   tqab::require(b->match&&b->match->upload==b->layout,"settled captured catalog row");receiver_(b->match,b->row);Telemetry(tqpc::HostEvent::admitted);Done(b);
  }
 }

};
} // namespace tqph
