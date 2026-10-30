#pragma once
// Isolated CPU-testable pair engine. No IO, registration, loader, or global state.
#include "saved_sha256.h"
#include <algorithm>
#include <atomic>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>
#include <array>
#include <set>
#include <unordered_map>

namespace tqab {
using Bytes=std::vector<unsigned char>;
inline void require(bool value,const char* message){if(!value)throw std::runtime_error(message);}
constexpr uint64_t MiB=1024u*1024u, MaxGpu=64*MiB, MaxCpu=64*MiB;
constexpr size_t MaxPairs=8192,MaxCatalog=100000,MaxViews=32768,MaxContexts=8;
struct Layout {
    uint32_t width=0,height=0,mips=0,family=0,representation=1;
    bool operator==(const Layout& b)const{return width==b.width&&height==b.height&&mips==b.mips&&family==b.family&&representation==b.representation;}
};
inline uint32_t Family(uint32_t format){return format>=70&&format<=72?1:format>=73&&format<=75?2:format>=76&&format<=78?3:(format==87||format==90||format==91)?4:(format==88||format==92||format==93)?5:(format==60||format==61)?6:format==31?7:0;}
inline uint32_t Block(uint32_t family){return family==1?8:family==2||family==3?16:0;}
struct Level {uint32_t width,height,row,rows;size_t bytes;};
inline std::vector<Level> Levels(Layout d){
    require(d.width&&d.height&&d.width<=16384&&d.height<=16384&&d.mips&&d.mips<=15&&d.family>=1&&d.family<=7&&d.representation==1,"layout unsupported");
    uint32_t maximum=1;for(uint32_t n=std::max(d.width,d.height);n>1;n>>=1)++maximum;
    require(d.mips<=maximum,"mip count exceeds dimensions");
    std::vector<Level> out;uint64_t total=0;
    for(uint32_t mip=0;mip<d.mips;++mip){uint32_t w=std::max(1u,d.width>>mip),h=std::max(1u,d.height>>mip),row=d.family<=3?((w+3)/4)*Block(d.family):w*(d.family==6?1u:4u),rows=d.family<=3?(h+3)/4:h;uint64_t bytes=uint64_t(row)*rows;total+=bytes;require(total<=MaxCpu,"layout byte limit");out.push_back({w,h,row,rows,static_cast<size_t>(bytes)});}
    return out;
}
inline size_t ByteCount(Layout d){size_t n=0;for(const auto& l:Levels(d))n+=l.bytes;return n;}
struct Span {const void* data=nullptr;uint32_t row=0,slice=0;size_t available=0;};
using Reader=bool(*)(void*,const void*,size_t);
inline bool Copy(void* d,const void* s,size_t n){if(!d||!s)return false;std::memcpy(d,s,n);return true;}
inline Bytes Pack(Layout layout,const Span* spans,size_t count,Reader read=Copy){
    const auto levels=Levels(layout);require(spans&&read&&count==levels.size(),"subresource array cardinality");
    Bytes result;result.reserve(ByteCount(layout));
    for(size_t mip=0;mip<count;++mip){const auto& l=levels[mip];const auto& s=spans[mip];
        require(s.data&&s.row>=l.row&&s.row<=MaxCpu,"source row pitch");
        const uint64_t last=uint64_t(l.rows-1)*s.row+l.row;
        require(last<=MaxCpu&&last<=s.available&&(!s.slice||last<=s.slice),"source span bounds");
        const auto* p=static_cast<const unsigned char*>(s.data);
        for(uint32_t y=0;y<l.rows;++y){size_t at=result.size();result.resize(at+l.row);require(read(result.data()+at,p+size_t(y)*s.row,l.row),"source read failed");}
    }
    return result;
}
inline void Word(Bytes& bytes,uint32_t n){for(unsigned i=0;i<4;++i)bytes.push_back(static_cast<unsigned char>(n>>(i*8)));}
inline std::string Fingerprint(Layout d,const Bytes& packed){
    require(packed.size()==ByteCount(d),"packed byte count");Bytes domain={'T','Q','A','B','U','P','0','1'};
    for(uint32_t n:{d.representation,d.width,d.height,d.mips,d.family,1u,1u})Word(domain,n);
    for(const auto& l:Levels(d))for(uint32_t n:{l.width,l.height,l.row,l.rows})Word(domain,n);
    domain.insert(domain.end(),packed.begin(),packed.end());return tq::Sha256(domain.data(),domain.size());
}
enum class CanonicalRole:uint32_t {stock=1,upscaled=2};
struct Row {std::string member,provenance,upload_sha256,stock_sha256;Layout upload,stock;Bytes stock_pixels;CanonicalRole role=CanonicalRole::upscaled;uint64_t payload_offset=0,payload_bytes=0;std::string payload_sha256;};
struct PayloadAccess {virtual ~PayloadAccess()=default;virtual std::shared_ptr<const Bytes> Get(const Row&)=0;virtual void Enabled(bool)=0;virtual bool Failed()=0;};
class Catalog {
    std::vector<Row> rows_;std::shared_ptr<PayloadAccess> payloads_;
    std::unordered_map<std::string,std::vector<const Row*>> hashes_;
    std::set<std::array<uint32_t,5>> layouts_;
public:
    explicit Catalog(std::vector<Row> rows,std::shared_ptr<PayloadAccess> payloads={}):rows_(std::move(rows)),payloads_(std::move(payloads)){
        require(!rows_.empty()&&rows_.size()<=MaxCatalog,"catalog row bound");uint64_t bytes=0;
        auto hex=[](const std::string&x){return x.size()==64&&std::all_of(x.begin(),x.end(),[](char c){return(c>='0'&&c<='9')||(c>='a'&&c<='f');});};
        for(const auto&r:rows_){require((r.role==CanonicalRole::stock||r.role==CanonicalRole::upscaled)&&!r.member.empty()&&r.member.size()<=4096&&!r.provenance.empty()&&r.provenance.size()<=4096,"catalog named role/provenance");
            require(hex(r.upload_sha256)&&hex(r.stock_sha256),"catalog digest syntax");(void)Levels(r.upload);(void)Levels(r.stock);
            require(r.upload.family==r.stock.family&&uint64_t(r.stock.width)*r.upload.height==uint64_t(r.stock.height)*r.upload.width,"catalog format/aspect");
            if(payloads_)require(r.stock_pixels.empty()&&r.payload_bytes==ByteCount(r.stock)&&hex(r.payload_sha256),"lazy payload metadata");
            else {require(Fingerprint(r.stock,r.stock_pixels)==r.stock_sha256,"stock payload pin differs");bytes+=r.stock_pixels.size();require(bytes<=MaxCpu,"embedded CPU byte bound");}
            hashes_[r.upload_sha256].push_back(&r);layouts_.insert({r.upload.width,r.upload.height,r.upload.mips,r.upload.family,r.upload.representation});
        }
    }
    const Row* Find(Layout layout,const std::string& digest,bool& ambiguous)const{
        const Row* result=nullptr;ambiguous=false;auto group=hashes_.find(digest);if(group==hashes_.end())return nullptr;
        for(auto r:group->second)if(r->upload==layout){if(result&&(!(r->stock==result->stock)||r->stock_sha256!=result->stock_sha256||r->role!=result->role)){ambiguous=true;return nullptr;}result=r;}return result;
    }
    bool HasUploadLayout(Layout l)const{return layouts_.count({l.width,l.height,l.mips,l.family,l.representation})!=0;}
    bool Contains(const Row*row)const{for(const auto&r:rows_)if(&r==row)return true;return false;}
    std::shared_ptr<const Bytes> Payload(const Row& row)const{return payloads_?payloads_->Get(row):std::make_shared<const Bytes>(row.stock_pixels);}
    void PayloadEnabled(bool value)const{if(payloads_)payloads_->Enabled(value);}
    bool PayloadFailed()const{return payloads_&&payloads_->Failed();}
};
struct Texture {virtual ~Texture()=default;};
struct View {uint64_t handle=0;virtual ~View()=default;};
struct Context {bool immediate=false;uint64_t identity=0;virtual ~Context()=default;};
struct DrawLease {uint64_t identity=0;virtual ~DrawLease()=default;};
struct BindingWitness {uint32_t stage=0,slot=0;uint64_t canonical=0,stock=0;};
struct ViewDesc {uint32_t format=0,first_mip=0,mips=0,layers=1;};
struct Port {
    virtual ~Port()=default;
    virtual std::shared_ptr<Texture> CreateStock(const Layout&,const Bytes&,uint32_t storage_format)=0;
    virtual std::shared_ptr<View> HoldView(uint64_t)=0;
    virtual std::shared_ptr<View> CreateView(const std::shared_ptr<Texture>&,const ViewDesc&)=0;
    virtual uint64_t Current(const std::shared_ptr<Context>&,uint32_t stage,uint32_t slot)=0;
    virtual bool Bind(const std::shared_ptr<Context>&,uint32_t stage,uint32_t slot,uint64_t view)=0;
    // 'returned' owns the one reference furnished by the native getter.
    virtual bool ReplaceGetter(uint64_t& returned,uint64_t canonical)=0;
    virtual bool ArmBindings(const std::shared_ptr<Context>&,const std::vector<BindingWitness>&)=0;
    // Acquired BEFORE the adapter mutex; held through every query/bind/draw/restore.
    virtual std::shared_ptr<DrawLease> LockContext(const std::shared_ptr<Context>&)=0;
};
struct DefaultAdmission {virtual ~DefaultAdmission()=default;virtual bool Valid(uint64_t)const noexcept=0;virtual bool Select()noexcept=0;virtual void Retire()noexcept=0;};
// A retained registry lifetime, never a sampled Boolean or raw-handle promise.
struct ViewAdmission {virtual ~ViewAdmission()=default;virtual bool Valid(uint64_t view)const noexcept=0;virtual uint64_t Resource()const noexcept=0;};
struct Witness {bool getter_safe=false,serialized_immediate=false,main_present=false;};
class Control {
    std::atomic<uint64_t> request_{0};
public:
    bool Request(uint64_t seq,bool original){if(!seq||seq>(UINT64_MAX>>1))return false;uint64_t previous=request_.load(std::memory_order_acquire),value=(seq<<1)|(original?1u:0u);do{if(seq<=previous>>1)return false;}while(!request_.compare_exchange_weak(previous,value,std::memory_order_release,std::memory_order_acquire));return true;}
    uint64_t Load()const{return request_.load(std::memory_order_acquire);}
};
struct Status {size_t pairs=0,views=0,bindings=0;uint64_t gpu_bytes=0,ack_sequence=0,frame=0,replaced_draws=0;bool original=false,unsafe=false,stopping=false,bindings_restored=false,closed=false,draw_scoped=false;std::string reason;};
// These batches are destroyed by the adapter AFTER releasing its dispatch mutex.
struct ReleaseBatch {std::vector<std::shared_ptr<View>> views;std::vector<std::shared_ptr<Texture>> textures;std::vector<std::shared_ptr<Context>> contexts;};
class Engine {
    struct Pair {Layout layout;std::string digest;const Row* row=nullptr;uint32_t format=0;bool immutable=false,dirty=false;uint64_t epoch=0;std::shared_ptr<Texture> stock;std::shared_ptr<DefaultAdmission> admission;};
    struct ViewRow {uint64_t resource=0,epoch=0;std::shared_ptr<View> canonical,stock;std::shared_ptr<ViewAdmission> lifetime;};
    struct BindRow {uint64_t view=0;uint32_t stage=0,slot=0;std::shared_ptr<ViewAdmission> lifetime;};
    std::shared_ptr<const Catalog> catalog_;std::shared_ptr<Port> port_;std::shared_ptr<Control> control_;Witness witness_;
    std::map<uint64_t,Pair> pairs_;std::map<uint64_t,ViewRow> views_;std::map<uint64_t,std::shared_ptr<Context>> contexts_;std::map<uint64_t,std::map<uint32_t,BindRow>> slots_;
    std::vector<std::shared_ptr<View>> retired_views_;std::vector<std::shared_ptr<Texture>> retired_textures_;std::vector<std::shared_ptr<Context>> retired_contexts_;
    uint64_t gpu_bytes_=0,epoch_=0,ack_=0,frame_=0,replaced_draws_=0;bool original_=false,unsafe_=false,stopping_=false,restored_=false,closed_=false,draw_scoped_=false,restoration_unknown_=false;std::string reason_;
    void PruneBindings(){for(auto& c:slots_)for(auto i=c.second.begin();i!=c.second.end();)if(i->second.lifetime&&!i->second.lifetime->Valid(i->second.view))i=c.second.erase(i);else ++i;}
    bool Enabled()const{return !closed_&&!unsafe_&&!stopping_&&(draw_scoped_||witness_.getter_safe)&&witness_.serialized_immediate&&witness_.main_present;}
    void Restore(bool original,uint64_t only_view=0,uint64_t only_context=0,bool force_canonical=true){
        // Draw-scoped mode leaves canonical state between calls. Never acquire
        // a native context lock while holding the adapter callback mutex here.
        if(draw_scoped_)return;
        for(auto& context:slots_){if(only_context&&context.first!=only_context)continue;const auto ci=contexts_.find(context.first);if(ci==contexts_.end())continue;
            for(auto& item:context.second){if(only_view&&item.second.view!=only_view)continue;auto vi=views_.find(item.second.view);if(vi==views_.end())continue;auto& v=vi->second;
                const uint64_t current=port_->Current(ci->second,item.second.stage,item.second.slot);
                if(current!=v.canonical->handle&&current!=v.stock->handle)continue;
                const auto ri=pairs_.find(v.resource);const bool admitted=ri!=pairs_.end()&&!ri->second.dirty&&ri->second.epoch==v.epoch;
                const bool alternate=!force_canonical&&admitted&&(original==(ri->second.row->role==CanonicalRole::upscaled));
                const uint64_t wanted=alternate?v.stock->handle:v.canonical->handle;
                if(current!=wanted)require(port_->Bind(ci->second,item.second.stage,item.second.slot,wanted),"native rebind not verified");
            }
        }
    }
public:
    Engine(std::shared_ptr<const Catalog> c,std::shared_ptr<Port> p,std::shared_ptr<Control> control,Witness w,bool draw_scoped=false):catalog_(std::move(c)),port_(std::move(p)),control_(std::move(control)),witness_(w),draw_scoped_(draw_scoped){require(catalog_&&port_&&control_,"engine owners missing");}
    int OwnedCapacity(Layout layout,const Bytes& packed)const{
        if(!Enabled())return -1;bool ambiguous=false;const Row*row=catalog_->Find(layout,Fingerprint(layout,packed),ambiguous);if(!row||ambiguous)return -1;
        return pairs_.size()+retired_textures_.size()>=MaxPairs||ByteCount(row->stock)>MaxGpu-gpu_bytes_?0:1;
    }
    bool Resource(uint64_t handle,Layout layout,uint32_t format,bool immutable,const Bytes& packed,std::shared_ptr<DefaultAdmission> admission={}){
        if(!Enabled()||!handle||pairs_.count(handle)||pairs_.size()+retired_textures_.size()>=MaxPairs||Family(format)!=layout.family||((!immutable&&!admission)||(admission&&!admission->Valid(handle))))return false;
        bool ambiguous=false;auto digest=Fingerprint(layout,packed);const Row* row=catalog_->Find(layout,digest,ambiguous);if(!row){if(ambiguous)reason_="ambiguous upload identity";return false;}
        const size_t bytes=ByteCount(row->stock);if(bytes>MaxGpu-gpu_bytes_){reason_="GPU byte cap";return false;}
        // A failed allocation is remembered by disabling this session; never retry.
        auto stock=port_->CreateStock(row->stock,row->stock_pixels,format);if(!stock){Unsafe("stock allocation failed; no retry");return false;}
        gpu_bytes_+=bytes;
        if(admission&&(!admission->Valid(handle)||!admission->Select())){retired_textures_.push_back(std::move(stock));return false;}
        pairs_.emplace(handle,Pair{layout,digest,row,format,immutable,false,++epoch_,std::move(stock),std::move(admission)});return true;
    }
    bool ViewInit(uint64_t resource,uint64_t handle,ViewDesc desc,std::shared_ptr<ViewAdmission> lifetime={}){
        if(lifetime&&(!lifetime->Valid(handle)||lifetime->Resource()!=resource))return false;
        if(!Enabled()||!handle||views_.count(handle)||views_.size()+retired_views_.size()/2>=MaxViews)return false;auto ri=pairs_.find(resource);if(ri==pairs_.end()||ri->second.dirty)return false;
        auto& r=ri->second;if(Family(desc.format)!=r.layout.family||desc.first_mip||desc.layers!=1||(desc.mips!=r.layout.mips&&desc.mips!=UINT32_MAX))return false;
        // A typeless SRV is invalid even if its storage-family identity matches.
        if(desc.format==70||desc.format==73||desc.format==76)return false;
        auto canonical=port_->HoldView(handle);if(!canonical)return false;desc.mips=r.row->stock.mips;
        auto stock=port_->CreateView(r.stock,desc);if(!stock){retired_views_.push_back(std::move(canonical));Unsafe("stock view creation failed; no retry");return false;}
        if(lifetime&&!lifetime->Valid(handle)){retired_views_.push_back(std::move(canonical));retired_views_.push_back(std::move(stock));return false;}
        views_.emplace(handle,ViewRow{resource,r.epoch,std::move(canonical),std::move(stock),std::move(lifetime)});return true;
    }
    bool ContextInit(uint64_t token,std::shared_ptr<Context> context){if(closed_||!token||!context||contexts_.count(token)||contexts_.size()+retired_contexts_.size()>=MaxContexts)return false;contexts_.emplace(token,std::move(context));return true;}
    void Bind(uint64_t context,uint32_t stages,uint32_t first,const uint64_t* views,size_t count,const std::shared_ptr<ViewAdmission>* pending=nullptr){
        auto ci=contexts_.find(context);if(ci==contexts_.end()||!views||!count||first>=128||count>128-first)return;
        if(!ci->second->immediate){for(size_t i=0;i<count;++i)if(views_.count(views[i])||(pending&&pending[i]&&pending[i]->Valid(views[i])))Unsafe("selected resource used in deferred context");return;}
        if(stages==0x7fffffffu){bool null=true;for(size_t i=0;i<count;++i)null&=views[i]==0;if(!null){Unsafe("unknown all-stage nonnull bind");return;}stages=0x3f;}
        if(!stages||(stages&~0x3fu)){Unsafe("unsupported shader stage");return;}
        for(uint32_t stage=1;stage<=32;stage<<=1)if(stages&stage)for(size_t i=0;i<count;++i){auto& map=slots_[context];const uint32_t slot=first+static_cast<uint32_t>(i),key=stage*128+slot;map.erase(key);
            for(const auto& v:views_)if(v.second.stock->handle==views[i]){require(port_->Bind(ci->second,stage,slot,v.second.canonical->handle),"escaped shadow restore failed");Unsafe("shadow SRV escaped through application getter/cache");return;}
            auto vi=views_.find(views[i]);auto lifetime=vi!=views_.end()?vi->second.lifetime:pending?pending[i]:std::shared_ptr<ViewAdmission>{};
            if(vi==views_.end()&&(!lifetime||!lifetime->Valid(views[i])))continue;
            if(lifetime&&!lifetime->Valid(views[i]))continue;
            map.emplace(key,BindRow{views[i],stage,slot,std::move(lifetime)});
        }
        if(Enabled()&&!draw_scoped_)Restore(original_,0,0,false);else Restore(false);
    }
    void Invalidate(uint64_t resource,const char* why){auto ri=pairs_.find(resource);if(ri!=pairs_.end()){ri->second.dirty=true;if(ri->second.admission)ri->second.admission->Retire();reason_=why;Restore(original_,0,0,draw_scoped_);}}
    void InvalidateView(uint64_t view,const char* why){auto vi=views_.find(view);if(vi!=views_.end())Invalidate(vi->second.resource,why);}
    void Unsafe(const char* why){unsafe_=true;stopping_=true;reason_=why;Restore(false);original_=false;restored_=!restoration_unknown_;}
    // A fault supplies no verified restoration witness. Poison is monotonic:
    // a later draw-scoped no-op Restore/Present/retirement cannot clear UNKNOWN.
    void Fault(const char* why)noexcept{unsafe_=true;stopping_=true;restored_=false;restoration_unknown_=true;try{reason_=why;}catch(...) {}}
    void Getter(uint64_t context){if(contexts_.count(context))Unsafe("getter observation requires external canonicalization; session refused");}
    bool AfterGetter(uint64_t context,uint32_t stage,uint32_t first,uint64_t* returned,size_t count){
        if(!contexts_.count(context)||!contexts_.at(context)->immediate||!returned||first>=128||count>128-first||!stage||(stage&(stage-1))||(stage&~0x3fu))return false;
        for(size_t i=0;i<count;++i)for(const auto& v:views_)if(returned[i]&&returned[i]==v.second.stock->handle){require(port_->ReplaceGetter(returned[i],v.second.canonical->handle),"post-get canonicalization failed");break;}
        return true;
    }
    // True means the wrapper original MUST be skipped. If invocation throws,
    // execution is unknown and must never be retried by falling through.
    bool DrawScope(uint64_t context,const std::function<void()>& draw,const std::shared_ptr<DrawLease>& lease){
        if(!draw_scoped_||!Enabled()||!draw)return false;auto ci=contexts_.find(context);if(ci==contexts_.end()||!ci->second->immediate)return false;
        if(!lease||!lease->identity||lease->identity!=ci->second->identity)return false;
        PruneBindings();std::vector<BindingWitness> work;
        for(const auto& item:slots_[context]){const auto& b=item.second;if(b.stage==32)continue;auto vi=views_.find(b.view);if(vi==views_.end())continue;const auto& v=vi->second;if(b.lifetime!=v.lifetime||(v.lifetime&&!v.lifetime->Valid(b.view)))continue;auto ri=pairs_.find(v.resource);if(ri==pairs_.end()||ri->second.dirty||((!ri->second.immutable&&!ri->second.admission)||(ri->second.admission&&!ri->second.admission->Valid(v.resource)))||ri->second.epoch!=v.epoch||original_!=(ri->second.row->role==CanonicalRole::upscaled))continue;
            if(port_->Current(ci->second,b.stage,b.slot)!=v.canonical->handle){Unsafe("draw preflight canonical binding differs");return false;}work.push_back({b.stage,b.slot,v.canonical->handle,v.stock->handle});}
        if(work.empty())return false;if(!port_->ArmBindings(ci->second,work)){Fault("exact draw binding-set refused");return false;}size_t attempted=0;bool invoked=false,failed=false,restored=true;
        try{for(const auto& s:work){++attempted;require(port_->Bind(ci->second,s.stage,s.slot,s.stock),"draw stock bind failed");}invoked=true;draw();++replaced_draws_;}catch(...){failed=true;}
        // Restore every attempted slot even after a reported bind failure.
        for(size_t i=0;i<attempted;++i)try{const auto& s=work[i];if(!port_->Bind(ci->second,s.stage,s.slot,s.canonical))restored=false;}catch(...){restored=false;}
        if(failed||!restored)Fault(restored?"draw/bind fault; canonical bindings restored; no draw retry":"draw scope restoration UNKNOWN; owners retained");
        return invoked||!restored;
    }
    void Deferred(){if(!pairs_.empty())Unsafe("deferred replay/finished-list roles unsupported for A/B");}
    void Present(){
        if(closed_||!witness_.main_present)return;PruneBindings();++frame_;
        if(stopping_||unsafe_){Restore(false);original_=false;restored_=!restoration_unknown_;return;}
        const uint64_t request=control_->Load();if((request>>1)>ack_){const bool mode=(request&1)!=0;
            if(mode&&!Enabled()){reason_="safety witnesses incomplete";return;}
            Restore(mode,0,0,draw_scoped_);original_=mode;ack_=request>>1;
        }
        Restore(original_,0,0,draw_scoped_);
    }
    void Stop(){stopping_=true;}
    void ViewDestroy(uint64_t handle){
        for(auto& c:slots_)for(auto i=c.second.begin();i!=c.second.end();)if(i->second.view==handle)i=c.second.erase(i);else ++i;
        auto vi=views_.find(handle);if(vi==views_.end())return;Restore(false,handle);
        retired_views_.push_back(std::move(vi->second.canonical));retired_views_.push_back(std::move(vi->second.stock));views_.erase(vi);
    }
    void ResourceDestroy(uint64_t handle){
        for(auto& c:slots_)for(auto i=c.second.begin();i!=c.second.end();)if(i->second.lifetime&&i->second.lifetime->Resource()==handle)i=c.second.erase(i);else ++i;
        auto ri=pairs_.find(handle);if(ri==pairs_.end())return;Invalidate(handle,"resource retired");
        std::vector<uint64_t> handles;for(const auto& v:views_)if(v.second.resource==handle)handles.push_back(v.first);for(auto h:handles)ViewDestroy(h);
        retired_textures_.push_back(std::move(ri->second.stock));pairs_.erase(ri);
    }
    void ContextDestroy(uint64_t token){auto ci=contexts_.find(token);if(ci==contexts_.end())return;Restore(false,0,token);slots_.erase(token);retired_contexts_.push_back(std::move(ci->second));contexts_.erase(ci);}
    void ResetContextBindings(uint64_t token){if(contexts_.count(token)){Restore(false,0,token);slots_.erase(token);}}
    // Caller supplies an EXTERNAL GPU-quiescent witness, after stop+safe present.
    // Destruction/retirement alone never frees objects in a callback.
    bool Drain(bool gpu_quiescent,ReleaseBatch& out){if(restoration_unknown_)return false;if(closed_)return true;if(!stopping_||!restored_||!gpu_quiescent||!out.views.empty()||!out.textures.empty()||!out.contexts.empty())return false;
        out.views.reserve(views_.size()*2+retired_views_.size());out.textures.reserve(pairs_.size()+retired_textures_.size());out.contexts.reserve(contexts_.size()+retired_contexts_.size());
        for(auto& v:views_){out.views.push_back(std::move(v.second.canonical));out.views.push_back(std::move(v.second.stock));}views_.clear();
        for(auto& p:pairs_)out.textures.push_back(std::move(p.second.stock));pairs_.clear();
        for(auto& c:contexts_)out.contexts.push_back(std::move(c.second));contexts_.clear();slots_.clear();
        for(auto& v:retired_views_)out.views.push_back(std::move(v));retired_views_.clear();for(auto& t:retired_textures_)out.textures.push_back(std::move(t));retired_textures_.clear();for(auto& c:retired_contexts_)out.contexts.push_back(std::move(c));retired_contexts_.clear();gpu_bytes_=0;closed_=true;return true;
    }
    Status Snapshot()const{Status s;for(const auto& r:pairs_)if(!r.second.dirty)++s.pairs;s.views=views_.size();for(const auto& c:slots_)s.bindings+=c.second.size();s.gpu_bytes=gpu_bytes_;s.ack_sequence=ack_;s.frame=frame_;s.replaced_draws=replaced_draws_;s.draw_scoped=draw_scoped_;s.original=original_;s.unsafe=unsafe_;s.stopping=stopping_;s.bindings_restored=restored_&&!restoration_unknown_;s.closed=closed_;s.reason=reason_;return s;}
};
}
