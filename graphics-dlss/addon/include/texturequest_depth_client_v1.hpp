#pragma once
#include "texturequest_depth_demand_v1.hpp"
#include <atomic>
namespace tq_depth_v1 {
// Observable post-call publication prevents /O2 tail calls from losing the actual
// add-on caller module at the provider's native return-address admission seam.
struct Client {
 Register register_fn = nullptr; Unregister unregister_fn = nullptr;
 SetRequired set_fn = nullptr; Query query_fn = nullptr;
 std::atomic<uint32_t> last_result { 0 };
 bool available = false, registered = false;
 void Resolve(HMODULE provider) {
  if (!provider) return;
  register_fn = reinterpret_cast<Register>(GetProcAddress(provider, RegisterExport));
  unregister_fn = reinterpret_cast<Unregister>(GetProcAddress(provider, UnregisterExport));
  set_fn = reinterpret_cast<SetRequired>(GetProcAddress(provider, SetRequiredExport));
  query_fn = reinterpret_cast<Query>(GetProcAddress(provider, QueryExport));
  available = register_fn && unregister_fn && set_fn && query_fn;
 }
 uint32_t RegisterModule(void *module, uint32_t mode) {
  const uint32_t result = available ? register_fn(module, mode) : 0;
  last_result.store(result, std::memory_order_release); registered = result != 0; return result;
 }
 uint32_t UnregisterModule(void *module) {
  const uint32_t result = registered ? unregister_fn(module) : 0;
  last_result.store(result, std::memory_order_release); registered = false; return result;
 }
 uint32_t Set(void *module, void *runtime, uint32_t needed) {
  const uint32_t result = registered ? set_fn(module, runtime, needed) : 0;
  last_result.store(result, std::memory_order_release); return result;
 }
 uint32_t Get(void *module, void *runtime, Status &status) {
  const uint32_t result = registered ? query_fn(module, runtime, &status, sizeof(status)) : 0;
  last_result.store(result, std::memory_order_release); return result;
 }
};
}
