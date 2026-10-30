#pragma once
// Typed API20 callback library only. No DllMain, register_event, DLL, or loader.
#include "engine.hpp"
#include <reshade.hpp>
#include "default_policy.hpp"
#include <functional>
#include <mutex>
#include <thread>
#include <type_traits>
namespace tqab {
namespace api=reshade::api;
struct AdapterPort:Port {
    virtual std::shared_ptr<Context> CaptureContext(api::command_list*)=0;
    virtual std::shared_ptr<DefaultAdmission> Admission(uint64_t){return {};}
    virtual std::shared_ptr<ViewAdmission> PendingView(uint64_t){return {};}
    virtual bool DescribeView(uint64_t view,uint64_t resource,ViewDesc& out)=0;
    virtual void Poison()noexcept=0;virtual bool Healthy()noexcept=0;
    virtual void ForwardDraw(const std::shared_ptr<Context>&,bool,uint32_t,uint32_t,uint32_t,int32_t,uint32_t)=0;
};
inline bool SafeRead(void* destination,const void* source,size_t bytes)noexcept{
#ifdef _MSC_VER
    __try{std::memcpy(destination,source,bytes);return true;}__except(EXCEPTION_EXECUTE_HANDLER){return false;}
#else
    return Copy(destination,source,bytes);
#endif
}
class Adapter {
    struct Pending {Layout layout;uint32_t format=0;bool immutable=false,disabled=false;std::vector<Bytes> mips;std::shared_ptr<DefaultAdmission> admission;};
    mutable std::mutex mutex_;mutable std::mutex status_mutex_;Status published_status_;std::atomic<uint32_t> fault_code_{0};std::shared_ptr<const Catalog> catalog_;std::shared_ptr<AdapterPort> port_;std::shared_ptr<Control> control_;Witness witness_;
    std::unique_ptr<Engine> engine_;api::device* device_=nullptr;api::command_queue* present_queue_=nullptr;api::swapchain* present_swapchain_=nullptr;std::map<api::command_list*,uint64_t> contexts_;uint64_t next_context_=0;std::map<uint64_t,Pending> pending_;size_t pending_bytes_=0;std::atomic<bool> fault_{false};bool draw_scoped_=false;
    inline static thread_local bool inside_=false;std::atomic<bool> pending_poison_{false};using Frame=tqdefault::HeldFrame<Adapter,std::unique_lock<std::mutex>>;
    void Poison(uint32_t code)noexcept{uint32_t zero=0;fault_code_.compare_exchange_strong(zero,code);port_->Poison();}
    void PublishStatus(){auto s=engine_?engine_->Snapshot():Status{};std::lock_guard read_lock(status_mutex_);published_status_=std::move(s);}
    void ApplyPoison(){if(pending_poison_.load()){fault_=true;if(engine_)engine_->Fault("unproved callback reentry; owners retained");}}
    struct Guard {Guard(){inside_=true;}~Guard(){inside_=false;}};
    template<class F>void Dispatch(F fn)noexcept{if(inside_){pending_poison_=true;Poison(3901);return;}Guard guard;std::unique_lock lock(mutex_,std::try_to_lock);if(!lock.owns_lock()){pending_poison_=true;Poison(3902);return;}Frame frame(this,lock);try{ApplyPoison();fn();if(engine_&&engine_->Snapshot().unsafe)Poison(3903);}catch(const std::exception& e){fault_=true;Poison(3904);if(engine_)engine_->Fault(e.what());}catch(...){fault_=true;Poison(3905);if(engine_)engine_->Fault("unknown callback fault");}}
    uint64_t ContextToken(api::command_list* command)const{auto i=contexts_.find(command);return i==contexts_.end()?0:i->second;}
    static bool Description(const api::resource_desc& d,Layout& layout,bool& immutable){
        const uint32_t permitted=static_cast<uint32_t>(api::resource_usage::shader_resource)|static_cast<uint32_t>(api::resource_usage::copy_source)|static_cast<uint32_t>(api::resource_usage::copy_dest)|static_cast<uint32_t>(api::resource_usage::resolve_dest);
        const uint32_t usage=static_cast<uint32_t>(d.usage),shader=static_cast<uint32_t>(api::resource_usage::shader_resource);
        if(d.type!=api::resource_type::texture_2d||d.heap!=api::memory_heap::default_||d.texture.depth_or_layers!=1||d.texture.samples!=1||!(usage&shader)||(usage&~permitted)||(d.flags!=api::resource_flags::none&&d.flags!=api::resource_flags::immutable)||!Family(static_cast<uint32_t>(d.texture.format)))return false;
        layout={d.texture.width,d.texture.height,d.texture.levels,Family(static_cast<uint32_t>(d.texture.format)),1};(void)Levels(layout);immutable=d.flags==api::resource_flags::immutable;return true;
    }
    void ClearPending(Pending& p){for(auto& mip:p.mips){pending_bytes_-=mip.size();mip.clear();}}
public:
    Adapter(std::shared_ptr<const Catalog> catalog,std::shared_ptr<AdapterPort> port,std::shared_ptr<Control> control,Witness witness,bool draw_scoped=false):catalog_(std::move(catalog)),port_(std::move(port)),control_(std::move(control)),witness_(witness),draw_scoped_(draw_scoped){require(catalog_&&port_&&control_,"adapter owners missing");}
    // NativePort has already validated the actual API2 recorded D3D11 pair.
    void InitDevice(api::device* device){Dispatch([&]{if(device_||!device||!port_->Healthy())return;device_=device;engine_=std::make_unique<Engine>(catalog_,port_,control_,witness_,draw_scoped_);});}
    bool AdmitPresent(api::command_queue* queue,api::swapchain* swapchain){std::lock_guard lock(mutex_);if(!engine_||!queue||!swapchain||present_queue_||present_swapchain_)return false;present_queue_=queue;present_swapchain_=swapchain;return true;}
    void InitContext(api::command_list* command){Dispatch([&]{if(!engine_||!command||contexts_.count(command))return;auto context=port_->CaptureContext(command);if(context&&engine_->ContextInit(next_context_+1,context)){contexts_.emplace(command,++next_context_);}});}
    void InitResource(api::device* device,const api::resource_desc& desc,const api::subresource_data* initial,api::resource resource){Dispatch([&]{
        if(!engine_||device!=device_||!resource.handle||pending_.count(resource.handle))return;Layout layout;bool immutable=false;if(!Description(desc,layout,immutable)||!catalog_->HasUploadLayout(layout)||pending_.size()>=64)return;
        auto admission=immutable?std::shared_ptr<DefaultAdmission>{}:port_->Admission(resource.handle);if(!immutable&&(!initial||!admission||!admission->Valid(resource.handle)))return;
        Pending p{layout,static_cast<uint32_t>(desc.texture.format),immutable,!immutable,std::vector<Bytes>(layout.mips),admission};
        if(initial){std::vector<Span> spans;auto levels=Levels(layout);for(size_t mip=0;mip<levels.size();++mip){api::subresource_data s{};require(SafeRead(&s,initial+mip,sizeof s),"initial metadata fence");uint64_t limit=uint64_t(s.row_pitch)*(levels[mip].rows-1)+levels[mip].row;require(limit<=MaxCpu,"initial pitch overflow");spans.push_back({s.data,s.row_pitch,s.slice_pitch,static_cast<size_t>(limit)});}auto packed=Pack(layout,spans.data(),spans.size(),SafeRead);engine_->Resource(resource.handle,layout,p.format,immutable,packed,p.admission);}
        pending_.emplace(resource.handle,std::move(p));
    });}
    // Only the genuine selected Present drain invokes these.0 BUSY retains work;
    //1 consumed;2 normal dirty cancellation;-1 explicit integration/capacity fault.
    int OwnedResource(uint64_t handle,Layout layout,uint32_t format,bool immutable,const Bytes& bytes,std::shared_ptr<DefaultAdmission> admission,const std::function<bool()>& publish)noexcept{
        if(inside_)return 0;std::unique_lock lock(mutex_,std::try_to_lock);if(!lock.owns_lock())return 0;Guard guard;Frame frame(this,lock);
        try{ApplyPoison();if(!engine_||fault_)return -1;if(!publish())return 2;if(!admission||!admission->Valid(handle))return 2;const auto capacity=engine_->OwnedCapacity(layout,bytes);if(capacity<=0)return admission->Valid(handle)?(capacity==0?-2:-1):2;
            if(engine_->Resource(handle,layout,format,immutable,bytes,admission))return admission->Valid(handle)?1:2;
            return admission->Valid(handle)?-1:2;
        }catch(...){pending_poison_=true;Poison(3906);return -1;}
    }
    int OwnedView(api::device*d,api::resource r,api::resource_view v)noexcept{
        if(inside_)return 0;std::unique_lock lock(mutex_,std::try_to_lock);if(!lock.owns_lock())return 0;Guard guard;Frame frame(this,lock);
        try{if(!engine_||d!=device_)return -1;auto lifetime=port_->PendingView(v.handle);if(!lifetime||!lifetime->Valid(v.handle))return 2;if(lifetime->Resource()!=r.handle)return -1;ViewDesc actual;if(!port_->DescribeView(v.handle,r.handle,actual))return lifetime->Valid(v.handle)?-1:2;return engine_->ViewInit(r.handle,v.handle,actual,lifetime)?1:lifetime->Valid(v.handle)?-1:2;}catch(...){Poison(3907);return -1;}
    }
    int OwnedDirty(uint64_t resource)noexcept{
        if(inside_)return 0;std::unique_lock lock(mutex_,std::try_to_lock);if(!lock.owns_lock())return 0;Guard guard;Frame frame(this,lock);
        try{if(engine_)engine_->Invalidate(resource,"observed creation lifetime retired");return 1;}catch(...){Poison(3908);return -1;}
    }
#if defined(TQPC_TEST_LEAVES)
    void TestHold(const std::function<void()>& leaf){std::lock_guard lock(mutex_);leaf();}
#endif
    void InitView(api::device* device,api::resource resource,api::resource_usage usage,api::resource_view view){Dispatch([&]{if(!engine_||device!=device_||usage!=api::resource_usage::shader_resource)return;ViewDesc actual;if(port_->DescribeView(view.handle,resource.handle,actual))engine_->ViewInit(resource.handle,view.handle,actual);});}
    void Push(api::command_list* command,api::shader_stage stages,uint32_t param,const api::descriptor_table_update& update){Dispatch([&]{
        if(!engine_||update.type!=api::descriptor_type::shader_resource_view||!update.descriptors||param!=1||update.array_offset||!update.count||update.binding>=128||update.count>128-update.binding)return;
        std::vector<uint64_t> handles(update.count);auto data=static_cast<const api::resource_view*>(update.descriptors);for(size_t i=0;i<handles.size();++i){api::resource_view copy{};require(SafeRead(&copy,data+i,sizeof copy),"descriptor metadata fence");handles[i]=copy.handle;}std::vector<std::shared_ptr<ViewAdmission>> lifetimes;lifetimes.reserve(handles.size());for(auto h:handles)lifetimes.push_back(port_->PendingView(h));engine_->Bind(ContextToken(command),static_cast<uint32_t>(stages),update.binding,handles.data(),handles.size(),lifetimes.data());
    });}
    bool Update(api::device* device,const api::subresource_data& data,api::resource resource,uint32_t mip,const api::subresource_box* box){Dispatch([&]{
        if(!engine_||device!=device_)return;engine_->Invalidate(resource.handle,"application texture update");auto it=pending_.find(resource.handle);if(it==pending_.end()||it->second.disabled)return;auto& p=it->second;
        const auto levels=Levels(p.layout);if(mip>=levels.size()){p.disabled=true;ClearPending(p);return;}const auto& l=levels[mip];
        if(box&&(box->left||box->top||box->front||box->right!=l.width||box->bottom!=l.height||box->back!=1)){p.disabled=true;ClearPending(p);return;}
        if(mip==0)ClearPending(p);else if(p.mips[0].empty()||!p.mips[mip].empty()){p.disabled=true;ClearPending(p);return;}
        Layout one{l.width,l.height,1,p.layout.family,p.layout.representation};uint64_t limit=uint64_t(data.row_pitch)*(l.rows-1)+l.row;require(limit<=MaxCpu,"update pitch overflow");Span span{data.data,data.row_pitch,data.slice_pitch,static_cast<size_t>(limit)};Bytes bytes=Pack(one,&span,1,SafeRead);
        require(bytes.size()<=MaxCpu-pending_bytes_,"collector CPU cap");pending_bytes_+=bytes.size();p.mips[mip]=std::move(bytes);
        if(std::all_of(p.mips.begin(),p.mips.end(),[](const Bytes& x){return !x.empty();})){Bytes packed;packed.reserve(ByteCount(p.layout));for(const auto& m:p.mips)packed.insert(packed.end(),m.begin(),m.end());engine_->Resource(resource.handle,p.layout,p.format,p.immutable,packed);ClearPending(p);}
    });return false;}
    void Dirty(api::device* device,api::resource resource,const char* why){Dispatch([&]{if(engine_&&device==device_){engine_->Invalidate(resource.handle,why);auto p=pending_.find(resource.handle);if(p!=pending_.end()){p->second.disabled=true;ClearPending(p->second);}}});}
    void DirtyCommand(api::command_list* command,api::resource resource,const char* why){Dispatch([&]{if(engine_&&ContextToken(command)){engine_->Invalidate(resource.handle,why);auto p=pending_.find(resource.handle);if(p!=pending_.end()){p->second.disabled=true;ClearPending(p->second);}}});}
    void DirtyView(api::command_list* command,api::resource_view view,const char* why){Dispatch([&]{if(engine_&&ContextToken(command))engine_->InvalidateView(view.handle,why);});}
    // External mutation bridge seams. The bridge must prove native ownership;
    // these functions do not install hooks or assert that coverage exists.
    bool NativeObserver(uint64_t resource,bool close,bool swap)noexcept{
        if(inside_){pending_poison_=true;Poison(3909);return false;}
        Guard guard;std::unique_lock lock(mutex_,std::try_to_lock);
        if(!lock.owns_lock()){pending_poison_=true;Poison(3910);return false;}
        Frame frame(this,lock);try{ApplyPoison();if(!engine_)return true;
            if(resource){engine_->Invalidate(resource,"provider-observed DEFAULT mutation");auto p=pending_.find(resource);if(p!=pending_.end()){p->second.disabled=true;ClearPending(p->second);}}
            if(swap)for(auto& c:contexts_)engine_->ResetContextBindings(c.second);
            if(close){engine_->Fault("provider observation closed");fault_=true;Poison(3911);return false;}return true;
        }catch(...){pending_poison_=true;Poison(3912);return false;}
    }
    void NativeFault()noexcept{pending_poison_=true;Poison(3913);}
    void InvalidateNativeResource(uint64_t resource,const char*){NativeObserver(resource,false,false);}
    void InvalidateContextBindings(api::command_list* command){Dispatch([&]{if(engine_)engine_->ResetContextBindings(ContextToken(command));});}
    void Deferred(api::command_list* command){Dispatch([&]{if(engine_&&ContextToken(command))engine_->Deferred();});}
    void UnsafeOperation(api::command_list* command,const char* why){Dispatch([&]{if(engine_&&ContextToken(command))engine_->Unsafe(why);});}
    // New provider seam: post native getter, original returned-array refs intact.
    void AfterGet(api::command_list* command,api::shader_stage stage,uint32_t first,uint32_t count,api::resource_view* returned){Dispatch([&]{
        if(!engine_||!returned||!count||first>=128||count>128-first)return;
        // Publish each adjusted reference immediately, even if a later slot faults.
        for(uint32_t i=0;i<count;++i)if(!engine_->AfterGetter(ContextToken(command),static_cast<uint32_t>(stage),first+i,&returned[i].handle,1))return;
    });}
    bool Draw(api::command_list* command,bool indexed,uint32_t count,uint32_t instances,uint32_t first,int32_t offset,uint32_t first_instance)noexcept{
        if(inside_){pending_poison_=true;Poison(3914);return false;}Guard guard;std::unique_lock lock(mutex_,std::defer_lock);
        try{auto observed=port_->CaptureContext(command);auto lease=port_->LockContext(observed);if(!lease)return false;if(!lock.try_lock()){pending_poison_=true;Poison(3915);return false;}Frame frame(this,lock);ApplyPoison();if(!engine_||!ContextToken(command))return false;
            // API20 uses all-zero draw arguments to represent D3D11 DrawAuto.
            if(!instances){if(!indexed&&!count&&!first&&!offset&&!first_instance)engine_->Unsafe("DrawAuto sentinel unsupported; original preserved");return false;}
            if(!count||instances!=1||first_instance)return false;
            bool consumed=engine_->DrawScope(ContextToken(command),[&]{port_->ForwardDraw(observed,indexed,count,instances,first,offset,first_instance);},lease);if(engine_->Snapshot().unsafe)Poison(3916);if(consumed)return true;if(!port_->Healthy()||engine_->Snapshot().unsafe)return false;try{port_->ForwardDraw(observed,indexed,count,instances,first,offset,first_instance);return true;}catch(...){engine_->Fault("raw original draw outcome UNKNOWN; no retry");Poison(3917);return true;}
        // DrawScope contains all bind/invocation/restore failures internally.
        // An exception escaping here is preflight-only: no invocation/bind has
        // occurred. Preserve the wrapper's untouched original draw (peer F1).
        }catch(const std::exception& e){pending_poison_=true;Poison(3918);if(lock.owns_lock()){fault_=true;if(engine_)engine_->Fault(e.what());}return false;}catch(...){pending_poison_=true;Poison(3919);if(lock.owns_lock()){fault_=true;if(engine_)engine_->Fault("draw preflight failed before invocation");}return false;}
    }
    void Present(){Dispatch([&]{if(engine_)engine_->Present();PublishStatus();});}
    void PresentEvent(api::command_queue* queue,api::swapchain* swapchain){Dispatch([&]{if(engine_&&queue==present_queue_&&swapchain==present_swapchain_&&queue&&swapchain){engine_->Present();PublishStatus();}});}
    void DestroyView(api::device* device,api::resource_view view){Dispatch([&]{if(engine_&&device==device_)engine_->ViewDestroy(view.handle);});}
    void DestroyResource(api::device* device,api::resource resource){Dispatch([&]{if(engine_&&device==device_)engine_->ResourceDestroy(resource.handle);auto p=pending_.find(resource.handle);if(p!=pending_.end()){ClearPending(p->second);pending_.erase(p);}});}
    void DestroyContext(api::command_list* command){Dispatch([&]{auto i=contexts_.find(command);if(engine_&&i!=contexts_.end()){engine_->ContextDestroy(i->second);contexts_.erase(i);}});}
    void Stop(){Dispatch([&]{if(engine_)engine_->Stop();});}
    void DestroyDevice(api::device* device){Dispatch([&]{if(engine_&&device==device_)engine_->Stop();});}
    bool Drain(bool gpu_quiescent){ReleaseBatch release;bool done=false;{std::lock_guard lock(mutex_);ApplyPoison();if(engine_)done=engine_->Drain(gpu_quiescent,release);PublishStatus();}return done;}
    // Last completed selected Present/drain observation. A reader never owns
    // the callback mutex and cannot poison native ownership on contention.
    Status Snapshot()const{std::lock_guard lock(status_mutex_);auto s=published_status_;if(pending_poison_.load()||fault_.load()||fault_code_.load()){s.unsafe=true;s.stopping=true;s.bindings_restored=false;}return s;}
    bool Faulted()const noexcept{return fault_.load()||pending_poison_.load()||fault_code_.load()!=0;}
    uint32_t FaultCode()const noexcept{return fault_code_.load();}
};
// Publication owns the callback target. Actual SDK registration is external.
class Entries {
    inline static std::mutex mutex_;inline static std::shared_ptr<Adapter> owner_;
    static std::shared_ptr<Adapter> Acquire(){std::lock_guard lock(mutex_);return owner_;}
public:
    static bool Publish(std::shared_ptr<Adapter> owner){std::lock_guard lock(mutex_);if(owner_||!owner)return false;owner_=std::move(owner);return true;}
    static bool Unpublish(){auto expected=Acquire();if(expected&&!expected->Snapshot().closed)return false;std::shared_ptr<Adapter> release;{std::lock_guard lock(mutex_);if(owner_!=expected)return false;release=std::move(owner_);}return true;}
    static void init_device(api::device* d){if(auto o=Acquire())o->InitDevice(d);}
    static void init_command_list(api::command_list* c){if(auto o=Acquire())o->InitContext(c);}
    static void init_resource(api::device* d,const api::resource_desc& desc,const api::subresource_data* data,api::resource_usage,api::resource r){if(auto o=Acquire())o->InitResource(d,desc,data,r);}
    static void init_resource_view(api::device* d,api::resource r,api::resource_usage u,const api::resource_view_desc&,api::resource_view v){if(auto o=Acquire())o->InitView(d,r,u,v);}
    static void push_descriptors(api::command_list* c,api::shader_stage s,api::pipeline_layout,uint32_t p,const api::descriptor_table_update& u){if(auto o=Acquire())o->Push(c,s,p,u);}
    static bool update_texture_region(api::device* d,const api::subresource_data& s,api::resource r,uint32_t m,const api::subresource_box* b){if(auto o=Acquire())return o->Update(d,s,r,m,b);return false;}
    static bool update_texture_region_command(api::command_list* c,const api::subresource_data&,api::resource r,uint32_t,const api::subresource_box*){if(auto o=Acquire())o->DirtyCommand(c,r,"deferred update");return false;}
    static void map_texture_region(api::device* d,api::resource r,uint32_t,const api::subresource_box*,api::map_access,api::subresource_data*){if(auto o=Acquire())o->Dirty(d,r,"map invalidates identity; no mapped bytes read");}
    static void unmap_texture_region(api::device* d,api::resource r,uint32_t){if(auto o=Acquire())o->Dirty(d,r,"unmap invalidates identity");}
    static bool copy_resource(api::command_list* c,api::resource,api::resource dest){if(auto o=Acquire())o->DirtyCommand(c,dest,"copy destination");return false;}
    static bool copy_texture_region(api::command_list* c,api::resource,uint32_t,const api::subresource_box*,api::resource dest,uint32_t,const api::subresource_box*,api::filter_mode){if(auto o=Acquire())o->DirtyCommand(c,dest,"texture copy destination");return false;}
    static bool copy_buffer_to_texture(api::command_list* c,api::resource,uint64_t,uint32_t,uint32_t,api::resource dest,uint32_t,const api::subresource_box*){if(auto o=Acquire())o->DirtyCommand(c,dest,"buffer copy destination");return false;}
    static bool resolve_texture_region(api::command_list* c,api::resource,uint32_t,const api::subresource_box*,api::resource dest,uint32_t,uint32_t,uint32_t,uint32_t,api::format){if(auto o=Acquire())o->DirtyCommand(c,dest,"resolve destination");return false;}
    static bool generate_mipmaps(api::command_list* c,api::resource_view view){if(auto o=Acquire())o->DirtyView(c,view,"generated mip identity changed");return false;}
    static void execute_secondary_command_list(api::command_list* c,api::command_list*){if(auto o=Acquire())o->Deferred(c);}
    static void reset_command_list(api::command_list* c){if(auto o=Acquire())o->UnsafeOperation(c,"context/reset state unsupported");}
    static void destroy_resource_view(api::device* d,api::resource_view v){if(auto o=Acquire())o->DestroyView(d,v);}
    static void destroy_resource(api::device* d,api::resource r){if(auto o=Acquire())o->DestroyResource(d,r);}
    static void destroy_command_list(api::command_list* c){if(auto o=Acquire())o->DestroyContext(c);}
    static void destroy_device(api::device* d){if(auto o=Acquire())o->DestroyDevice(d);}
    // A coordinator MUST admit exact queue/swapchain ownership before dispatch.
    static void present(api::command_queue* q,api::swapchain* s,const api::rect*,const api::rect*,uint32_t,const api::rect*){if(auto o=Acquire())o->PresentEvent(q,s);}
    static bool draw(api::command_list* c,uint32_t count,uint32_t instances,uint32_t first,uint32_t first_instance){if(auto o=Acquire())return o->Draw(c,false,count,instances,first,0,first_instance);return false;}
    static bool draw_indexed(api::command_list* c,uint32_t count,uint32_t instances,uint32_t first,int32_t offset,uint32_t first_instance){if(auto o=Acquire())return o->Draw(c,true,count,instances,first,offset,first_instance);return false;}
    static bool draw_or_dispatch_indirect(api::command_list* c,api::indirect_command,api::resource,uint64_t,uint32_t,uint32_t){if(auto o=Acquire())o->UnsafeOperation(c,"indirect draw/dispatch unsupported; original preserved");return false;}
    static void after_get(api::command_list* c,api::shader_stage s,uint32_t first,uint32_t count,api::resource_view* returned){if(auto o=Acquire())o->AfterGet(c,s,first,count,returned);}
};
#define TQAB_TRAIT(name) static_assert(std::is_same_v<decltype(&Entries::name),reshade::addon_event_traits<reshade::addon_event::name>::decl>,"API20 trait mismatch: " #name)
TQAB_TRAIT(init_device);TQAB_TRAIT(init_command_list);TQAB_TRAIT(init_resource);TQAB_TRAIT(init_resource_view);TQAB_TRAIT(push_descriptors);TQAB_TRAIT(update_texture_region);TQAB_TRAIT(update_texture_region_command);TQAB_TRAIT(map_texture_region);TQAB_TRAIT(unmap_texture_region);TQAB_TRAIT(copy_resource);TQAB_TRAIT(copy_texture_region);TQAB_TRAIT(execute_secondary_command_list);TQAB_TRAIT(reset_command_list);TQAB_TRAIT(destroy_resource_view);TQAB_TRAIT(destroy_resource);TQAB_TRAIT(destroy_command_list);TQAB_TRAIT(destroy_device);TQAB_TRAIT(present);TQAB_TRAIT(draw);TQAB_TRAIT(draw_indexed);TQAB_TRAIT(draw_or_dispatch_indirect);
TQAB_TRAIT(copy_buffer_to_texture);TQAB_TRAIT(resolve_texture_region);TQAB_TRAIT(generate_mipmaps);
#undef TQAB_TRAIT
}
