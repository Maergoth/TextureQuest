#pragma once
#include <atomic>
#include <cstdint>
namespace tq_world {
// Setter is graphics-free. Only a post-cleanup native Present commits it.
class Master {
    std::atomic<bool> requested_{false},effective_{false};
    std::atomic<std::uint64_t> device_{0};
public:
    void Request(std::uint32_t value)noexcept{requested_.store(value!=0,std::memory_order_release);}
    bool Requested()const noexcept{return requested_.load(std::memory_order_acquire);}
    bool Enabled()const noexcept{return effective_.load(std::memory_order_acquire);}
    void Commit(bool snapshot)noexcept{effective_.store(snapshot,std::memory_order_release);}
    bool Device(std::uint64_t native)noexcept{
        if(!native)return false;std::uint64_t empty=0;
        device_.compare_exchange_strong(empty,native,std::memory_order_acq_rel);
        return device_.load(std::memory_order_acquire)==native;
    }
    void Release(std::uint64_t native)noexcept{
        if(!native)return;
        if(device_.compare_exchange_strong(native,0,std::memory_order_acq_rel))effective_.store(false,std::memory_order_release);
    }
};
inline Master Managed;
}
