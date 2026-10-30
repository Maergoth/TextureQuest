#include "full_pack_engine.hpp"
#include "full_catalog.hpp"
#include <cassert>
#include <iostream>
#include <chrono>

using namespace tqab;using namespace tqgx;
struct Available final:PayloadAccess {
 std::shared_ptr<const Bytes> bytes;bool enabled=false;unsigned gets=0;
 explicit Available(Bytes b):bytes(std::make_shared<const Bytes>(std::move(b))){}
 std::shared_ptr<const Bytes> Get(const Row&)override{++gets;return enabled?bytes:nullptr;}
 void Enabled(bool b)override{enabled=b;}bool Failed()override{return false;}
};
struct FakePort final:FullPackPort {
 unsigned calls=0,draws=0,allocations=0,forgets=0,fences=0;bool poisoned=false,complete=false,throw_draw=false;
 uint64_t next_view=10000,current=100,alternate=0;std::shared_ptr<Context> context=std::make_shared<Context>();
 FakePort(){context->identity=7;context->immediate=true;}
 void Poison()noexcept override{poisoned=true;}bool Healthy()noexcept override{return !poisoned;}
 std::shared_ptr<Context> CaptureContext(reshade::api::command_list*)override{++calls;return context;}
 std::shared_ptr<DrawLease> LockContext(const std::shared_ptr<Context>&c)override{++calls;assert(c==context);auto r=std::make_shared<DrawLease>();r->identity=7;return r;}
 std::shared_ptr<Texture> CreateStock(const Layout&l,const Bytes&b,uint32_t f)override{++calls;++allocations;assert(l.width==4&&l.height==4&&b==Bytes(16,17)&&f==73);return std::make_shared<Texture>();}
 std::shared_ptr<View> CreateView(const std::shared_ptr<Texture>&t,const ViewDesc&d)override{++calls;assert(t&&d.first_mip==0&&d.mips==1&&d.layers==1&&(d.format==74||d.format==75));auto v=std::make_shared<View>();v->handle=next_view++;return v;}
 std::shared_ptr<View> HoldView(uint64_t)override{assert(false);return {};}
 bool DescribeView(uint64_t,uint64_t,ViewDesc&)override{assert(false);return false;}
 uint64_t Current(const std::shared_ptr<Context>&,uint32_t,uint32_t)override{++calls;return current;}
 bool Bind(const std::shared_ptr<Context>&,uint32_t s,uint32_t slot,uint64_t h)override{++calls;assert(s==16&&slot==0&&(h==100||h>=10000));current=h;return true;}
 bool ReplaceGetter(uint64_t&,uint64_t)override{assert(false);return false;}
 bool ArmBindings(const std::shared_ptr<Context>&,const std::vector<BindingWitness>&set)override{++calls;assert(set.size()==1&&set[0].canonical==100);alternate=set[0].stock;return true;}
 void ForwardDraw(const std::shared_ptr<Context>&,bool indexed,uint32_t count,uint32_t instances,uint32_t,int32_t,uint32_t)override{++calls;++draws;assert(indexed&&count==3&&instances==1&&current==alternate);if(throw_draw)throw std::runtime_error("invented unknown native draw result");}
 std::vector<FullSlot> AllBound()override{++calls;return{{16,0,100}};}
 bool DescribeBound(uint32_t stage,uint32_t slot,uint64_t expected,uint64_t resource,ViewDesc&d)override{++calls;assert(stage==16&&slot==0&&expected==100&&resource==10);d={74,0,2,1};return true;}
 std::shared_ptr<void> Fence()override{++calls;++fences;return std::make_shared<int>(123);}
 int FenceReady(const std::shared_ptr<void>&f)override{++calls;assert(f);return complete?1:0;}
 void ForgetTargets(const std::vector<std::shared_ptr<View>>&v,const std::vector<std::shared_ptr<Texture>>&t)override{++calls;++forgets;assert(v.size()==2&&t.size()==1);}
};
struct Setup {
 tqph::Session session;tqph::Resources registry{session};std::shared_ptr<Available> access=std::make_shared<Available>(Bytes(16,17));
 std::shared_ptr<const Catalog> catalog;const Row* row=nullptr;std::shared_ptr<tqph::Resources::Lease> life;
 Setup(){Layout upload{8,8,2,2,1},original{4,4,1,2,1};Bytes pixels(ByteCount(upload),43);Row r{"literal/member.dds","owner:complete-source-and-target-dds",Fingerprint(upload,pixels),Fingerprint(original,*access->bytes),upload,original,{},CanonicalRole::upscaled};r.payload_bytes=16;r.payload_sha256=tq::Sha256(access->bytes->data(),16);catalog=std::make_shared<Catalog>(std::vector<Row>{r},access);bool ambiguous=false;row=catalog->Find(upload,r.upload_sha256,ambiguous);assert(row&&!ambiguous);
  life=registry.Register(10,10,std::make_shared<int>(123));assert(life&&life->epoch.Ready(life->epoch.Value())&&life->epoch.Admit(life->epoch.Value()));assert(registry.RememberView(100,10,std::make_shared<int>(456)));
 }
};
int main(){
 // No native work whatsoever for the normal full-pack Upscaled draw path.
 {Setup s;auto p=std::make_shared<FakePort>();FullPackEngine e(s.catalog,p,s.registry,false);e.Admit(s.row,s.life);e.Present(2);for(unsigned i=0;i<20000;++i)assert(!e.Draw(nullptr,true,3,1,0,0,0));assert(p->calls==0&&s.access->gets==0);}
 // On-demand Original, exactly one draw, canonical restoration, bounded
 // deferred release, and return to an idle Upscaled path.
 {Setup s;auto p=std::make_shared<FakePort>();FullPackEngine e(s.catalog,p,s.registry,false);e.Admit(s.row,s.life);e.Present(3);assert(!e.Draw(nullptr,true,3,1,0,0,0));e.Present(3);assert(p->allocations==1);assert(e.Draw(nullptr,true,3,1,0,0,0));assert(p->draws==1&&p->current==100&&!p->poisoned);assert(e.Snapshot().gpu_bytes==16);
  e.Present(4);assert(p->fences==1&&e.Snapshot().gpu_bytes==16);e.Present(4);assert(e.Snapshot().gpu_bytes==16);p->complete=true;e.Present(4);assert(e.Snapshot().gpu_bytes==0&&p->forgets==1);unsigned calls=p->calls;for(unsigned i=0;i<5000;++i)assert(!e.Draw(nullptr,true,3,1,0,0,0));assert(p->calls==calls);
 }
 // Dual-load prefetches observed bindings while drawing Upscaled directly.
 {Setup s;auto p=std::make_shared<FakePort>();FullPackEngine e(s.catalog,p,s.registry,true);e.Admit(s.row,s.life);uint64_t view=100;e.Bind(16,0,&view,1);e.Present(2);assert(p->allocations==1);auto calls=p->calls;assert(!e.Draw(nullptr,true,3,1,0,0,0)&&p->calls==calls);e.Present(2);assert(!p->fences&&e.Snapshot().gpu_bytes==16);}
 // An unknown draw result is consumed, restored, and terminal; never repeated.
 {Setup s;auto p=std::make_shared<FakePort>();FullPackEngine e(s.catalog,p,s.registry,false);e.Admit(s.row,s.life);e.Present(3);e.Draw(nullptr,true,3,1,0,0,0);e.Present(3);p->throw_draw=true;assert(e.Draw(nullptr,true,3,1,0,0,0));assert(p->draws==1&&p->current==100&&p->poisoned&&e.Snapshot().unsafe);assert(!e.Draw(nullptr,true,3,1,0,0,0)&&p->draws==1);}
 // A full-size metadata domain is indexed without preloading any pixels.
 {Layout a{8,8,2,2,1},b{4,4,1,2,1};auto access=std::make_shared<Available>(Bytes(16,17));std::vector<Row> rows;for(unsigned i=0;i<100000;++i){char hex[65];snprintf(hex,sizeof hex,"%064x",i);Row r{"member"+std::to_string(i),"source-and-target",hex,Fingerprint(b,*access->bytes),a,b,{},CanonicalRole::upscaled};r.payload_bytes=16;r.payload_sha256=tq::Sha256(access->bytes->data(),16);rows.push_back(std::move(r));}Catalog catalog(std::move(rows),access);bool ambiguous=false;char hex[65];snprintf(hex,sizeof hex,"%064x",99999);assert(catalog.Find(a,hex,ambiguous)&&!ambiguous&&access->gets==0);}
 // Weak resource observations cannot keep game COM textures alive. A recycled
 // pointer gets a different generation, and old view tokens remain invalid.
 {tqph::Session session;tqph::Resources r(session);unsigned releases=0;auto owner=std::shared_ptr<void>(new int(1),[&](void*p){++releases;delete static_cast<int*>(p);});
  auto a=r.Register(10,11,owner);owner.reset();assert(!releases);a->epoch.Ready(a->epoch.Value());assert(r.RememberView(12,10,std::make_shared<int>(2)));auto view=r.Binding(12);assert(view&&view->Valid(12));a->native.reset();assert(releases==1&&view->Valid(12));
  assert(r.Destruction(true,[]{return 12ull;},[](uint64_t){return true;}));assert(!view->Valid(12)&&!a->destroyed&&r.Find(10)==a);
  auto oldgen=a->generation;assert(r.Destruction(false,[]{return 10ull;},[](uint64_t){return true;}));assert(a->destroyed&&!r.Find(10)&&!r.FindCanonical(11));auto b=r.Register(10,11,std::make_shared<int>(3));assert(b&&b->generation>oldgen&&!r.Valid(a,10,oldgen,a->epoch.Value()));
 }
 // Real asynchronous payload worker: no disk callback before a request,
 // completed hashes/fingerprint, borrowed immutable bytes, cache eviction.
 {Setup s;auto row=*s.row;std::atomic<unsigned> reads{0};PayloadPool pool([&](const PayloadJob& j){++reads;assert(j.bytes==16);return Bytes(16,17);});assert(!pool.Get(row)&&reads==0);pool.Enabled(true);assert(!pool.Get(row));std::shared_ptr<const Bytes> ready;auto end=std::chrono::steady_clock::now()+std::chrono::seconds(3);while(!(ready=pool.Get(row))&&std::chrono::steady_clock::now()<end)Sleep(1);assert(ready&&*ready==Bytes(16,17)&&reads==1&&!pool.Failed());pool.Enabled(false);assert(pool.ResidentBytes()==0&&*ready==Bytes(16,17)&&!pool.Get(row));}
 // Tampered ranges never become a completed cache entry.
 {Setup s;auto row=*s.row;PayloadPool pool([](const PayloadJob&){return Bytes(16,18);});pool.Enabled(true);assert(!pool.Get(row));auto end=std::chrono::steady_clock::now()+std::chrono::seconds(3);while(!pool.Failed()&&std::chrono::steady_clock::now()<end)Sleep(1);assert(pool.Failed()&&!pool.Get(row)&&pool.ResidentBytes()==0);}
 std::cout<<"Full-pack engine regression plus NEW weak-lifetime/ABA/asynchronous-payload/tamper CPU cases PASS\n";
}
