#pragma once
#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <algorithm>
#include <cstdint>
#include <string>
#include <utility>

// Candidate-only transport. The baseline shader and its resources are never rewritten.
namespace glow_transport
{
using Microsoft::WRL::ComPtr;
constexpr UINT TileSize = 16, MaxLights = 512, MaskWords = 16, ListStride = MaxLights + MaskWords, MaxTiles = 16384;
constexpr float ErrorBudget = 1.0f / 2048;
inline ComPtr<ID3D11PixelShader> ReducedShader, IndependentShader;
inline ComPtr<ID3D11ComputeShader> ListShader;
inline ComPtr<ID3D11Buffer> Parameters, Indices, Headers;
inline ComPtr<ID3D11ShaderResourceView> IndexView, HeaderView;
inline ComPtr<ID3D11UnorderedAccessView> IndexTarget, HeaderTarget;
inline UINT TilesX = 0, TilesY = 0;
inline bool CompileFailed = false;
// Low counts do not amortize a separate construction pass. This chooses work, never light quality.
constexpr UINT MinimumListLights = 32;
inline bool LastUsedLists = false;

constexpr const char *Declarations = R"(
cbuffer TransportParameters : register(b2) { uint4 TransportGrid; };
StructuredBuffer<uint> TransportIndices : register(t13);
StructuredBuffer<uint2> TransportHeaders : register(t14);
static const float TRANSPORT_ERROR = 1.0 / 2048;
)";

constexpr const char *TotalFunction = R"(
float FindTotal(float3 world, float3 normal, out float mask, out float3 bounce, out float3 direct, float2 position)
{
    float total = 0;
    mask = 0;
    bounce = 0;
    direct = 0;
    uint2 region = (uint2)position / 16;
    bool listValid = all(region < TransportGrid.xy);
    uint tile = region.y * TransportGrid.x + region.x;
    uint2 header = listValid ? TransportHeaders[tile] : uint2(0xffffffff, 0);
    float omittedBound = asfloat(header.y);
    listValid = listValid && header.x <= (uint)Counts.x && isfinite(omittedBound) && omittedBound >= 0 && omittedBound <= TRANSPORT_ERROR;
    uint admittedBits = 0;
    [loop]
    for (uint i = 0; i < (uint)Counts.x; ++i)
    {
        float inside;
        float3 contribution = FindContribution(Lights[i], world, normal, inside);
        total += max(contribution.r, max(contribution.g, contribution.b));
        mask = max(mask, inside);
        if (listValid && (i & 31) == 0) admittedBits = TransportIndices[tile * 528 + (i >> 5)];
        if (listValid && ((admittedBits >> (i & 31)) & 1) == 0) direct += contribution;
        bounce += FindBounce(Lights[i], world, normal);
    }
    return total;
}
)";

