#pragma once
#include "reflex_driver.hpp"
#include <d3d11.h>
#include <dxgi.h>
#include <mutex>

namespace tq::reflex {
// The first native game Present is admitted before TextureQuest enables AIO's
// output runtime. Keep that source window anchored across device recreation;
// an output/secondary window must never take over the driver's request.
class Primary {
    mutable std::mutex Lock;
    Controller Driver;
    void* Owner = nullptr;
    IUnknown* Native = nullptr; // Borrowed until the matching destroy_device.
    HWND Window = nullptr;

    static bool SameDevice(IDXGISwapChain* surface, IUnknown* native, HWND window) {
        DXGI_SWAP_CHAIN_DESC desc{};
        DWORD process = 0;
        if (!surface || !native || !window || FAILED(surface->GetDesc(&desc)) ||
            desc.OutputWindow != window || !GetWindowThreadProcessId(window, &process) ||
            process != GetCurrentProcessId()) return false;
        IUnknown *surfaceDevice = nullptr, *identity = nullptr;
        ID3D11Device* d3d11 = nullptr;
        if (FAILED(native->QueryInterface(IID_PPV_ARGS(&d3d11)))) return false;
        d3d11->Release();
        const HRESULT fromSurface = surface->GetDevice(IID_PPV_ARGS(&surfaceDevice));
        const HRESULT fromNative = native->QueryInterface(IID_PPV_ARGS(&identity));
        const bool same = SUCCEEDED(fromSurface) && SUCCEEDED(fromNative) &&
            surfaceDevice == identity;
        if (surfaceDevice) surfaceDevice->Release();
        if (identity) identity->Release();
        return same;
    }
public:
    Primary() = default;
    explicit Primary(Api& injected) : Driver(injected) {}
    void Present(void* owner, IUnknown* native, IDXGISwapChain* surface, HWND window,
                 bool requested, bool allowed, uint64_t now) {
        const std::lock_guard guard(Lock);
        if (!owner || !native || !window || (Window && Window != window)) return;
        if (!Owner) {
            if (!SameDevice(surface, native, window)) return;
            Window = window;
            Owner = owner;
            Native = native;
        }
        if (Owner != owner || Native != native) return;
        Driver.Service(owner, native, window, requested, allowed, now);
    }
    void Destroy(void* owner) {
        const std::lock_guard guard(Lock);
        if (owner != Owner) return;
        Driver.Destroy(owner); // The native device is still valid in this event.
        Owner = nullptr;
        Native = nullptr;
    }
    void Shutdown() {
        const std::lock_guard guard(Lock);
        Driver.Shutdown(); // AddonUninit, outside loader lock and device callbacks.
        Owner = nullptr;
        Native = nullptr;
    }
    reflex::Snapshot Snapshot() const {
        const std::lock_guard guard(Lock);
        return Driver.Snapshot();
    }
};
}
