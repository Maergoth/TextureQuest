// Windows boundary. Source only in this stage: no provider build/load performed.
#include <Windows.h>
#include <intrin.h>
#include <cstring>
#include <d3d11_4.h>
#include "texturequest_provider_protection.hpp"
#include "texturequest_provider_mutation.hpp"
#if RESHADE_ADDON != 2
#error The private extension requires the reviewed full-addon provider configuration.
#endif
namespace {
tqpm::Core core;
tqpm::Registration observer{};
std::atomic_flag attempted=ATOMIC_FLAG_INIT;
tqpm::ProtectionPolicy protection;
std::atomic_flag protection_attempted=ATOMIC_FLAG_INIT;
// No hot-unload/cleanup path: the selected native graph remains held to natural process exit.
ID3D11Multithread *held_multithread=nullptr;
IUnknown *held_device_identity=nullptr,*held_context_identity=nullptr,*held_mt_identity=nullptr;
HMODULE Module(const void *address) noexcept {
    HMODULE value=nullptr;
    if(!address||!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(address),&value))return nullptr;
    return value;
}
bool Read(void *destination,const void *source,size_t bytes) noexcept {
    __try{std::memcpy(destination,source,bytes);return true;}__except(EXCEPTION_EXECUTE_HANDLER){return false;}
}
bool Invoke(void *,const tqpm::Event &event) noexcept {
    // Leaf SEH fence: no C++ automatic objects needing unwind. Fatal fail-fast,
    // stack exhaustion and malicious blocking callbacks are not recoverability claims.
    __try{return tqpm::Dispatch(observer.callbacks,observer.cookie,event)==1;}
    __except(EXCEPTION_EXECUTE_HANDLER){return false;}
}
uint64_t TQPM_CALL Register(const tqpm::Registration *input) noexcept {
    tqpm::Registration request{};
    if(!Read(&request,input,sizeof(request))||!tqpm::ValidRegistrationShape(request))return 0;
    const auto owner=static_cast<HMODULE>(request.observer_module);
    if(Module(_ReturnAddress())!=owner||Module(reinterpret_cast<const void*>(request.callbacks.resource))!=owner||
       Module(reinterpret_cast<const void*>(request.callbacks.view))!=owner||Module(reinterpret_cast<const void*>(request.callbacks.view1))!=owner||
       Module(reinterpret_cast<const void*>(request.callbacks.swap))!=owner||Module(reinterpret_cast<const void*>(request.callbacks.boundary))!=owner)return 0;
    if(attempted.test_and_set(std::memory_order_acq_rel))return 0; // Never replace the published callback table.
    HMODULE retained=nullptr;
    if(!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_PIN,
        reinterpret_cast<LPCWSTR>(owner),&retained)||retained!=owner)return 0;
    observer=request; // Module/process lifetime retention; no unregister/hot-unload path.
    return core.Register(reinterpret_cast<uintptr_t>(owner),request.nonce,&Invoke,nullptr);
}
uint32_t TQPM_CALL Bind(uint64_t token,const void *context) noexcept {
    if(!core.Snapshot().registered||token!=core.Snapshot().owner_token||Module(_ReturnAddress())!=observer.observer_module)return 0;
    if(!protection.Verified(context)){core.Close(tqpm::Error::protection,context);return 0;}
    return core.Bind(reinterpret_cast<uintptr_t>(Module(_ReturnAddress())),token,context)?1u:0u;
}
uint32_t TQPM_CALL Status(tqpm::Status *output) noexcept {
    uint32_t size=0;if(!Read(&size,output,sizeof(size))||size!=sizeof(tqpm::Status))return 0;
    auto value=core.Snapshot();value.protection_holding=protection.Holding()?1u:0u;
    value.protection_verified=protection.Verified(protection.Context())?1u:0u;
    value.protection_device=protection.Device();value.protection_context=protection.Context();
    return Read(output,&value,sizeof(value))?1u:0u;
}
uint64_t TQPM_CALL Begin(uint64_t token,const void *target,tqpm::Use use) noexcept {
    return core.BeginUse(reinterpret_cast<uintptr_t>(Module(_ReturnAddress())),token,target,use);
}
uint32_t TQPM_CALL End(uint64_t token,uint64_t serial) noexcept {
    return core.EndUse(reinterpret_cast<uintptr_t>(Module(_ReturnAddress())),token,serial)?1u:0u;
}
void TQPM_CALL Close(uint64_t token) noexcept {
    const auto s=core.Snapshot();if(s.registered&&s.owner_token==token&&Module(_ReturnAddress())==observer.observer_module)core.Close(tqpm::Error::consumer_closed);
}
uint32_t TQPM_CALL Owner(uint64_t token,uint64_t serial,const void *context,tqpm::NativeOwner *output) noexcept {
    uint32_t size=0;if(!Read(&size,output,sizeof(size))||size!=sizeof(tqpm::NativeOwner))return 0;
    tqpm::NativeOwner value{};
    if(!core.NativeRead(reinterpret_cast<uintptr_t>(Module(_ReturnAddress())),token,serial,context,value))return 0;
    return Read(output,&value,sizeof(value))?1u:0u;
}
bool AcquireNative(const tqpm::NativeOwner &pair) noexcept {
    // Raw local COM pointers only. Reference ownership is explicit on every known failure.
    auto *device=static_cast<ID3D11Device *>(pair.native_device);
    auto *context=static_cast<ID3D11DeviceContext *>(pair.native_context);
    ID3D11Device *context_device=nullptr;IUnknown *di=nullptr,*ci=nullptr,*cdi=nullptr,*mi=nullptr;ID3D11Multithread *mt=nullptr;
    if(context->GetType()!=D3D11_DEVICE_CONTEXT_IMMEDIATE||(device->GetCreationFlags()&D3D11_CREATE_DEVICE_SINGLETHREADED))return false;
    context->GetDevice(&context_device);
    const bool valid=context_device&&SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&di)))&&
        SUCCEEDED(context_device->QueryInterface(IID_PPV_ARGS(&cdi)))&&di==cdi&&
        SUCCEEDED(context->QueryInterface(IID_PPV_ARGS(&ci)))&&SUCCEEDED(context->QueryInterface(IID_PPV_ARGS(&mt)))&&
        SUCCEEDED(mt->QueryInterface(IID_PPV_ARGS(&mi)));
    if(context_device)context_device->Release();if(cdi)cdi->Release();
    if(!valid){if(di)di->Release();if(ci)ci->Release();if(mt)mt->Release();if(mi)mi->Release();return false;}
    // This is a real exact-owner native group, not a boolean admission token.
    mt->Enter();
    const bool began=protection.Begin(pair.api_device,pair.api_context,mi);
    if(!began){mt->Leave();di->Release();ci->Release();mt->Release();mi->Release();return false;}
    held_device_identity=di;held_context_identity=ci;held_mt_identity=mi;held_multithread=mt;
    mt->SetMultithreadProtected(TRUE);
    const bool verified=protection.Verify(mt->GetMultithreadProtected()!=FALSE);
    mt->Leave();return verified; // Poison/UNKNOWN retains the graph and never restores FALSE.
}
bool AcquireFence(const tqpm::NativeOwner &pair) noexcept {
    __try{return AcquireNative(pair);}__except(EXCEPTION_EXECUTE_HANDLER){protection.Poison();return false;}
}
uint32_t TQPM_CALL Acquire(uint64_t token,uint64_t serial,const void *context) noexcept {
    tqpm::NativeOwner pair{};
    if(!core.NativeRead(reinterpret_cast<uintptr_t>(Module(_ReturnAddress())),token,serial,context,pair))return 0;
    if(pair.size!=sizeof(pair)||pair.abi_version!=tqpm::version||pair.context_kind!=1||pair.retained!=1||
       !pair.api_device||!pair.api_context||!pair.native_device||!pair.native_context)return 0;
    if(protection_attempted.test_and_set(std::memory_order_acq_rel)){core.Close(tqpm::Error::protection,context);return 0;}
    if(!AcquireFence(pair)){core.Close(tqpm::Error::protection,context);return 0;}
    return 1;
}
const tqpm::Api api{sizeof(tqpm::Api),tqpm::version,tqpm::capabilities,&Register,&Bind,&Status,&Begin,&End,&Close,&Owner,&Acquire};
} // namespace
namespace tqpm {
Core &ProviderCore() noexcept{return core;}
void ObserveNativeEscape(const void *object,const void *caller) noexcept {
    core.Escape(object,reinterpret_cast<uintptr_t>(Module(caller)));
}
void RecordDevice(const void *api,const void *proxy,const void *dxgi,ID3D11Device *native) noexcept {
    const bool retain=core.Registered();
    if(core.DeviceCreated(api,proxy,dxgi,retain?native:nullptr)&&retain)native->AddRef();
}
void RecordContext(const void *api,const void *queue,const void *proxy,const void *device,ID3D11DeviceContext *native) noexcept {
    const bool retain=core.Registered();
    if(core.ContextCreated(api,queue,proxy,device,retain?native:nullptr)&&retain)native->AddRef();
}
void UnsupportedNativeState(ID3D11DeviceContext *native) noexcept {core.UnsupportedNative(native);}
bool ProtectionRestore(const void *api_device,ID3D11Multithread *native,bool prior) noexcept {
    if(!api_device||!protection.Holding()||protection.Device()!=api_device)return prior;
    IUnknown *identity=nullptr;
    const bool known=native&&SUCCEEDED(native->QueryInterface(IID_PPV_ARGS(&identity)));
    bool mismatch=false;const bool target=protection.Restore(api_device,known?identity:nullptr,prior,mismatch);
    if(identity)identity->Release();if(mismatch)core.Close(Error::protection,api_device);return target;
}
void ProtectionRestored(const void *api_device,bool protected_now) noexcept {
    if(protection.Holding()&&protection.Device()==api_device&&!protected_now){protection.Poison();core.Close(Error::protection,api_device);}
}
} // namespace tqpm
extern "C" __declspec(dllexport) const tqpm::Api *TQPM_CALL TQPM_GetInterfaceV2(uint32_t abi,uint32_t bytes) noexcept {
    return abi==tqpm::version&&bytes==sizeof(tqpm::Api)?&api:nullptr;
}

#include "texturequest_provider_creation_impl.hpp"
