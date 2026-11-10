#pragma once
#include <string>

// Candidate-only, world-space shadow handoff. Depth and light pose remain paired;
// current RGB is deliberately used at both endpoints so fire animation stays live.
namespace glow_transition_shader
{
inline constexpr const char *Declarations = R"(
struct GlowTransition
{
    float4 OldPosition; // xyz retained source position, w new endpoint weight
    float4 StaticMap;   // tier, cube, face size, retained map far plane
    float4 MovingMap;   // tier, cube, face size, valid moving snapshot
};
cbuffer GlowTransitions : register(b4) { GlowTransition Transitions[512]; };
TextureCubeArray<float> PreviousMaps0 : register(t15);
TextureCubeArray<float> PreviousMaps1 : register(t16);
TextureCubeArray<float> PreviousMaps2 : register(t17);
bool HasTransition(uint index)
{
    return Transitions[index].StaticMap.z > 0 && Transitions[index].OldPosition.w >= 0
        && Transitions[index].OldPosition.w < 1;
}
)";

inline constexpr const char *Helpers = R"(
GlowLight PreviousLight(uint index, GlowLight current)
{
    GlowTransition history = Transitions[index];
    current.Position = float4(history.OldPosition.xyz, history.StaticMap.x);
    current.Normal.w = history.StaticMap.y;
    current.Shadow.x = history.StaticMap.w;
    current.Moving.x = 0;
    current.Shadow.zw = 0;
    return current;
}
float3 FindTransitionContribution(uint index, GlowLight light, float3 world, float3 normal, out float inside)
{
    float3 current = FindContribution(light, world, normal, inside);
    if (!HasTransition(index)) return current;
    float oldInside;
    float3 previous = FindContribution(PreviousLight(index, light), world, normal, oldInside);
    inside = lerp(oldInside, inside, Transitions[index].OldPosition.w);
    return lerp(previous, current, Transitions[index].OldPosition.w);
}
float3 FindTransitionBounce(uint index, GlowLight light, float3 world, float3 normal)
{
    float3 current = FindBounce(light, world, normal);
    if (!HasTransition(index)) return current;
    return lerp(FindBounce(PreviousLight(index, light), world, normal), current, Transitions[index].OldPosition.w);
}
float FilterPrevious(float tier, float cube, float3 axis, float3 tangent, float3 bitangent,
    float2x2 rotation, float reference, bool full, float along, float farPlane)
{
    if (tier < .5) return FilterShadow(PreviousMaps0, axis, tangent, bitangent, rotation, cube, reference, full, along, farPlane);
    if (tier < 1.5) return FilterShadow(PreviousMaps1, axis, tangent, bitangent, rotation, cube, reference, full, along, farPlane);
    return FilterShadow(PreviousMaps2, axis, tangent, bitangent, rotation, cube, reference, full, along, farPlane);
}
float EndpointVisibility(uint index, GlowLight light, float3 world, float3 normal,
    float strength, float total, float2x2 rotation, bool previous)
{
    float faceSize = FaceSizes[min((uint)light.Position.w, 2u)];
    if (previous)
    {
        faceSize = Transitions[index].StaticMap.z;
        if (Transitions[index].MovingMap.w > .5) faceSize = min(faceSize, Transitions[index].MovingMap.z);
    }
    else
    {
        if (light.Shadow.z > 0) faceSize = min(faceSize, 256);
        if (light.Moving.x > 0) faceSize = min(faceSize, FaceSizes.w);
    }
    float lightDistance = distance(light.Position.xyz, world);
    float texel = 2 * lightDistance / faceSize;
    float3 toPixel = world + normal * texel * 1.5 - light.Position.xyz;
    float3 axes = abs(toPixel);
    float along = max(axes.x, max(axes.y, axes.z));
    float lit = 1;
    if (along < light.Shadow.x && along > 0 && Counts.w <= 0 && Counts.z > 0)
    {
        float adjusted = max(along * .985 - .02, Counts.y);
        float reference = DeviceDepth(adjusted, Counts.y, light.Shadow.x);
        float3 axis = toPixel / along;
        float3 up = abs(axis.y) < .99 * length(axis) ? float3(0, 1, 0) : float3(1, 0, 0);
        float3 tangent = normalize(cross(up, axis));
        float3 bitangent = normalize(cross(axis, tangent));
        bool full = strength >= FULL_FILTER_SHARE * total;
        if (previous)
        {
            GlowTransition history = Transitions[index];
            lit = FilterPrevious(history.StaticMap.x, history.StaticMap.y, axis, tangent, bitangent,
                rotation, reference, full, adjusted, light.Shadow.x);
            if (history.MovingMap.w > .5)
                lit = min(lit, FilterPrevious(history.MovingMap.x, history.MovingMap.y, axis, tangent, bitangent,
                    rotation, reference, full, adjusted, light.Shadow.x));
        }
        else
        {
            lit = FilterLightShadow(light.Position.w, axis, tangent, bitangent, rotation, light.Normal.w,
                reference, full, adjusted, light.Shadow.x);
            if (light.Moving.x > 0)
                lit = min(lit, FilterShadow(MovingMaps, axis, tangent, bitangent, rotation, light.Moving.z,
                    reference, full, adjusted, light.Shadow.x));
            if (light.Shadow.z > 0)
                lit = min(lit, FilterParticleShadow(light.Shadow.w, axis, tangent, bitangent, rotation,
                    light.Shadow.z - 1, reference, full, adjusted, light.Shadow.x));
        }
    }
    return 1 - saturate(Counts.z) * (1 - lit);
}
)";

