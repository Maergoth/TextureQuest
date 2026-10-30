#pragma once
#include "texturequest_provider_mutation_core.hpp"
#include <exception>
struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11Multithread;
namespace tqpm {
Core &ProviderCore() noexcept;
void ObserveNativeEscape(const void *object,const void *caller) noexcept;
void RecordDevice(const void *api,const void *proxy,const void *dxgi,ID3D11Device *native) noexcept;
void RecordContext(const void *api,const void *queue,const void *proxy,const void *device,ID3D11DeviceContext *native) noexcept;
void UnsupportedNativeState(ID3D11DeviceContext *native) noexcept;
bool ProtectionRestore(const void *api_device,ID3D11Multithread *native,bool prior) noexcept;
void ProtectionRestored(const void *api_device,bool protected_now) noexcept;
inline Boundary MakeBoundary(BoundaryKind kind,Origin origin,const void *destination=nullptr,const void *source=nullptr,
    uint32_t subresource=0,const void *array=nullptr,uint32_t count=0) noexcept {
    return {sizeof(Boundary),version,kind,origin,destination,source,array,count,subresource};
}

} // namespace tqpm

#include "texturequest_provider_creation.hpp"
