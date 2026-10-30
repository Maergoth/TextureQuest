#pragma once
#include <atomic>
#include <array>
#include <cstdint>
#include <limits>
namespace tqpc {
// Shared by the actual provider frames and host resource leases. No native calls.
class CallSlots {
 std::atomic<uint64_t> next_{0},active_{0};
 std::array<std::atomic<uint64_t>,128> slots_{};
public:
 CallSlots()noexcept{for(auto&s:slots_)s.store(0);}
 uint64_t Next()noexcept{auto old=next_.load();for(;;){if(old==std::numeric_limits<uint64_t>::max())return 0;if(next_.compare_exchange_weak(old,old+1))return old+1;}}
 bool Reserve(uint64_t call)noexcept{if(!call)return false;for(auto&s:slots_){uint64_t zero=0;if(s.compare_exchange_strong(zero,call)){active_.fetch_add(1);return true;}}return false;}
 bool Release(uint64_t call)noexcept{if(!call)return false;for(auto&s:slots_){auto wanted=call;if(s.compare_exchange_strong(wanted,0)){active_.fetch_sub(1);return true;}}return false;}
 uint64_t Active()const noexcept{return active_.load();}
};
class ResourceEpoch {
 // State0 captures/native-bound,1 ready,2 admitted,3 retired,4 destroyed.
 std::atomic<uint32_t> state_{0};std::atomic<uint64_t> epoch_{1};
public:
 uint64_t Value()const noexcept{return epoch_.load(std::memory_order_acquire);}
 bool Ready(uint64_t value)noexcept{if(value!=Value())return false;uint32_t zero=0;if(!state_.compare_exchange_strong(zero,1))return false;return value==Value()&&state_.load()==1;}
 bool Admit(uint64_t value)noexcept{if(value!=Value())return false;uint32_t ready=1;if(!state_.compare_exchange_strong(ready,2))return false;return value==Value()&&state_.load()==2;}
 bool Valid(uint64_t value)const noexcept{return value==Value()&&state_.load(std::memory_order_acquire)==2;}
 bool Clean(uint64_t value)const noexcept{return value==Value()&&state_.load(std::memory_order_acquire)<3;}
 bool Dirty(bool destroyed=false)noexcept{auto old=state_.exchange(destroyed?4u:3u,std::memory_order_acq_rel);if(old>=3)return false;epoch_.fetch_add(1,std::memory_order_acq_rel);return true;}
 bool Retired()const noexcept{return state_.load(std::memory_order_acquire)>=3;}
};
} // namespace tqpc
