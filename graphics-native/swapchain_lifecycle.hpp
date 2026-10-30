#pragma once
#include <atomic>
#include <cstdint>
namespace tqph {
// Selected API swapchain identity stays fixed. A resize only suspends use;
// actual destruction or failed continuity closes this selection permanently.
class SwapchainLifecycle {
 enum : uint32_t { empty=0,ready=1,resizing=2,closed=3 };
 std::atomic<uintptr_t> identity_{0};
 std::atomic<uint32_t> phase_{empty};
public:
 bool Select(uintptr_t id) noexcept {
  if(!id)return false;uintptr_t zero=0;
  if(!identity_.compare_exchange_strong(zero,id))return false;
  uint32_t expected=empty;return phase_.compare_exchange_strong(expected,ready);
 }
 bool BeginResize(uintptr_t id) noexcept {
  if(!id||identity_.load()!=id)return false;
  uint32_t expected=ready;return phase_.compare_exchange_strong(expected,resizing);
 }
 bool Resume(uintptr_t id,bool continuity) noexcept {
  if(!continuity||!id||identity_.load()!=id){Close();return false;}
  uint32_t expected=resizing;return phase_.compare_exchange_strong(expected,ready);
 }
 bool Ready()const noexcept{return phase_.load()==ready;}
 void Close()noexcept{phase_.store(closed);}
};
}