constexpr const char *ComputeShader = R"(
RWStructuredBuffer<uint> ListIndices : register(u0);
RWStructuredBuffer<uint2> ListHeaders : register(u1);
groupshared uint3 RegionLow, RegionHigh;
groupshared uint RegionValid;
groupshared float LightUpper[512];
groupshared uint AdmittedBits[16];
uint OrderedFloat(float value)
{
    uint bits = asuint(value);
    return bits ^ ((bits & 0x80000000) ? 0xffffffff : 0x80000000);
}
float UnorderedFloat(uint value)
{
    return asfloat(value ^ ((value & 0x80000000) ? 0x80000000 : 0xffffffff));
}
float ShadowUpper(GlowLight light, float3 low, float3 high)
{
    if (!all(isfinite(light.Position.xyz)) || !all(isfinite(light.Colour))
        || !isfinite(light.Moving.y) || light.Colour.w <= 0 || light.Moving.y <= 0)
        return 3.402823466e+38;
    // Expand the receiver box; rounding must admit more lights, never silently remove one.
    float magnitude = max(1, max(max(abs(low.x), abs(low.y)), abs(low.z)));
    magnitude = max(magnitude, max(max(abs(high.x), abs(high.y)), abs(high.z)));
    magnitude = max(magnitude, max(max(abs(light.Position.x), abs(light.Position.y)), abs(light.Position.z)));
    float margin = magnitude * 0.00000762939453125;
    float3 nearest = max(max(low - margin - light.Position.xyz, light.Position.xyz - high - margin), 0);
    float distanceSquared = dot(nearest, nearest);
    float share = distanceSquared / (light.Colour.w * light.Colour.w);
    float window = saturate(1 - share * share);
    float colour = max(abs(light.Colour.r), max(abs(light.Colour.g), abs(light.Colour.b)));
    float upper = colour * window * window / (distanceSquared + light.Moving.y);
    upper = upper * 1.0001 + 1e-12;
    return isfinite(upper) ? upper : 3.402823466e+38;
}
[numthreads(16,16,1)]
void cs_transport(uint3 group : SV_GroupID, uint3 thread : SV_GroupThreadID, uint local : SV_GroupIndex)
{
    if (local == 0) { RegionLow = 0xffffffff; RegionHigh = 0; RegionValid = 0; }
    GroupMemoryBarrierWithGroupSync();
    uint2 reducedPixel = group.xy * 16 + thread.xy;
    uint2 reducedSize = TransportGrid.zw;
    if (all(reducedPixel < reducedSize))
    {
        float2 pixel = floor((float2(reducedPixel) + 0.5) * Reduced.x) + 0.5;
        float depth = Depth.Load(int3(pixel, 0));
        float2 uv = pixel * Size.zw;
        float4 clip = float4(uv.x * 2 - 1, 1 - uv.y * 2, depth, 1);
        float4 relative = float4(dot(InverseRows[0], clip), dot(InverseRows[1], clip), dot(InverseRows[2], clip), dot(InverseRows[3], clip));
        float3 world = relative.xyz / relative.w - CameraOffset.xyz;
        if (depth < 1 && all(isfinite(world)))
        {
            uint3 ordered = uint3(OrderedFloat(world.x), OrderedFloat(world.y), OrderedFloat(world.z));
            InterlockedMin(RegionLow.x, ordered.x); InterlockedMin(RegionLow.y, ordered.y); InterlockedMin(RegionLow.z, ordered.z);
            InterlockedMax(RegionHigh.x, ordered.x); InterlockedMax(RegionHigh.y, ordered.y); InterlockedMax(RegionHigh.z, ordered.z);
            InterlockedAdd(RegionValid, 1);
        }
    }
    GroupMemoryBarrierWithGroupSync();
    uint count = min((uint)Counts.x, 512u);
    uint tile = group.y * TransportGrid.x + group.x;
    if (RegionValid == 0)
    {
        // Unknown/empty regions use the exact full loop if the pixel shader finds a surface.
        if (local == 0) ListHeaders[tile] = uint2(0xffffffff, 0);
        return;
    }
    float3 low = float3(UnorderedFloat(RegionLow.x), UnorderedFloat(RegionLow.y), UnorderedFloat(RegionLow.z));
    float3 high = float3(UnorderedFloat(RegionHigh.x), UnorderedFloat(RegionHigh.y), UnorderedFloat(RegionHigh.z));
    [loop] for (uint i = local; i < count; i += 256)
        LightUpper[i] = Counts.w > 0 ? 0 : saturate(Counts.z) * ShadowUpper(Lights[i], low, high);
    GroupMemoryBarrierWithGroupSync();
    if (local == 0)
    {
        float remaining = TRANSPORT_ERROR * 0.999;
        float used = 0;
        uint admitted = 0;
        [unroll] for (uint word = 0; word < 16; ++word) AdmittedBits[word] = 0;
        [loop] for (uint i = 0; i < count; ++i)
        {
            float upper = LightUpper[i];
            if (isfinite(upper) && upper >= 0 && upper <= remaining)
            { remaining -= upper; used += upper; }
            else
            {
                ListIndices[tile * 528 + 16 + admitted++] = i;
                AdmittedBits[i >> 5] |= 1u << (i & 31);
            }
        }
        [unroll] for (uint outputWord = 0; outputWord < 16; ++outputWord)
            ListIndices[tile * 528 + outputWord] = AdmittedBits[outputWord];
        // A dense list only adds indirection. The exact independent loop is cheaper there.
        ListHeaders[tile] = admitted * 8 >= count * 7 ? uint2(0xffffffff, 0)
            : uint2(admitted, asuint(used * 1.0001 + 1e-10));
    }
}
)";