inline constexpr const char *LoopBody = R"(
        float inside;
        float3 contribution = FindContribution(light, world, normal, inside);
        float strength = max(contribution.r, max(contribution.g, contribution.b));
        bool transitioning = HasTransition(j);
        float weight = transitioning ? Transitions[j].OldPosition.w : 1;
        float3 oldContribution = 0;
        float oldStrength = 0;
        GlowLight oldLight = light;
        if (transitioning)
        {
            oldLight = PreviousLight(j, light);
            oldContribution = FindContribution(oldLight, world, normal, inside);
            oldStrength = max(oldContribution.r, max(oldContribution.g, oldContribution.b));
        }
        if (max(strength, oldStrength) <= 0) continue;
        float lit = strength > 0 ? EndpointVisibility(j, light, world, normal, strength, total, rotation, false) : 1;
        float oldLit = oldStrength > 0 ? EndpointVisibility(j, oldLight, world, normal, oldStrength, total, rotation, true) : 1;
        // Each endpoint combines its own static/dynamic blockers BEFORE blending.
        sum += lerp(oldContribution * oldLit, contribution * lit, weight);
        unshadowed += lerp(oldContribution, contribution, weight);
    }
    return sum;
}

)";

inline bool PatchTotal(std::string &source)
{
    const auto begin = source.find("float FindTotal(");
    const auto end = source.find("// the light the glow lights add with their shadows", begin);
    if (begin == std::string::npos || end == std::string::npos) return false;
    std::string total = source.substr(begin, end - begin);
    const auto patch = [&](const char *from, const char *to) {
        const auto offset = total.find(from);
        if (offset == std::string::npos) return false;
        total.replace(offset, std::char_traits<char>::length(from), to); return true;
    };
    if (!patch("FindContribution(Lights[i], world, normal, inside)", "FindTransitionContribution(i, Lights[i], world, normal, inside)")
        || !patch("FindBounce(Lights[i], world, normal)", "FindTransitionBounce(i, Lights[i], world, normal)")) return false;
    source.replace(begin, end - begin, total); return true;
}

inline bool Apply(std::string &source)
{
    if (!PatchTotal(source)) return false;
    const auto total = source.find("float FindTotal(");
    source.insert(total, Helpers);
    const auto shadow = source.find("float3 FindShadowedLight(");
    const auto light = source.find("        GlowLight light = Lights[j];", shadow);
    const auto end = source.find("struct ReducedOutput", light);
    if (shadow == std::string::npos || light == std::string::npos || end == std::string::npos) return false;
    const auto begin = light + std::string("        GlowLight light = Lights[j];").size();
    source.replace(begin, end - begin, LoopBody);
    source.insert(0, Declarations);
    return true;
}
}
