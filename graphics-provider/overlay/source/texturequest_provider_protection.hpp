// Portable durable-lease policy. Native caller serializes Begin/Verify/Restore
// using the exact ID3D11Multithread group, including SetProtected before Leave.
#pragma once
#include <atomic>
#include <cstdint>
namespace tqpm {
class ProtectionPolicy {
    std::atomic<uint32_t> phase_{0}; // 0 absent, 1 holding/unverified, 2 verified, 3 poison
    std::atomic<const void*> device_{nullptr},context_{nullptr},identity_{nullptr};
public:
    bool Begin(const void *device,const void *context,const void *identity) noexcept {
        // The external one-attempt flag and native group serialize this publication.
        if(!device||!context||!identity||phase_.load())return false;
        device_=device;context_=context;identity_=identity;
        phase_.store(1,std::memory_order_release);return true;
    }
    bool Verify(bool protected_now) noexcept {
        uint32_t holding=1;
        if(!protected_now){Poison();return false;}
        return phase_.compare_exchange_strong(holding,2);
    }
    void Poison() noexcept {if(phase_.load())phase_.store(3,std::memory_order_release);}
    bool Holding() const noexcept {return phase_.load(std::memory_order_acquire)!=0;}
    bool Verified(const void *context) const noexcept {return phase_.load(std::memory_order_acquire)==2&&context_.load()==context;}
    bool Restore(const void *device,const void *identity,bool prior,bool &mismatch) noexcept {
        mismatch=false;
        if(!Holding()||device_.load()!=device)return prior;
        if(!identity||identity_.load()!=identity){mismatch=true;Poison();}
        return true; // Never restore FALSE for this owner, including poison/closed states.
    }
    const void *Device() const noexcept {return Holding()?device_.load():nullptr;}
    const void *Context() const noexcept {return Holding()?context_.load():nullptr;}
};
}