inline bool Replace(std::string &source, const std::string &before, const std::string &after)
{
    const auto position = source.find(before);
    if (position == std::string::npos || source.find(before, position + before.size()) != std::string::npos) return false;
    source.replace(position, before.size(), after);
    return true;
}

inline bool Sources(const char *baseline, std::string &pixel, std::string &compute)
{
    std::string original(baseline);
    original.erase(std::remove(original.begin(), original.end(), '\r'), original.end());
    pixel = original;
    const auto total = pixel.find("float FindTotal(");
    const auto shadowComment = pixel.find("// the light the glow lights add with their shadows", total);
    if (total == std::string::npos || shadowComment == std::string::npos) return false;
    pixel.replace(total, shadowComment - total, TotalFunction);
    if (!Replace(pixel, "    float3 skipped = 0;\n", "")
        || !Replace(pixel, "    [loop]\n    for (uint j = 0; j < (uint)Counts.x; ++j)\n    {\n        GlowLight light = Lights[j];",
            "    uint2 region = (uint2)position / 16;\n"
            "    bool listValid = all(region < TransportGrid.xy);\n"
            "    uint tile = region.y * TransportGrid.x + region.x;\n"
            "    uint2 header = listValid ? TransportHeaders[tile] : uint2(0xffffffff, 0);\n"
            "    float omittedBound = asfloat(header.y);\n"
            "    listValid = listValid && header.x <= (uint)Counts.x && isfinite(omittedBound) && omittedBound >= 0 && omittedBound <= TRANSPORT_ERROR;\n"
            "    uint count = listValid ? header.x : (uint)Counts.x;\n"
            "    [loop] for (uint entry = 0; entry < count; ++entry)\n    {\n"
            "        uint j = listValid ? TransportIndices[tile * 528 + 16 + entry] : entry;\n"
            "        GlowLight light = Lights[j];")
        || !Replace(pixel, "        if (strength < CUTOFF)\n", "        if (strength <= 0)\n")) return false;
    const auto shortcut = pixel.find("        // Particle shadows must not disappear");
    const auto shadowBegin = pixel.find("        // compare from about a texel", shortcut);
    if (shortcut == std::string::npos || shadowBegin == std::string::npos) return false;
    pixel.erase(shortcut, shadowBegin - shortcut);
    if (!Replace(pixel, "    sum += skipped * (unshadowed > 0 ? sum / max(unshadowed, 1e-30) : 1);\n    unshadowed += skipped;\n", "")
        || !Replace(pixel, "    float3 bounce;\n    float total = FindTotal(world, normal, mask, bounce);",
            "    float3 bounce, direct;\n    float total = FindTotal(world, normal, mask, bounce, direct, position.xy);")
        || !Replace(pixel, "    output.Light = float4(bounce, cameraDistance);", "    output.Light = float4(bounce + direct, cameraDistance);")) return false;
    pixel.insert(0, Declarations);
    const auto constantsEnd = original.find("static const float2 POISSON");
    if (constantsEnd == std::string::npos) return false;
    compute = std::string(Declarations) + original.substr(0, constantsEnd) + ComputeShader;
    return true;
}

