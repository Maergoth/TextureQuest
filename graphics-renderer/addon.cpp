// EverQuest II graphics improvements.
#define ImTextureID ImU64
#include <imgui.h>
#include <reshade.hpp>
#include <atomic>
#include <cstdio>
#include <mutex>
#include <unordered_map>
#include "texturequest_world_guard.hpp"
#include "depth_demand_v1.hpp"
#include "capture.hpp"
#include "lighting_experiment.hpp"
#include "shadows.hpp"

using namespace reshade::api;

extern "C" __declspec(dllexport) const char *NAME = "EQ2 Graphics";
extern "C" __declspec(dllexport) const char *DESCRIPTION = "Graphics improvements for EverQuest II.";

using shadows::CONFIG_SECTION;
constexpr int ANISOTROPY_LEVELS = 4;
constexpr uint32_t MAX_BOUND_SAMPLERS = 16;

// a sampler the game created that is drawn with a copy: a trilinear one gets an anisotropic copy per level (2x, 4x,
// 8x, 16x), and a point-filtered one a bilinear copy, so the game's scaled interface text is smoothed instead of
// pixel-doubled; each copy is created on first use
struct ReplacedSampler
{
    sampler_desc Desc;
    bool Point = false;
    D3D11_SAMPLER_DESC Original{};
    sampler Bilinear = {};
    sampler Copies[ANISOTROPY_LEVELS] = {};
};

// A copied descriptor, never a borrowed original pointer. It survives even
// when EQ2 releases the original sampler while our substitute remains bound.
constexpr GUID TQSamplerTag={0xa203924e,0x85dd,0x4b93,{0x91,0x7b,0xdb,0x28,0x7a,0x18,0x93,0x0c}};
struct TQSamplerOrigin {uint32_t Version=1,Substituted=1;sampler_desc Source{};D3D11_SAMPLER_DESC Native{};};
static thread_local bool TQSamplerBoundary=false;
static bool TQTagSampler(sampler copy,const ReplacedSampler&entry) {
    const TQSamplerOrigin tag{1,1,entry.Desc,entry.Original};
    return SUCCEEDED(reinterpret_cast<ID3D11SamplerState*>(copy.handle)->SetPrivateData(TQSamplerTag,sizeof(tag),&tag));
}
static bool TQReadSampler(ID3D11SamplerState*value,TQSamplerOrigin&tag) {
    UINT bytes=sizeof(tag);
    return value&&SUCCEEDED(value->GetPrivateData(TQSamplerTag,&bytes,&tag))&&bytes==sizeof(tag)&&tag.Version==1&&tag.Substituted<=1;
}
static std::mutex Lock;
static std::unordered_map<uint64_t, ReplacedSampler> Samplers;
static std::atomic<bool> AnisotropyEnabled = true;
static std::atomic<int> AnisotropyLevel = ANISOTROPY_LEVELS - 1;
static std::atomic<bool> BilinearEnabled = false;

// the point-filtered sampler's filter with bilinear minification and magnification
static filter_mode FindBilinearFilter(filter_mode filter)
{
    constexpr uint32_t LINEAR_MIN_MAG = 0x14;
    return static_cast<filter_mode>(static_cast<uint32_t>(filter) | LINEAR_MIN_MAG);
}

static void OnInitSampler(device *, const sampler_desc &desc, sampler handle)
{
    if(TQSamplerBoundary)return;
    constexpr uint32_t LINEAR_MIN_MAG = 0x14;
    // comparison samplers read shadow maps and are left alone
    constexpr uint32_t COMPARISON = 0x80;
    const uint32_t filter = static_cast<uint32_t>(desc.filter);
    const bool point = (filter & COMPARISON) == 0 && (filter & LINEAR_MIN_MAG) != LINEAR_MIN_MAG;
    if (desc.filter != filter_mode::min_mag_mip_linear && !point)
        return;

    std::lock_guard guard(Lock);
    auto&entry=Samplers[handle.handle];entry={desc,point};
    reinterpret_cast<ID3D11SamplerState*>(handle.handle)->GetDesc(&entry.Original);
}

