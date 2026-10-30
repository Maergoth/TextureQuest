#pragma once
#include "texturequest_provider_mutation_v2.hpp"
namespace tqpc {
constexpr uint32_t version=1;
enum Capability:uint64_t { per_call=1,native_before_init=2,mandatory_settlement=4,scoped_identity=8 };
constexpr uint64_t capabilities=15;
enum class Factory:uint32_t { buffer=1,texture1d=2,texture2d=3,texture3d=4,texture2d1=5,texture3d1=6,sdk_resource=7 };
enum class Phase:uint32_t { capture=1,native_result=2,settle=3 };
enum class Outcome:uint32_t { failed=0,succeeded=1,unknown=2 };
enum class HostEvent:uint32_t { queued=1,admitted=2,dirty_cancelled=3,capacity_fault=4 };
struct Descriptor {
 uint32_t width,height,mips,layers,format,samples,quality,usage,bind,cpu,misc,texture_layout;
};
struct Subresource {const void* data;uint32_t row_pitch,slice_pitch;};
struct Info {
 uint32_t size,abi_version;uint64_t call,parent,thread,owner_token,device_generation;
 const void* api_device;Factory factory;uint32_t expected_init;
 Descriptor descriptor;const Subresource* initial;uint32_t initial_count,reserved;
};
struct NativeResult {uint32_t size,abi_version;const Info* info;int32_t hresult;uint32_t known;void* resource;};
struct Settlement {uint32_t size,abi_version;const Info* info;Outcome outcome;uint32_t init_seen;};
using Capture=uint32_t(TQPM_CALL*)(void*,const Info*,void**); // 0 exact nonmatch/ineligible,1 match; other closes.
using Native=uint32_t(TQPM_CALL*)(void*,void*,const NativeResult*); // Exactly1 acknowledges.
using Settle=uint32_t(TQPM_CALL*)(void*,void*,const Settlement*); // Exactly1 acknowledges.
struct Callbacks {uint32_t size,abi_version;Capture capture;Native native_result;Settle settle;};
struct Registration {uint32_t size,abi_version;uint64_t required_capabilities;void* observer_module;void* cookie;uint8_t nonce[16];Callbacks callbacks;};
struct Diagnostics {
 uint32_t size,abi_version;uint64_t owner_token;const void* observer_module;uint8_t nonce[16];
 uint64_t active,started,settled,failed,cancelled,eligible,nonmatching,matching,queued,admitted,dirty_cancelled,capacity_faults,faults;
 uint64_t first_call,first_parent,first_thread;uint32_t first_factory,first_phase,first_reason,first_known;
};
struct Api {
 uint32_t size,abi_version;uint64_t capabilities;
 uint64_t(TQPM_CALL* register_observer)(const Registration*);
 uint64_t(TQPM_CALL* begin_identity)(uint64_t,uint64_t);
 uint32_t(TQPM_CALL* read_diagnostics)(uint64_t,Diagnostics*);
 uint32_t(TQPM_CALL* host_event)(uint64_t,HostEvent);
 void(TQPM_CALL* close)(uint64_t,uint32_t);
};
static_assert(sizeof(Descriptor)==48&&sizeof(Subresource)==16&&sizeof(Info)==128);
static_assert(sizeof(NativeResult)==32&&sizeof(Settlement)==24&&sizeof(Callbacks)==32&&sizeof(Registration)==80);
static_assert(sizeof(Diagnostics)==184&&sizeof(Api)==56);
} // namespace tqpc
