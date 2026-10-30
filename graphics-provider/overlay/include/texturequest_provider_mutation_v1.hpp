// Private TextureQuest provider extension. No upstream API20 enum/ABI modification.
#pragma once
#include <cstdint>
struct ID3D11Resource;
struct ID3D11View;
struct ID3DDeviceContextState;
struct tagRECT;
using D3D11_RECT = tagRECT; // Matches D3D11's typedef RECT, without importing native APIs here.
#if defined(_WIN32)
#define TQPM_CALL __stdcall
#else
#define TQPM_CALL
#endif
namespace tqpm {
constexpr uint32_t version = 1;
enum Capability : uint64_t { typed_before=1, swap_completion=2, monotonic_close=4,
    bounded_serial_dispatch=8, observed_escape_close=16, scoped_host_uses=32, retained_observer=64 };
constexpr uint64_t capabilities = 127; // Deliberately NO complete-native-write-coverage capability.
enum class Operation : uint32_t { discard_resource=1, discard_view=2, discard_view1=3, swap_state=4 };
enum class Phase : uint32_t { before_native=1, native_returned=2 };
enum class Use : uint32_t { identity_read=1, owned_shadow_creation=2, held_draw_bindings=3 };
enum class Error : uint32_t { none=0, late_registration=1, capacity=2, overlap=3, observer_fault=4,
    wrong_context=5, retired=6, escape=7, invalid_lease=8, sequence_exhausted=9, consumer_closed=10 };
struct Header {
    uint32_t size, abi_version;
    Operation operation; Phase phase;
    uint64_t sequence, operation_sequence, owner_token, context_id;
    const void *api_context, *api_device;
};
using ResourceCallback = uint32_t(TQPM_CALL *)(void *, const Header *, ID3D11Resource *);
using ViewCallback = uint32_t(TQPM_CALL *)(void *, const Header *, ID3D11View *);
using View1Callback = uint32_t(TQPM_CALL *)(void *, const Header *, ID3D11View *, const D3D11_RECT *, uint32_t);
using SwapCallback = uint32_t(TQPM_CALL *)(void *, const Header *, ID3DDeviceContextState *, ID3DDeviceContextState *const *);
struct Callbacks {
    uint32_t size, abi_version;
    ResourceCallback resource; ViewCallback view; View1Callback view1; SwapCallback swap;
}; // Return exactly 1 to acknowledge; any other return or recoverable SEH closes observation.
struct Registration {
    uint32_t size, abi_version;
    uint64_t required_capabilities;
    void *observer_module, *cookie;
    uint8_t nonce[16];
    Callbacks callbacks;
};
struct Status {
    uint32_t size, abi_version, registered, bound;
    uint32_t healthy, error, native_bypass_gap, in_flight;
    uint64_t capabilities, owner_token, sequence, context_id;
    const void *api_context, *api_device;
    uint8_t nonce[16];
    uint64_t first_close_sequence, first_close_context_id, first_close_caller;
    const void *first_close_object;
    const void *observer_module;
    uint64_t first_close_last_allocated_sequence, first_close_known_flags; // 1 event seq, 2 context, 4 caller, 8 object.
};
struct Api {
    uint32_t size, abi_version; uint64_t capabilities;
    uint64_t(TQPM_CALL *register_observer)(const Registration *);
    uint32_t(TQPM_CALL *bind_context)(uint64_t, const void *);
    uint32_t(TQPM_CALL *read_status)(Status *);
    uint64_t(TQPM_CALL *begin_use)(uint64_t, const void *, Use);
    uint32_t(TQPM_CALL *end_use)(uint64_t, uint64_t);
    void(TQPM_CALL *close)(uint64_t);
};
static_assert(sizeof(void*)==8, "private provider ABI is x64 only");
static_assert(sizeof(Header)==64 && sizeof(Callbacks)==40 && sizeof(Registration)==88);
static_assert(sizeof(Status)==152 && sizeof(Api)==64);
} // namespace tqpm