static void DestroyCopies(device *dev, ReplacedSampler &entry)
{
    if (entry.Bilinear.handle)
        dev->destroy_sampler(entry.Bilinear);
    for (sampler copy : entry.Copies)
        if (copy.handle)
            dev->destroy_sampler(copy);
}

static void OnDestroySampler(device *dev, sampler handle)
{
    std::lock_guard guard(Lock);
    auto it = Samplers.find(handle.handle);
    if (it == Samplers.end())
        return;

    DestroyCopies(dev, it->second);
    Samplers.erase(it);
}

static void OnDestroyDevice(device *dev)
{
    std::lock_guard guard(Lock);
    for (auto &[handle, entry] : Samplers)
        DestroyCopies(dev, entry);
    Samplers.clear();
}

// the copy to bind in place of a replaced sampler under the current settings, created if needed, or an empty handle
// to keep the game's own
static sampler FindCopy(device *dev, ReplacedSampler &entry)
{
    sampler_desc desc = entry.Desc;
    if (entry.Point)
    {
        if (!BilinearEnabled)
            return {};
        desc.filter = FindBilinearFilter(desc.filter);
    }
    // a point sampler made trilinear by the bilinear copy is filtered anisotropically too
    if (AnisotropyEnabled && desc.filter == filter_mode::min_mag_mip_linear)
    {
        const int level = AnisotropyLevel;
        sampler &copy = entry.Copies[level];
        if (!copy.handle)
        {
            desc.filter = filter_mode::anisotropic;
            desc.max_anisotropy = static_cast<float>(2 << level);
            if (!dev->create_sampler(desc, &copy))
                return {};
            if(!TQTagSampler(copy,entry)){dev->destroy_sampler(copy);copy={};return{};}
        }
        return copy;
    }
    if (!entry.Point)
        return {};

    if (!entry.Bilinear.handle) {
        if(!dev->create_sampler(desc,&entry.Bilinear))return{};
        if(!TQTagSampler(entry.Bilinear,entry)){dev->destroy_sampler(entry.Bilinear);entry.Bilinear={};return{};}
    }
    return entry.Bilinear;
}

// binds the copy of each replaced sampler the game binds, so a settings change applies on the next draw
static void OnPushDescriptors(command_list *cmd, shader_stage stages, pipeline_layout layout, uint32_t param, const descriptor_table_update &update)
{
    const shadowmaps::CpuScope scope(shadowmaps::BindCpu);
    if (TQSamplerBoundary || !tq_world::Managed.Enabled() || (!AnisotropyEnabled && !BilinearEnabled) || update.type != descriptor_type::sampler || !update.count
        || update.count > MAX_BOUND_SAMPLERS)
        return;

    const auto *bound = static_cast<const sampler *>(update.descriptors);
    sampler replaced[MAX_BOUND_SAMPLERS];
    bool changed = false;
    {
        std::lock_guard guard(Lock);
        for (uint32_t i = 0; i < update.count; ++i)
        {
            replaced[i] = bound[i];
            auto it = Samplers.find(bound[i].handle);
            if (it == Samplers.end())
                continue;

            const sampler copy = FindCopy(cmd->get_device(), it->second);
            if (!copy.handle)
                continue;

            replaced[i] = copy;
            changed = true;
        }
    }
    if (!changed)
        return;

    descriptor_table_update replacement = update;
    replacement.descriptors = replaced;
    cmd->push_descriptors(stages, layout, param, replacement);
}