inline bool Compile(const std::string &source, const char *entry, const char *profile, ComPtr<ID3DBlob> &code)
{
    ComPtr<ID3DBlob> errors;
    return SUCCEEDED(D3DCompile(source.data(), source.size(), "TextureQuest-transport-candidate", nullptr, nullptr,
        entry, profile, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors));
}

inline bool IndependentSources(const char *baseline, std::string &source)
{
    std::string unused;
    if (!Sources(baseline, source, unused)) return false;
    std::string original(baseline);
    original.erase(std::remove(original.begin(), original.end(), '\r'), original.end());
    const auto total = original.find("float FindTotal(");
    const auto shadow = original.find("// the light the glow lights add with their shadows", total);
    const auto candidateTotal = source.find("float FindTotal(");
    const auto candidateShadow = source.find("// the light the glow lights add with their shadows", candidateTotal);
    source.replace(candidateTotal, candidateShadow - candidateTotal, original.substr(total, shadow - total));
    const auto function = source.find("float3 FindShadowedLight(");
    const auto begin = source.find("    uint2 region = ", function);
    const auto end = source.find("        GlowLight light = Lights[j];", begin);
    if (begin == std::string::npos || end == std::string::npos) return false;
    source.replace(begin, end - begin, "    [loop] for (uint j = 0; j < (uint)Counts.x; ++j)\n    {\n");
    return Replace(source, "    float3 bounce, direct;\n    float total = FindTotal(world, normal, mask, bounce, direct, position.xy);",
            "    float3 bounce;\n    float total = FindTotal(world, normal, mask, bounce);")
        && Replace(source, "    output.Light = float4(bounce + direct, cameraDistance);", "    output.Light = float4(bounce, cameraDistance);");
}

inline ID3D11PixelShader *Shader(UINT count)
{
    return count < MinimumListLights ? IndependentShader.Get() : ReducedShader.Get();
}

inline bool Prepare(ID3D11Device *device, const char *baseline, UINT width, UINT height, UINT count = MaxLights)
{
    if (CompileFailed || !width || !height) return false;
    if (!IndependentShader)
    {
        std::string source;
        ComPtr<ID3DBlob> code;
        if (!IndependentSources(baseline, source) || !Compile(source, "ps_reduced_light", "ps_5_0", code)
            || FAILED(device->CreatePixelShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &IndependentShader)))
        { CompileFailed = true; return false; }
    }
    if (count < MinimumListLights) return true;
    const UINT x = (width + TileSize - 1) / TileSize, y = (height + TileSize - 1) / TileSize;
    if (uint64_t(x) * y > MaxTiles) return false;
    if (!ReducedShader || !ListShader)
    {
        std::string pixel, compute;
        ComPtr<ID3DBlob> pixelCode, computeCode;
        if (!Sources(baseline, pixel, compute)
            || !Compile(pixel, "ps_reduced_light", "ps_5_0", pixelCode)
            || !Compile(compute, "cs_transport", "cs_5_0", computeCode)
            || FAILED(device->CreatePixelShader(pixelCode->GetBufferPointer(), pixelCode->GetBufferSize(), nullptr, &ReducedShader))
            || FAILED(device->CreateComputeShader(computeCode->GetBufferPointer(), computeCode->GetBufferSize(), nullptr, &ListShader)))
        { CompileFailed = true; return false; }
    }
    if (TilesX == x && TilesY == y && Parameters && IndexView && HeaderView && IndexTarget && HeaderTarget) return true;
    ComPtr<ID3D11Buffer> indices, headers, parameters;
    ComPtr<ID3D11ShaderResourceView> indexView, headerView;
    ComPtr<ID3D11UnorderedAccessView> indexTarget, headerTarget;
    const auto make = [device](UINT entries, UINT stride, ComPtr<ID3D11Buffer> &buffer,
        ComPtr<ID3D11ShaderResourceView> &view, ComPtr<ID3D11UnorderedAccessView> &target) {
        D3D11_BUFFER_DESC desc = { entries * stride, D3D11_USAGE_DEFAULT,
            D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS, 0, D3D11_RESOURCE_MISC_BUFFER_STRUCTURED, stride };
        return SUCCEEDED(device->CreateBuffer(&desc, nullptr, &buffer))
            && SUCCEEDED(device->CreateShaderResourceView(buffer.Get(), nullptr, &view))
            && SUCCEEDED(device->CreateUnorderedAccessView(buffer.Get(), nullptr, &target));
    };
    const D3D11_BUFFER_DESC parameterDesc = { 16, D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER };
    if (!make(x * y * ListStride, 4, indices, indexView, indexTarget)
        || !make(x * y, 8, headers, headerView, headerTarget)
        || FAILED(device->CreateBuffer(&parameterDesc, nullptr, &parameters))) return false;
    Indices = std::move(indices); Headers = std::move(headers); Parameters = std::move(parameters);
    IndexView = std::move(indexView); HeaderView = std::move(headerView);
    IndexTarget = std::move(indexTarget); HeaderTarget = std::move(headerTarget);
    TilesX = x; TilesY = y;
    return true;
}

