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
#include "lighting_diagnostics.hpp"
#include "shadows.hpp"
#include "quality_presets.hpp"
#include <string_view>
#include <type_traits>

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
    // This hot callback is registered directly: reject inactive/non-sampler
    // events before ownership queries or diagnostic timing.
    if (TQSamplerBoundary || !tq_world::Managed.Enabled() || (!AnisotropyEnabled && !BilinearEnabled) || update.type != descriptor_type::sampler || !update.count
        || update.count > MAX_BOUND_SAMPLERS)
        return;
    if (!tq_world::Accept(cmd)) return;
    const shadowmaps::CpuScope scope(shadowmaps::BindCpu);

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

// Presets are generated from the installer's canonical JSON. Read actual state,
// rather than a saved preset name. Only performance edits change the tier;
// appearance controls are initialized separately for genuinely fresh profiles.
template<class T>
static bool TQPresetSetting(const char *name, T &value, bool apply)
{
    const std::string_view key(name);
    const auto live = [&](auto &setting) {
        using Value = decltype(setting.load());
        if (apply) setting.store(static_cast<Value>(value));
        else value = static_cast<T>(setting.load());
        return true;
    };
    if (key == "AnisotropicFiltering") return live(AnisotropyEnabled);
    if (key == "Anisotropy") {
        if (apply) AnisotropyLevel = ANISOTROPY_LEVELS - 1;
        else value = static_cast<T>(2 << AnisotropyLevel.load());
        return true;
    }
    if (key == "SmoothInterfaceText") return live(BilinearEnabled);
    if (key == "PointLightShadows") return live(shadows::Enabled);
    if (key == "CharacterShadows") return live(shadowmaps::MovingShadows);
    if (key == "ShadowStrength") return live(shadowmaps::Strength);
    if (key == "PointShadowSoftness") return live(shadowmaps::PointSoftness);
    if (key == "OwnLampShaders") return live(shadows::OwnLampShaders);
    if (key == "ShadowedLights") return live(shadowmaps::LightLimit);
    if (key == "ParticleShadowLimit") return live(shadowmaps::ParticleShadowLimit);
    if (key == "EmissiveLights") return live(glow::Enabled);
    if (key == "EmissiveParticles") return live(glow::ParticlesEnabled);
    if (key == "EmissiveObjects") return live(glow::ObjectsEnabled);
    if (key == "EmissiveLightStrength") return live(glow::Strength);
    if (key == "ParticleLightStrength") return live(particle_emission::Strength);
    if (key == "ParticleLightStrengthV2") return live(particle_emission::Strength);
    if (key == "ParticleLightStrengthV3") return live(particle_emission::Strength);
    if (key == "ParticleLightRadius") return live(particle_emission::Radius);
    if (key == "PaintedEmissionLight") return live(glow::PaintedLight);
    if (key == "PaintedEmissionKept") return live(glow::PaintedKept);
    if (key == "SmoothEmissiveLighting") return live(glow::SmoothLighting);
    if (key == "EmissiveLightLimit") return live(glow::LightLimit);
    if (key == "ParticleLightLimit") return live(glow::ParticleLightLimit);
    if (key == "AmbientOcclusion") return live(lightbuffer::OcclusionEnabled);
    if (key == "OcclusionRadius") return live(lightbuffer::OcclusionRadius);
    if (key == "OcclusionStrength") return live(lightbuffer::OcclusionStrength);
    if (key == "SurfaceSaturation") {
        if (apply) lightbuffer::SurfaceSaturation = static_cast<float>(value) / 100.0f;
        else value = static_cast<T>(lightbuffer::SurfaceSaturation.load() * 100.0f);
        return true;
    }
    if (key == "SunLighting") return live(lightbuffer::SunEnabled);
    if (key == "SunShadowSoftening") {
        const float active = lightbuffer::SunSoftness.load();
        if (!apply) { value = static_cast<T>(active > 0); return true; }
        const bool enabled = value != 0;
        const auto settings = sun_softness_settings::Load(
            [](const char *key, auto &current) { return reshade::get_config_value(nullptr, CONFIG_SECTION, key, current); },
            [](const char *key, auto current) { reshade::set_config_value(nullptr, CONFIG_SECTION, key, current); });
        const float preference = !enabled && active > 0 ? active : settings.Preference;
        const float selected = enabled ? (active > 0 ? active : preference) : 0.0f;
        lightbuffer::SunSoftness = selected;
        sun_softness_settings::Store(selected, preference,
            [](const char *key, auto current) { reshade::set_config_value(nullptr, CONFIG_SECTION, key, current); });
        return true;
    }
    if (key == "SunSoftness") return live(lightbuffer::SunSoftness);
    if (key == "SunShadowMode") return live(lightbuffer::SunShadowMode);
    if (key == "SunShadowDirectionFix") return live(lightbuffer::SunDirectionFix);
    if (key == "SmoothIndoorTransition") return live(lightbuffer::SmoothIndoorTransition);
    if (key == "AmbientKept") return live(lightbuffer::AmbientKept);
    if (key == "TextureQuestSunSoftnessPreferenceRealSun" || key == "TextureQuestSunSoftnessUnits"
        || key == "ParticleLightStrengthUnits") {
        if (!apply) reshade::get_config_value(nullptr, CONFIG_SECTION, name, value);
        return true;
    }
    return false;
}

extern "C" __declspec(dllexport) int32_t TextureQuestWorldGraphicsQualityPreset()
{
    for (unsigned index = 0; index < tq_quality::Count; ++index) {
        bool matches = true;
        tq_quality::VisitSettings(index, [&](const char *name, auto expected) {
            auto actual = expected;
            if (!TQPresetSetting(name, actual, false)) { matches = false; return; }
            if constexpr (std::is_floating_point_v<decltype(expected)>)
                matches = matches && std::isfinite(actual) && std::abs(actual - expected) <= 0.0001f;
            else matches = matches && actual == expected;
        });
        if (matches) return static_cast<int32_t>(index);
    }
    return -1;
}