static void DrawSettings(effect_runtime *)
{
    bool enabled = AnisotropyEnabled;
    if (ImGui::Checkbox("Anisotropic filtering (16x)", &enabled))
    {
        AnisotropyEnabled = enabled;
        AnisotropyLevel = ANISOTROPY_LEVELS - 1;
        reshade::set_config_value(nullptr, CONFIG_SECTION, "AnisotropicFiltering", enabled);
        reshade::set_config_value(nullptr, CONFIG_SECTION, "Anisotropy", 16);
    }

    bool bilinear = BilinearEnabled;
    if (ImGui::Checkbox("Smooth interface text", &bilinear))
    {
        BilinearEnabled = bilinear;
        reshade::set_config_value(nullptr, CONFIG_SECTION, "SmoothInterfaceText", bilinear);
    }

    ImGui::Separator();
    shadows::DrawSettings();

    if (ImGui::CollapsingHeader("Advanced"))
    {
        lighting_experiment::DrawSettings();
        shadows::DrawAdvancedSettings();
        if (ImGui::TreeNode("Debug"))
        {
            shadows::DrawDebugSettings();
            ImGui::Separator();
            capture::DrawSettings();
            ImGui::TreePop();
        }
    }
}

static void LoadSettings()
{
    bool enabled = true;
    reshade::get_config_value(nullptr, CONFIG_SECTION, "AnisotropicFiltering", enabled);
    AnisotropyEnabled = enabled;

    AnisotropyLevel = ANISOTROPY_LEVELS - 1;

    bool bilinear = false;
    reshade::get_config_value(nullptr, CONFIG_SECTION, "SmoothInterfaceText", bilinear);
    BilinearEnabled = bilinear;
}


