// TextureQuest add-on for stock ReShade: swaps matched stock textures for
// upscaled ones at creation and keeps an Original/Upscaled comparison shortcut.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#define ImTextureID ImU64
#include <imgui.h>
#include <reshade.hpp>
#include "config.hpp"
#include "shortcut.hpp"
#include "stock_copies.hpp"
#include "stock_paks.hpp"
#include "dds_reader.hpp"
#include "effects_controller.hpp"
#include "renderer_binding_probe.hpp"
#include "dlss_controller.hpp"
#include "aio_controller.hpp"
#include "fps_counter.hpp"
#include "world_graphics_controller.hpp"
#include "depth_demand_v1.hpp"
#include <atomic>
#include <condition_variable>
#include <deque>
#include <fstream>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>

extern "C" __declspec(dllexport) const char* NAME = "TextureQuest";
extern "C" __declspec(dllexport) const char* DESCRIPTION = "Loads verified TextureQuest DDS files when the game creates textures.";

using namespace reshade::api;

namespace {

constexpr uint64_t STOCK_BUDGET_BYTES = 512ull * 1024 * 1024;

// One swapped texture. Generation tells a queued stock load whether the handle
// still names the same texture when the load finishes.
struct Replaced {
    const tq::MapRow* Row = nullptr;
    uint64_t Generation = 0;
    resource Stock{0};
    resource_view StockView{0};
    uint64_t StockBytes = 0;
    bool Queued = false;
};

struct StockJob {
    device* Device = nullptr;
    uint64_t Handle = 0, Generation = 0;
    const tq::MapRow* Row = nullptr;
    resource_desc Desc;
    format ViewFormat = format::unknown;
};

// The game's stock data for the texture being created, kept between
// create_resource and init_resource of the same CreateTexture2D call so a
// texture created while Original is active gets its stock copy without a disk read.
// Pixels is the upscaled chain handed to the device; init_resource accepts the
// handoff only while its initial data still points there, so a creation that
// failed after create_resource leaves nothing for an unrelated init_resource.
struct Handoff {
    const tq::MapRow* Row = nullptr;
    const void* Pixels = nullptr;
    bool KeepStock = false;
    resource_desc Desc;
    subresource_data Levels[16] = {};
};

struct State {
    using OverlayDemandFn = uint32_t(WINAPI *)(void*, void*, uint32_t);
    HMODULE Owner = nullptr;
    OverlayDemandFn SetOverlayDemand = nullptr;
    tq::Config Config;
    tq::TextureMap Map;
    std::unique_ptr<tq::DDSReader> Reader;
    std::unique_ptr<tq::StockPaks> Paks;
    std::unique_ptr<tq::Shortcut> Shortcut;
    std::atomic<bool> Original{false}, Stopping{false};
    bool PushRegistered = false;
    std::mutex Lock;
    std::unordered_map<uint64_t, Replaced> Textures;
    tq::StockBudget Budget{STOCK_BUDGET_BYTES};
    std::atomic<uint64_t> Generation{0}, Swapped{0}, Failed{0}, SwappedBytes{0};
    std::mutex JobLock;
    // Held by the worker for the whole of one job; taken before Lock, never while holding it.
    std::mutex WorkLock;
    std::condition_variable JobWake;
    std::deque<StockJob> Jobs;
    std::thread Worker;
    tq::EffectsController<effect_runtime> Effects;
    tq::DlssController<effect_runtime> DLSS;
    tq::AioController AIO;
    tq::RuntimeFpsCounters FPS;
    tq::WorldGraphicsController WorldGraphics;
    tq::RendererBindingProbe RendererProbe;
    std::atomic<bool> ShowFPS{true};
    std::mutex FpsVisibilityLock;
    std::mutex EffectsStatusLock;
    uint64_t EffectsStatusSignature = UINT64_MAX;
    unsigned EffectsStatusLogs = 0;
    std::string DepthStatusSignature[2];
    unsigned DepthStatusLogs = 0;
};

State* Global = nullptr;
thread_local Handoff Pending;

void OnFpsOverlay(effect_runtime* runtime);

void SetShowFPS(bool visible) {
    // Multiple runtime threads may initialize or change this shared preference.
    // Publish the counter, UI and provider state as one ordered transition.
    std::lock_guard guard(Global->FpsVisibilityLock);
    Global->FPS.SetVisible(visible);
    Global->ShowFPS = visible;
    // Keep event registration stable: ReShade dispatches its callback vector
    // without a lock. The optional provider gate changes only overlay demand.
    // Older providers retain their existing GUI behavior when this is absent.
    if (Global->SetOverlayDemand)
        Global->SetOverlayDemand(Global->Owner, reinterpret_cast<void*>(&OnFpsOverlay), visible ? 1u : 0u);
}

void Log(reshade::log::level level, const std::string& text) { reshade::log::message(level, text.c_str()); }

std::wstring ModuleFolder(HMODULE module) {
    wchar_t path[MAX_PATH * 4];
    DWORD length = GetModuleFileNameW(module, path, DWORD(std::size(path)));
    while (length && path[length - 1] != L'\\')
        --length;
    return std::wstring(path, length);
}

uint64_t ChainBytes(uint32_t family, uint32_t width, uint32_t height, uint32_t mips) {
    uint64_t bytes = 0;
    for (uint32_t mip = 0; mip < mips; ++mip) {
        uint32_t row_bytes, rows;
        tq::LevelSize(family, width, height, mip, row_bytes, rows);
        bytes += uint64_t(row_bytes) * rows;
    }
    return bytes;
}

// view_format must be typed; a typeless resource takes the format of the view the game binds.
bool CreateCopy(device* dev, const resource_desc& desc, const subresource_data* levels, format view_format, Replaced& entry) {
    resource copy{0};
    if (!dev->create_resource(desc, levels, resource_usage::shader_resource, &copy))
        return false;
    resource_view view{0};
    if (!dev->create_resource_view(copy, resource_usage::shader_resource, resource_view_desc(view_format, 0, desc.texture.levels, 0, 1), &view)) {
        dev->destroy_resource(copy);
        return false;
    }
    entry.Stock = copy;
    entry.StockView = view;
    entry.StockBytes = ChainBytes(tq::FamilyOf(uint32_t(desc.texture.format)), desc.texture.width, desc.texture.height, desc.texture.levels);
    return true;
}

void DestroyCopy(device* dev, Replaced& entry) {
    if (entry.StockView.handle)
        dev->destroy_resource_view(entry.StockView);
    if (entry.Stock.handle)
        dev->destroy_resource(entry.Stock);
    entry.Stock = {0};
    entry.StockView = {0};
    entry.StockBytes = 0;
}

// Caller holds Global->Lock. Destroys the copies of any textures the budget evicts.
bool Admit(device* dev, uint64_t handle, Replaced& entry) {
    std::vector<uint64_t> evicted;
    if (!Global->Budget.Admit(handle, entry.StockBytes, evicted)) {
        DestroyCopy(dev, entry);
        return false;
    }
    for (uint64_t victim : evicted) {
        if (auto it = Global->Textures.find(victim); it != Global->Textures.end() && victim != handle)
            DestroyCopy(dev, it->second);
    }
    return true;
}

// EQ2's resources belong to its DX11 device. DLSS also owns a DX12 compositor;
// its callbacks must never use or clear the game's texture/copy state.
bool GameDevice(device* dev) {
    return dev && dev->get_api() == device_api::d3d11;
}

bool GameRuntime(effect_runtime* runtime) {
    return runtime && GameDevice(runtime->get_device()) && runtime->get_hwnd() != nullptr;
}

void OnRendererPipeline(device* dev,pipeline_layout,uint32_t count,const pipeline_subobject*items,pipeline handle) {
    if(Global&&GameDevice(dev))Global->RendererProbe.Pipeline(dev,count,items,handle);
}
void OnRendererPipelineDestroyed(device*dev,pipeline handle) {
    if(Global&&GameDevice(dev))Global->RendererProbe.DestroyPipeline(handle);
}
void OnRendererResourceDestroyed(device*dev,resource handle) {
    if(Global&&GameDevice(dev))Global->RendererProbe.DestroyResource(handle);
}
bool OnRendererDraw(command_list*cmd,uint32_t count,uint32_t,uint32_t,uint32_t) {
    if(Global)Global->RendererProbe.Draw(cmd,count);return false;
}
bool OnRendererDrawIndexed(command_list*cmd,uint32_t count,uint32_t,uint32_t,int32_t,uint32_t) {
    if(Global)Global->RendererProbe.Draw(cmd,count);return false;
}
void OnRendererMap(device*dev,resource r,uint64_t offset,uint64_t size,map_access access,void**data) {
    if(Global&&Global->RendererProbe.NeedsCaptureCallbacks()&&GameDevice(dev))Global->RendererProbe.Map(r,offset,size,access,data);
}
void OnRendererUnmap(device*dev,resource r) {
    if(Global&&Global->RendererProbe.NeedsCaptureCallbacks()&&GameDevice(dev))Global->RendererProbe.Unmap(r);
}
bool OnRendererUpdate(device*dev,const void*data,resource r,uint64_t offset,uint64_t size) {
    if(Global&&Global->RendererProbe.NeedsCaptureCallbacks()&&GameDevice(dev))Global->RendererProbe.Update(r,data,offset,size);return false;
}


bool OnCreateResource(device* dev, resource_desc& desc, subresource_data* initial_data, resource_usage) {
    if (!GameDevice(dev)) return false;
    Pending.Row = nullptr;
    if (desc.type != resource_type::texture_2d || !initial_data || desc.texture.depth_or_layers != 1 || !desc.texture.levels ||
        desc.texture.levels > 16 || (desc.flags & resource_flags::cube_compatible) == resource_flags::cube_compatible)
        return false;
    const uint32_t family = tq::FamilyOf(uint32_t(desc.texture.format));
    const tq::Layout layout{desc.texture.width, desc.texture.height, desc.texture.levels, family};
    if (!family || !Global->Map.Layouts.contains(layout))
        return false;
    tq::Subresource levels[16];
    for (uint32_t mip = 0; mip < desc.texture.levels; ++mip)
        levels[mip] = {initial_data[mip].data, initial_data[mip].row_pitch};
    const tq::MapRow* row = Global->Map.Find(tq::Fingerprint(layout, levels), layout);
    if (!row)
        return false;
    const unsigned char* pixels = Global->Reader->Read(*row);
    if (!pixels || ChainBytes(family, row->TargetWidth, row->TargetHeight, row->TargetMips) != row->PixelBytes) {
        if (Global->Failed++ == 0)
            Log(reshade::log::level::warning, "could not read a texture from the downloaded pack; it stays stock");
        return false;
    }
    Pending.Row = row;
    Pending.Pixels = pixels;
    // A typeless texture's view format is only known once the game binds it, so its copy is loaded then.
    Pending.KeepStock = Global->Original && !format_is_typeless(desc.texture.format);
    if (Pending.KeepStock) {
        Pending.Desc = desc;
        for (uint32_t mip = 0; mip < desc.texture.levels; ++mip)
            Pending.Levels[mip] = initial_data[mip];
    }
    uint64_t offset = 0;
    for (uint32_t mip = 0; mip < row->TargetMips; ++mip) {
        uint32_t row_bytes, rows;
        tq::LevelSize(family, row->TargetWidth, row->TargetHeight, mip, row_bytes, rows);
        initial_data[mip] = {const_cast<unsigned char*>(pixels + offset), row_bytes, row_bytes * rows};
        offset += uint64_t(row_bytes) * rows;
    }
    desc.texture.width = row->TargetWidth;
    desc.texture.height = row->TargetHeight;
    desc.texture.levels = uint16_t(row->TargetMips);
    ++Global->Swapped;
    Global->SwappedBytes += row->PixelBytes;
    return true;
}

void OnInitResource(device* dev, const resource_desc& desc, const subresource_data* initial_data, resource_usage, resource handle) {
    if (!GameDevice(dev)) return;
    const tq::MapRow* row = Pending.Row;
    Pending.Row = nullptr;
    if (!row || desc.type != resource_type::texture_2d || !initial_data || initial_data[0].data != Pending.Pixels)
        return;
    Replaced entry{row, ++Global->Generation};
    if (Pending.KeepStock && !CreateCopy(dev, Pending.Desc, Pending.Levels, Pending.Desc.texture.format, entry))
        entry = Replaced{row, entry.Generation};
    std::lock_guard lock(Global->Lock);
    if (entry.StockView.handle && !(Global->Original && Admit(dev, handle.handle, entry)))
        DestroyCopy(dev, entry);
    Global->Textures[handle.handle] = entry;
}

void OnDestroyResource(device* dev, resource handle) {
    if (!GameDevice(dev)) return;
    std::lock_guard lock(Global->Lock);
    auto it = Global->Textures.find(handle.handle);
    if (it == Global->Textures.end())
        return;
    DestroyCopy(dev, it->second);
    Global->Budget.Release(handle.handle);
    Global->Textures.erase(it);
}

// Worker thread: loads stock copies of textures that already existed when
// Original was selected. Never runs on the render thread.
void StockWorker() {
    std::vector<unsigned char> buffer;
    for (;;) {
        StockJob job;
        std::unique_lock work(Global->WorkLock, std::defer_lock);
        {
            std::unique_lock lock(Global->JobLock);
            Global->JobWake.wait(lock, [] { return Global->Stopping || !Global->Jobs.empty(); });
            if (Global->Stopping)
                return;
            work.lock();
            job = Global->Jobs.front();
            Global->Jobs.pop_front();
        }
        Replaced copy{job.Row, job.Generation};
        bool loaded = false;
        if (Global->Original) {
            const size_t at = Global->Paks->Read(*job.Row, buffer);
            const tq::MapRow& row = *job.Row;
            if (at && at + ChainBytes(row.Family, row.Width, row.Height, row.Mips) <= buffer.size()) {
                subresource_data levels[16];
                tq::Subresource check[16];
                uint64_t offset = at;
                for (uint32_t mip = 0; mip < row.Mips; ++mip) {
                    uint32_t row_bytes, rows;
                    tq::LevelSize(row.Family, row.Width, row.Height, mip, row_bytes, rows);
                    levels[mip] = {buffer.data() + offset, row_bytes, row_bytes * rows};
                    check[mip] = {buffer.data() + offset, row_bytes};
                    offset += uint64_t(row_bytes) * rows;
                }
                const XXH128_hash_t key = tq::Fingerprint({row.Width, row.Height, row.Mips, row.Family}, check);
                resource_desc desc = job.Desc;
                desc.texture.width = row.Width;
                desc.texture.height = row.Height;
                desc.texture.levels = uint16_t(row.Mips);
                loaded = key.low64 == row.KeyLow && key.high64 == row.KeyHigh && CreateCopy(job.Device, desc, levels, job.ViewFormat, copy);
            }
        }
        std::lock_guard lock(Global->Lock);
        auto it = Global->Textures.find(job.Handle);
        const bool current = it != Global->Textures.end() && it->second.Generation == job.Generation;
        if (current)
            it->second.Queued = false;
        if (!loaded)
            continue;
        if (!current || !Global->Original || it->second.StockView.handle || !Admit(job.Device, job.Handle, copy)) {
            DestroyCopy(job.Device, copy);
            continue;
        }
        it->second.Stock = copy.Stock;
        it->second.StockView = copy.StockView;
        it->second.StockBytes = copy.StockBytes;
    }
}

// Drops the device's queued jobs, waits out the one in flight, then releases
// every stock copy so nothing touches the device after it is gone.
void OnDestroyDevice(device* dev) {
    if (!GameDevice(dev)) return;
    {
        std::lock_guard lock(Global->JobLock);
        std::erase_if(Global->Jobs, [dev](const StockJob& job) { return job.Device == dev; });
    }
    {
        std::lock_guard work(Global->WorkLock);
    }
    std::lock_guard lock(Global->Lock);
    for (auto& [handle, entry] : Global->Textures) {
        DestroyCopy(dev, entry);
        Global->Budget.Release(handle);
    }
    Global->Textures.clear();
}

void OnPushDescriptors(command_list* cmd, shader_stage stages, pipeline_layout layout, uint32_t param, const descriptor_table_update& update) {
    if (!cmd || !GameDevice(cmd->get_device())) return;
    if (update.type != descriptor_type::texture_shader_resource_view || !update.count || update.count > 128)
        return;
    device* dev = cmd->get_device();
    const auto* bound = static_cast<const resource_view*>(update.descriptors);
    resource_view views[128];
    bool changed = false;
    std::vector<StockJob> queue;
    {
        std::lock_guard lock(Global->Lock);
        for (uint32_t i = 0; i < update.count; ++i) {
            views[i] = bound[i];
            if (!bound[i].handle)
                continue;
            const resource texture = dev->get_resource_from_view(bound[i]);
            auto it = Global->Textures.find(texture.handle);
            if (it == Global->Textures.end())
                continue;
            Replaced& entry = it->second;
            if (entry.StockView.handle) {
                Global->Budget.Touch(it->first);
                views[i] = entry.StockView;
                changed = true;
            } else if (!entry.Queued) {
                entry.Queued = true;
                queue.push_back({dev, it->first, entry.Generation, entry.Row, dev->get_resource_desc(texture),
                                 dev->get_resource_view_desc(bound[i]).format});
            }
        }
    }
    if (!queue.empty()) {
        std::lock_guard lock(Global->JobLock);
        for (StockJob& job : queue)
            Global->Jobs.push_back(job);
        Global->JobWake.notify_one();
    }
    if (!changed)
        return;
    descriptor_table_update replacement = update;
    replacement.descriptors = views;
    cmd->push_descriptors(stages, layout, param, replacement);
}

// Runs on the render thread, the only thread that raises push_descriptors, so
// changing that event's registration here cannot race its invocation. This
// relies on the game binding shader resources only on the immediate context:
// deferred-context command lists would raise push_descriptors on other threads.
void OnPresent(command_queue* queue, swapchain* surface, const rect*, const rect*, uint32_t, const rect*) {
    if (!queue || !GameDevice(queue->get_device()) || !surface || !surface->get_hwnd()) return;
    const bool original = Global->Original;
    Global->WorldGraphics.Reconcile(Global->Config.WorldGraphics, original);
    Global->AIO.Reconcile(Global->Config.DLSS && Global->Config.DLSSBackend == "aio", original);
    if (original == Global->PushRegistered)
        return;
    Global->PushRegistered = original;
    if (original) {
        reshade::register_event<reshade::addon_event::push_descriptors>(&OnPushDescriptors);
        return;
    }
    reshade::unregister_event<reshade::addon_event::push_descriptors>(&OnPushDescriptors);
    {
        std::lock_guard lock(Global->JobLock);
        Global->Jobs.clear();
    }
    device* dev = queue->get_device();
    std::lock_guard lock(Global->Lock);
    for (auto& [handle, entry] : Global->Textures) {
        DestroyCopy(dev, entry);
        entry.Queued = false;
        Global->Budget.Release(handle);
    }
}

bool OwnedEffectsEnabled() {
    return tq::EffectsEnabled(Global->Config.Effects, Global->Original.load());
}

bool ExactEffectText(const char* text, size_t size, const char* expected, size_t capacity) {
    return tq::ExactEffectText(text, size, expected, capacity);
}

// TQ_DEPTH_DESCRIPTOR_POLICY_BEGIN
// Pure copied-descriptor validation; no resource/view handles are retained.
const char* DepthDescriptorReason(const resource_desc& desc, const resource_view_desc& view) {
    if (desc.type != resource_type::texture_2d) return "resource_type";
    if (view.type != resource_view_type::texture_2d) return "view_type";
    if (desc.texture.samples != 1) return "samples";
    if (desc.texture.depth_or_layers != 1) return "resource_layers";
    if (!desc.texture.width || !desc.texture.height || desc.texture.width > 16384 || desc.texture.height > 16384)
        return "extent";
    if (!desc.texture.levels || desc.texture.levels > 32) return "resource_mips";
    if (view.texture.first_level >= desc.texture.levels) return "first_mip";
    if (!view.texture.levels) return "view_mips";
    if (view.texture.first_layer != 0) return "first_layer";
    // D3D11's non-array SRV converter leaves the ignored layers field zero.
    // This exception is reached only for an exact 2D view of a single-layer 2D resource.
    if (view.texture.layers != 0 && view.texture.layers != 1 && view.texture.layers != UINT32_MAX)
        return "view_layers";
    if (view.texture.levels != UINT32_MAX && view.texture.levels > desc.texture.levels - view.texture.first_level)
        return "mip_range";
    bool compatible = false;
    switch (view.format) {
    case format::r16_unorm:
        compatible = desc.texture.format == format::r16_typeless || desc.texture.format == format::d16_unorm || desc.texture.format == view.format;
        break;
    case format::r32_float:
        compatible = desc.texture.format == format::r32_typeless || desc.texture.format == format::d32_float || desc.texture.format == view.format;
        break;
    case format::r24_unorm_x8_uint:
        compatible = desc.texture.format == format::r24_g8_typeless || desc.texture.format == format::d24_unorm_s8_uint ||
            desc.texture.format == format::d24_unorm_x8_uint || desc.texture.format == view.format;
        break;
    case format::r32_float_x8_uint:
        compatible = desc.texture.format == format::r32_g8_typeless || desc.texture.format == format::d32_float_s8_uint || desc.texture.format == view.format;
        break;
    default: return "unsupported_view_format";
    }
    return compatible ? nullptr : "incompatible_resource_format";
}
// TQ_DEPTH_DESCRIPTOR_POLICY_END

bool RecordDepthResult(unsigned phase, const char* reason, const std::string& facts = {}) {
    const std::string message = std::string("depth binding: phase=") +
        (phase == 0 ? "begin_effects_binding" : "effects_present_binding") +
        " reason=" + (reason ? reason : "accepted") + facts;
    bool emit = false;
    {
        std::lock_guard lock(Global->EffectsStatusLock);
        if (Global->DepthStatusLogs < 16 && Global->DepthStatusSignature[phase] != message) {
            Global->DepthStatusSignature[phase] = message;
            ++Global->DepthStatusLogs;
            emit = true;
        }
    }
    if (emit) Log(reshade::log::level::info, message);
    return reason == nullptr;
}

// Fresh descriptor/readiness query inside the controller's claimed render
// callback only. These copied facts describe this query phase, not a later
// draw's exact binding, world projection or the original game's MSAA.
bool OwnedDepthAvailable(effect_runtime* runtime, unsigned phase,
    const char* effectsFile = tq::EffectsFile, const char* readyName = "TQDepthReady",
    const char* depthName = "TQDiagnosticDepth") {
    const auto ready = runtime->find_uniform_variable(effectsFile, readyName);
    const auto depth = runtime->find_texture_variable(effectsFile, depthName);
    if (!ready.handle || !depth.handle) return RecordDepthResult(phase, "missing_uniform_or_texture",
        " uniform=" + std::to_string(ready.handle != 0) + " texture=" + std::to_string(depth.handle != 0));
    char file[256]{}, name[128]{}, annotation[64]{};
    size_t a = sizeof(file), b = sizeof(name), c = sizeof(annotation);
    runtime->get_uniform_variable_effect_name(ready, file, &a);
    runtime->get_uniform_variable_name(ready, name, &b);
    if (!ExactEffectText(file, a, effectsFile, sizeof(file)) ||
        !ExactEffectText(name, b, readyName, sizeof(name))) return RecordDepthResult(phase, "uniform_identity");
    if (!runtime->get_annotation_string_from_uniform_variable(ready, "source", annotation, &c) ||
        !ExactEffectText(annotation, c, "bufready_depth", sizeof(annotation))) return RecordDepthResult(phase, "source_annotation");
    bool available = false;
    runtime->get_uniform_value_bool(ready, &available, 1);
    if (!available) return RecordDepthResult(phase, "readiness_false");
    a = sizeof(file); b = sizeof(name);
    runtime->get_texture_variable_effect_name(depth, file, &a);
    runtime->get_texture_variable_name(depth, name, &b);
    if (!ExactEffectText(file, a, effectsFile, sizeof(file)) ||
        !ExactEffectText(name, b, depthName, sizeof(name))) return RecordDepthResult(phase, "texture_identity");
    resource_view linear{}, srgb{};
    runtime->get_texture_binding(depth, &linear, &srgb);
    if (!linear.handle) return RecordDepthResult(phase, "linear_unbound"); // Never substitute sRGB.
    auto* dev = runtime->get_device();
    if (!dev) return RecordDepthResult(phase, "no_device");
    std::string facts = " api=" + std::to_string(static_cast<uint32_t>(dev->get_api()));
    const auto resource = dev->get_resource_from_view(linear);
    if (!resource.handle) return RecordDepthResult(phase, "no_resource", facts);
    const auto view = dev->get_resource_view_desc(linear);
    const auto desc = dev->get_resource_desc(resource);
    facts += " resource_type=" + std::to_string(static_cast<uint32_t>(desc.type)) +
        " view_type=" + std::to_string(static_cast<uint32_t>(view.type)) +
        " view_format=" + std::to_string(static_cast<uint32_t>(view.format));
    if (desc.type == resource_type::texture_2d) facts +=
        " resource_format=" + std::to_string(static_cast<uint32_t>(desc.texture.format)) +
        " width=" + std::to_string(desc.texture.width) + " height=" + std::to_string(desc.texture.height) +
        " resource_layers=" + std::to_string(desc.texture.depth_or_layers) +
        " samples=" + std::to_string(desc.texture.samples) + " resource_mips=" + std::to_string(desc.texture.levels);
    if (view.type == resource_view_type::texture_2d) facts +=
        " first_mip=" + std::to_string(view.texture.first_level) + " view_mips=" + std::to_string(view.texture.levels) +
        " first_layer=" + std::to_string(view.texture.first_layer) + " view_layers=" + std::to_string(view.texture.layers);
    return RecordDepthResult(phase, DepthDescriptorReason(desc, view), facts);
}

void OnInitEffectRuntime(effect_runtime* runtime) {
    if (!GameRuntime(runtime)) return;
    bool showFPS = true;
    reshade::get_config_value(nullptr, "TextureQuest", "ShowFPS", showFPS);
    Global->FPS.Register(runtime);
    SetShowFPS(showFPS);
    Global->Effects.Reload(runtime);
    Global->DLSS.Reload(runtime);
}

void OnReloadedEffects(effect_runtime* runtime) {
    if (!GameRuntime(runtime)) return;
    Global->Effects.Reload(runtime); // Metadata only; requests survive, handles do not.
    Global->DLSS.Reload(runtime);
}

void OnDestroyEffectRuntime(effect_runtime* runtime) {
    Global->FPS.Forget(runtime);
    Global->Effects.Forget(runtime);
    Global->DLSS.Forget(runtime);
}

bool OnTechniqueState(effect_runtime* runtime, effect_technique technique, bool enabled) {
    if (!GameRuntime(runtime)) return false;
    // Dispatch before acquiring either controller's mutex. A synchronous setter
    // callback must never take the other controller's lock in reverse order.
    if (Global->Config.DLSS && Global->Config.DLSSBackend == "feeder" && Global->DLSS.IsOwned(runtime, technique))
        return Global->DLSS.TechniqueState(runtime, technique, enabled, true);
    const int owned = Global->Effects.OwnedTechniqueIndex(runtime, technique);
    if (owned >= 0 || owned == -2)
        return Global->Effects.TechniqueState(runtime, technique, enabled, Global->Config.Effects);
    return false;
}

void ReconcileEffects(effect_runtime* runtime, unsigned phase) {
    if (!GameRuntime(runtime)) return;
    Global->WorldGraphics.Reconcile(Global->Config.WorldGraphics, Global->Original.load());
    Global->AIO.Reconcile(Global->Config.DLSS && Global->Config.DLSSBackend == "aio", Global->Original.load());
    Global->DLSS.Reconcile(runtime, Global->Config.DLSS && Global->Config.DLSSBackend == "feeder", Global->Original.load(),
        [phase](effect_runtime* current) { return OwnedDepthAvailable(current, phase,
            tq::DlssFeedFile, "TQDlssDepthReady", "TQDlssDepth"); });
    if (!Global->Effects.Reconcile(runtime, OwnedEffectsEnabled(),
        [phase](effect_runtime* current) { return OwnedDepthAvailable(current, phase); })) return;
    const auto state = Global->Effects.Snapshot(runtime);
    const bool selected = Global->Config.Effects, original = Global->Original.load();
    const uint64_t signature = state.Requested | (uint64_t(state.Available) << 6) |
        (uint64_t(state.Active) << 12) | (uint64_t(state.DepthReady) << 18) |
        (uint64_t(selected) << 19) | (uint64_t(original) << 20);
    bool emit = false;
    {
        std::lock_guard lock(Global->EffectsStatusLock);
        if (Global->EffectsStatusLogs < 16 && (state.Available || Global->EffectsStatusLogs) &&
            signature != Global->EffectsStatusSignature) {
            Global->EffectsStatusSignature = signature;
            ++Global->EffectsStatusLogs;
            emit = true;
        }
    }
    if (emit) Log(reshade::log::level::info, "effects status: selected=" + std::to_string(selected) +
        " original=" + std::to_string(original) + " requested=" + std::to_string(state.Requested) +
        " available=" + std::to_string(state.Available) + " active=" + std::to_string(state.Active) +
        " depth=" + std::to_string(state.DepthReady));
}

void OnBeginEffects(effect_runtime* runtime, command_list*, resource_view, resource_view) {
    ReconcileEffects(runtime, 0);
}

void OnReshadePresent(effect_runtime* runtime) {
    // Runs even with no active FX, so the next Upscaled frame can restore the
    // requested effects. This query is phase-qualified, not a draw observation.
    ReconcileEffects(runtime, 1);
    if (!Global || !GameRuntime(runtime)) return;
    if (Global->ShowFPS.load(std::memory_order_relaxed)) {
        const auto aio = Global->AIO.Snapshot();
        const bool outputActive = tq::AioOutputActive(aio.Compatible, Global->Original.load(), aio.Status.Flags,
            aio.ControlsCompatible ? &aio.Controls : nullptr);
        Global->FPS.Frame(runtime, GetTickCount64(), outputActive, aio.Status.PresentedFrames);
    }
    if (Global->RendererProbe.NeedsCaptureCallbacks())
        Global->RendererProbe.Frame(runtime->get_device()->get_native());
}

void OnFpsOverlay(effect_runtime* runtime) {
    if (!Global || !GameRuntime(runtime) || !Global->ShowFPS.load()) return;
    const auto rates = Global->FPS.Snapshot(runtime);
    ImGui::SetNextWindowPos(ImVec2(10, 10), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.65f);
    constexpr auto flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoInputs |
        ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoDocking;
    if (ImGui::Begin("TextureQuest FPS##TQFPS", nullptr, flags)) {
        if (rates.Ready) {
            ImGui::Text("Game %.0f FPS", rates.Game);
            if (rates.OutputActive) ImGui::Text("Output %.0f FPS", rates.Output);
        } else ImGui::TextUnformatted("FPS: measuring...");
    }
    ImGui::End();
}

bool OnOverlayTechnique(effect_runtime* runtime, effect_technique technique) {
    if (!Global) return false;
    if (!GameRuntime(runtime)) return false;
    if (Global->Config.DLSS && Global->Config.DLSSBackend == "feeder" && Global->DLSS.IsFeed(runtime, technique)) {
        const auto state = Global->DLSS.Snapshot(runtime);
        ImGui::TextDisabled("[%s]", tq::DlssStatus(state, Global->Config.DLSS, Global->Original.load()));
        ImGui::SameLine();
        bool requested = state.Requested;
        ImGui::BeginDisabled(!Global->Config.DLSS);
        if (ImGui::Checkbox("DLSS 5 [DLSS5_Feed.fx]", &requested))
            Global->DLSS.SetRequested(runtime, requested);
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("Neural rendering with estimated motion. Feeding is not a model-success check.\nOriginal stops both motion estimation and neural input; re-enable resets history.");
        return true;
    }
    const int owned = Global->Effects.OwnedTechniqueIndex(runtime, technique);
    if (owned == -2) return true; // Hide retired effects from retained owned caches.
    if (owned < 0) return false; // Preserve ReShade's controls for every other FX.
    const size_t index = static_cast<size_t>(owned);
    const bool selected = Global->Config.Effects, original = Global->Original.load();
    const auto state = Global->Effects.Snapshot(runtime);
    // Keep the checkbox last so ReShade's existing focus, drag and context-menu
    // handling still targets that item and its normal lower strength editor.
    ImGui::TextDisabled("[%s]", tq::EffectStatus(state, index, selected, original));
    ImGui::SameLine();
    bool requested = (state.Requested & (1u << index)) != 0;
    const std::string label = std::string(tq::EffectNames[index]) + " [" + tq::EffectsFile + "]";
    ImGui::BeginDisabled(!selected);
    if (ImGui::Checkbox(label.c_str(), &requested))
        Global->Effects.SetRequested(runtime, index, requested);
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("Checked = your choice. Status = what is running.\nDepth effects wait for depth; Original temporarily disables all.");
    return true;
}

enum class AioPanel { FrameGeneration, Advanced, Debug };

void DrawAioControls(bool original, AioPanel panel) {
    const auto aio = Global->AIO.Snapshot();
    const auto& status = aio.Status;
    if (!aio.Compatible || !aio.ControlsCompatible ||
        !(aio.Controls.Flags & TQAIO_ControlsLoaded)) {
        if (panel == AioPanel::FrameGeneration) {
            bool disabled = false;
            ImGui::BeginDisabled();
            ImGui::Checkbox("Frame Gen", &disabled);
            ImGui::EndDisabled();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                ImGui::SetTooltip("DLSS controls: %s. See Debug for details.", !aio.Present ? "waiting for add-on" :
                    !aio.ControlsCompatible ? "update the graphics hook" : "initializing");
        } else if (panel == AioPanel::Debug) {
            ImGui::TextDisabled("DLSS controls: %s", !aio.Present ? "waiting for add-on" :
                !aio.ControlsCompatible ? "update the graphics hook" : "initializing");
        }
        return;
    }
    auto options = aio.Controls.Requested;
    // ABI2 retains its legacy SR fields for binary compatibility. The managed
    // profile no longer offers or requests Super Resolution.
    options.EnabledMask &= ~uint32_t(TQAIO_SR);
    options.SrMode = TQAIO_SR_Current;
    bool changed = false;
    const auto component = [&options, &changed](const char* label, uint32_t bit, const char* id,
                                               int* value, const char* const* items, int count) {
        bool enabled = (options.EnabledMask & bit) != 0;
        if (ImGui::Checkbox(label, &enabled)) {
            options.EnabledMask = enabled ? options.EnabledMask | bit : options.EnabledMask & ~bit;
            changed = true;
        }
        ImGui::SameLine(190.0f);
        ImGui::SetNextItemWidth(210.0f);
        return ImGui::Combo(id, value, items, count);
    };
    if (panel == AioPanel::FrameGeneration) {
        static constexpr const char* frameModes[] = {"2x", "3x", "4x"};
        int frameMode = int(options.FrameMultiplier) - 2;
        if (component("Frame Gen", TQAIO_FG, "##TQFrameGeneration", &frameMode, frameModes, 3)) {
            options.FrameMultiplier = uint32_t(frameMode + 2);
            changed = true;
        }
    }
    if (panel == AioPanel::Advanced) {
        static constexpr const char* neuralPresets[] = {"Preset 1 (Default)", "Preset 2", "Preset 3", "Custom"};
        int neural = options.NrPasses != 1 ? 3 : int(options.NrModel) - 1;
        if (component("Neural Rendering", TQAIO_NR, "##TQNeuralRendering", &neural, neuralPresets, 4)) {
            if (neural < 3) {
                options.NrModel = uint32_t(neural + 1);
                options.NrPasses = 1;
                changed = true;
            }
            if (neural == 3) ImGui::OpenPopup("Neural Rendering Advanced");
        }
        ImGui::SameLine();
        if (ImGui::Button("Advanced")) ImGui::OpenPopup("Neural Rendering Advanced");
        if (ImGui::BeginPopup("Neural Rendering Advanced")) {
            bool enabled = (options.EnabledMask & TQAIO_NR) != 0;
            if (ImGui::Checkbox("Neural rendering", &enabled)) {
                options.EnabledMask = enabled ? options.EnabledMask | TQAIO_NR : options.EnabledMask & ~TQAIO_NR;
                changed = true;
            }
            static constexpr const char* models[] = {"Preset 1", "Preset 2", "Preset 3"};
            int model = int(options.NrModel) - 1;
            if (ImGui::Combo("Model", &model, models, 3)) { options.NrModel = uint32_t(model + 1); changed = true; }
            int passes = int(options.NrPasses);
            if (ImGui::SliderInt("Passes", &passes, 1, 3)) { options.NrPasses = uint32_t(passes); changed = true; }
            ImGui::TextDisabled("More passes repeat neural rendering and cost GPU time.");
            ImGui::EndPopup();
        }
    }
    if (changed && !Global->AIO.RequestControls(options))
        ImGui::TextDisabled("The DLSS setting could not be applied.");
    if (panel == AioPanel::Debug) {
        ImGui::Text("Game frame: %ux%u   Output: %ux%u", status.SourceWidth, status.SourceHeight, status.OutputWidth, status.OutputHeight);
        ImGui::Text("Completed: NR %llu / FG %llu",
            static_cast<unsigned long long>(status.NrFrames), static_cast<unsigned long long>(status.FgFrames));
        if ((options.EnabledMask & TQAIO_FG) && options.FrameMultiplier == 4 && (aio.Controls.Flags & TQAIO_Unsupported4x))
            ImGui::TextDisabled("4x is unavailable on this GPU/runtime.");
        if ((options.EnabledMask & TQAIO_FG) && options.FrameMultiplier == 3 && (aio.Controls.Flags & TQAIO_Unsupported3x))
            ImGui::TextDisabled("3x is unavailable on this GPU/runtime.");
        if (original) ImGui::TextDisabled("Paused in Original mode. Your choices are kept.");
        else if (aio.Controls.Flags & TQAIO_ControlsPending) ImGui::TextDisabled("Applying DLSS changes...");
    }
}

void OnTextureQuestOverlay(effect_runtime* runtime) {
    if (!GameRuntime(runtime) || !Global) return;
    const bool original = Global->Original.load();
    const bool selected = Global->Config.Effects;
    const bool aioSelected = Global->Config.DLSS && Global->Config.DLSSBackend == "aio";
    const auto state = Global->Effects.Snapshot(runtime);
    bool showFPS = Global->ShowFPS.load();
    if (ImGui::Checkbox("Show FPS", &showFPS)) {
        SetShowFPS(showFPS);
        reshade::set_config_value(nullptr, "TextureQuest", "ShowFPS", showFPS);
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Game = rendered frames. Output = compositor Presents, including generated frames.\nOutput is not a monitor scanout measurement.");
    if (aioSelected) DrawAioControls(original, AioPanel::FrameGeneration);
    else {
        bool enabled = false;
        ImGui::BeginDisabled();
        ImGui::Checkbox("Frame Gen", &enabled);
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("Select Graphical Effects / DLSS in the TextureQuest launcher to install supported DLSS processing.");
    }
    int32_t preset = -1;
    const bool presetsReady = Global->Config.WorldGraphics && Global->WorldGraphics.ReadPreset(preset);
    static constexpr const char* qualityNames[] = {"Low", "Medium", "High", "Max"};
    const char* qualityPreview = !presetsReady ? "Unavailable" :
        preset >= 0 && preset < 4 ? qualityNames[preset] : "Custom";
    ImGui::BeginDisabled(!presetsReady);
    ImGui::SetNextItemWidth(210.0f);
    if (ImGui::BeginCombo("Quality Presets", qualityPreview)) {
        for (uint32_t i = 0; i < std::size(qualityNames); ++i) {
            if (ImGui::Selectable(qualityNames[i], preset == static_cast<int32_t>(i)))
                Global->WorldGraphics.RequestPreset(i);
            if (preset == static_cast<int32_t>(i)) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip(presetsReady ?
            "Sets lighting features and light/shadow limits for your performance budget.\nKeeps your brightness, radius, color and softness settings. Changes to preset-controlled options show Custom.\nFrame Gen and Neural Rendering remain independent. Advanced contains individual controls." :
            "Quality presets require the current managed EQ2 Graphics add-on. See Debug for details.");

    ImGui::Separator();
    if (ImGui::TreeNode("Advanced")) {
        ImGui::Text("Textures: %s", original ? "Original" : "TextureQuest");
        ImGui::TextDisabled("Press Home to show or hide these settings.");
        if (original) ImGui::TextDisabled("Graphics paused in Original. Your choices are kept.");
        if (aioSelected) {
            DrawAioControls(original, AioPanel::Advanced);
            ImGui::Separator();
        }
        if (Global->Config.WorldGraphics) {
            if (!Global->WorldGraphics.DrawAdvanced(runtime))
                ImGui::TextDisabled("Waiting for the managed EQ2 Graphics add-on.");
        } else {
            constexpr const char* labels[] = {"Ambient occlusion", "Contact shadows"};
            ImGui::BeginDisabled(!selected);
            for (size_t i = 0; i < std::size(labels); ++i) {
                bool requested = (state.Requested & (1u << i)) != 0;
                if (ImGui::Checkbox(labels[i], &requested)) Global->Effects.SetRequested(runtime, i, requested);
            }
            ImGui::EndDisabled();
        }
        ImGui::TextDisabled("Your texture hotkey switches textures and graphics off/on.");
        ImGui::TreePop();
    }
    if (ImGui::TreeNode("Debug")) {
        if (aioSelected) DrawAioControls(original, AioPanel::Debug);
        if (Global->Config.WorldGraphics) {
            if (!Global->WorldGraphics.DrawDebug(runtime))
                ImGui::TextDisabled("Managed graphics diagnostics unavailable; update the graphics hook.");
        } else {
            if (ImGui::Button("Capture world lighting (3 frames)"))
                Global->RendererProbe.Arm(runtime->get_device()->get_native(), std::filesystem::path(Global->Config.Downloads).parent_path().parent_path());
            const auto captureStatus = Global->RendererProbe.Status();
            ImGui::Text("Lighting capture: %s", captureStatus == 1 ? "Capturing" : captureStatus == 2 ? "Saving" : captureStatus == 3 ? "Saved to data diagnostics" : captureStatus == 4 ? "Could not save" : "Off");
            ImGui::Text("Graphics effects: %s", selected ? "Selected" : "Not installed");
            bool depthView = (state.Requested & (1u << 2)) != 0;
            ImGui::BeginDisabled(!selected);
            if (ImGui::Checkbox("Depth view", &depthView)) Global->Effects.SetRequested(runtime, 2, depthView);
            ImGui::EndDisabled();
            for (size_t i = 0; i < tq::EffectNames.size(); ++i)
                ImGui::Text("%s: %s", tq::EffectNames[i], tq::EffectStatus(state, i, selected, original));
        }
        ImGui::TreePop();
    }
}

void Start(HMODULE module, HMODULE reshadeModule) {
    auto* state = new State();
    state->Owner = module;
    if (reshadeModule)
        state->SetOverlayDemand = reinterpret_cast<State::OverlayDemandFn>(
            GetProcAddress(reshadeModule, "TextureQuestSetOverlayCallbackDemandV1"));
    std::string error;
    if (!tq::ReadConfig(ModuleFolder(module) + L"TextureQuest.ini", state->Config, error)) {
        Log(reshade::log::level::error, error + "; textures stay stock");
        delete state;
        return;
    }
    std::ifstream file(state->Config.Map, std::ios::binary);
    const std::vector<unsigned char> raw(std::istreambuf_iterator<char>(file), {});
    if (!state->Map.Load(raw) || !state->Map.Loose) {
        Log(reshade::log::level::error, "texture map is missing or invalid; textures stay stock");
        delete state;
        return;
    }
    state->Reader = std::make_unique<tq::DDSReader>(state->Config.Downloads, state->Map);
    state->Paks = std::make_unique<tq::StockPaks>(state->Config.GamePaks, state->Map);
    state->Shortcut = std::make_unique<tq::Shortcut>(state->Config.Shortcut, state->Original);
    Global = state;
    reshade::register_event<reshade::addon_event::create_resource>(&OnCreateResource);
    reshade::register_event<reshade::addon_event::init_resource>(&OnInitResource);
    reshade::register_event<reshade::addon_event::destroy_resource>(&OnDestroyResource);
    reshade::register_event<reshade::addon_event::present>(&OnPresent);
    reshade::register_event<reshade::addon_event::destroy_device>(&OnDestroyDevice);
    reshade::register_event<reshade::addon_event::init_effect_runtime>(&OnInitEffectRuntime);
    reshade::register_event<reshade::addon_event::destroy_effect_runtime>(&OnDestroyEffectRuntime);
    reshade::register_event<reshade::addon_event::reshade_reloaded_effects>(&OnReloadedEffects);
    reshade::register_event<reshade::addon_event::reshade_set_technique_state>(&OnTechniqueState);
    reshade::register_event<reshade::addon_event::reshade_begin_effects>(&OnBeginEffects);
    reshade::register_event<reshade::addon_event::reshade_present>(&OnReshadePresent);
    reshade::register_event<reshade::addon_event::reshade_overlay>(&OnFpsOverlay);
    SetShowFPS(state->ShowFPS.load()); // Safe default until the saved choice is read at runtime init.
    reshade::register_event<reshade::addon_event::reshade_overlay_technique>(&OnOverlayTechnique);
    reshade::register_overlay("TextureQuest", &OnTextureQuestOverlay);
    reshade::register_event<reshade::addon_event::init_pipeline>(&OnRendererPipeline);
    reshade::register_event<reshade::addon_event::destroy_pipeline>(&OnRendererPipelineDestroyed);
    reshade::register_event<reshade::addon_event::destroy_resource>(&OnRendererResourceDestroyed);
    reshade::register_event<reshade::addon_event::draw>(&OnRendererDraw);
    reshade::register_event<reshade::addon_event::draw_indexed>(&OnRendererDrawIndexed);
    reshade::register_event<reshade::addon_event::map_buffer_region>(&OnRendererMap);
    reshade::register_event<reshade::addon_event::unmap_buffer_region>(&OnRendererUnmap);
    reshade::register_event<reshade::addon_event::update_buffer_region>(&OnRendererUpdate);
    Global->Worker = std::thread(StockWorker);
    Global->Shortcut->Start();
    Log(reshade::log::level::info, "loaded " + std::to_string(state->Map.Rows.size()) + " textures from " + std::to_string(state->Map.Archives.size()) + " archives");
}

// Unregisters every event and joins both threads before freeing Global, so no
// add-on code runs once AddonUninit returns and a later AddonInit starts clean.
void Stop() {
    if (!Global)
        return;
    reshade::unregister_overlay("TextureQuest", &OnTextureQuestOverlay);
    reshade::unregister_event<reshade::addon_event::reshade_overlay_technique>(&OnOverlayTechnique);
    reshade::unregister_event<reshade::addon_event::create_resource>(&OnCreateResource);
    reshade::unregister_event<reshade::addon_event::init_resource>(&OnInitResource);
    reshade::unregister_event<reshade::addon_event::destroy_resource>(&OnDestroyResource);
    reshade::unregister_event<reshade::addon_event::present>(&OnPresent);
    reshade::unregister_event<reshade::addon_event::destroy_device>(&OnDestroyDevice);
    reshade::unregister_event<reshade::addon_event::init_effect_runtime>(&OnInitEffectRuntime);
    reshade::unregister_event<reshade::addon_event::destroy_effect_runtime>(&OnDestroyEffectRuntime);
    reshade::unregister_event<reshade::addon_event::reshade_reloaded_effects>(&OnReloadedEffects);
    reshade::unregister_event<reshade::addon_event::reshade_set_technique_state>(&OnTechniqueState);
    reshade::unregister_event<reshade::addon_event::reshade_begin_effects>(&OnBeginEffects);
    reshade::unregister_event<reshade::addon_event::reshade_present>(&OnReshadePresent);
    reshade::unregister_event<reshade::addon_event::reshade_overlay>(&OnFpsOverlay);
    reshade::unregister_event<reshade::addon_event::update_buffer_region>(&OnRendererUpdate);
    reshade::unregister_event<reshade::addon_event::unmap_buffer_region>(&OnRendererUnmap);
    reshade::unregister_event<reshade::addon_event::map_buffer_region>(&OnRendererMap);
    reshade::unregister_event<reshade::addon_event::draw_indexed>(&OnRendererDrawIndexed);
    reshade::unregister_event<reshade::addon_event::draw>(&OnRendererDraw);
    reshade::unregister_event<reshade::addon_event::destroy_resource>(&OnRendererResourceDestroyed);
    reshade::unregister_event<reshade::addon_event::destroy_pipeline>(&OnRendererPipelineDestroyed);
    reshade::unregister_event<reshade::addon_event::init_pipeline>(&OnRendererPipeline);
    Global->RendererProbe.Stop();
    Global->Effects.Clear();
    Global->DLSS.Clear();
    Global->AIO.Clear();
    Global->WorldGraphics.Clear();
    if (Global->PushRegistered)
        reshade::unregister_event<reshade::addon_event::push_descriptors>(&OnPushDescriptors);
    {
        std::lock_guard lock(Global->JobLock);
        Global->Stopping = true;
    }
    Global->JobWake.notify_all();
    Global->Worker.join();
    Global->Shortcut->Stop();
    Log(reshade::log::level::info, "session: " + std::to_string(Global->Swapped.load()) + " swapped, " + std::to_string(Global->Failed.load()) +
                                       " failed, " + std::to_string(Global->SwappedBytes.load() >> 20) + " MiB");
    delete Global;
    Global = nullptr;
}

}

// ReShade calls these outside the loader lock, right after LoadLibrary and
// right before FreeLibrary, once per load of the add-on set.
extern "C" __declspec(dllexport) bool AddonInit(HMODULE addon_module, HMODULE reshade_module) {
    if (!reshade::register_addon(addon_module, reshade_module))
        return false;
    // Ordinary enabled effect techniques are tracked by the provider itself.
    // This add-on never manually renders a GenericDepth effect technique.
    if (reshade_module) {
        const auto participate = reinterpret_cast<tq_depth_v1::Register>(
            GetProcAddress(reshade_module, tq_depth_v1::RegisterExport));
        if (participate)
            participate(addon_module, tq_depth_v1::NoManualDepth);
    }
    Start(addon_module, reshade_module);
    return true;
}

extern "C" __declspec(dllexport) void AddonUninit(HMODULE addon_module, HMODULE reshade_module) {
    Stop();
    if (reshade_module) {
        const auto leave = reinterpret_cast<tq_depth_v1::Unregister>(
            GetProcAddress(reshade_module, tq_depth_v1::UnregisterExport));
        if (leave)
            leave(addon_module);
    }
    reshade::unregister_addon(addon_module, reshade_module);
}