inline void Build(ID3D11DeviceContext *context, ID3D11Buffer *frame, ID3D11Buffer *lights,
    ID3D11Buffer *camera, ID3D11ShaderResourceView *depth, UINT width, UINT height, UINT count = MaxLights)
{
    ID3D11ShaderResourceView *noPixelViews[] = { nullptr, nullptr };
    context->PSSetShaderResources(13, 2, noPixelViews);
    LastUsedLists = count >= MinimumListLights;
    if (!LastUsedLists) return;
    const UINT grid[] = { TilesX, TilesY, width, height };
    context->UpdateSubresource(Parameters.Get(), 0, nullptr, grid, 0, 0);
    ID3D11Buffer *buffers[] = { frame, lights, Parameters.Get(), camera };
    context->CSSetConstantBuffers(0, 4, buffers);
    context->CSSetShaderResources(0, 1, &depth);
    ID3D11UnorderedAccessView *targets[] = { IndexTarget.Get(), HeaderTarget.Get() };
    context->CSSetUnorderedAccessViews(0, 2, targets, nullptr);
    context->CSSetShader(ListShader.Get(), nullptr, 0);
    context->Dispatch(TilesX, TilesY, 1);
    ID3D11UnorderedAccessView *none[] = { nullptr, nullptr };
    context->CSSetUnorderedAccessViews(0, 2, none, nullptr);
    ID3D11ShaderResourceView *noDepth = nullptr;
    context->CSSetShaderResources(0, 1, &noDepth);
    context->CSSetShader(nullptr, nullptr, 0);
    ID3D11Buffer *noComputeBuffers[] = { nullptr, nullptr, nullptr, nullptr };
    context->CSSetConstantBuffers(0, 4, noComputeBuffers);
    context->PSSetConstantBuffers(2, 1, buffers + 2);
    ID3D11ShaderResourceView *views[] = { IndexView.Get(), HeaderView.Get() };
    context->PSSetShaderResources(13, 2, views);
}

inline void Unbind(ID3D11DeviceContext *context)
{
    ID3D11ShaderResourceView *views[] = { nullptr, nullptr };
    context->PSSetShaderResources(13, 2, views);
    ID3D11Buffer *buffer = nullptr;
    context->PSSetConstantBuffers(2, 1, &buffer);
}

inline void Reset()
{
    ReducedShader.Reset(); IndependentShader.Reset(); ListShader.Reset(); Parameters.Reset(); Indices.Reset(); Headers.Reset();
    IndexView.Reset(); HeaderView.Reset(); IndexTarget.Reset(); HeaderTarget.Reset();
    TilesX = TilesY = 0; CompileFailed = false; LastUsedLists = false;
}
}
