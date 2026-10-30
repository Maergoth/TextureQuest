// Allocation-free protocol/forwarding core, also used by the invented-leaf CPU cases.
#pragma once
#include "texturequest_provider_mutation_v2.hpp"
#include <atomic>
#include <limits>
#include <type_traits>
namespace tqpm {
struct Event {
    Header header{}; Boundary boundary{};
    ID3D11Resource *resource=nullptr; ID3D11View *view=nullptr;
    const D3D11_RECT *rects=nullptr; uint32_t rect_count=0;
    ID3DDeviceContextState *state=nullptr; ID3DDeviceContextState *const *previous=nullptr;
};
inline uint32_t Dispatch(const Callbacks &c, void *cookie, const Event &e) {
    switch(e.header.operation) {
    case Operation::discard_resource:return c.resource(cookie,&e.header,e.resource);
    case Operation::discard_view:return c.view(cookie,&e.header,e.view);
    case Operation::discard_view1:return c.view1(cookie,&e.header,e.view,e.rects,e.rect_count);
    case Operation::swap_state:return c.swap(cookie,&e.header,e.state,e.previous);
    case Operation::boundary:return c.boundary(cookie,&e.header,&e.boundary);
    } return 0;
}
using Delivery = bool(*)(void *, const Event &) noexcept;
inline bool ValidRegistrationShape(const Registration &r) noexcept {
    bool nonzero=false;for(auto x:r.nonce)nonzero=nonzero||x!=0;
    return r.size==sizeof(r)&&r.abi_version==version&&!(r.required_capabilities&~capabilities)&&r.observer_module&&nonzero&&
        r.callbacks.size==sizeof(r.callbacks)&&r.callbacks.abi_version==version&&r.callbacks.resource&&r.callbacks.view&&r.callbacks.view1&&r.callbacks.swap&&r.callbacks.boundary;
}
inline bool NextSequence(uint64_t &value) noexcept {
    if(value==std::numeric_limits<uint64_t>::max())return false;
    ++value; return true;
}
class Core {
    struct Device {std::atomic<const void*> api{nullptr},proxy{nullptr},dxgi{nullptr};std::atomic<void*> native{nullptr};
        std::atomic<uint64_t> pending{0};std::atomic<bool> live{false},unsupported{false};};
    struct Context {std::atomic<const void*> api{nullptr},queue{nullptr},proxy{nullptr},device{nullptr};
        std::atomic<void*> native{nullptr};std::atomic<uint64_t> id{0};std::atomic<uint32_t> kind{0};std::atomic<bool> live{false};};
    Device devices_[16]; Context contexts_[64];
    std::atomic_flag write_=ATOMIC_FLAG_INIT;
    enum class HealthPhase : uint32_t { pristine, registering, open, dispatching, closed };
    std::atomic<HealthPhase> phase_{HealthPhase::pristine};
    std::atomic<uint32_t> registered_{0},error_{0},inflight_{0};
#if defined(TQPM_CPU_TEST_SEAMS)
public:
    enum class TestPoint { register_precommit, bind_prepublish, retire_premark, callback_preadmission,
        creation_precommit, boundary_preadmission };
    using TestPause = void(*)(void *, TestPoint) noexcept;
    void SetTestPause(TestPause fn,void *cookie) noexcept {test_pause_=fn;test_cookie_=cookie;}
private:
    TestPause test_pause_=nullptr;void *test_cookie_=nullptr;
    void Pause(TestPoint p) noexcept {if(test_pause_)test_pause_(test_cookie_,p);}
#endif
    std::atomic<uint64_t> first_sequence_{0},first_context_{0},first_caller_{0};
    std::atomic<uint64_t> first_last_sequence_{0},first_known_{0};
    std::atomic<const void*> first_object_{nullptr},first_device_{nullptr};
    std::atomic<uint32_t> first_operation_{0},first_origin_{0};
    std::atomic<uint64_t> device_count_{0},context_count_{0},sequence_{0},bound_{0},lease_sequence_{0},creation_sequence_{0};
    std::atomic<const void*> selected_context_{nullptr},selected_device_{nullptr};
    uintptr_t owner_=0; uint64_t token_=0; uint8_t nonce_[16]{};
    Delivery delivery_=nullptr; void *delivery_cookie_=nullptr;
    struct LocalUse {Core *core=nullptr;const void *target=nullptr;uint64_t serial=0;Use purpose=Use::identity_read;};
    inline static thread_local LocalUse local_{};
    bool Healthy() const noexcept {const auto p=phase_.load(std::memory_order_acquire);return p==HealthPhase::open||p==HealthPhase::dispatching;}
    bool Lock() noexcept {if(write_.test_and_set(std::memory_order_acquire)){Close(Error::overlap);return false;}return true;}
    void Unlock() noexcept {write_.clear(std::memory_order_release);}
    Device *FindDevice(const void *p) noexcept {
        for(auto &d:devices_)if(d.api.load()==p||d.proxy.load()==p||d.dxgi.load()==p)if(p)return &d;return nullptr;
    }
    Context *FindContext(const void *p) noexcept {
        for(auto &c:contexts_)if(c.id.load(std::memory_order_acquire)&&p&&(c.api.load()==p||c.queue.load()==p||c.proxy.load()==p))return &c;return nullptr;
    }
    bool SameObject(const void *a,const void *b) noexcept {
        if(a==b)return true;auto *c=FindContext(a);if(c)return c==FindContext(b);auto*d=FindDevice(a);return d&&d==FindDevice(b);
    }
    bool Authorized(uintptr_t owner,uint64_t token) const noexcept {return registered_.load(std::memory_order_acquire)&&owner==owner_&&token==token_;}
    bool Deliver(Event &e) noexcept {
#if defined(TQPM_CPU_TEST_SEAMS)
        Pause(TestPoint::callback_preadmission);
#endif
        auto available=HealthPhase::open;
        if(!phase_.compare_exchange_strong(available,HealthPhase::dispatching))return false;
        const auto prior_use=local_;
        uint64_t sequence=sequence_.load();
        if(!NextSequence(sequence)){Close(Error::sequence_exhausted);return false;}
        sequence_=sequence;e.header.sequence=sequence;
        if(!e.header.operation_sequence)e.header.operation_sequence=sequence;
        if(!delivery_(delivery_cookie_,e))Close(Error::observer_fault,
            e.header.operation==Operation::boundary?(e.boundary.destination?e.boundary.destination:e.boundary.resource_array):e.header.api_context,
            e.header.context_id,owner_,e.header.sequence,e.header.api_device,
            e.header.operation==Operation::boundary?static_cast<uint32_t>(e.boundary.kind):static_cast<uint32_t>(e.header.operation),e.header.operation==Operation::boundary?static_cast<uint32_t>(e.boundary.origin):0);
        if(prior_use.core!=local_.core||prior_use.serial!=local_.serial)Close(Error::invalid_lease,e.header.api_context,e.header.context_id,owner_,e.header.sequence);
        auto admitted=HealthPhase::dispatching;
        phase_.compare_exchange_strong(admitted,HealthPhase::open); // Closed is terminal.
        return Healthy();
    }
public:
    struct Ticket {bool owns_inflight=false;Event event{};};
    void Close(Error error,const void *object=nullptr,uint64_t context=0,uintptr_t caller=0,uint64_t event_sequence=0,const void *device=nullptr,uint32_t operation=0,uint32_t origin=0) noexcept {
        // The first transition owns its witness; a later Close cannot steal publication.
        if(phase_.exchange(HealthPhase::closed,std::memory_order_acq_rel)!=HealthPhase::closed){
            error_.store(0xffffffffu,std::memory_order_release);
            first_sequence_=event_sequence;first_last_sequence_=sequence_.load();first_context_=context;first_caller_=caller;first_object_=object;
            first_device_=device;first_operation_=operation;first_origin_=origin;
            first_known_=(event_sequence?1u:0u)|(context?2u:0u)|(caller?4u:0u)|(object?8u:0u);
            error_.store(static_cast<uint32_t>(error),std::memory_order_release);
        }
    }
    uint64_t Register(uintptr_t owner,const uint8_t nonce[16],Delivery delivery,void *cookie) noexcept {
        if(!owner||!delivery||!Lock())return 0;
        if(registered_.load()){Unlock();return 0;}
        if(device_count_.load()||context_count_.load()){Close(Error::late_registration);Unlock();return 0;}
        auto pristine=HealthPhase::pristine;
        if(!phase_.compare_exchange_strong(pristine,HealthPhase::registering)){Unlock();return 0;}
        owner_=owner;token_=1;delivery_=delivery;delivery_cookie_=cookie;
        for(uint32_t i=0;i<16;++i)nonce_[i]=nonce[i];
#if defined(TQPM_CPU_TEST_SEAMS)
        Pause(TestPoint::register_precommit);
#endif
        auto registering=HealthPhase::registering;
        if(!phase_.compare_exchange_strong(registering,HealthPhase::open,std::memory_order_acq_rel,std::memory_order_acquire)){Unlock();return 0;}
        registered_.store(1,std::memory_order_release);Unlock();return Healthy()?token_:0;
    }
    bool DeviceCreated(const void *api,const void *proxy,const void *dxgi=nullptr,void *native=nullptr) noexcept {
        if(!Lock())return false;const uint64_t n=device_count_.fetch_add(1);
        if(n>=16||!api||!proxy){Close(Error::capacity);Unlock();return false;}
        devices_[n].api=api;devices_[n].proxy=proxy;devices_[n].dxgi=dxgi;devices_[n].native=native;devices_[n].live=true;Unlock();return true;
    }
    bool ContextCreated(const void *api,const void *queue,const void *proxy,const void *device,void *native=nullptr) noexcept {
        if(!Lock())return false;const uint64_t n=context_count_.fetch_add(1);
        if(n>=64||!api||!queue||!proxy||!FindDevice(device)){Close(Error::capacity);Unlock();return false;}
        auto &c=contexts_[n];c.api=api;c.queue=queue;c.proxy=proxy;c.device=device;c.native=native;c.live=true;c.id.store(n+1,std::memory_order_release);Unlock();return true;
    }
    void ContextKind(const void *p,bool immediate) noexcept {
        if(!Lock())return;
        if(auto*c=FindContext(p)){c->kind=immediate?1u:2u;if(!immediate)if(auto*d=FindDevice(c->device.load())){
            d->unsupported=true;if(selected_device_.load()==d->api.load())Close(Error::unsupported,p,c->id.load());}}
        else Close(Error::wrong_context);
        Unlock();
    }
    void Retire(const void *p) noexcept {
        if(!Lock())return; // Contention closes observation; no wait or native-call suppression.
#if defined(TQPM_CPU_TEST_SEAMS)
        Pause(TestPoint::retire_premark);
#endif
        if(auto*c=FindContext(p)){c->live=false;if(bound_.load()==c->id.load())Close(Error::retired,p,c->id.load());}
        if(auto*d=FindDevice(p)){d->live=false;if(selected_device_.load()==d->api.load())Close(Error::retired,p);}
        Unlock();
    }
    bool Bind(uintptr_t owner,uint64_t token,const void *api) noexcept {
        if(!Authorized(owner,token)||!Healthy()||!Lock())return false;
        auto*c=FindContext(api);auto*d=c?FindDevice(c->device.load()):nullptr;
        if(bound_.load()||!c||c->api.load()!=api||!c->live.load()||c->kind.load()!=1||!d||!d->live.load()||d->unsupported.load()||d->pending.load()||inflight_.load()){
            Close(Error::wrong_context);Unlock();return false;}
#if defined(TQPM_CPU_TEST_SEAMS)
        Pause(TestPoint::bind_prepublish);
#endif
        if(!Healthy()){Unlock();return false;}
        selected_context_=api;selected_device_=d->api.load();bound_=c->id.load();Unlock();return Healthy();
    }
    Status Snapshot() const noexcept {
        Status s{};s.size=sizeof(s);s.abi_version=version;s.registered=registered_.load(std::memory_order_acquire);
        s.bound=bound_.load()?1u:0u;s.healthy=Healthy();s.error=error_.load();s.native_bypass_gap=1;s.in_flight=inflight_.load();
        for(const auto &d:devices_)if(d.pending.load())++s.pending_creations;
        s.capabilities=capabilities;s.sequence=sequence_.load();s.context_id=bound_.load();s.api_context=selected_context_.load();s.api_device=selected_device_.load();
        if(s.registered){s.owner_token=token_;s.observer_module=reinterpret_cast<const void*>(owner_);for(uint32_t i=0;i<16;++i)s.nonce[i]=nonce_[i];}
        if(s.error&&s.error!=0xffffffffu){s.first_close_sequence=first_sequence_.load();s.first_close_context_id=first_context_.load();
            s.first_close_caller=first_caller_.load();s.first_close_object=first_object_.load();s.first_close_last_allocated_sequence=first_last_sequence_.load();s.first_close_known_flags=first_known_.load();s.first_close_api_device=first_device_.load();s.first_close_operation=first_operation_.load();s.first_close_origin=first_origin_.load();}return s;
    }
    uint64_t BeginUse(uintptr_t owner,uint64_t token,const void *target,Use use) noexcept {
        if(!Authorized(owner,token)||!Healthy()||local_.core){Close(Error::invalid_lease);return 0;}
        auto*c=FindContext(target);auto*d=FindDevice(target);
        bool valid=(c&&c->live.load())||(d&&d->live.load());
        if(use==Use::owned_shadow_creation)valid=valid&&bound_.load()&&d&&d->api.load()==selected_device_.load();
        else if(use==Use::held_draw_bindings)valid=valid&&bound_.load()&&c&&c->id.load()==bound_.load();
        else if(use!=Use::identity_read)valid=false;
        if(!valid){Close(Error::invalid_lease);return 0;}
        const uint64_t serial=lease_sequence_.fetch_add(1)+1;
        if(!serial){Close(Error::sequence_exhausted);return 0;}
        local_={this,target,serial,use};return serial;
    }
    bool EndUse(uintptr_t owner,uint64_t token,uint64_t serial) noexcept {
        if(!Authorized(owner,token)||local_.core!=this||!serial||local_.serial!=serial){Close(Error::invalid_lease);return false;}
        local_={};return true;
    }
    void Escape(const void *object,uintptr_t caller_owner) noexcept {
        if(!FindContext(object)&&!FindDevice(object))return; // Other backends are outside this D3D11 registry.
        if(!registered_.load()){Close(Error::escape);return;}
        if(caller_owner!=owner_||local_.core!=this||!SameObject(local_.target,object)){
            auto*c=FindContext(object);Close(Error::escape,object,c?c->id.load():0,caller_owner);}
    }
    Ticket Before(const void *proxy,Operation op,ID3D11Resource *resource=nullptr,ID3D11View *view=nullptr,
                  const D3D11_RECT *rects=nullptr,uint32_t count=0,ID3DDeviceContextState *state=nullptr,ID3DDeviceContextState *const *previous=nullptr) noexcept {
        Ticket t{};if(!registered_.load()||!Healthy()||!bound_.load())return t;
        if(!Lock())return t;
        if(!bound_.load()){Unlock();return t;}
        uint32_t zero=0;if(!inflight_.compare_exchange_strong(zero,1)){auto*c=FindContext(proxy);Close(Error::overlap,proxy,c?c->id.load():0);Unlock();return t;}t.owns_inflight=true;
        auto*c=FindContext(proxy);
        if(!c||!c->live.load()||c->kind.load()!=1||c->id.load()!=bound_.load()){Close(Error::wrong_context,proxy,c?c->id.load():0);Unlock();return t;}
        if(auto*d=FindDevice(c->device.load()))if(d->pending.load()){
            const void *object=resource?static_cast<const void*>(resource):(view?static_cast<const void*>(view):static_cast<const void*>(state));
            Close(Error::pending_write,object,c->id.load(),0,0,d->api.load(),static_cast<uint32_t>(op));Unlock();return t;}
        t.event.header={sizeof(Header),version,op,Phase::before_native,0,0,token_,c->id.load(),c->api.load(),c->device.load()};
        t.event.resource=resource;t.event.view=view;t.event.rects=rects;t.event.rect_count=count;t.event.state=state;t.event.previous=previous;
        Unlock();Deliver(t.event);return t;
    }
    void UnsupportedNative(const void *native) noexcept {
        if(!Registered()||!Healthy())return;
        for(auto &c:contexts_)if(c.id.load(std::memory_order_acquire)&&c.native.load()==native&&native){Unsupported(c.api.load());return;}
        Close(Error::unsupported,native);
    }
    bool Registered() const noexcept {return registered_.load(std::memory_order_acquire)!=0;}
    bool CreationRegistrationOpen()const noexcept{return !device_count_.load()&&!context_count_.load()&&!bound_.load();}
    bool NativeRead(uintptr_t owner,uint64_t token,uint64_t serial,const void *target,NativeOwner &out) noexcept {
        if(!Authorized(owner,token)||!Healthy()||local_.core!=this||local_.serial!=serial||
           local_.purpose!=Use::identity_read||!SameObject(local_.target,target))return false;
        if(!FindContext(target)){
            auto*d=FindDevice(target);if(!d||!d->live.load()||!d->native.load())return false;
            out={sizeof(NativeOwner),version,0,1,d->api.load(),nullptr,d->native.load(),nullptr};return true;
        }
        auto*c=FindContext(target);auto*d=c?FindDevice(c->device.load()):nullptr;
        if(!c||!d||!c->live.load()||!d->live.load()||c->kind.load()!=1||!c->native.load()||!d->native.load())return false;
        out={sizeof(NativeOwner),version,c->kind.load(),1,d->api.load(),c->api.load(),d->native.load(),c->native.load()};return true;
    }
    uint64_t BeginCreation(const void *object) noexcept {
        if(!Registered()||!Healthy()||!bound_.load())return 0;
        if(!Lock())return 0;
        if(!bound_.load()){Unlock();return 0;}
        if(inflight_.load()){Close(Error::creation_overlap,object);Unlock();return 0;}
        auto*d=FindDevice(object);if(!d||!d->live.load()){Close(Error::wrong_context,object);Unlock();return 0;}
        const uint64_t ticket=creation_sequence_.fetch_add(1)+1;
        if(!ticket){Close(Error::sequence_exhausted,object);Unlock();return 0;}
#if defined(TQPM_CPU_TEST_SEAMS)
        Pause(TestPoint::creation_precommit);
#endif
        if(!Healthy()){Unlock();return 0;}
        uint64_t zero=0;if(!d->pending.compare_exchange_strong(zero,ticket)){Close(Error::creation_overlap,object);Unlock();return 0;}
        Unlock();return ticket;
    }
    void EndCreation(const void *object,uint64_t ticket) noexcept {
        if(!ticket||!Lock())return;auto*d=FindDevice(object);if(!d||!d->pending.compare_exchange_strong(ticket,0))Close(Error::creation_overlap,object);Unlock();
    }
    void Unsupported(const void *object) noexcept {
        if(!Registered()||!Healthy())return;
        auto*c=FindContext(object);auto*d=c?FindDevice(c->device.load()):FindDevice(object);
        if(!d){Close(Error::unsupported,object);return;}
        d->unsupported=true;
        if(!bound_.load()||selected_device_.load()==d->api.load())Close(Error::unsupported,object,c?c->id.load():0,0,0,d->api.load());
    }
    Ticket BeforeBoundary(const void *object,const Boundary &b) noexcept {
        Ticket t{};if(!Registered()||!Healthy()||!bound_.load())return t;
        if(!Lock())return t;
        if(!bound_.load()){Unlock();return t;}
        auto*c=FindContext(object);auto*d=c?FindDevice(c->device.load()):FindDevice(object);
        if(!d||!d->live.load()||(c&&(!c->live.load()||c->kind.load()!=1))){Close(Error::wrong_context,object,c?c->id.load():0);Unlock();return t;}
        // A proved foreign registry owner cannot mutate selected-device resources through these valid API leaves.
        // Wrong-device/invalid native API calls are not an admission proof and remain native-gap qualified.
        if(bound_.load()&&selected_device_.load()!=d->api.load()){Unlock();return t;}
        if(d->pending.load()){Close(Error::pending_write,b.destination?b.destination:b.resource_array,c?c->id.load():0,0,0,d->api.load(),static_cast<uint32_t>(b.kind),static_cast<uint32_t>(b.origin));Unlock();return t;}
        if((b.kind==BoundaryKind::offer||b.kind==BoundaryKind::reclaim)&&(!b.resource_array||!b.count||b.count>64)){
            Close(Error::invalid_array,b.resource_array,c?c->id.load():0,0,0,d->api.load(),static_cast<uint32_t>(b.kind),static_cast<uint32_t>(b.origin));Unlock();return t;}
#if defined(TQPM_CPU_TEST_SEAMS)
        Pause(TestPoint::boundary_preadmission);
#endif
        if(!Healthy()){Unlock();return t;}
        uint32_t zero=0;if(!inflight_.compare_exchange_strong(zero,1)){Close(Error::overlap,object,c?c->id.load():0);Unlock();return t;}t.owns_inflight=true;
        t.event.header={sizeof(Header),version,Operation::boundary,Phase::before_native,0,0,token_,c?c->id.load():0,c?c->api.load():nullptr,d->api.load()};
        t.event.boundary=b;Unlock();Deliver(t.event);return t;
    }
    template<class F>decltype(auto) BoundaryCall(const void *object,const Boundary &b,F &&native) {
        auto t=BeforeBoundary(object,b);
        if constexpr(std::is_void_v<std::invoke_result_t<F>>){native();After(t);}
        else {auto result=native();After(t);return result;}
    }
    void After(Ticket &t) noexcept {
        if(!t.owns_inflight)return;
        if(t.event.header.operation==Operation::swap_state&&t.event.header.operation_sequence&&Healthy()){
            t.event.header.phase=Phase::native_returned;Deliver(t.event);}
        inflight_=0;t.owns_inflight=false;
    }
    template<class F>void Resource(const void*p,ID3D11Resource*r,F &&native){auto t=Before(p,Operation::discard_resource,r);native();After(t);}
    template<class F>void View(const void*p,ID3D11View*v,F &&native){auto t=Before(p,Operation::discard_view,nullptr,v);native();After(t);}
    template<class F>void View1(const void*p,ID3D11View*v,const D3D11_RECT*r,uint32_t n,F &&native){auto t=Before(p,Operation::discard_view1,nullptr,v,r,n);native();After(t);}
    template<class F>void Swap(const void*p,ID3DDeviceContextState*s,ID3DDeviceContextState**previous,F &&native){auto t=Before(p,Operation::swap_state,nullptr,nullptr,nullptr,0,s,previous);native();After(t);}
};
} // namespace tqpm
