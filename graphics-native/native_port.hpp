#pragma once
#include "full_pack_port.hpp"
#include "provider_session.hpp"
#include "resource_registry.hpp"
#include "texturequest_provider_creation_v1.hpp"
#include <d3d11_4.h>
#include <wrl/client.h>
#include <set>
namespace tqab {
using Microsoft::WRL::ComPtr;
class NativePort final:public FullPackPort {
 struct NativeTexture final:Texture {ComPtr<ID3D11Texture2D> value;std::shared_ptr<void> module;};
 struct NativeView final:View {ComPtr<ID3D11ShaderResourceView> value;std::shared_ptr<void> module;};
 struct NativeContext final:Context {ComPtr<ID3D11DeviceContext> value;std::shared_ptr<void> module;};
 struct AdmissionLease final:DefaultAdmission {tqph::Resources* registry=nullptr;std::shared_ptr<tqph::Resources::Lease> row;
  uint64_t generation=0,epoch=0;bool Valid(uint64_t h)const noexcept override{return registry&&registry->Valid(row,h,generation,epoch);}bool Select()noexcept override{return registry&&registry->Select(row,generation,epoch);}void Retire()noexcept override{if(registry)registry->Retire(row);}};
 tqph::Session& session_;tqph::Resources& registry_;std::shared_ptr<void> module_;api::device* api_device_;api::command_list* api_context_;
 ComPtr<ID3D11Device> device_;ComPtr<IUnknown> device_id_,context_id_,mt_id_;ComPtr<ID3D11Multithread> mt_;std::shared_ptr<NativeContext> context_;
 std::mutex shadows_mutex_;std::set<uint64_t> textures_,canonical_views_,shadow_views_;
 struct NativeFence {ComPtr<ID3D11Query> query;};
 struct NativeLease final:DrawLease {
  NativePort* owner=nullptr;std::unique_ptr<tqph::Session::Use> use;std::vector<BindingWitness> bindings;std::vector<ComPtr<ID3D11ShaderResourceView>> originals;std::set<uint64_t> witnessed;bool entered=false;NativeLease* previous=nullptr;
  ~NativeLease(){if(owner){if(active_!=this)owner->Poison();active_=previous;if(entered)owner->mt_->Leave();}use.reset();}
 };
 inline static thread_local NativeLease* active_=nullptr;
 bool PendingAllowed()const noexcept{return false;}
 // Ambient before-native notices can be in flight on another thread. Their
 // dirty epochs remain authoritative; the selected immediate-context lease
 // serializes the actual getter/bind/draw/restore. A mere notice is not a fault.
 bool Live(bool callback=false){return session_.Health(true,PendingAllowed(),callback,!callback);}
 tqph::Session::Use Identity(bool callback=false){return tqph::Session::Use(session_,api_device_,tqpm::Use::identity_read,PendingAllowed(),callback,!callback);}
 bool RawSameDevice(ID3D11Device* d)const{ComPtr<IUnknown> id;return d&&d->QueryInterface(IID_PPV_ARGS(&id))==S_OK&&id.Get()==device_id_.Get();}
 bool SameDevice(ID3D11Device* d,uint64_t identity_serial)const{
  if(!d||!identity_serial)return false;tqpm::NativeOwner pair{};pair.size=sizeof(pair);
  if(session_.Api()->read_native_owner(session_.Token(),identity_serial,d,&pair)==1){
   if(pair.size!=48||pair.abi_version!=2||pair.context_kind!=0||pair.retained!=1||pair.api_device!=api_device_||pair.api_context||!pair.native_device||pair.native_context)return false;
   return RawSameDevice(static_cast<ID3D11Device*>(pair.native_device));
  }
  return RawSameDevice(d);
 }
 bool Group()const noexcept{return active_&&active_->owner==this&&active_->entered;}
 bool Known(uint64_t h,bool shadow){std::unique_lock lock(shadows_mutex_,std::try_to_lock);if(!lock.owns_lock()){Poison();return false;}return shadow?shadow_views_.count(h)!=0:canonical_views_.count(h)!=0;}
 static ComPtr<ID3D11ShaderResourceView> Get(ID3D11DeviceContext*c,uint32_t stage,uint32_t slot){ComPtr<ID3D11ShaderResourceView> r;
  switch(stage){case 1:c->VSGetShaderResources(slot,1,&r);break;case 2:c->HSGetShaderResources(slot,1,&r);break;case 4:c->DSGetShaderResources(slot,1,&r);break;case 8:c->GSGetShaderResources(slot,1,&r);break;case 16:c->PSGetShaderResources(slot,1,&r);break;case 32:c->CSGetShaderResources(slot,1,&r);break;default:throw std::runtime_error("native stage");}return r;}
 bool DescriptorInner(api::resource r,const api::resource_desc& description,D3D11_USAGE usage,uint64_t identity_serial,ComPtr<ID3D11Texture2D>* retained=nullptr){
  ComPtr<ID3D11Texture2D> t;auto raw=reinterpret_cast<IUnknown*>(r.handle);if(!raw||raw->QueryInterface(IID_PPV_ARGS(&t))!=S_OK)return false;ComPtr<ID3D11Device> owner;t->GetDevice(&owner);if(!SameDevice(owner.Get(),identity_serial))return false;
  D3D11_TEXTURE2D_DESC d{};t->GetDesc(&d);if(d.Usage!=usage||d.CPUAccessFlags||d.MiscFlags||d.BindFlags!=D3D11_BIND_SHADER_RESOURCE||d.ArraySize!=1||d.SampleDesc.Count!=1||d.SampleDesc.Quality||d.Width!=description.texture.width||d.Height!=description.texture.height||d.MipLevels!=description.texture.levels||uint32_t(d.Format)!=uint32_t(description.texture.format))return false;
  ComPtr<ID3D11Texture2D1> t1;auto hr=t.As(&t1);if(hr==S_OK){D3D11_TEXTURE2D_DESC1 d1{};t1->GetDesc1(&d1);if(d1.TextureLayout!=D3D11_TEXTURE_LAYOUT_UNDEFINED)return false;}else if(hr!=E_NOINTERFACE||t1)return false;
  if(retained)*retained=t;return true;
 }
public:
 NativePort(tqph::Session& s,tqph::Resources& r,api::device* d,api::command_list*c,std::shared_ptr<void> module):session_(s),registry_(r),module_(std::move(module)),api_device_(d),api_context_(c){
  require(d&&c&&module_&&d->get_api()==api::device_api::d3d11,"native owner/API");
  require(session_.Select(d,c,[&](const tqpm::NativeOwner& pair){
   auto nd=static_cast<ID3D11Device*>(pair.native_device);auto nc=static_cast<ID3D11DeviceContext*>(pair.native_context);auto value=std::make_shared<NativeContext>();
   if(nd->QueryInterface(IID_PPV_ARGS(&device_))!=S_OK||device_.As(&device_id_)!=S_OK||nc->QueryInterface(IID_PPV_ARGS(&value->value))!=S_OK||value->value.As(&context_id_)!=S_OK||value->value.As(&mt_)!=S_OK||mt_.As(&mt_id_)!=S_OK)return false;
   ComPtr<ID3D11Device> owner;value->value->GetDevice(&owner);if(!RawSameDevice(owner.Get())||value->value->GetType()!=D3D11_DEVICE_CONTEXT_IMMEDIATE||(device_->GetCreationFlags()&D3D11_CREATE_DEVICE_SINGLETHREADED))return false;
   value->identity=reinterpret_cast<uint64_t>(context_id_.Get());value->immediate=true;value->module=module_;context_=value;return true;
  }),"provider native pair/protection/bind");
 }
 void Poison()noexcept override{session_.Close(tqph::Session::native);}
 bool Healthy()noexcept override{return Live();}
 uint32_t ProtectionEnabled()const noexcept{return !session_.Closed()?1u:0u;}
 uint32_t ProtectionPrevious()const noexcept{return 0;} // No previous-value export exists; status uses UNKNOWN instead.
 std::shared_ptr<Context> CaptureContext(api::command_list*c)override{return c==api_context_&&!session_.Closed()?context_:nullptr;}
 std::shared_ptr<ViewAdmission> PendingView(uint64_t h)override{return registry_.Binding(h);}
 std::shared_ptr<DefaultAdmission> Admission(uint64_t h)override{auto row=registry_.Find(h);if(!row)return {};auto lease=std::make_shared<AdmissionLease>();lease->registry=&registry_;lease->generation=row->generation;lease->epoch=row->epoch.Value();lease->row=std::move(row);return lease;}
 std::shared_ptr<DrawLease> LockContext(const std::shared_ptr<Context>& c)override{
  if(c!=context_||active_||!Live())return {};auto lease=std::make_shared<NativeLease>();lease->owner=this;lease->identity=c->identity;
  lease->use=std::make_unique<tqph::Session::Use>(session_,api_context_,tqpm::Use::held_draw_bindings,false,false,true);if(!*lease->use)return {};
  mt_->Enter();lease->entered=true;lease->previous=active_;active_=lease.get();
  if(!Live()||!mt_->GetMultithreadProtected()){Poison();return {};}
  ComPtr<IUnknown> id;if(mt_.As(&id)!=S_OK||id.Get()!=mt_id_.Get()){Poison();return {};}return lease;
 }
 bool ArmBindings(const std::shared_ptr<Context>& c,const std::vector<BindingWitness>& set)override{
  if(c!=context_||!Group()||!Live()||set.size()>256)return false;
  for(const auto& b:set)if(!b.stage||(b.stage&(b.stage-1))||(b.stage&~31u)||b.slot>=128||(!active_->witnessed.count(b.canonical)&&!Known(b.canonical,false))||!Known(b.stock,true))return false;
  active_->bindings=set;return true;
 }
 std::shared_ptr<Texture> CreateStock(const Layout& l,const Bytes& pixels,uint32_t format)override{
  require(pixels.size()==ByteCount(l)&&Family(format)==l.family,"full immutable shadow bytes");tqph::Session::Use use(session_,api_device_,tqpm::Use::owned_shadow_creation,PendingAllowed(),false,true);if(!use)return {};
  auto levels=Levels(l);D3D11_TEXTURE2D_DESC d{};d.Width=l.width;d.Height=l.height;d.MipLevels=l.mips;d.ArraySize=1;d.Format=DXGI_FORMAT(format);d.SampleDesc.Count=1;d.Usage=D3D11_USAGE_IMMUTABLE;d.BindFlags=D3D11_BIND_SHADER_RESOURCE;
  std::vector<D3D11_SUBRESOURCE_DATA> data;size_t at=0;for(auto& m:levels){data.push_back({pixels.data()+at,m.row,UINT(m.bytes)});at+=m.bytes;}
  auto r=std::make_shared<NativeTexture>();if(!Live()||device_->CreateTexture2D(&d,data.data(),&r->value)!=S_OK)return {};r->module=module_;
  std::unique_lock lock(shadows_mutex_,std::try_to_lock);if(!lock.owns_lock()||textures_.size()>=4096||!textures_.insert(reinterpret_cast<uint64_t>(r->value.Get())).second){Poison();return {};}return r;
 }
 std::shared_ptr<View> HoldView(uint64_t h)override{
  auto use=Identity();if(!use||!h)return {};auto r=std::make_shared<NativeView>();if(reinterpret_cast<IUnknown*>(h)->QueryInterface(IID_PPV_ARGS(&r->value))!=S_OK)return {};ComPtr<ID3D11Device> owner;r->value->GetDevice(&owner);if(!SameDevice(owner.Get(),use.Serial()))return {};
  r->handle=reinterpret_cast<uint64_t>(r->value.Get());r->module=module_;std::unique_lock lock(shadows_mutex_,std::try_to_lock);if(!lock.owns_lock()||canonical_views_.size()>=256){Poison();return {};}canonical_views_.insert(r->handle);return r;
 }
 bool DescribeView(uint64_t h,uint64_t resource,ViewDesc& out)override{
  auto use=Identity();if(!use||!h)return false;ComPtr<ID3D11ShaderResourceView> held;if(reinterpret_cast<IUnknown*>(h)->QueryInterface(IID_PPV_ARGS(&held))!=S_OK)return false;ComPtr<ID3D11Device> owner;held->GetDevice(&owner);if(!SameDevice(owner.Get(),use.Serial()))return false;ComPtr<ID3D11Resource> source;held->GetResource(&source);ComPtr<IUnknown> a,b;
  if(!source||source.As(&a)!=S_OK||!resource||reinterpret_cast<IUnknown*>(resource)->QueryInterface(IID_PPV_ARGS(&b))!=S_OK||a.Get()!=b.Get())return false;
  D3D11_SHADER_RESOURCE_VIEW_DESC d{};held->GetDesc(&d);if(d.ViewDimension!=D3D11_SRV_DIMENSION_TEXTURE2D)return false;out={uint32_t(d.Format),d.Texture2D.MostDetailedMip,d.Texture2D.MipLevels,1};return registry_.RememberView(h,resource);
 }
 std::shared_ptr<View> CreateView(const std::shared_ptr<Texture>& texture,const ViewDesc& desc)override{
  auto t=std::dynamic_pointer_cast<NativeTexture>(texture);if(!t)return {};{std::unique_lock lock(shadows_mutex_,std::try_to_lock);if(!lock.owns_lock()||!textures_.count(reinterpret_cast<uint64_t>(t->value.Get()))){Poison();return {};}}
  tqph::Session::Use use(session_,api_device_,tqpm::Use::owned_shadow_creation,PendingAllowed());if(!use)return {};
  D3D11_SHADER_RESOURCE_VIEW_DESC d{};d.Format=DXGI_FORMAT(desc.format);d.ViewDimension=D3D11_SRV_DIMENSION_TEXTURE2D;d.Texture2D.MostDetailedMip=desc.first_mip;d.Texture2D.MipLevels=desc.mips;
  auto r=std::make_shared<NativeView>();if(!Live()||device_->CreateShaderResourceView(t->value.Get(),&d,&r->value)!=S_OK)return {};r->handle=reinterpret_cast<uint64_t>(r->value.Get());r->module=module_;
  std::unique_lock lock(shadows_mutex_,std::try_to_lock);if(!lock.owns_lock()||shadow_views_.size()>=8192||!shadow_views_.insert(r->handle).second){Poison();return {};}return r;
 }
 uint64_t Current(const std::shared_ptr<Context>& c,uint32_t stage,uint32_t slot)override{
  require(c==context_&&Group()&&Live()&&slot<128,"exact held native getter");auto result=Get(context_->value.Get(),stage,slot);return reinterpret_cast<uint64_t>(result.Get()); // Getter's one ref always balances.
 }
 bool Bind(const std::shared_ptr<Context>& c,uint32_t stage,uint32_t slot,uint64_t h)override{
  if(c!=context_||!Group()||!Live())return false;bool allowed=false;for(auto& b:active_->bindings)if(b.stage==stage&&b.slot==slot&&(h==b.canonical||h==b.stock))allowed=true;if(!allowed){Poison();return false;}
  auto v=reinterpret_cast<ID3D11ShaderResourceView*>(h);auto n=context_->value.Get();switch(stage){case 1:n->VSSetShaderResources(slot,1,&v);break;case 2:n->HSSetShaderResources(slot,1,&v);break;case 4:n->DSSetShaderResources(slot,1,&v);break;case 8:n->GSSetShaderResources(slot,1,&v);break;case 16:n->PSSetShaderResources(slot,1,&v);break;default:Poison();return false;}return Current(c,stage,slot)==h;
 }
 bool ReplaceGetter(uint64_t& returned,uint64_t canonical)override{
  if(!Group()||!Live()||!Known(returned,true)||!Known(canonical,false))return false;reinterpret_cast<IUnknown*>(canonical)->AddRef();reinterpret_cast<IUnknown*>(returned)->Release();returned=canonical;return true;
 }
 void ForwardDraw(const std::shared_ptr<Context>& c,bool indexed,uint32_t count,uint32_t instances,uint32_t first,int32_t offset,uint32_t first_instance)override{
  // Mandatory original forwarding after admission is not cancelled by a later close.
  // Its unknown exception is consumed by Engine::DrawScope and never retried.
  require(c==context_&&Group()&&instances==1&&!first_instance&&count,"supported once native Draw/DrawIndexed");
  if(indexed)context_->value->DrawIndexed(count,first,offset);else context_->value->Draw(count,first);
 }
 std::vector<FullSlot> AllBound()override{
  require(Group()&&Live(),"held full binding snapshot");std::vector<FullSlot> result;
  for(uint32_t stage=1;stage<=16;stage<<=1){ComPtr<ID3D11ShaderResourceView> all[128];ID3D11ShaderResourceView* raw[128]{};
   switch(stage){case 1:context_->value->VSGetShaderResources(0,128,raw);break;case 2:context_->value->HSGetShaderResources(0,128,raw);break;case 4:context_->value->DSGetShaderResources(0,128,raw);break;case 8:context_->value->GSGetShaderResources(0,128,raw);break;case 16:context_->value->PSGetShaderResources(0,128,raw);break;}
   for(uint32_t i=0;i<128;++i){all[i].Attach(raw[i]);if(raw[i])result.push_back({stage,i,reinterpret_cast<uint64_t>(raw[i])});}
  }return result;
 }
 bool DescribeBound(uint32_t stage,uint32_t slot,uint64_t expected,uint64_t resource,ViewDesc&desc){
  require(Group()&&Live(),"held selected binding identity");auto view=Get(context_->value.Get(),stage,slot);
  if(!view||reinterpret_cast<uint64_t>(view.Get())!=expected)return false;
  ComPtr<ID3D11Device> device;view->GetDevice(&device);if(!RawSameDevice(device.Get()))return false;
  ComPtr<ID3D11Resource> texture;view->GetResource(&texture);ComPtr<IUnknown> id;if(!texture||texture.As(&id)!=S_OK||reinterpret_cast<uint64_t>(id.Get())!=resource)return false;
  D3D11_SHADER_RESOURCE_VIEW_DESC d{};view->GetDesc(&d);if(d.ViewDimension!=D3D11_SRV_DIMENSION_TEXTURE2D)return false;
  desc={uint32_t(d.Format),d.Texture2D.MostDetailedMip,d.Texture2D.MipLevels,1};active_->witnessed.insert(expected);active_->originals.push_back(std::move(view));return true;
 }
 std::shared_ptr<void> Fence(){
  auto lease=LockContext(context_);if(!lease)return {};auto q=std::make_shared<NativeFence>();D3D11_QUERY_DESC desc{D3D11_QUERY_EVENT,0};
  if(device_->CreateQuery(&desc,&q->query)!=S_OK)return {};context_->value->End(q->query.Get());return q;
 }
 int FenceReady(const std::shared_ptr<void>&fence){
  if(!fence)return -1;auto lease=LockContext(context_);if(!lease)return -1;auto q=std::static_pointer_cast<NativeFence>(fence);
  BOOL complete=FALSE;auto hr=context_->value->GetData(q->query.Get(),&complete,sizeof complete,D3D11_ASYNC_GETDATA_DONOTFLUSH);return hr==S_FALSE?0:hr==S_OK&&complete?1:-1;
 }
 void ForgetTargets(const std::vector<std::shared_ptr<View>>&views,const std::vector<std::shared_ptr<Texture>>&textures){
  std::lock_guard lock(shadows_mutex_);for(const auto&v:views)if(v)shadow_views_.erase(v->handle);
  for(const auto&t:textures)if(auto native=std::dynamic_pointer_cast<NativeTexture>(t))textures_.erase(reinterpret_cast<uint64_t>(native->value.Get()));
 }
 bool CanonicalResource(const void* pointer,bool view,uint64_t& id,bool callback){
  tqph::Session::Use use(session_,api_device_,tqpm::Use::identity_read,false,callback);if(!use||!pointer)return false;
  ComPtr<ID3D11Resource> r;if(view){ComPtr<ID3D11View> v;if(reinterpret_cast<IUnknown*>(const_cast<void*>(pointer))->QueryInterface(IID_PPV_ARGS(&v))!=S_OK)return false;ComPtr<ID3D11Device> d;v->GetDevice(&d);if(!SameDevice(d.Get(),use.Serial()))return false;v->GetResource(&r);}else if(reinterpret_cast<IUnknown*>(const_cast<void*>(pointer))->QueryInterface(IID_PPV_ARGS(&r))!=S_OK)return false;
  if(!r)return false;ComPtr<ID3D11Device> d;r->GetDevice(&d);if(!SameDevice(d.Get(),use.Serial()))return false;ComPtr<IUnknown> canonical;if(r.As(&canonical)!=S_OK)return false;id=reinterpret_cast<uint64_t>(canonical.Get());return true;
 }
};
} // namespace tqab