// All native GUI/lighting callbacks for this frame have finished before this
// registered final Present callback. Already submitted D11 work stays ordered;
// resources remain owned, and the next frame observes the new effective gate.
template<class Get,class Set>
static bool TQApplySamplerStage(device*api,ID3D11Device*dev,ID3D11DeviceContext*ctx,Get get,Set set,bool enabled) {
    ID3D11SamplerState*raw[D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT]={};
    (ctx->*get)(0,D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT,raw);
    bool ok=true;
    for(UINT i=0;i<D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT;++i) {
        Microsoft::WRL::ComPtr<ID3D11SamplerState>current;current.Attach(raw[i]);if(!current)continue;
        TQSamplerOrigin tag{};const bool tagged=TQReadSampler(current.Get(),tag);
        if(!enabled) {
            if(!tagged||!tag.Substituted)continue;
            Microsoft::WRL::ComPtr<ID3D11SamplerState>stock;
            if(FAILED(dev->CreateSamplerState(&tag.Native,&stock))){ok=false;continue;}
            tag.Substituted=0;
            if(FAILED(stock->SetPrivateData(TQSamplerTag,sizeof(tag),&tag))){ok=false;continue;}
            ID3D11SamplerState*bound=stock.Get();(ctx->*set)(i,1,&bound);
        } else {
            if(tagged&&tag.Substituted)continue;
            sampler copy{};
            {
                std::lock_guard guard(Lock);const uint64_t key=reinterpret_cast<uintptr_t>(current.Get());
                auto entry=Samplers.find(key);
                if(entry==Samplers.end()&&tagged) {
                    const auto f=uint32_t(tag.Source.filter);
                    const bool point=(f&0x80)==0&&(f&0x14)!=0x14;
                    entry=Samplers.emplace(key,ReplacedSampler{tag.Source,point,tag.Native}).first;
                }
                if(entry!=Samplers.end())copy=FindCopy(api,entry->second);
            }
            if(copy.handle){auto*bound=reinterpret_cast<ID3D11SamplerState*>(copy.handle);(ctx->*set)(i,1,&bound);}
        }
    }
    return ok;
}
static uint64_t TQLightingSettingsIdentity()
{
    uint64_t hash = 0xcbf29ce484222325ull;
    const auto add = [&](uint64_t value) { hash = (hash ^ value) * 0x100000001b3ull; };
    const auto real = [&](float value) { add(std::bit_cast<uint32_t>(value)); };
    add(tq_world::Managed.Requested()); add(shadows::Enabled.load());
    add(AnisotropyEnabled.load()); add(BilinearEnabled.load()); add(shadows::OwnLampShaders.load());
    add(shadowmaps::LightLimit.load()); add(shadowmaps::ParticleShadowLimit.load());
    add(shadowmaps::MovingShadows.load()); add(shadowmaps::LegacyPointSoftness.load());
    add(shadowmaps::ShowColours.load()); real(shadowmaps::Strength.load()); real(shadowmaps::PointSoftness.load());
    add(glow::Enabled.load()); add(glow::ObjectsEnabled.load()); add(glow::ParticlesEnabled.load());
    add(glow::LightLimit.load()); add(glow::ParticleLightLimit.load()); add(glow::SmoothLighting.load());
    real(glow::Strength.load()); real(glow::PaintedKept.load()); real(glow::PaintedLight.load());
    real(particle_emission::Strength.load()); real(particle_emission::Radius.load());
    add(lightbuffer::SunEnabled.load()); add(static_cast<unsigned>(lightbuffer::SunShadowMode.load()));
    add(lightbuffer::SunDirectionFix.load()); add(lightbuffer::SmoothIndoorTransition.load());
    real(lightbuffer::SunSoftness.load()); real(lightbuffer::AmbientKept.load());
    add(lightbuffer::OcclusionEnabled.load()); real(lightbuffer::OcclusionRadius.load());
    real(lightbuffer::OcclusionStrength.load()); real(lightbuffer::SurfaceSaturation.load());
    add(lightbuffer::ShowOcclusion.load());
    add(shadowmaps::ScreenWidth); add(shadowmaps::ScreenHeight);
    return hash;
}
static void TQWorldBoundary(command_queue*queue,swapchain*,const rect*,const rect*,uint32_t,const rect*) {
    const auto comparisonSettings = lighting_experiment::RecordRequested.load() || lighting_experiment::Recording
        || lighting_experiment::Enabled.load() || lighting_experiment::Requested.load()
        ? TQLightingSettingsIdentity() : lighting_experiment::SettingsId;
    lighting_experiment::Boundary(reinterpret_cast<ID3D11DeviceContext*>(queue->get_native()), comparisonSettings);
    shadowmaps::transitions::SyncSettings(comparisonSettings);
    const bool wanted=tq_world::Managed.Requested(),was=tq_world::Managed.Enabled();
    static bool samplerRetry=false;
    static std::uint32_t lastFiltering=0;
    const std::uint32_t filtering=(wanted&&AnisotropyEnabled?1u:0u)|(wanted&&BilinearEnabled?2u:0u);
    const auto work=tq_world::WorldMask(shadows::SelectedWorldFeatures(),wanted,capture::State==capture::Phase::Capturing);
    if(wanted==was&&!samplerRetry&&filtering==lastFiltering&&work==tq_world::Work.Mask())return;
    auto*ctx=reinterpret_cast<ID3D11DeviceContext*>(queue->get_native());
    if(!ctx||ctx->GetType()!=D3D11_DEVICE_CONTEXT_IMMEDIATE)return;
    Microsoft::WRL::ComPtr<ID3D11Device>dev;ctx->GetDevice(&dev);
    // The next-frame render gate changes here only. Requested upstream choices
    // are never overwritten or persisted by this master control.
    tq_world::Managed.Commit(wanted);
    const bool startTracking=!tq_world::Work.Tracking()&&work!=0;
    shadows::CommitWorldWork(work);
    if(startTracking)capture::RefreshBindings(ctx);
    if(wanted==was&&!samplerRetry&&filtering==lastFiltering)return;
    const bool restore=filtering!=lastFiltering||!wanted||samplerRetry;
    lastFiltering=filtering;
    TQSamplerBoundary=true;
    bool ok=true;
    const auto apply=[&](bool enabled){
        ok=TQApplySamplerStage(queue->get_device(),dev.Get(),ctx,&ID3D11DeviceContext::VSGetSamplers,&ID3D11DeviceContext::VSSetSamplers,enabled)&&ok;
        ok=TQApplySamplerStage(queue->get_device(),dev.Get(),ctx,&ID3D11DeviceContext::PSGetSamplers,&ID3D11DeviceContext::PSSetSamplers,enabled)&&ok;
        ok=TQApplySamplerStage(queue->get_device(),dev.Get(),ctx,&ID3D11DeviceContext::GSGetSamplers,&ID3D11DeviceContext::GSSetSamplers,enabled)&&ok;
        ok=TQApplySamplerStage(queue->get_device(),dev.Get(),ctx,&ID3D11DeviceContext::HSGetSamplers,&ID3D11DeviceContext::HSSetSamplers,enabled)&&ok;
        ok=TQApplySamplerStage(queue->get_device(),dev.Get(),ctx,&ID3D11DeviceContext::DSGetSamplers,&ID3D11DeviceContext::DSSetSamplers,enabled)&&ok;
        ok=TQApplySamplerStage(queue->get_device(),dev.Get(),ctx,&ID3D11DeviceContext::CSGetSamplers,&ID3D11DeviceContext::CSSetSamplers,enabled)&&ok;
    };
    if(restore)apply(false);
    if(wanted)apply(true);
    TQSamplerBoundary=false;
    samplerRetry=!ok;
    if(wanted&&!was)shadowmaps::RebuildAll();
    if(!ok)reshade::log::message(reshade::log::level::error,"TextureQuest world sampler transition could not restore every original descriptor");
}
static void TQWorldDeviceReleased(device*dev){lighting_experiment::Release();tq_world::Work.Clear();tq_world::Managed.Release(dev->get_native());}
extern "C" __declspec(dllexport) uint32_t TextureQuestWorldGraphicsABI()noexcept{return 1;}
extern "C" __declspec(dllexport) void TextureQuestSetWorldGraphicsEnabled(uint32_t enabled)noexcept{tq_world::Managed.Request(enabled);}
extern "C" __declspec(dllexport) void TextureQuestDrawWorldGraphics(effect_runtime*runtime){if(tq_world::Accept(runtime))DrawSettings(runtime);}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        if (!reshade::register_addon(module))
            return FALSE;
        // Native world effects read their own DSVs, not GenericDepth. Declare
        // that fact to the existing private provider; older providers are fine.
        if(auto provider=reshade::internal::get_reshade_module_handle())
            if(auto join=reinterpret_cast<tq_depth_v1::Register>(GetProcAddress(provider,tq_depth_v1::RegisterExport)))
                join(module,tq_depth_v1::NoManualDepth);
        LoadSettings();
        reshade::register_event<reshade::addon_event::init_sampler>(&tq_world::Guard<&OnInitSampler>::Call);
        reshade::register_event<reshade::addon_event::destroy_sampler>(&tq_world::Guard<&OnDestroySampler>::Call);
        reshade::register_event<reshade::addon_event::destroy_device>(&tq_world::Guard<&OnDestroyDevice>::Call);
        reshade::register_event<reshade::addon_event::push_descriptors>(&tq_world::Guard<&OnPushDescriptors>::Call);
        capture::Register();
        shadows::Register();
        // Register after upstream frame cleanup; same pinned provider dispatch
        // order also releases the owner after its module/device cleanup.
        reshade::register_event<reshade::addon_event::present>(&tq_world::Guard<&TQWorldBoundary>::Call);
        reshade::register_event<reshade::addon_event::destroy_device>(&tq_world::Guard<&TQWorldDeviceReleased>::Call);
    }
    else if (reason == DLL_PROCESS_DETACH)
    {
        if(auto provider=reshade::internal::get_reshade_module_handle())
            if(auto leave=reinterpret_cast<tq_depth_v1::Unregister>(GetProcAddress(provider,tq_depth_v1::UnregisterExport)))
                leave(module);
        reshade::unregister_addon(module);
    }
    return TRUE;
}