extern "C" __declspec(dllexport) uint32_t TextureQuestApplyWorldGraphicsQualityPreset(uint32_t index)
{
    // Validate every key before changing anything; future schema extensions must
    // not partly apply a preset to an older renderer.
    bool known = true;
    if (!tq_quality::VisitSettings(index, [&](const char *name, auto value) {
        known = TQPresetSetting(name, value, false) && known;
    }) || !known) return 0;
    tq_quality::VisitSettings(index, [&](const char *name, auto value) {
        TQPresetSetting(name, value, true);
        // The logical toggle stores the existing native softness/preferences;
        // never persist an independent boolean that can become stale.
        if (std::string_view(name) != "SunShadowSoftening")
            reshade::set_config_value(nullptr, CONFIG_SECTION, name, value);
    });
    reshade::set_config_value(nullptr, CONFIG_SECTION, "TextureQuestQualityPreset", tq_quality::Names[index]);
    return 1;
}

static void TQApplyFreshWorldDefaults()
{
    tq_quality::VisitFreshDefaults([](const char *name, auto value) {
        TQPresetSetting(name, value, true);
        reshade::set_config_value(nullptr, CONFIG_SECTION, name, value);
    });
    shadowmaps::LegacyPointSoftness = false;
}

static bool TQHasExistingWorldSettings()
{
    const auto present = [](const char *key) {
        size_t size = 0;
        // Probe presence, not parse success: even a malformed saved choice is
        // existing user state and must not trigger an automatic full preset.
        return reshade::get_config_value(nullptr, CONFIG_SECTION, key, nullptr, &size);
    };
    bool existing = false;
    tq_quality::VisitSettings(0, [&](const char *key, auto) { existing = present(key) || existing; });
    tq_quality::VisitFreshDefaults([&](const char *key, auto) { existing = present(key) || existing; });
    constexpr const char *legacy[] = {
        "GlowLights", "GlowLightStrength", "PaintedGlowKept", "ShadowSoftness", "SunShadowSize", "ParticleLightStrength", "ParticleLightStrengthV2",
        "TextureQuestSunSoftnessPreference", "TextureQuestPresetVersion", "TextureQuestQualityPreset",
        "TextureQuestPreset2PreviousShadowSoftness", "TextureQuestPreset1PreviousShadowStrength",
        "TextureQuestPreset1PreviousShadowSoftness", "TextureQuestPreset1PreviousOcclusionRadius",
        "TextureQuestPreset1PreviousOcclusionStrength", "TextureQuestPreset1PreviousShadowedLights",
        "TextureQuestPreset1PreviousAnisotropy"
    };
    for (const char *key : legacy) existing = present(key) || existing;
    return existing;
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

    shadows::DrawAdvancedSettings();
}

static void DrawDebugSettings(effect_runtime *)
{
    shadows::DrawDebugSettings();
    ImGui::Separator();
    lighting_diagnostics::DrawSettings();
    ImGui::Separator();
    capture::DrawSettings();
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
    if (lighting_diagnostics::NeedsBoundary()) {
        const auto performanceSettings = lighting_diagnostics::RecordRequested.load() || lighting_diagnostics::Recording
            ? TQLightingSettingsIdentity() : lighting_diagnostics::SettingsId;
        lighting_diagnostics::Boundary(reinterpret_cast<ID3D11DeviceContext*>(queue->get_native()), performanceSettings);
    }
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
static void TQWorldDeviceReleased(device*dev){lighting_diagnostics::Release();tq_world::Work.Clear();tq_world::Managed.Release(dev->get_native());}
extern "C" __declspec(dllexport) uint32_t TextureQuestWorldGraphicsABI()noexcept{return 1;}
extern "C" __declspec(dllexport) void TextureQuestSetWorldGraphicsEnabled(uint32_t enabled)noexcept{tq_world::Managed.Request(enabled);}
extern "C" __declspec(dllexport) void TextureQuestDrawWorldGraphicsAdvanced(effect_runtime*runtime)
{if(tq_world::Accept(runtime))DrawSettings(runtime);}
extern "C" __declspec(dllexport) void TextureQuestDrawWorldGraphicsDebug(effect_runtime*runtime)
{if(tq_world::Accept(runtime))DrawDebugSettings(runtime);}
// Older controllers already provide an Advanced wrapper around this fallback.
extern "C" __declspec(dllexport) void TextureQuestDrawWorldGraphics(effect_runtime*runtime)
{
    if(!tq_world::Accept(runtime))return;
    DrawSettings(runtime);
    if(ImGui::TreeNode("Debug")){DrawDebugSettings(runtime);ImGui::TreePop();}
}

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
        const bool freshWorldSettings = !TQHasExistingWorldSettings();
        LoadSettings();
        reshade::register_event<reshade::addon_event::init_sampler>(&tq_world::Guard<&OnInitSampler>::Call);
        reshade::register_event<reshade::addon_event::destroy_sampler>(&tq_world::Guard<&OnDestroySampler>::Call);
        reshade::register_event<reshade::addon_event::destroy_device>(&tq_world::Guard<&OnDestroyDevice>::Call);
        reshade::register_event<reshade::addon_event::push_descriptors>(&OnPushDescriptors);
        capture::Register();
        shadows::Register();
        if (freshWorldSettings) {
            TQApplyFreshWorldDefaults();
            TextureQuestApplyWorldGraphicsQualityPreset(0);
        }
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
