#pragma once
// Included inside shadowmaps after its resource/setting helpers. No map history is
// allocated or sampled while the existing lighting experiment is disabled.
namespace transitions
{
namespace policy = shadow_transition_policy;
constexpr UINT TierCount = 3;
constexpr UINT HistorySizes[TierCount] = { 1024, 512, 256 };
constexpr UINT HistoryCapacities[TierCount] = { 2, 4, 8 };
// At most 84 MiB of D32 history, lazily allocated per matching-resolution bank.
static_assert((2u * 1024u * 1024u + 4u * 512u * 512u + 8u * 256u * 256u) * 6u * 4u == 84u * 1024u * 1024u);
struct Endpoint
{
    XMFLOAT3 Position{};
    float Radius = 0;
    UINT Tier = 0, Cube = 0, FaceSize = 0;
    float NewWeight = 1;
    UINT MovingTier = 0, MovingCube = 0, MovingFaceSize = 0;
};
struct Bank
{
    ComPtr<ID3D11Texture2D> Texture;
    ComPtr<ID3D11ShaderResourceView> View;
    bool Failed = false;
};
struct Committed
{
    uint64_t Owner = 0, Epoch = 0, BuiltFrame = 0;
    policy::Source Source{};
    bool Valid = false;
};
struct MovingSnapshot
{
    uint64_t Owner = 0, Epoch = 0, Frame = 0;
    policy::Source Source{};
    ComPtr<ID3D11Texture2D> Texture;
    UINT Tier = 0, Cube = 0;
    bool Valid = false;
};
struct History
{
    policy::State State;
    policy::Source Source{};
    UINT Tier = 0, Cube = 0, MovingTier = 0, MovingCube = 0;
    bool HasMoving = false;
};
struct Counters
{
    uint64_t Captured = 0, Coalesced = 0, Expired = 0, Capacity = 0, Ineligible = 0, MissingMoving = 0,
        ResourceFailures = 0, Reassigned = 0, SettingsResets = 0;
};
struct ShadowSettings
{
    float Strength = 0, Softness = 0;
    int Limit = 0, ParticleLimit = 0;
    bool Moving = false;
    bool operator==(const ShadowSettings &) const = default;
};
static std::array<Bank, TierCount> Banks;
static std::array<Committed, MAX_LIGHTS> Maps;
static std::array<MovingSnapshot, MAX_LIGHTS> Moving;
static std::array<History, policy::MaximumTransitions> Histories;
static Counters Stats;
static bool Active = false, SettingsKnown = false, ExternalSettingsKnown = false;
static uint64_t Epoch = 0, ExternalSettings = 0;
static ShadowSettings Settings{};
static double Now()
{
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
static void Invalidate()
{
    Maps = {}; Moving = {}; Histories = {};
}
static void Reset()
{
    Invalidate(); Banks = {}; Stats = {};
    Active = false; Epoch = 0; SettingsKnown = false; ExternalSettingsKnown = false; ExternalSettings = 0;
}
static void Prune(double now)
{
    for (auto &history : Histories)
        if (history.State.Active && !policy::Live(history.State, history.State.Owner, history.State.Slot, Epoch, now))
        { history = {}; ++Stats.Expired; }
}
static bool Sync()
{
    const bool enabled = lighting_experiment::Enabled.load();
    const uint64_t epoch = lighting_experiment::CurrentEpoch();
    if (!enabled)
    {
        if (Active) Reset();
        return false;
    }
    if (!Active || Epoch != epoch)
    {
        Reset(); Active = true; Epoch = epoch;
    }
    const ShadowSettings current{ Strength.load(), PointSoftness.load(), LightLimit.load(), ParticleShadowLimit.load(), MovingShadows.load() };
    if (SettingsKnown && !(current == Settings)) { Invalidate(); ++Stats.SettingsResets; }
    Settings = current; SettingsKnown = true;
    Prune(Now());
    return true;
}
// The caller supplies its complete lighting-settings identity before packing lights.
static void SyncSettings(uint64_t identity)
{
    if (!Sync()) return;
    if (ExternalSettingsKnown && identity != ExternalSettings) { Invalidate(); ++Stats.SettingsResets; }
    ExternalSettingsKnown = true; ExternalSettings = identity;
}
static ID3D11ShaderResourceView *View(UINT tier)
{
    return Active && tier < TierCount ? Banks[tier].View.Get() : nullptr;
}
static policy::Source Describe(const Light &light, bool built)
{
    const auto &position = built ? light.BuiltPosition : light.Position;
    policy::Source source;
    source.Position = { position.x, position.y, position.z };
    source.Normal = { light.GlowNormal.x, light.GlowNormal.y, light.GlowNormal.z };
    source.Radius = light.Radius;
    source.Core = built ? light.BuiltParticleSourceRadius : light.Particle ? particle_source_shadow::Clean(light.ParticleSourceRadius) : 0.f;
    source.Range = light.GlowRange; source.Area = light.GlowArea; source.Emitter = light.EmitterKey; source.Particle = light.Particle;
    return source;
}
static bool Owner(uint64_t key, const Light &light)
{
    if (!key || light.Slot < 0 || light.Slot >= MAX_LIGHTS || !light.Glow || !light.BuiltFrame
        || SlotLights[light.Slot] != key) return false;
    const auto found = Lights.find(key);
    return found != Lights.end() && &found->second == &light;
}
static void ForgetSlot(int slot)
{
    if (!Active || slot < 0 || slot >= MAX_LIGHTS) return;
    for (auto &history : Histories) if (history.State.Active && history.State.Slot == slot)
    { history = {}; ++Stats.Reassigned; }
    Maps[slot] = {}; Moving[slot] = {};
}
static UINT TierForSize(UINT size)
{
    return size == 1024 ? 0u : size == 512 ? 1u : 2u;
}
static bool EnsureBank(ID3D11Device *dev, UINT tier)
{
    auto &bank = Banks[tier];
    if (bank.View) return true;
    if (bank.Failed) return false;
    Bank candidate;
    D3D11_TEXTURE2D_DESC desc{ HistorySizes[tier], HistorySizes[tier], 1, HistoryCapacities[tier] * 6,
        DXGI_FORMAT_R32_TYPELESS, {1,0}, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE, 0, D3D11_RESOURCE_MISC_TEXTURECUBE };
    D3D11_SHADER_RESOURCE_VIEW_DESC view{};
    view.Format = DXGI_FORMAT_R32_FLOAT; view.ViewDimension = D3D11_SRV_DIMENSION_TEXTURECUBEARRAY;
    view.TextureCubeArray = { 0, 1, 0, HistoryCapacities[tier] };
    if (FAILED(dev->CreateTexture2D(&desc, nullptr, &candidate.Texture))
        || FAILED(dev->CreateShaderResourceView(candidate.Texture.Get(), &view, &candidate.View)))
    { bank.Failed = true; ++Stats.ResourceFailures; return false; }
    bank = std::move(candidate); return true;
}
static bool ValidCopy(ID3D11Texture2D *source, UINT cube, UINT tier)
{
    if (!source || tier >= TierCount) return false;
    D3D11_TEXTURE2D_DESC desc{}; source->GetDesc(&desc);
    return desc.Width == HistorySizes[tier] && desc.Height == HistorySizes[tier] && desc.MipLevels == 1
        && desc.ArraySize >= (cube + 1) * 6 && desc.SampleDesc.Count == 1 && desc.Format == DXGI_FORMAT_R32_TYPELESS;
}
static bool Reserve(UINT tier, bool moving, UINT movingTier, UINT &cube, UINT &movingCube)
{
    std::array<unsigned, TierCount> used{};
    for (const auto &history : Histories) if (history.State.Active)
    {
        used[history.Tier] |= 1u << history.Cube;
        if (history.HasMoving) used[history.MovingTier] |= 1u << history.MovingCube;
    }
    const auto take = [&](UINT bank, UINT &place) {
        for (UINT i = 0; i < HistoryCapacities[bank]; ++i) if (!(used[bank] & (1u << i)))
        { place = i; used[bank] |= 1u << i; return true; }
        return false;
    };
    return take(tier, cube) && (!moving || take(movingTier, movingCube));
}
static void CopyCube(ID3D11DeviceContext *ctx, ID3D11Texture2D *source, UINT sourceCube, UINT tier, UINT cube)
{
    for (UINT face = 0; face < 6; ++face)
        ctx->CopySubresourceRegion(Banks[tier].Texture.Get(), cube * 6 + face, 0, 0, 0, source, sourceCube * 6 + face, nullptr);
}
static void BeforeDraw(ID3D11DeviceContext1 *ctx, Light &light)
{
    if (!Sync() || light.Slot < 0 || light.Slot >= MAX_LIGHTS) return;
    const int slot = light.Slot; const uint64_t key = SlotLights[slot];
    if (!Owner(key, light) || light.SlotChanged) return;
    const double now = Now();
    const auto next = Describe(light, false);
    for (auto &history : Histories) if (history.State.Active && history.State.Owner == key && history.State.Slot == slot)
    {
        if (!policy::BoundedMove(history.Source, next)) { history = {}; ++Stats.Ineligible; }
        else { ++Stats.Coalesced; return; }
    }
    const auto &map = Maps[slot]; const auto &moving = Moving[slot];
    if (!map.Valid || map.Owner != key || map.Epoch != Epoch || map.BuiltFrame != light.BuiltFrame
        || !policy::SamePose(map.Source, Describe(light, true)) || !policy::BoundedMove(map.Source, next))
    { ++Stats.Ineligible; return; }
    // This is the exact map selected by the last completed draw, including a
    // positively known absence. Unknown or cross-pose dynamic data fails closed.
    if (!moving.Valid || moving.Owner != key || moving.Epoch != Epoch || moving.Frame + 1 != FrameNumber
        || !policy::SamePose(map.Source, moving.Source))
    { ++Stats.MissingMoving; return; }
    auto entry = std::find_if(Histories.begin(), Histories.end(), [](const History &history) { return !history.State.Active; });
    const UINT sizeIndex = static_cast<UINT>(GetSizeIndex(slot));
    const UINT tier = TierForSize(FACE_SIZES[sizeIndex]);
    const UINT sourceCube = static_cast<UINT>(slot - SIZE_STARTS[sizeIndex]);
    const bool hasMoving = moving.Texture != nullptr;
    UINT cube = 0, movingCube = 0;
    if (entry == Histories.end() || !Reserve(tier, hasMoving, moving.Tier, cube, movingCube))
    { ++Stats.Capacity; return; }
    if (!ValidCopy(CubeTextures[sizeIndex].Get(), sourceCube, tier)
        || (hasMoving && !ValidCopy(moving.Texture.Get(), moving.Cube, moving.Tier)))
    { ++Stats.Ineligible; return; }
    ComPtr<ID3D11Device> dev; ctx->GetDevice(&dev);
    if (!EnsureBank(dev.Get(), tier) || (hasMoving && !EnsureBank(dev.Get(), moving.Tier))) return;
    // History SRVs are private t15..17. Preserve the caller's bindings while
    // ensuring the copy destinations are not simultaneously shader inputs.
    ID3D11ShaderResourceView *previous[TierCount]{}; ctx->PSGetShaderResources(15, TierCount, previous);
    ID3D11ShaderResourceView *empty[TierCount]{}; ctx->PSSetShaderResources(15, TierCount, empty);
    CopyCube(ctx, CubeTextures[sizeIndex].Get(), sourceCube, tier, cube);
    if (hasMoving) CopyCube(ctx, moving.Texture.Get(), moving.Cube, moving.Tier, movingCube);
    ctx->PSSetShaderResources(15, TierCount, previous);
    for (auto *view : previous) if (view) view->Release();
    History captured{}; policy::Start(captured.State, key, slot, Epoch, now);
    captured.Source = map.Source; captured.Tier = tier; captured.Cube = cube;
    captured.HasMoving = hasMoving; captured.MovingTier = moving.Tier; captured.MovingCube = movingCube;
    *entry = captured; ++Stats.Captured;
    if (capture::State == capture::Phase::Capturing && capture::Log)
        fprintf(capture::Log, "shadow handoff owner %016llx slot %d old frame %llu pose %.4f %.4f %.4f -> %.4f %.4f %.4f far %.4f core %.4f history %u/%u moving %u/%u/%d duration %.3f\n",
            static_cast<unsigned long long>(key), slot, static_cast<unsigned long long>(map.BuiltFrame),
            map.Source.Position[0], map.Source.Position[1], map.Source.Position[2], next.Position[0], next.Position[1], next.Position[2],
            map.Source.Radius, map.Source.Core, tier, cube, moving.Tier, movingCube, hasMoving ? 1 : 0, policy::DurationSeconds);
}
static void AfterDraw(const Light &light)
{
    if (!Sync() || light.Slot < 0 || light.Slot >= MAX_LIGHTS) return;
    const uint64_t key = SlotLights[light.Slot];
    if (!Owner(key, light)) return;
    auto &map = Maps[light.Slot];
    map.Owner = key; map.Epoch = Epoch; map.BuiltFrame = light.BuiltFrame; map.Source = Describe(light, true);
    map.Valid = policy::Finite(map.Source);
}
static void CommitMovingFrame()
{
    if (!Sync()) return;
    for (int slot = 0; slot < MAX_LIGHTS; ++slot)
    {
        Moving[slot] = {};
        const uint64_t key = SlotLights[slot]; const auto found = Lights.find(key);
        if (!SlotActive[slot] || found == Lights.end() || !Owner(key, found->second)) continue;
        const auto &light = found->second; const auto &map = Maps[slot]; const auto source = Describe(light, true);
        if (!map.Valid || map.Owner != key || map.Epoch != Epoch || map.BuiltFrame != light.BuiltFrame
            || !policy::SamePose(map.Source, source)) continue;
        auto &moving = Moving[slot];
        moving.Owner = key; moving.Epoch = Epoch; moving.Frame = FrameNumber; moving.Source = source;
        ID3D11Texture2D *particle = nullptr; UINT particleCube = 0;
        if (particle_moving::Snapshot(slot, light, FrameNumber, particle, particleCube))
        { moving.Texture = particle; moving.Tier = 2; moving.Cube = particleCube; }
        else if (slot < SIZE_STARTS[1] && MovingMapUsed[slot])
        {
            if (!MovingTexture || !MovingView) continue;
            moving.Texture = MovingTexture; moving.Tier = 1; moving.Cube = static_cast<UINT>(slot);
        }
        moving.Valid = true;
    }
}
static bool Get(uint64_t key, const Light &light, Endpoint &endpoint)
{
    endpoint = {};
    if (!Sync() || !Owner(key, light) || !SlotActive[light.Slot]) return false;
    const auto source = Describe(light, true); const auto &map = Maps[light.Slot]; const auto &moving = Moving[light.Slot];
    if (!map.Valid || map.Owner != key || map.Epoch != Epoch || map.BuiltFrame != light.BuiltFrame
        || !policy::SamePose(map.Source, source) || !moving.Valid || moving.Owner != key || moving.Epoch != Epoch
        || moving.Frame + 1 != FrameNumber || !policy::SamePose(moving.Source, source)) return false;
    const double now = Now();
    for (auto &history : Histories) if (policy::Live(history.State, key, light.Slot, Epoch, now))
    {
        if (!policy::BoundedMove(history.Source, source)) { history = {}; ++Stats.Ineligible; return false; }
        if (!Banks[history.Tier].View || (history.HasMoving && !Banks[history.MovingTier].View)) return false;
        endpoint.Position = { history.Source.Position[0], history.Source.Position[1], history.Source.Position[2] };
        endpoint.Radius = history.Source.Radius; endpoint.Tier = history.Tier; endpoint.Cube = history.Cube;
        endpoint.FaceSize = HistorySizes[history.Tier]; endpoint.NewWeight = policy::NewWeight(history.State, now);
        if (history.HasMoving)
        { endpoint.MovingTier = history.MovingTier; endpoint.MovingCube = history.MovingCube; endpoint.MovingFaceSize = HistorySizes[history.MovingTier]; }
        return true;
    }
    return false;
}
static void WriteState(FILE *file)
{
    if (!file) return;
    unsigned allocated = 0, active = 0;
    for (const auto &bank : Banks) if (bank.Texture) ++allocated;
    for (const auto &history : Histories) if (history.State.Active) ++active;
    fprintf(file, "shadow handoffs: active %d epoch %llu duration %.3f logical %u/%u banks %u/3 max84MiB captured %llu coalesced %llu expired %llu capacity %llu ineligible %llu missing moving %llu resource failures %llu reassigned %llu settings resets %llu\n",
        Active ? 1 : 0, static_cast<unsigned long long>(Epoch), policy::DurationSeconds, active, policy::MaximumTransitions, allocated,
        static_cast<unsigned long long>(Stats.Captured), static_cast<unsigned long long>(Stats.Coalesced), static_cast<unsigned long long>(Stats.Expired),
        static_cast<unsigned long long>(Stats.Capacity), static_cast<unsigned long long>(Stats.Ineligible), static_cast<unsigned long long>(Stats.MissingMoving),
        static_cast<unsigned long long>(Stats.ResourceFailures), static_cast<unsigned long long>(Stats.Reassigned), static_cast<unsigned long long>(Stats.SettingsResets));
    const double now = Now();
    for (const auto &history : Histories) if (history.State.Active)
        fprintf(file, "  handoff owner %016llx slot %d old %.4f %.4f %.4f far %.4f core %.4f static %u/%u moving %u/%u/%d new weight %.4f age %.4f\n",
            static_cast<unsigned long long>(history.State.Owner), history.State.Slot, history.Source.Position[0], history.Source.Position[1], history.Source.Position[2],
            history.Source.Radius, history.Source.Core, history.Tier, history.Cube, history.MovingTier, history.MovingCube, history.HasMoving ? 1 : 0,
            policy::NewWeight(history.State, now), now - history.State.Started);
}
}
