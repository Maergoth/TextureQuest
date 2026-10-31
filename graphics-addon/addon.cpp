// TextureQuest add-on for stock ReShade: swaps matched stock textures for
// upscaled ones at creation and keeps an Original/Upscaled comparison shortcut.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <reshade.hpp>
#include "config.hpp"
#include "shortcut.hpp"
#include "stock_copies.hpp"
#include "stock_paks.hpp"
#include "dds_reader.hpp"
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
};

State* Global = nullptr;
thread_local Handoff Pending;

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

bool OnCreateResource(device*, resource_desc& desc, subresource_data* initial_data, resource_usage) {
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
void OnPresent(command_queue* queue, swapchain*, const rect*, const rect*, uint32_t, const rect*) {
    const bool original = Global->Original;
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

void Start(HMODULE module) {
    auto* state = new State();
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
    Global->Worker = std::thread(StockWorker);
    Global->Shortcut->Start();
    Log(reshade::log::level::info, "loaded " + std::to_string(state->Map.Rows.size()) + " textures from " + std::to_string(state->Map.Archives.size()) + " archives");
}

// Unregisters every event and joins both threads before freeing Global, so no
// add-on code runs once AddonUninit returns and a later AddonInit starts clean.
void Stop() {
    if (!Global)
        return;
    reshade::unregister_event<reshade::addon_event::create_resource>(&OnCreateResource);
    reshade::unregister_event<reshade::addon_event::init_resource>(&OnInitResource);
    reshade::unregister_event<reshade::addon_event::destroy_resource>(&OnDestroyResource);
    reshade::unregister_event<reshade::addon_event::present>(&OnPresent);
    reshade::unregister_event<reshade::addon_event::destroy_device>(&OnDestroyDevice);
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
    Start(addon_module);
    return true;
}

extern "C" __declspec(dllexport) void AddonUninit(HMODULE addon_module, HMODULE reshade_module) {
    Stop();
    reshade::unregister_addon(addon_module, reshade_module);
}
