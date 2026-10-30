#pragma once
// Stable interface-only x64 wire ABI. No implementation/runtime dependencies.
#include <cstdint>
#include <cstddef>
namespace tqgr {
struct InitV3 {
    char magic[8];uint32_t bytes,version,pid,flags;uint64_t birth100ns,provider,catalog_address;
    uint32_t catalog_bytes,reserved;uint64_t main_hwnd;char nonce[33];uint8_t pad[7];char catalog_sha256[65];uint8_t pad2[7];uint64_t bootstrap,profile_receipt;
};
struct CommandV3 {char magic[8];uint32_t bytes,version,pid,mode;uint64_t birth100ns,sequence,main_hwnd;char nonce[33];uint8_t pad[7];};
struct StatusV3 {
    char magic[8];uint32_t bytes,version,pid,error;uint64_t birth100ns;char nonce[33];uint8_t pad[7];
    uint32_t attempted,registered,input_ready,main_ready,comparison_ready,default_safe,user_original,stopping;
    uint64_t requested,acknowledged,frames,pairs,replaced_draws,canonical_stock_rows,canonical_upscaled_rows;
    uint32_t protection_enabled,protection_previous,group_guard_ready,reserved;uint32_t private_experimental,native_bypass_gap,provider_error,profile_ready;uint64_t first_close_sequence,first_close_context,first_close_caller,first_close_object,first_close_last_allocated,first_close_known_flags;uint32_t first_close_operation,first_close_origin;
};
static_assert(sizeof(InitV3)==192&&offsetof(InitV3,nonce)==64&&offsetof(InitV3,catalog_sha256)==104);
static_assert(sizeof(CommandV3)==88&&offsetof(CommandV3,nonce)==48&&offsetof(CommandV3,main_hwnd)==40);
static_assert(sizeof(StatusV3)==248&&offsetof(StatusV3,requested)==104&&offsetof(StatusV3,protection_enabled)==160);
}
