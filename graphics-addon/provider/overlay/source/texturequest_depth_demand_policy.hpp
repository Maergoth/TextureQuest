#pragma once
#include <atomic>
#include <cstdint>
#include <algorithm>
namespace tq_depth_policy {
struct Gate {
 std::atomic<bool> tracking { true }, fresh { false };
 std::atomic<uint64_t> epoch { 1 };
 std::atomic<uint32_t> reasons { 8 };
 // Called only at the device Present boundary. A changed epoch discards a partial frame.
 bool Step(uint32_t why) {
  reasons.store(why, std::memory_order_release);
  const bool required = why != 0;
  if (tracking.load(std::memory_order_acquire) == required) return false;
  fresh.store(false, std::memory_order_release);
  epoch.fetch_add(1, std::memory_order_acq_rel);
  tracking.store(required, std::memory_order_release);
  return true;
 }
 bool Active() const { return tracking.load(std::memory_order_acquire); }
 bool Ready(uint64_t ready) const { return Active() && fresh.load(std::memory_order_acquire) && ready == epoch.load(std::memory_order_acquire); }
};
template<class Passes, class Samplers, class Textures>
bool PassesNeedDepth(const Passes &passes, const Samplers &samplers, const Textures &textures) {
 for (const auto &pass : passes) for (const auto &binding : pass.texture_bindings) {
  if (binding.index >= samplers.size()) return true; // Unknown metadata is conservative.
  const auto &name = samplers[binding.index].texture_name;
  auto found = std::find_if(textures.begin(), textures.end(), [&](const auto &t) { return t.unique_name == name; });
  if (found == textures.end() || found->semantic == "DEPTH") return true;
 }
 return false;
}
}
