// Point-light shadows. The game lights an object with each point light in one of two ways:
// - an alpha pass that writes how much of the light reaches each pixel into the framebuffer alpha, followed by a colour
//   pass that adds the light's colour scaled by that alpha;
// - a single pass that outputs the light amount as its own alpha and blends with src_alpha * src + dst.
// Lamp passes whose pixel shader is in LAMP_SHADERS are drawn with a replacement that computes the same light, faded by
// the game's own fade textures, and multiplies its alpha by the shadow factor read from the light's cube map (see
// shadow_maps.hpp) when the light has one. The colour pass of a two-pass lamp, which carries that lamp's fade, is drawn
// with a replacement too. Other lamp passes get
// the shadow from an extra draw after the alpha pass that multiplies the alpha; a single pass is split into an alpha
// pass and a colour pass for this.
// The game draws a moving mesh, such as a character, for some of the lamps that reach it and not others, and changes
// its pick from frame to frame. Once the light buffer's lamps are done, each moving mesh is drawn again for the lamps
// in reach that the game did not draw it for (DrawAddedLamps).
#pragma once
#include <imgui.h>
#include "texturequest_world_presets.hpp"
#include <reshade.hpp>
#include <future>
#include <d3dcompiler.h>
#include <regex>
#include <string>
#include <unordered_set>
#include "capture.hpp"
#include "shadow_maps.hpp"
#include "light_buffer.hpp"
#include "glow.hpp"
#include "projected_shadows.hpp"
#include "sun_softness_settings.hpp"
#include "particle_emission.hpp"
#include "offscreen_lamp_policy.hpp"

using namespace reshade::api;

namespace shadows
{
using Microsoft::WRL::ComPtr;

constexpr const char *CONFIG_SECTION = "EQ2Graphics";
constexpr UINT MAP_SLOT = 15;
constexpr UINT MOVING_MAP_SLOT = 12;
constexpr UINT POINT_SAMPLER_SLOT = 14;
constexpr UINT TABLE_SLOT = 2;
constexpr UINT CAMERA_OFFSET_SLOT = 3;
constexpr UINT VIEW_SLOT = 4;
constexpr UINT ADDED_LAMP_SLOT = 5;
constexpr UINT ADDED_MAPS_SLOT = 16;
constexpr UINT ADDED_LIST_SLOT = 19;
constexpr UINT DRAW_SLOT_SLOT = 6;
constexpr UINT CULLED_SLOT = 7;
constexpr UINT CULLED_DEPTH_SLOT = 20;
constexpr UINT CULLED_MARK_SLOT = 21;
constexpr UINT WINDOW_SLOT = 8;
constexpr UINT WINDOW_MAP_SLOT = 22;

// point-light vertex shaders compute light position (cb0[13]) minus vertex position
static const std::regex POINT_LIGHT_PATTERN(R"(add r\d\.\w+, -r\d\.\w+, cb0\[13\]\.)");
// static geometry reads its world rows directly; skinned geometry indexes a bone palette
static const std::regex WORLD_ROWS_PATTERN(R"(dp4 r\d\.\w, r\d\.xyzw, cb0\[7\]\.xyzw)");
static const std::regex INDEXED_CONSTANTS_PATTERN(R"(cb0\[r\d)");
// the game's sun-lit base light shaders: the vertex shader passes the sun's colour (cb0[11]) on, and the pixel shader
// adds the ambient colour (cb0[0])
static const std::regex SUN_COLOUR_PATTERN(R"(mov_sat o1\.xyzw, cb0\[11\]\.xyzw)");
static const std::regex AMBIENT_PATTERN(R"(max r\d\.xyzw, cb0\[0\]\.wxyz)");
// sun-lit animated mesh shaders light from the sun direction in cb0[13], turned into the surface's tangent space
static const std::regex SUN_DIRECTION_PATTERN(R"(dp3 r\d\.\w, r\d\.xyzx, cb0\[13\]\.xyzx)");

// the vertex shader repeats the game's position transform exactly, so the game's equal depth test passes
// split into several literals, since the compiler limits each literal to about 16 KB
constexpr const char *SHADOW_SHADER = R"(
cbuffer cb : register(b0) { float4 c[1024]; };
cbuffer sb : register(b1) { float4 s[11]; };
cbuffer LightTable : register(b2) { float4 Lights[MAX_LIGHTS]; float4 Params; float4 Softness; float4 Moving[MAX_LIGHTS]; };
// this frame's camera offset; w is 0 until one is known
cbuffer Camera : register(b3) { float4 CameraOffset; };
TextureCubeArray<float> Maps : register(t15);
TextureCubeArray<float> MovingMaps : register(t12);
SamplerComparisonState Compare : register(s15);
SamplerState PointSampler : register(s14);

static const float2 POISSON[16] = {
    float2(-0.94201624, -0.39906216), float2(0.94558609, -0.76890725), float2(-0.09418410, -0.92938870),
    float2(0.34495938, 0.29387760), float2(-0.91588581, 0.45771432), float2(-0.81544232, -0.87912464),
    float2(-0.38277543, 0.27676845), float2(0.97484398, 0.75648379), float2(0.44323325, -0.97511554),
    float2(0.53742981, -0.47373420), float2(-0.26496911, -0.41893023), float2(0.79197514, 0.19090188),
    float2(-0.24188840, 0.99706507), float2(-0.81409955, 0.91437590), float2(0.19984126, 0.78641367),
    float2(0.14383161, -0.14100790) };

float LinearDepth(float depth, float nearPlane, float farPlane)
{
    return nearPlane * farPlane / (farPlane - depth * (farPlane - nearPlane));
}

float DeviceDepth(float along, float nearPlane, float farPlane)
{
    return farPlane / (farPlane - nearPlane) - farPlane * nearPlane / ((farPlane - nearPlane) * along);
}

// contact-hardening soft shadow: finds the average distance to whatever blocks the light, then widens the sampled area
// by how far the pixel is behind it
float SoftShadow(TextureCubeArray<float> maps, float3 toPixel, float slot, float along, float nearPlane, float farPlane,
    float2 pixel)
{
    float lightSize = Softness.x;
    float3 direction = normalize(toPixel);
    if (lightSize <= 0)
        return maps.SampleCmpLevelZero(Compare, float4(direction, slot), DeviceDepth(along, nearPlane, farPlane));

    float3 up = abs(direction.y) < 0.99 ? float3(0, 1, 0) : float3(1, 0, 0);
    float3 tangent = normalize(cross(up, direction));
    float3 bitangent = cross(direction, tangent);
    float angle = 6.2831853 * frac(52.9829189 * frac(dot(pixel, float2(0.06711056, 0.00583715))));
    float2x2 rotation = float2x2(cos(angle), -sin(angle), sin(angle), cos(angle));

    float searchRadius = min(lightSize * 2 / along, 0.2);
    float blockerSum = 0;
    float blockerCount = 0;
    [unroll]
    for (int n = 0; n < 16; ++n)
    {
        float2 offset = mul(rotation, POISSON[n]) * searchRadius;
        float3 sampleDirection = direction + tangent * offset.x + bitangent * offset.y;
        float blocker = LinearDepth(maps.SampleLevel(PointSampler, float4(sampleDirection, slot), 0), nearPlane, farPlane);
        if (blocker < along)
        {
            blockerSum += blocker;
            blockerCount += 1;
        }
    }
    if (blockerCount == 0)
        return 1;

    float blockerDistance = blockerSum / blockerCount;
    float penumbra = clamp(lightSize * (along - blockerDistance) / (blockerDistance * along), 0.0015, 0.15);
    float reference = DeviceDepth(along, nearPlane, farPlane);
    float lit = 0;
    [unroll]
    for (int m = 0; m < 16; ++m)
    {
        float2 offset = mul(rotation, POISSON[m]) * penumbra;
        lit += maps.SampleCmpLevelZero(Compare, float4(direction + tangent * offset.x + bitangent * offset.y, slot), reference);
    }
    return lit / 16;
}

// the surface's normal at the pixel, from how its position changes between neighbouring pixels; zero where that is unknown
float3 FindSurfaceNormal(float3 worldPosition)
{
    float3 normal = cross(ddx(worldPosition), ddy(worldPosition));
    return dot(normal, normal) > 1e-20 ? normalize(normal) : float3(0, 0, 0);
}

// how much of the light reaches a pixel at this absolute position, from 0 in shadow to 1 lit, with the light's map in
// maps, the array of its slot's size
float FindLitIn(TextureCubeArray<float> maps, float3 worldPosition, float3 normal, float3 lightPosition, float slot,
    float2 pixel)
{
    float nearPlane = Params.x;
    float farPlane = Params.y;
    // move the pixel off its surface by about one texel of the coarser map, more where the light grazes the surface, so
    // ground lit from low down does not shadow itself
    float3 toLight = lightPosition - worldPosition;
    float lightDistance = max(length(toLight), 1e-4);
    float facing = dot(normal, toLight) / lightDistance;
    // the draw binds the array of its light's map size, so the cube is the slot's place in that array
    float layer = slot < MIDDLE_START ? slot : slot < FAR_START ? slot - MIDDLE_START : slot - FAR_START;
    float faceSize = slot < MIDDLE_START ? MOVING_FACE_SIZE : slot < FAR_START ? MIDDLE_FACE_SIZE : FAR_FACE_SIZE;
    float texel = 2 * lightDistance / faceSize;
    worldPosition += (facing < 0 ? -normal : normal) * texel * (1 + 2 * (1 - abs(facing)));
    float3 toPixel = worldPosition - lightPosition;
    float3 axes = abs(toPixel);
    float along = max(axes.x, max(axes.y, axes.z));
    if (along >= farPlane)
        return 1;

    // compare slightly closer to the light than the pixel, so surfaces do not shadow themselves
    along = max(along * 0.985 - 0.02, nearPlane);
    float lit = SoftShadow(maps, toPixel, layer, along, nearPlane, farPlane, pixel);
    if (slot < MIDDLE_START && Moving[(int)slot].x > 0)
        lit = min(lit, SoftShadow(MovingMaps, toPixel, slot, along, nearPlane, farPlane, pixel));
    return lit;
}

// how much of the light reaches a pixel, with the light's map in Maps, which the draw binds for its light
float FindLit(float3 worldPosition, float3 normal, float3 lightPosition, float slot, float2 pixel)
{
    return FindLitIn(Maps, worldPosition, normal, lightPosition, slot, pixel);
}

struct Interpolants
{
    float4 position : SV_Position;
    float3 worldPosition : WORLD_POSITION;
    nointerpolation float4 lightPosition : LIGHT_POSITION;
    // the light's map, or -1 when it has none
    nointerpolation float slot : SLOT;
};

Interpolants vs_main(float3 vertex : POSITION)
{
    Interpolants o;
    precise float4 p = float4(vertex, c[1].w);
    precise float x = dot(p, c[3]);
    precise float y = dot(p, c[4]);
    precise float z = dot(p, c[5]);
    precise float w = dot(p, c[6]);
    precise float2 offset = float2(1, w) * s[10].zw;
    precise float2 shifted = mad(offset, float2(w, -1), float2(x, y));
    o.position = float4(s[10].x > 0.5 ? shifted : float2(x, y), z, w);
    o.worldPosition = float3(dot(p, c[7]), dot(p, c[8]), dot(p, c[9])) - CameraOffset.xyz;
    float3 light = c[13].xyz - CameraOffset.xyz;
    float nearest = LIGHT_MATCH_DISTANCE;
    o.slot = -1;
    o.lightPosition = float4(light, CameraOffset.w);
    for (int n = 0; n < MAX_LIGHTS; ++n)
        if (Lights[n].w >= 0 && distance(Lights[n].xyz, light) < nearest)
        {
            nearest = distance(Lights[n].xyz, light);
            o.slot = Lights[n].w;
            o.lightPosition.xyz = Lights[n].xyz;
        }
    return o;
}

float4 ps_main(Interpolants i) : SV_Target
{
    // colours: magenta where the camera offset is missing, blue where the light has no map, grey beyond the shadow
    // range, red in shadow and green lit
    bool showColours = Params.w > 0;
    float3 normal = FindSurfaceNormal(i.worldPosition);
    if (i.lightPosition.w < 0.5)
        return showColours ? float4(1, 0, 1, 1) : float4(0, 0, 0, 1);

    float slot = i.slot;
    if (slot < 0)
        return showColours ? float4(0, 0, 1, 1) : float4(0, 0, 0, 1);

    float3 axes = abs(i.worldPosition - i.lightPosition.xyz);
    if (max(axes.x, max(axes.y, axes.z)) >= Params.y)
        return showColours ? float4(0.5, 0.5, 0.5, 1) : float4(0, 0, 0, 1);

    float lit = FindLit(i.worldPosition, normal, i.lightPosition.xyz, slot, i.position.xy);
    if (showColours)
        return float4(1 - lit, lit, 0, 1);
    return float4(0, 0, 0, 1 - Params.z * (1 - lit));
}

)"
R"(// the shadow of a directional light the game projects through a window, drawn with vs_main. WindowRows give an
// absolute position the window's picture coordinates u and v and its distance along the beam s; WindowMap holds, over
// the picture, the distance along the beam to the nearest object times WindowParams.z. WindowParams.x: the depth bias
// in s plus where the map starts in s; y: the filter's radius in u and v. Pixels outside the picture are left alone, as
// another window's light may cover them
cbuffer WindowShadow : register(b8) { float4 WindowRows[3]; float4 WindowParams; };
Texture2D<float> WindowMap : register(t22);

float4 window_main(Interpolants i) : SV_Target
{
    // colours: red in shadow and green lit
    bool showColours = Params.w > 0;
    float4 position = float4(i.worldPosition, 1);
    float2 uv = float2(dot(WindowRows[0], position), dot(WindowRows[1], position));
    if (any(uv < 0) || any(uv > 1))
        discard;

    float reference = (dot(WindowRows[2], position) - WindowParams.x) * WindowParams.z;
    float angle = 6.2831853 * frac(52.9829189 * frac(dot(i.position.xy, float2(0.06711056, 0.00583715))));
    float2x2 rotation = float2x2(cos(angle), -sin(angle), sin(angle), cos(angle));
    float lit = 0;
    [unroll]
    for (int n = 0; n < 16; ++n)
        lit += WindowMap.SampleCmpLevelZero(Compare, uv + mul(rotation, POISSON[n]) * WindowParams.y, reference);
    lit /= 16;
    if (showColours)
        return float4(1 - lit, lit, 0, 1);
    return float4(0, 0, 0, 1 - Params.z * (1 - lit));
}
)"
R"(// the lamp shaders below replace the game's own lamp pixel shaders; each computes the same light as the game's shader,
// faded by the game's fade textures, and applies the light's shadow in the same draw

Texture2D Stage0 : register(t0);
Texture2D Stage1 : register(t1);
Texture2D Stage2 : register(t2);
Texture2D Stage3 : register(t3);
TextureCube CubeStage0 : register(t36);
TextureCube CubeStage1 : register(t37);
TextureCube CubeStage2 : register(t38);
TextureCube CubeStage3 : register(t39);
SamplerState StageSampler0 : register(s0);
SamplerState StageSampler1 : register(s1);
SamplerState StageSampler2 : register(s2);
SamplerState StageSampler3 : register(s3);
cbuffer View : register(b4) { float4 InverseViewProjection[4]; float4 Viewport; float4 DepthRange; };
// the slot of the draw's light map, or -1 when its light has none
cbuffer DrawSlot : register(b6) { float4 Slot; };

// the game's lamp vertex shaders all write this layout
struct GameInterpolants
{
    float4 position : SV_POSITION;
    float4 color0 : COLOR0;
    float4 color1 : COLOR1;
    float4 texcoord0 : TEXCOORD0;
    float4 texcoord1 : TEXCOORD1;
    float4 texcoord2 : TEXCOORD2;
    float4 texcoord3 : TEXCOORD3;
    float4 texcoord4 : TEXCOORD4;
    float4 texcoord5 : TEXCOORD5;
    float4 texcoord6 : TEXCOORD6;
    float4 texcoord7 : TEXCOORD7;
    float4 texcoord8 : TEXCOORD8;
    float4 fog : FOG;
    float4 pointSize : PSIZE;
};

// a stage's projected 2D coordinates, divided by w when it is set
float2 Project(float4 coordinates)
{
    return coordinates.w != 0 ? coordinates.xy / coordinates.w : coordinates.xy;
}

// samples a texture stage, which the game binds as either a 2D texture with projected coordinates or a cube map
float4 SampleStage(Texture2D flat, TextureCube cube, SamplerState stageSampler, float4 coordinates, bool isFlat)
{
    if (isFlat)
        return flat.Sample(stageSampler, Project(coordinates));
    return cube.Sample(stageSampler, coordinates.xyz);
}

// discards the pixel when the game's alpha test is on and the alpha fails it; s[0] holds the test's on flag, reference
// value and comparison
void TestAlpha(float alpha)
{
    if (s[0].x <= 0.5)
        return;

    float reference = s[0].y;
    bool passes;
    switch ((uint)(s[0].z + 0.5))
    {
        case 1: passes = false; break;
        case 2: passes = alpha < reference; break;
        case 3: passes = abs(alpha - reference) < 0.004; break;
        case 4: passes = alpha - 0.004 < reference; break;
        case 5: passes = reference < alpha; break;
        case 6: passes = 0.004 < abs(alpha - reference); break;
        case 7: passes = reference - 0.004 < alpha; break;
        default: passes = true; break;
    }
    if (!passes)
        discard;
}

// the camera-relative position of the pixel at this screen position and depth
float3 FindRelativePosition(float4 position)
{
    float2 ndc = float2((position.x - Viewport.x) / Viewport.z * 2 - 1, 1 - (position.y - Viewport.y) / Viewport.w * 2);
    float4 clip = float4(ndc, (position.z - DepthRange.x) / (DepthRange.y - DepthRange.x), 1);
    float4 relative = float4(dot(InverseViewProjection[0], clip), dot(InverseViewProjection[1], clip),
        dot(InverseViewProjection[2], clip), dot(InverseViewProjection[3], clip));
    return relative.xyz / relative.w;
}

// the share of the draw's light that reaches the pixel, from its screen position and depth
float FindShadowFactor(float4 position)
{
    if (CameraOffset.w < 0.5 || Slot.x < 0)
        return 1;

    float3 worldPosition = FindRelativePosition(position) - CameraOffset.xyz;
    float lit = FindLit(worldPosition, FindSurfaceNormal(worldPosition), Lights[(int)Slot.x].xyz, Slot.x, position.xy);
    return 1 - Params.z * (1 - lit);
}

// discards the pixel when the game's user clip plane (s[8], on when s[7].x is set) is behind it
void TestClipPlane(float4 clipCoordinates)
{
    if (s[7].x > 0.5 && dot(clipCoordinates, s[8]) < 0)
        discard;
}

float4 SampleStage0(GameInterpolants i) { return SampleStage(Stage0, CubeStage0, StageSampler0, i.texcoord0, s[1].x < 0.5); }
float4 SampleStage1(GameInterpolants i) { return SampleStage(Stage1, CubeStage1, StageSampler1, i.texcoord1, s[1].y < 0.5); }
float4 SampleStage2(GameInterpolants i) { return SampleStage(Stage2, CubeStage2, StageSampler2, i.texcoord2, s[1].z < 0.5); }
float4 SampleStage3(GameInterpolants i) { return SampleStage(Stage3, CubeStage3, StageSampler3, i.texcoord3, s[1].w < 0.5); }

// fades the light vector toward straight up by the material's bump weight (c[0].w), still in the 0..1 texture encoding
float3 WeightLightVector(float3 lightVector)
{
    return clamp(c[0].w, -1, 1) * (lightVector - float3(0.5, 1, 0.5)) + float3(0.5, 1, 0.5);
}

// turns a vector stored in a texture's 0..1 range into -1..1
float3 Expand(float3 stored)
{
    return 2 * (stored - 0.5);
}

// blends the colour toward the game's fog colour (s[9], on when s[9].w is set) by the vertex fog amount
float3 ApplyFog(float3 color, float fog)
{
    return s[9].w > 0.5 ? lerp(s[9].xyz, color, saturate(fog)) : color;
}

// the light amount scaled by the material's lighting scale (c[2].w)
float ApplyLightingScale(float light)
{
    return saturate(2 * light * clamp(c[2].w, -1, 1));
}

float Pow4(float value)
{
    value *= value;
    return value * value;
}

// a tight specular highlight: the facing above 0.75, stretched back to 0..1
float FindHighlight(float facing)
{
    return max((facing - 0.75) * 4, 0);
}

// samples a lamp's fade texture at its coordinates stretched about the light, at 0.5, to LIGHT_REACH times their reach,
// as StretchLight stretches the lamp passes it sees
float4 SampleFade(Texture2D stage, SamplerState stageSampler, float4 coordinates)
{
    return stage.Sample(stageSampler, (Project(coordinates) - 0.5) / LIGHT_REACH + 0.5);
}

// the game's fade at a squared distance over the lamp's reach, where nothing samples its fade texture: (1 - d^2) to the
// power FindFadePower measured from the texture, which is within about 1.2% rms of the texture's own values
float FindFade(float squared, float power)
{
    return pow(saturate(1 - squared), power);
}

)"
R"(// the lamps come in two kinds. A single-pass lamp returns the lit colour and the light amount in alpha, and is blended
// by that alpha. The first pass of a two-pass lamp writes the light amount to alpha for the second pass to multiply its
// colour by. Either way the shadow scales the alpha. A single-pass lamp is faded as the game's shader fades it: its colour
// by the fade texture across the light's horizontal disc on stage 2, and its light amount, before the lighting scale, by
// the alpha of the one along its vertical strip on stage 3.

// single pass: N.L times the lighting scale
float4 lamp_8b55e251966bcd36(GameInterpolants i) : SV_Target
{
    TestClipPlane(i.texcoord8);
    float3 lightVector = Expand(WeightLightVector(SampleStage0(i).xyz));
    float3 normal = Expand(SampleStage1(i).xyz);
    float alpha = ApplyLightingScale(SampleStage3(i).w * saturate(dot(lightVector, normal) * 0.5));
    TestAlpha(alpha);
    return float4(ApplyFog(SampleStage2(i).xyz * i.color0.xyz, i.fog.x), alpha * FindShadowFactor(i.position));
}

// single pass: N.L
float4 lamp_d878054c72da7a41(GameInterpolants i) : SV_Target
{
    TestClipPlane(i.texcoord8);
    float3 lightVector = Expand(WeightLightVector(SampleStage0(i).xyz));
    float3 normal = Expand(SampleStage1(i).xyz);
    float alpha = SampleStage3(i).w * saturate(dot(lightVector, normal) * 0.5);
    TestAlpha(alpha);
    return float4(ApplyFog(SampleStage2(i).xyz * i.color0.xyz, i.fog.x), alpha * FindShadowFactor(i.position));
}

// single pass: the specular highlight times the lighting scale
float4 lamp_42ebee0d2ac17140(GameInterpolants i) : SV_Target
{
    TestClipPlane(i.texcoord8);
    float3 lightVector = Expand(WeightLightVector(SampleStage0(i).xyz));
    float3 normal = Expand(SampleStage1(i).xyz);
    float highlight = Pow4(FindHighlight(saturate(dot(lightVector, normal))));
    float alpha = ApplyLightingScale(SampleStage3(i).w * highlight);
    TestAlpha(alpha);
    return float4(ApplyFog(SampleStage2(i).xyz * i.color0.xyz, i.fog.x), alpha * FindShadowFactor(i.position));
}

// single pass: N.L to the 4th power times the lighting scale
float4 lamp_2afff55627e709db(GameInterpolants i) : SV_Target
{
    TestClipPlane(i.texcoord8);
    float3 lightVector = Expand(WeightLightVector(SampleStage0(i).xyz));
    float3 normal = Expand(SampleStage1(i).xyz);
    float alpha = ApplyLightingScale(SampleStage3(i).w * Pow4(saturate(dot(lightVector, normal))));
    TestAlpha(alpha);
    return float4(ApplyFog(SampleStage2(i).xyz * i.color0.xyz, i.fog.x), alpha * FindShadowFactor(i.position));
}

// single pass: N.L times the lighting scale
float4 lamp_e1d488a4887b4728(GameInterpolants i) : SV_Target
{
    TestClipPlane(i.texcoord8);
    float3 lightVector = Expand(WeightLightVector(SampleStage0(i).xyz));
    float3 normal = Expand(SampleStage1(i).xyz);
    float alpha = ApplyLightingScale(SampleStage3(i).w * saturate(dot(lightVector, normal)));
    TestAlpha(alpha);
    return float4(ApplyFog(SampleStage2(i).xyz * i.color0.xyz, i.fog.x), alpha * FindShadowFactor(i.position));
}

// single pass: N.L times the light vector texture's alpha and the lighting scale
float4 lamp_56e204312ac2da95(GameInterpolants i) : SV_Target
{
    TestClipPlane(i.texcoord8);
    float4 stage0 = SampleStage0(i);
    float3 lightVector = Expand(WeightLightVector(stage0.xyz));
    float3 normal = Expand(SampleStage1(i).xyz);
    float light = saturate(dot(lightVector, normal) * 0.5);
    float alpha = ApplyLightingScale(SampleStage3(i).w * stage0.w * light);
    TestAlpha(alpha);
    return float4(ApplyFog(SampleStage2(i).xyz * i.color0.xyz, i.fog.x), alpha * FindShadowFactor(i.position));
}

// first pass: the specular highlight from the normal map (stage 2) times the attenuation (stage 1 alpha)
float4 lamp_8a0eb51e677cfbb5(GameInterpolants i) : SV_Target
{
    TestClipPlane(i.texcoord8);
    float3 lightVector = Expand(WeightLightVector(SampleStage0(i).xyz));
    float3 normal = Expand(SampleStage2(i).xyz);
    float alpha = SampleStage1(i).w * Pow4(FindHighlight(saturate(dot(lightVector, normal))));
    TestAlpha(alpha);
    return float4(ApplyFog(i.color0.xyz, i.fog.x), alpha * FindShadowFactor(i.position));
}

// first pass: the specular highlight from the normal map (stage 2) times the attenuation (stage 1 alpha) and the
// lighting scale
float4 lamp_e6e10a5d3c16953a(GameInterpolants i) : SV_Target
{
    TestClipPlane(i.texcoord8);
    float3 lightVector = Expand(WeightLightVector(SampleStage0(i).xyz));
    float3 normal = Expand(SampleStage2(i).xyz);
    float alpha = ApplyLightingScale(SampleStage1(i).w * Pow4(FindHighlight(saturate(dot(lightVector, normal)))));
    TestAlpha(alpha);
    return float4(ApplyFog(i.color0.xyz, i.fog.x), alpha * FindShadowFactor(i.position));
}

// first pass: N.L to the 4th power from the normal map (stage 2) times the attenuation (stage 1 alpha)
float4 lamp_10482f3ee3154cf3(GameInterpolants i) : SV_Target
{
    TestClipPlane(i.texcoord8);
    float3 lightVector = Expand(WeightLightVector(SampleStage0(i).xyz));
    float3 normal = Expand(SampleStage2(i).xyz);
    float alpha = SampleStage1(i).w * Pow4(saturate(dot(lightVector, normal)));
    TestAlpha(alpha);
    return float4(ApplyFog(i.color0.xyz, i.fog.x), alpha * FindShadowFactor(i.position));
}

// first pass: N.L from the normal map (stage 2) times the attenuation (stage 1 alpha)
float4 lamp_a296886dedace78c(GameInterpolants i) : SV_Target
{
    TestClipPlane(i.texcoord8);
    float3 lightVector = Expand(WeightLightVector(SampleStage0(i).xyz));
    float3 normal = Expand(SampleStage2(i).xyz);
    float alpha = SampleStage1(i).w * saturate(dot(lightVector, normal) * 0.5);
    TestAlpha(alpha);
    return float4(ApplyFog(i.color0.xyz, i.fog.x), alpha * FindShadowFactor(i.position));
}

// first pass: N.L from the normal map (stage 2) times the attenuation (stage 1 alpha), plus the specular highlight
float4 lamp_b59e31a14e9ab909(GameInterpolants i) : SV_Target
{
    TestClipPlane(i.texcoord8);
    float3 lightVector = Expand(WeightLightVector(SampleStage0(i).xyz));
    float3 normal = Expand(SampleStage2(i).xyz);
    float facing = saturate(dot(lightVector, normal));
    float highlight = FindHighlight(facing);
    float alpha = saturate((facing * SampleStage1(i).w + highlight * highlight) * 0.5);
    TestAlpha(alpha);
    return float4(ApplyFog(i.color0.xyz, i.fog.x), alpha * FindShadowFactor(i.position));
}

// first pass: N.L to the 4th power from the normal map (stage 1)
float4 lamp_4d404501b58c7528(GameInterpolants i) : SV_Target
{
    TestClipPlane(i.texcoord8);
    float3 lightVector = Expand(WeightLightVector(SampleStage0(i).xyz));
    float3 normal = Expand(SampleStage1(i).xyz);
    float alpha = Pow4(saturate(dot(lightVector, normal)));
    TestAlpha(alpha);
    return float4(ApplyFog(i.color0.xyz, i.fog.x), alpha * FindShadowFactor(i.position));
}

// first pass: N.L from the normal map (stage 2) plus the specular highlight, with the colour from stage 1
float4 lamp_df1a2a5a18427518(GameInterpolants i) : SV_Target
{
    TestClipPlane(i.texcoord8);
    float3 lightVector = Expand(WeightLightVector(SampleStage0(i).xyz));
    float3 normal = Expand(SampleStage2(i).xyz);
    float facing = saturate(dot(lightVector, normal));
    float highlight = FindHighlight(facing);
    float alpha = (highlight * highlight + facing) * 0.5;
    TestAlpha(alpha);
    return float4(ApplyFog(SampleStage1(i).xyz * i.color0.xyz, i.fog.x), alpha * FindShadowFactor(i.position));
}

// first pass: the light vector (stage 0) blended toward a second vector (stage 1) by stage 2's alpha, N.L from the
// normal map (stage 3) times the lighting scale
float4 lamp_2f610e8f0e4bd0d7(GameInterpolants i) : SV_Target
{
    TestClipPlane(i.texcoord8);
    float3 lightVector = Expand(lerp(WeightLightVector(SampleStage0(i).xyz), SampleStage1(i).xyz, SampleStage2(i).w));
    float3 normal = Expand(SampleStage3(i).xyz);
    float alpha = ApplyLightingScale(saturate(dot(lightVector, normal) * 0.5));
    TestAlpha(alpha);
    return float4(ApplyFog(i.color0.xyz, i.fog.x), alpha * FindShadowFactor(i.position));
}

// first pass: as lamp_2f610e8f0e4bd0d7 with the specular highlight
float4 lamp_259bd118d0784679(GameInterpolants i) : SV_Target
{
    TestClipPlane(i.texcoord8);
    float3 lightVector = Expand(lerp(WeightLightVector(SampleStage0(i).xyz), SampleStage1(i).xyz, SampleStage2(i).w));
    float3 normal = Expand(SampleStage3(i).xyz);
    float alpha = ApplyLightingScale(Pow4(FindHighlight(saturate(dot(lightVector, normal)))));
    TestAlpha(alpha);
    return float4(ApplyFog(i.color0.xyz, i.fog.x), alpha * FindShadowFactor(i.position));
}

// first pass: as lamp_2f610e8f0e4bd0d7 with N.L to the 4th power
float4 lamp_25ec5d02d0951406(GameInterpolants i) : SV_Target
{
    TestClipPlane(i.texcoord8);
    float3 lightVector = Expand(lerp(WeightLightVector(SampleStage0(i).xyz), SampleStage1(i).xyz, SampleStage2(i).w));
    float3 normal = Expand(SampleStage3(i).xyz);
    float alpha = ApplyLightingScale(Pow4(saturate(dot(lightVector, normal))));
    TestAlpha(alpha);
    return float4(ApplyFog(i.color0.xyz, i.fog.x), alpha * FindShadowFactor(i.position));
}

// first pass: the light vector (stage 0) blended toward a second vector (stage 1) by stage 1's alpha, N.L from the
// normal map (stage 2) times the lighting scale
float4 lamp_853db6c1e619d6c7(GameInterpolants i) : SV_Target
{
    TestClipPlane(i.texcoord8);
    float4 stage1 = SampleStage1(i);
    float3 lightVector = Expand(lerp(WeightLightVector(SampleStage0(i).xyz), stage1.xyz, stage1.w));
    float3 normal = Expand(SampleStage2(i).xyz);
    float alpha = ApplyLightingScale(saturate(dot(lightVector, normal) * 0.5));
    TestAlpha(alpha);
    return float4(ApplyFog(i.color0.xyz, i.fog.x), alpha * FindShadowFactor(i.position));
}

// first pass: as lamp_853db6c1e619d6c7 with N.L to the 4th power
float4 lamp_c14a53d862d44a33(GameInterpolants i) : SV_Target
{
    TestClipPlane(i.texcoord8);
    float4 stage1 = SampleStage1(i);
    float3 lightVector = Expand(lerp(WeightLightVector(SampleStage0(i).xyz), stage1.xyz, stage1.w));
    float3 normal = Expand(SampleStage2(i).xyz);
    float alpha = ApplyLightingScale(Pow4(saturate(dot(lightVector, normal))));
    TestAlpha(alpha);
    return float4(ApplyFog(i.color0.xyz, i.fog.x), alpha * FindShadowFactor(i.position));
}

// first pass: as lamp_c14a53d862d44a33, also masked by stage 2's alpha blended from the material's mask value (c[1].w)
// by stage 1's alpha
float4 lamp_0c928bb772bbcf6d(GameInterpolants i) : SV_Target
{
    TestClipPlane(i.texcoord8);
    float4 stage1 = SampleStage1(i);
    float3 lightVector = Expand(lerp(WeightLightVector(SampleStage0(i).xyz), stage1.xyz, stage1.w));
    float mask = lerp(clamp(c[1].w, -1, 1), SampleStage2(i).w, stage1.w);
    float3 normal = Expand(SampleStage3(i).xyz);
    float alpha = ApplyLightingScale(mask * Pow4(saturate(dot(lightVector, normal))));
    TestAlpha(alpha);
    return float4(ApplyFog(i.color0.xyz, i.fog.x), alpha * FindShadowFactor(i.position));
}

// first pass: the light vector (stage 0) blended toward a second vector (stage 1) by the material's blend value
// (c[1].w), N.L from the normal map (stage 2) times the lighting scale
float4 lamp_9eb393f4abc8bb09(GameInterpolants i) : SV_Target
{
    TestClipPlane(i.texcoord8);
    float3 lightVector = Expand(lerp(WeightLightVector(SampleStage0(i).xyz), SampleStage1(i).xyz, clamp(c[1].w, -1, 1)));
    float3 normal = Expand(SampleStage2(i).xyz);
    float alpha = ApplyLightingScale(saturate(dot(lightVector, normal) * 0.5));
    TestAlpha(alpha);
    return float4(ApplyFog(i.color0.xyz, i.fog.x), alpha * FindShadowFactor(i.position));
}

)"
R"(// the colour pass of a two-pass lamp adds the light's colour times the alpha the first pass left, faded by the game's
// fade textures across the light's horizontal disc and along its vertical strip, multiplied as the game's shader
// multiplies them. StretchLight leaves their coordinates alone, so these stretch them to LIGHT_REACH times their reach.
// They draw with no shadow, which the first pass's alpha already holds.

// colour pass: the light's colour, faded by stages 0 and 1
float4 lamp_687c281bc7d62541(GameInterpolants i) : SV_Target
{
    TestClipPlane(i.texcoord8);
    float4 disc = SampleFade(Stage0, StageSampler0, i.texcoord0);
    float alpha = disc.w * i.color0.w;
    TestAlpha(alpha);
    return float4(ApplyFog(disc.xyz * SampleFade(Stage1, StageSampler1, i.texcoord1).xyz * i.color0.xyz, i.fog.x), alpha);
}

// colour pass: as lamp_687c281bc7d62541, times the colour of stage 2
float4 lamp_8f927125951d8372(GameInterpolants i) : SV_Target
{
    TestClipPlane(i.texcoord8);
    float4 disc = SampleFade(Stage0, StageSampler0, i.texcoord0);
    float alpha = disc.w * i.color0.w;
    TestAlpha(alpha);
    float3 fade = disc.xyz * SampleFade(Stage1, StageSampler1, i.texcoord1).xyz;
    return float4(ApplyFog(fade * SampleStage2(i).xyz * i.color0.xyz, i.fog.x), alpha);
}

// colour pass: the light's colour times the material's colour, blended from c[0] to c[1] by stage 0's alpha, faded by
// stages 1 and 2
float4 lamp_665754972b036446(GameInterpolants i) : SV_Target
{
    TestClipPlane(i.texcoord8);
    float4 material = lerp(clamp(c[0], -1, 1), clamp(c[1], -1, 1), SampleStage0(i).w);
    float4 lit = material * SampleFade(Stage1, StageSampler1, i.texcoord1) * SampleFade(Stage2, StageSampler2, i.texcoord2)
        * i.color0;
    TestAlpha(lit.w);
    return float4(ApplyFog(lit.xyz, i.fog.x), lit.w);
}
)"
R"(// the lamps added to a moving mesh, all drawn in one draw after the light buffer's lamps (DrawAddedLamps). Each lamp adds
// what a single-pass lamp adds: its faded colour times its light amount and shadow.

// the maps of every size, nearest first, for the added lamps, whose lamps can have maps of any size
TextureCubeArray<float> AddedMaps[3] : register(t16);

// AddedHeader: x the lamp count; y the single-pass lamp shader, an index into LAMP_SHADERS, whose light amount the lamps
// follow; z 1 when the mesh's texture coordinates go through rows c20 to c23; w 1 to light by the normal map in the
// mesh's tangent frame rather than by the surface's normal. AddedEye: the camera-relative eye, c0, with w 1 to light
// by the half vector toward it, as the game's highlight shaders do. AddedRange.x: where the draw's lamps start in
// AddedLampList; y 1 when the mesh keeps its normal in NORMAL and its second tangent in TEXCOORD4, 0 the other way round.
cbuffer AddedLamps : register(b5)
{
    float4 AddedHeader;
    float4 AddedEye;
    float4 AddedRange;
};
// every added lamp of the frame, three rows each: its camera-relative position with its reach in w, its colour, and its
// map's slot in x, -1 for none, with its fade's power (FindFade) in y
StructuredBuffer<float4> AddedLampList : register(t19);

struct AddedInterpolants
{
    float4 position : SV_Position;
    float3 relative : RELATIVE_POSITION;
    float3 tangent : TANGENT;
    float3 bitangent : BITANGENT;
    float3 normal : NORMAL;
    float4 texcoord : TEXCOORD0;
    float4 clip : CLIP_POSITION;
    float fog : FOG;
};

// repeats the position, clip coordinates, texture coordinates and fog of the game's character lamp and base light vertex
// shaders exactly, and passes on the camera-relative position and the tangent frame they turn the light vector into.
// The position is not precise: it compiles to the game's own instructions, so the driver rounds it as it rounds the
// game's shaders, whose depths match each other's exactly, as the kept passes' equal depth tests need.
AddedInterpolants added_vs(float3 vertex : POSITION, float3 normalInput : NORMAL, float4 texcoord : TEXCOORD0,
    float3 tangent : TEXCOORD3, float3 texcoord4 : TEXCOORD4)
{
    AddedInterpolants o;
    float4 p = float4(vertex, c[1].w);
    float x = dot(p, c[3]);
    float y = dot(p, c[4]);
    float z = dot(p, c[5]);
    float w = dot(p, c[6]);
    float2 offset = float2(1, w) * s[10].zw;
    float2 shifted = mad(offset, float2(w, -1), float2(x, y));
    o.position = float4(s[10].x > 0.5 ? shifted : float2(x, y), z, w);
    o.clip = float4(x, y, z, w);
    o.relative = float3(dot(p, c[7]), dot(p, c[8]), dot(p, c[9]));
    float3x3 world = float3x3(c[7].xyz, c[8].xyz, c[9].xyz);
    // the normal map's up axis, y, is the surface's normal
    bool normalInNormal = AddedRange.y > 0.5;
    o.tangent = mul(world, tangent);
    o.bitangent = mul(world, normalInNormal ? texcoord4 : normalInput);
    o.normal = mul(world, normalInNormal ? normalInput : texcoord4);
    o.texcoord = AddedHeader.z > 0.5
        ? float4(dot(texcoord, c[20]), dot(texcoord, c[21]), dot(texcoord, c[22]), dot(texcoord, c[23])) : texcoord;
    float depth = max(min((w - c[14].x) * c[14].w, c[1].x), c[0].x);
    float twice = c[1].x + c[1].x;
    o.fog = (twice / exp2(depth * depth) - twice) * c[14].z - c[14].y + c[1].x;
    return o;
}

// a lamp's light amount for how much the surface faces it, as the single-pass lamp shader at this index in LAMP_SHADERS
// computes it; stage0Alpha is the alpha of the normal map that one of them uses, and strip the lamp's fade along its
// vertical strip, which the game's shaders multiply in before the lighting scale
float FindLampAmount(uint shader, float facing, float stage0Alpha, float strip)
{
    switch (shader)
    {
        case 0: return ApplyLightingScale(strip * saturate(facing * 0.5));
        case 1: return strip * saturate(facing * 0.5);
        case 2: return ApplyLightingScale(strip * Pow4(FindHighlight(saturate(facing))));
        case 3: return ApplyLightingScale(strip * Pow4(saturate(facing)));
        case 4: return ApplyLightingScale(strip * saturate(facing));
        default: return ApplyLightingScale(strip * stage0Alpha * saturate(facing * 0.5));
    }
}

// the share of a lamp's light its shadow leaves at a camera-relative position, from its map in slot, or 1 when it has
// none. Each lamp turns its shadow's sample pattern by its own seed, so the pattern is not worked out once for every
// lamp and held in registers.
float FindLampShadow(float slot, float3 relative, float3 surfaceNormal, float2 seed)
{
    if (slot < 0 || CameraOffset.w < 0.5)
        return 1;

    float3 worldPosition = relative - CameraOffset.xyz;
    float3 lightPosition = Lights[(int)slot].xyz;
    float lit;
    [branch] if (slot < MIDDLE_START)
        lit = FindLitIn(AddedMaps[0], worldPosition, surfaceNormal, lightPosition, slot, seed);
    else if (slot < FAR_START)
        lit = FindLitIn(AddedMaps[1], worldPosition, surfaceNormal, lightPosition, slot, seed);
    else
        lit = FindLitIn(AddedMaps[2], worldPosition, surfaceNormal, lightPosition, slot, seed);
    return 1 - Params.z * (1 - lit);
}

// the light the added lamps give a pixel at this camera-relative position, faded as the game's single-pass lamps fade:
// each lamp's colour faded across its horizontal disc, times its light amount faded along its vertical strip, times its
// shadow. The light amount comes from the normal map value bump, turned by the tangent frame, when bumped is set, else
// from the surface's normal.
float3 SumAddedLamps(float3 relative, float3 surfaceNormal, bool bumped, float3x3 frame, float4 bump, float fog,
    float2 pixel)
{
    float3 sum = 0;
    float3 bumpNormal = Expand(WeightLightVector(bump.xyz));
    [loop]
    for (uint n = (uint)AddedRange.x; n < (uint)AddedRange.x + (uint)AddedHeader.x * 3; n += 3)
    {
        float4 lamp = AddedLampList[n];
        float3 toLamp = lamp.xyz - relative;
        float power = AddedLampList[n + 2].y;
        float disc = FindFade(dot(toLamp.xz, toLamp.xz) / (lamp.w * lamp.w), power);
        float strip = FindFade(toLamp.y * toLamp.y / (lamp.w * lamp.w), power);
        if (disc <= 0 || strip <= 0)
            continue;

        float distance = length(toLamp);
        float3 direction = toLamp / max(distance, 1e-4);
        float facing = dot(surfaceNormal, direction);
        if (bumped)
        {
            float3 toward = AddedEye.w > 0.5 ? normalize(AddedEye.xyz - relative) + direction : toLamp;
            float3 lightVector = mul(frame, toward);
            facing = dot(bumpNormal, lightVector * rsqrt(max(dot(lightVector, lightVector), 1e-12)));
        }
        float amount = FindLampAmount((uint)AddedHeader.y, facing, bump.w, strip);
        if (amount <= 0)
            continue;

        amount *= FindLampShadow(AddedLampList[n + 2].x, relative, surfaceNormal, pixel + n * float2(0.618034, 0.414214));
        sum += ApplyFog(disc * saturate(AddedLampList[n + 1].xyz), fog) * amount;
    }
    return sum;
}

// a normal at a camera-relative position turned toward the camera at the origin, which a drawn surface faces
float3 FaceCamera(float3 normal, float3 relative)
{
    return dot(normal, relative) > 0 ? -normal : normal;
}

// an added lamp draw's light, and a mark that the pixel's mesh has its lamps, which lamps_culled leaves out
struct AddedOutput
{
    float4 Light : SV_Target0;
    float Mark : SV_Target1;
};

// the added lamps on a mesh drawn with added_vs, by its normal map when AddedHeader.w is set, else by its normal
AddedOutput lamps_added(AddedInterpolants i)
{
    TestClipPlane(i.clip);
    float4 bump = SampleStage(Stage0, CubeStage0, StageSampler0, i.texcoord, s[1].x < 0.5);
    float3x3 frame = float3x3(i.tangent, i.normal, i.bitangent);
    AddedOutput output;
    output.Light = float4(SumAddedLamps(i.relative, FaceCamera(normalize(i.normal), i.relative), AddedHeader.w > 0.5, frame,
        bump, i.fog, i.position.xy), 1);
    output.Mark = 1;
    return output;
}

// the added lamps on a mesh drawn with its own vertex shader, whose outputs tell nothing of the surface: by the surface's
// normal at the pixel's position, from its screen position and depth
AddedOutput lamps_screen(GameInterpolants i)
{
    TestClipPlane(i.texcoord8);
    float3 relative = FindRelativePosition(i.position);
    AddedOutput output;
    output.Light = float4(SumAddedLamps(relative, FaceCamera(FindSurfaceNormal(relative), relative), false, (float3x3)0, 0,
        i.fog.x, i.position.xy), 1);
    output.Mark = 1;
    return output;
}
)"
R"(// the remembered lamps the game drew nothing for this frame, which leaves every surface in their reach without them,
// added to the light buffer from its depth in one pass after the added lamp draws (DrawCulledLamps)

// the light buffer's depth, and the marks the added lamp draws leave on the moving meshes they relight
Texture2D<float> CulledDepth : register(t20);
Texture2D<float> AddedMark : register(t21);

// CulledInverse: the inverse of the camera-relative world-to-clip transform, as rows. CulledSize: the light buffer's
// width and height and their inverses. CulledCount.x: the lamp count. CulledLamps: three rows per lamp, as
// AddedLampList holds them.
cbuffer Culled : register(b7)
{
    float4 CulledInverse[4];
    float4 CulledSize;
    float4 CulledCount;
    float4 CulledLamps[MAX_CULLED_LAMPS * 3];
};

float4 culled_vs(uint id : SV_VertexID) : SV_Position
{
    float2 uv = float2((id << 1) & 2, id & 2);
    return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
}

// the culled lamps' light at a pixel of the light buffer, by the surface's normal from its depth, with the light amount
// a kept base light draw follows, N.L, faded as SumAddedLamps fades it; a pixel an added lamp draw marked already has
// these lamps
float4 lamps_culled(float4 position : SV_Position) : SV_Target
{
    int3 pixel = int3(position.xy, 0);
    float depth = CulledDepth.Load(pixel);
    float2 uv = position.xy * CulledSize.zw;
    float4 clip = float4(uv.x * 2 - 1, 1 - uv.y * 2, depth, 1);
    float4 projected = float4(dot(CulledInverse[0], clip), dot(CulledInverse[1], clip), dot(CulledInverse[2], clip),
        dot(CulledInverse[3], clip));
    float3 relative = projected.xyz / projected.w;
    float3 normal = FaceCamera(FindSurfaceNormal(relative), relative);
    if (depth >= 1 || AddedMark.Load(pixel) > 0)
        discard;

    float3 sum = 0;
    [loop]
    for (uint n = 0; n < (uint)CulledCount.x * 3; n += 3)
    {
        float4 lamp = CulledLamps[n];
        float3 toLamp = lamp.xyz - relative;
        float power = CulledLamps[n + 2].y;
        float disc = FindFade(dot(toLamp.xz, toLamp.xz) / (lamp.w * lamp.w), power);
        float strip = FindFade(toLamp.y * toLamp.y / (lamp.w * lamp.w), power);
        if (disc <= 0 || strip <= 0)
            continue;

        float amount = FindLampAmount(1, dot(normal, toLamp / max(length(toLamp), 1e-4)), 0, strip);
        if (amount <= 0)
            continue;

        amount *= FindLampShadow(CulledLamps[n + 2].x, relative, normal, position.xy + n * float2(0.618034, 0.414214));
        sum += disc * saturate(CulledLamps[n + 1].xyz) * amount;
    }
    return float4(sum, 1);
}
)";

// the game's lamp pixel shaders that have a replacement, by the hash of their code, with the first of the two stages
// whose textures fade the light, across its horizontal disc and then along its vertical strip, or -1 for the first pass
// of a two-pass lamp, whose colour pass carries the fade
struct LampShader
{
    uint64_t Hash;
    const char *Entry;
    int FadeStage;
};

constexpr LampShader LAMP_SHADERS[] = {
    { 0x8b55e251966bcd36, "lamp_8b55e251966bcd36", 2 },
    { 0xd878054c72da7a41, "lamp_d878054c72da7a41", 2 },
    { 0x42ebee0d2ac17140, "lamp_42ebee0d2ac17140", 2 },
    { 0x2afff55627e709db, "lamp_2afff55627e709db", 2 },
    { 0xe1d488a4887b4728, "lamp_e1d488a4887b4728", 2 },
    { 0x56e204312ac2da95, "lamp_56e204312ac2da95", 2 },
    { 0x8a0eb51e677cfbb5, "lamp_8a0eb51e677cfbb5", -1 },
    { 0xe6e10a5d3c16953a, "lamp_e6e10a5d3c16953a", -1 },
    { 0x10482f3ee3154cf3, "lamp_10482f3ee3154cf3", -1 },
    { 0xa296886dedace78c, "lamp_a296886dedace78c", -1 },
    { 0xb59e31a14e9ab909, "lamp_b59e31a14e9ab909", -1 },
    { 0x4d404501b58c7528, "lamp_4d404501b58c7528", -1 },
    { 0xdf1a2a5a18427518, "lamp_df1a2a5a18427518", -1 },
    { 0x2f610e8f0e4bd0d7, "lamp_2f610e8f0e4bd0d7", -1 },
    { 0x259bd118d0784679, "lamp_259bd118d0784679", -1 },
    { 0x25ec5d02d0951406, "lamp_25ec5d02d0951406", -1 },
    { 0x853db6c1e619d6c7, "lamp_853db6c1e619d6c7", -1 },
    { 0xc14a53d862d44a33, "lamp_c14a53d862d44a33", -1 },
    { 0x0c928bb772bbcf6d, "lamp_0c928bb772bbcf6d", -1 },
    { 0x9eb393f4abc8bb09, "lamp_9eb393f4abc8bb09", -1 },
    { 0x687c281bc7d62541, "lamp_687c281bc7d62541", 0 },
    { 0x8f927125951d8372, "lamp_8f927125951d8372", 0 },
    { 0x665754972b036446, "lamp_665754972b036446", 1 },
};
constexpr size_t LAMP_SHADER_COUNT = std::size(LAMP_SHADERS);

enum class LightPass { None, Alpha, SinglePass };

static std::mutex Lock;
static std::unordered_set<uint64_t> AdditiveBlends, SourceAlphaBlends, PointLightShaders, StaticLightShaders;
// Captured directional/projected VS with admitted POSITION/world/clip layout, not the unknown skinned variant.
static std::unordered_set<uint64_t> ProjectedLightShaders;
// blends by source alpha over the target, as see-through surfaces are drawn
static std::unordered_set<uint64_t> AlphaBlends;
static std::unordered_set<uint64_t> SunVertexShaders, SunPixelShaders, SunDirectionShaders;

static std::atomic<bool> Enabled = true;
// draws lamps whose pixel shader has a replacement with that replacement, which applies the shadow in the same draw
static std::atomic<bool> OwnLampShaders = false;
// the game's pixel shaders that have a replacement, with the index of their replacement in LAMP_SHADERS
static std::unordered_map<uint64_t, size_t> ReplacedShaders;
static pipeline LampPixelShaders[LAMP_SHADER_COUNT];


static pipeline ShadowVertexShader, ShadowPixelShader, MultiplyAlphaBlend, AlphaOnlyBlend, ColorByAlphaBlend,
    PaintBlend;
// window_main, which stays null when it cannot be made, leaving windows unshadowed
static pipeline WindowPixelShader;
static bool CreateFailed;

// a lamp drawn into the light buffer this frame, or one remembered: its camera-relative position (c13), its colour (c11),
// its reach in world units, 0 while not known, its colour over its brightest channel, its map's slot, found once the
// frame's lamps are done, and its fade's power (FindFadePower), 0 while not known
struct FrameLamp
{
    DirectX::XMFLOAT4 Position;
    DirectX::XMFLOAT4 Colour;
    float Reach;
    DirectX::XMFLOAT3 Hue;
    int Slot;
    float FadePower;
};

// the rows of a lamp draw's vertex constants that can hold its fade texture projections
constexpr size_t FIRST_FADE_ROW = 20;
constexpr size_t FADE_ROW_END = 36;
constexpr UINT KEPT_VERTEX_SLOTS = 16;
// the pixel shader resources the lamp shaders sample: the stages' 2D textures, then their cube maps
constexpr UINT KEPT_VIEW_SLOTS[] = { 0, 1, 2, 3, 36, 37, 38, 39 };
constexpr UINT KEPT_SAMPLERS = 4;
// the most constants copied from one binding
constexpr UINT MAX_KEPT_CONSTANTS = 256;
// the most remembered lamps lamps_culled adds in a frame, the nearest first; its lamp list holds this many
constexpr int MAX_CULLED_LAMPS = 8;
// how far past its origin a moving mesh is taken to reach
constexpr float MESH_RADIUS = 2.0f;
// the game's fade covers a cylinder of a lamp's reach in radius and half height, whose rim is this many reaches away
constexpr float FADE_EXTENT = 1.41421356f;
// the fade power of the fade texture most lamps use, for a lamp whose own texture is not known or cannot be read
constexpr float COMMON_FADE_POWER = 1.05f;
// a lamp stays remembered while the camera is within its reach and this much more
constexpr float REMEMBER_MARGIN = 15.0f;
// a lamp that has been in view this many frames in a row without the game drawing it is off; the game stops drawing a
// lamp once the lamp itself leaves the view, though its reach is still in view
constexpr uint32_t MISSES_TO_FORGET = 5;
// the first LAMP_SHADERS, which are the single-pass ones whose light amount lamps_added repeats
constexpr size_t SINGLE_PASS_SHADERS = 6;
// the single-pass highlight lamp shader, kept only for a mesh with no other single-pass lamp pass
constexpr size_t HIGHLIGHT_SHADER = 2;
static_assert(LAMP_SHADERS[0].Hash == 0x8b55e251966bcd36 && LAMP_SHADERS[1].Hash == 0xd878054c72da7a41
    && LAMP_SHADERS[2].Hash == 0x42ebee0d2ac17140 && LAMP_SHADERS[3].Hash == 0x2afff55627e709db
    && LAMP_SHADERS[4].Hash == 0xe1d488a4887b4728 && LAMP_SHADERS[5].Hash == 0x56e204312ac2da95);
// the first of the last LAMP_SHADERS, the colour passes of two-pass lamps
constexpr size_t FIRST_COLOUR_PASS_SHADER = 20;
static_assert(LAMP_SHADERS[FIRST_COLOUR_PASS_SHADER].Hash == 0x687c281bc7d62541
    && LAMP_SHADERS[FIRST_COLOUR_PASS_SHADER + 1].Hash == 0x8f927125951d8372
    && LAMP_SHADERS[FIRST_COLOUR_PASS_SHADER + 2].Hash == 0x665754972b036446
    && FIRST_COLOUR_PASS_SHADER + 3 == LAMP_SHADER_COUNT);

// the game's character lamp and base light vertex shaders that added_vs repeats, by the hash of their code: whether they
// light by the half vector toward the eye, whether their texture coordinates go through rows c20 to c23, and whether
// the mesh keeps its normal in NORMAL rather than TEXCOORD4. They read a position, NORMAL, texture coordinates,
// TEXCOORD3 and TEXCOORD4, and light by the vector in the tangent frame of TEXCOORD3, the normal and the other one.
struct RepeatedVertexShader
{
    uint64_t Hash;
    bool HalfVector;
    bool MovedTexture;
    bool NormalInNormal;
};

constexpr RepeatedVertexShader REPEATED_VERTEX_SHADERS[] = {
    { 0x24323d07b950957a, false, false, false },
    { 0x42d48eff0f2c8602, false, false, true },
    { 0x8081e0ac16e300e4, true, false, false },
    { 0x6f29c6fb4fb07f3e, true, false, true },
    { 0x91d0178304b04e20, false, true, false },
    { 0xb97da48d07dbbf25, true, true, false },
    { 0x92f68ad8f88a96b3, false, false, false },
    { 0x4780c178ee4ee3ea, false, false, true },
};

// a moving mesh copy drawn into the light buffer this frame. Its first single-pass lamp pass whose pixel shader has a
// replacement is kept with everything it bound, so it can be drawn again for other lamps; until one comes, its base
// light draw is kept when a known lamp is near.
struct KeptDraw
{
    bool Base = false;
    size_t LampShader = 0;
    // the kept draw's vertex shader in REPEATED_VERTEX_SHADERS, or -1 when added_vs does not repeat it
    int Repeated = -1;
    // the lamps in FrameLamps the game drew the mesh for this frame
    std::vector<size_t> DrawnFor;
    uint64_t Mesh = 0;
    shadowmaps::DrawCall Call = {};
    // null while no draw is kept
    ComPtr<ID3D11VertexShader> VertexShader;
    ComPtr<ID3D11InputLayout> Layout;
    D3D11_PRIMITIVE_TOPOLOGY Topology = {};
    ComPtr<ID3D11Buffer> VertexBuffers[KEPT_VERTEX_SLOTS];
    UINT Strides[KEPT_VERTEX_SLOTS] = {}, Offsets[KEPT_VERTEX_SLOTS] = {};
    ComPtr<ID3D11Buffer> IndexBuffer;
    DXGI_FORMAT IndexFormat = {};
    UINT IndexOffset = 0;
    // copies of the constants bound at vertex b0 and b1, then pixel b0 and b1, each empty when nothing was bound
    std::vector<DirectX::XMFLOAT4> Constants[4];
    ComPtr<ID3D11ShaderResourceView> Views[std::size(KEPT_VIEW_SLOTS)];
    ComPtr<ID3D11SamplerState> Samplers[KEPT_SAMPLERS];
    ComPtr<ID3D11RasterizerState> Rasterizer;
    ComPtr<ID3D11DepthStencilState> Depth;
    UINT StencilReference = 0;
    ComPtr<ID3D11RenderTargetView> Target;
    ComPtr<ID3D11DepthStencilView> DepthView;
    D3D11_VIEWPORT Viewport = {};
};

static std::vector<FrameLamp> FrameLamps;
// this frame's moving mesh copies, by mesh and the eighth-of-a-unit cell of their origin
static std::unordered_map<uint64_t, KeptDraw> KeptDraws;
// moving meshes with a lamp pass this frame that could not be told apart from the mesh's other copies or whose lamp could
// not be read; they get no lamps added, since one could be added on top of the game's own
static std::unordered_set<uint64_t> UnsureMeshes;
// the game's vertex shaders that added_vs repeats, with their index in REPEATED_VERTEX_SHADERS
static std::unordered_map<uint64_t, int> RepeatedShaders;
// read-only copies of the game's depth-stencil states, which test as the game's do without writing depth or stencil,
// keyed by the game's state, which each holds so its address is not reused
static std::unordered_map<ID3D11DepthStencilState *,
    std::pair<ComPtr<ID3D11DepthStencilState>, ComPtr<ID3D11DepthStencilState>>> ReadOnlyDepthStates;
// the added lamps' shaders as native objects: added_vs with lamps_added, and lamps_screen for a kept draw whose vertex
// shader added_vs does not repeat
static ComPtr<ID3D11VertexShader> AddedVertexShader;
static ComPtr<ID3D11PixelShader> AddedPixelShader, ScreenPixelShader;
// adds the lamps' light to the colour and leaves alpha, and tests a base light draw's depth without writing
static ComPtr<ID3D11BlendState> AddedBlend;
static ComPtr<ID3D11DepthStencilState> AddedDepth;
// the constants of every added lamp draw, and the list of every added lamp, written once per frame
static ComPtr<ID3D11Buffer> AddedConstants, AddedLampBuffer;
static ComPtr<ID3D11ShaderResourceView> AddedLampView;
static UINT AddedConstantsSize, AddedLampRows;
// lamps_culled with its full-screen vertex shader, rasterizer and constants
static ComPtr<ID3D11VertexShader> CulledVertexShader;
static ComPtr<ID3D11PixelShader> CulledPixelShader;
static ComPtr<ID3D11RasterizerState> CulledRasterizer;
static ComPtr<ID3D11Buffer> CulledBuffer;
// the marks the added lamp draws leave on the moving meshes they relight, cleared on each frame they draw
static ComPtr<ID3D11Texture2D> AddedMarkTexture;
static ComPtr<ID3D11RenderTargetView> AddedMarkTarget;
static ComPtr<ID3D11ShaderResourceView> AddedMarkView;
static uint64_t AddedMarkFrame = UINT64_MAX;
// the remembered lamps the game drew nothing for this frame whose reach is in view, for DrawCulledLamps
static std::vector<FrameLamp> CulledLamps;
// the lamp fade textures FindFadePower has measured, held so that their addresses are not reused
struct MeasuredFade
{
    ComPtr<ID3D11Resource> Texture;
    float Power;
    ComPtr<ID3D11Texture2D> Pending;
    UINT Width=0,Height=0;
    uint64_t PollFrame=UINT64_MAX;
    bool Complete=false;
};
static std::vector<MeasuredFade> MeasuredFades;
constexpr size_t MAX_MEASURED_FADES=64;

// lamps_culled's constants, as its Culled buffer lays them out
struct CulledConstants
{
    DirectX::XMFLOAT4X4 Inverse;
    DirectX::XMFLOAT4 Size, Count;
    DirectX::XMFLOAT4 Lamps[MAX_CULLED_LAMPS * 3];
};

static std::string Disassemble(const shader_desc &desc)
{
    ID3DBlob *text = nullptr;
    if (FAILED(D3DDisassemble(desc.code, desc.code_size, 0, nullptr, &text)))
        return {};

    std::string listing(static_cast<const char *>(text->GetBufferPointer()), text->GetBufferSize());
    text->Release();
    return listing;
}

static void OnInitPipeline(device *, pipeline_layout, uint32_t count, const pipeline_subobject *subobjects, pipeline handle)
{
    const shadowmaps::CpuScope scope(shadowmaps::ShaderSortCpu);
    for (uint32_t i = 0; i < count; ++i)
    {
        if (!subobjects[i].data)
            continue;

        if (subobjects[i].type == pipeline_subobject_type::vertex_shader)
        {
            const auto &desc = *static_cast<const shader_desc *>(subobjects[i].data);
            const uint64_t hash = capture::HashCode(desc.code, desc.code_size);
            particle_emission::RememberShader(hash,handle.handle,true);
            if (projected_shadow_policy::AdmittedVertex(hash))
            {
                std::lock_guard guard(Lock);
                ProjectedLightShaders.insert(handle.handle);
            }
            for (size_t index = 0; index < std::size(REPEATED_VERTEX_SHADERS); ++index)
                if (REPEATED_VERTEX_SHADERS[index].Hash == hash)
                {
                    std::lock_guard guard(Lock);
                    RepeatedShaders[handle.handle] = static_cast<int>(index);
                }
            const std::string listing = Disassemble(desc);
            // lamp shaders pass the lamp's colour the same way, with its position where the sun's direction would be
            if (!sun_state_policy::KnownProjected(hash) && std::regex_search(listing, SUN_COLOUR_PATTERN) && !std::regex_search(listing, POINT_LIGHT_PATTERN))
            {
                std::lock_guard guard(Lock);
                SunVertexShaders.insert(handle.handle);
                if (std::regex_search(listing, SUN_DIRECTION_PATTERN))
                    SunDirectionShaders.insert(handle.handle);
            }
            if (!std::regex_search(listing, POINT_LIGHT_PATTERN))
                continue;

            const bool isStatic = std::regex_search(listing, WORLD_ROWS_PATTERN)
                && !std::regex_search(listing, INDEXED_CONSTANTS_PATTERN);
            std::lock_guard guard(Lock);
            PointLightShaders.insert(handle.handle);
            if (isStatic)
                StaticLightShaders.insert(handle.handle);
        }
        else if (subobjects[i].type == pipeline_subobject_type::pixel_shader)
        {
            const auto &desc = *static_cast<const shader_desc *>(subobjects[i].data);
            glow::ClassifyPixelShader(desc.code, desc.code_size, handle.handle);
            if (std::regex_search(Disassemble(desc), AMBIENT_PATTERN))
            {
                std::lock_guard guard(Lock);
                SunPixelShaders.insert(handle.handle);
            }
            const uint64_t hash = capture::HashCode(desc.code, desc.code_size);
            particle_emission::RememberShader(hash,handle.handle,false);
            for (size_t index = 0; index < LAMP_SHADER_COUNT; ++index)
                if (LAMP_SHADERS[index].Hash == hash)
                {
                    std::lock_guard guard(Lock);
                    ReplacedShaders[handle.handle] = index;
                }
        }
        else if (subobjects[i].type == pipeline_subobject_type::blend_state)
        {
            const auto &desc = *static_cast<const blend_desc *>(subobjects[i].data);
            if (desc.blend_enable[0] && desc.source_color_blend_factor[0] == blend_factor::source_alpha
                && desc.dest_color_blend_factor[0] == blend_factor::one_minus_source_alpha)
            {
                std::lock_guard guard(Lock);
                AlphaBlends.insert(handle.handle);
            }
            if (!desc.blend_enable[0] || desc.dest_color_blend_factor[0] != blend_factor::one)
                continue;

            std::lock_guard guard(Lock);
            AdditiveBlends.insert(handle.handle);
            if (desc.source_color_blend_factor[0] == blend_factor::source_alpha)
                SourceAlphaBlends.insert(handle.handle);
        }
    }
}

static void OnDestroyPipeline(device *, pipeline handle)
{
    std::lock_guard guard(Lock);
    AdditiveBlends.erase(handle.handle);
    SourceAlphaBlends.erase(handle.handle);
    AlphaBlends.erase(handle.handle);
    PointLightShaders.erase(handle.handle);
    ProjectedLightShaders.erase(handle.handle);
    StaticLightShaders.erase(handle.handle);
    SunVertexShaders.erase(handle.handle);
    SunPixelShaders.erase(handle.handle);
    SunDirectionShaders.erase(handle.handle);
    ReplacedShaders.erase(handle.handle);
    RepeatedShaders.erase(handle.handle);
    glow::ForgetPixelShader(handle.handle);
    particle_emission::ForgetShader(handle.handle);
}

static void DestroyPipeline(device *dev, pipeline &handle)
{
    if (handle.handle)
        dev->destroy_pipeline(handle);
    handle = {};
}

static void OnDestroyDevice(device *dev)
{
    projected_shadows::Reset();
    particle_emission::Reset();
    { std::lock_guard guard(Lock); ProjectedLightShaders.clear(); }
    DestroyPipeline(dev, ShadowVertexShader);
    DestroyPipeline(dev, ShadowPixelShader);
    DestroyPipeline(dev, WindowPixelShader);
    DestroyPipeline(dev, MultiplyAlphaBlend);
    DestroyPipeline(dev, AlphaOnlyBlend);
    DestroyPipeline(dev, ColorByAlphaBlend);
    DestroyPipeline(dev, PaintBlend);
    for (pipeline &shader : LampPixelShaders)
        DestroyPipeline(dev, shader);
    AddedVertexShader.Reset();
    AddedPixelShader.Reset();
    ScreenPixelShader.Reset();
    CulledVertexShader.Reset();
    CulledPixelShader.Reset();
    CulledRasterizer.Reset();
    CulledBuffer.Reset();
    AddedMarkTexture.Reset();
    AddedMarkTarget.Reset();
    AddedMarkView.Reset();
    AddedMarkFrame = UINT64_MAX;
    CulledLamps.clear();
    MeasuredFades.clear();
    AddedBlend.Reset();
    AddedDepth.Reset();
    AddedConstants.Reset();
    AddedConstantsSize = 0;
    AddedLampBuffer.Reset();
    AddedLampView.Reset();
    AddedLampRows = 0;
    ReadOnlyDepthStates.clear();
    AddedMarkFrame=UINT64_MAX;CulledLamps.clear();
    FrameLamps.clear();
    KeptDraws.clear();
    UnsureMeshes.clear();
    glow::Reset();
    shadowmaps::Reset();
    CreateFailed = false;
}

// an entry point of SHADOW_SHADER and its shader model
struct ShaderEntry
{
    const char *Entry;
    const char *Target;
};

static ComPtr<ID3DBlob> CompileShader(const ShaderEntry &shader)
{
    char lightCount[8];
    sprintf_s(lightCount, "%d", shadowmaps::MAX_LIGHTS);
    char matchDistance[16];
    sprintf_s(matchDistance, "%g", shadowmaps::LIGHT_MATCH_DISTANCE);
    char movingFaceSize[16];
    sprintf_s(movingFaceSize, "%u", shadowmaps::MOVING_FACE_SIZE);
    char middleStart[8], farStart[8], middleFaceSize[16], farFaceSize[16];
    sprintf_s(middleStart, "%d", shadowmaps::SIZE_STARTS[1]);
    sprintf_s(farStart, "%d", shadowmaps::SIZE_STARTS[2]);
    sprintf_s(middleFaceSize, "%u", shadowmaps::FACE_SIZES[1]);
    sprintf_s(farFaceSize, "%u", shadowmaps::FACE_SIZES[2]);
    char lightReach[16];
    sprintf_s(lightReach, "%g", shadowmaps::LIGHT_REACH);
    char culledLamps[8];
    sprintf_s(culledLamps, "%d", MAX_CULLED_LAMPS);
    static_assert(shadowmaps::SIZE_COUNT == 3);
    const D3D_SHADER_MACRO macros[] = { { "MAX_LIGHTS", lightCount }, { "LIGHT_MATCH_DISTANCE", matchDistance },
        { "MOVING_FACE_SIZE", movingFaceSize }, { "MIDDLE_START", middleStart }, { "FAR_START", farStart },
        { "MIDDLE_FACE_SIZE", middleFaceSize }, { "FAR_FACE_SIZE", farFaceSize },
        { "LIGHT_REACH", lightReach }, { "MAX_CULLED_LAMPS", culledLamps }, { nullptr, nullptr } };

    ComPtr<ID3DBlob> code, errors;
    if (SUCCEEDED(D3DCompile(SHADOW_SHADER, strlen(SHADOW_SHADER), "shadows", macros, nullptr, shader.Entry, shader.Target,
            D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors)))
        return code;

    if (errors)
        reshade::log::message(reshade::log::level::error, static_cast<const char *>(errors->GetBufferPointer()));
    return nullptr;
}

// the lamp shaders, then the shadow pass's vertex and pixel shader, then the added lamps' vertex shader and their two
// pixel shaders, then the culled lamps' vertex and pixel shader, then the window shadow's pixel shader
static std::vector<ShaderEntry> ListShaders()
{
    std::vector<ShaderEntry> shaders;
    for (const LampShader &lamp : LAMP_SHADERS)
        shaders.push_back({ lamp.Entry, "ps_5_0" });
    shaders.push_back({ "vs_main", "vs_5_0" });
    shaders.push_back({ "ps_main", "ps_5_0" });
    shaders.push_back({ "added_vs", "vs_5_0" });
    shaders.push_back({ "lamps_added", "ps_5_0" });
    shaders.push_back({ "lamps_screen", "ps_5_0" });
    shaders.push_back({ "culled_vs", "vs_5_0" });
    shaders.push_back({ "lamps_culled", "ps_5_0" });
    shaders.push_back({ "window_main", "ps_5_0" });
    return shaders;
}

// compiling every shader takes a moment, so it starts when the device is created and runs off the render thread
static std::future<std::vector<ComPtr<ID3DBlob>>> CompiledShaders;

static void OnInitDevice(device *)
{
    if (CompiledShaders.valid())
        return;

    CompiledShaders = std::async(std::launch::async, [] {
        std::vector<ComPtr<ID3DBlob>> compiled;
        for (const ShaderEntry &shader : ListShaders())
            compiled.push_back(CompileShader(shader));
        return compiled;
    });
}

static bool CreateShader(device *dev, ID3DBlob *code, pipeline_subobject_type type, pipeline &out)
{
    if (!code)
        return false;

    shader_desc shader = { code->GetBufferPointer(), code->GetBufferSize() };
    pipeline_subobject subobject = { type, 1, &shader };
    return dev->create_pipeline({}, 1, &subobject, &out);
}

static bool CreateBlend(device *dev, blend_factor source, blend_factor dest, blend_factor sourceAlpha,
    blend_factor destAlpha, uint8_t writeMask, pipeline &out)
{
    blend_desc blend;
    blend.blend_enable[0] = true;
    blend.source_color_blend_factor[0] = source;
    blend.dest_color_blend_factor[0] = dest;
    blend.source_alpha_blend_factor[0] = sourceAlpha;
    blend.dest_alpha_blend_factor[0] = destAlpha;
    blend.render_target_write_mask[0] = writeMask;
    pipeline_subobject subobject = { pipeline_subobject_type::blend_state, 1, &blend };
    return dev->create_pipeline({}, 1, &subobject, &out);
}

// creates the objects the added lamps draw with; AddedDepth stays null when any cannot be made, which leaves the added
// lamps off. Then the culled lamps' objects, CulledPixelShader last, which stays null when any of them cannot be made.
static void CreateAddedLampObjects(ID3D11Device *dev, const std::vector<ComPtr<ID3DBlob>> &compiled)
{
    // they come after the shadow pass's two shaders
    ID3DBlob *vertexCode = compiled[LAMP_SHADER_COUNT + 2].Get();
    ID3DBlob *addedCode = compiled[LAMP_SHADER_COUNT + 3].Get();
    ID3DBlob *screenCode = compiled[LAMP_SHADER_COUNT + 4].Get();
    if (!vertexCode || !addedCode || !screenCode
        || FAILED(dev->CreateVertexShader(vertexCode->GetBufferPointer(), vertexCode->GetBufferSize(), nullptr,
            &AddedVertexShader))
        || FAILED(dev->CreatePixelShader(addedCode->GetBufferPointer(), addedCode->GetBufferSize(), nullptr,
            &AddedPixelShader))
        || FAILED(dev->CreatePixelShader(screenCode->GetBufferPointer(), screenCode->GetBufferSize(), nullptr,
            &ScreenPixelShader)))
        return;

    // the mark, when its target is bound, is written as it is
    D3D11_BLEND_DESC blend = {};
    blend.IndependentBlendEnable = TRUE;
    blend.RenderTarget[0] = { TRUE, D3D11_BLEND_ONE, D3D11_BLEND_ONE, D3D11_BLEND_OP_ADD, D3D11_BLEND_ZERO,
        D3D11_BLEND_ONE, D3D11_BLEND_OP_ADD, D3D11_COLOR_WRITE_ENABLE_RED | D3D11_COLOR_WRITE_ENABLE_GREEN
            | D3D11_COLOR_WRITE_ENABLE_BLUE };
    blend.RenderTarget[1].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_RED;
    D3D11_DEPTH_STENCIL_DESC depth = {};
    depth.DepthEnable = TRUE;
    depth.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
    depth.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
    if (FAILED(dev->CreateBlendState(&blend, &AddedBlend)))
        return;
    dev->CreateDepthStencilState(&depth, &AddedDepth);

    ID3DBlob *culledVertexCode = compiled[LAMP_SHADER_COUNT + 5].Get();
    ID3DBlob *culledCode = compiled[LAMP_SHADER_COUNT + 6].Get();
    D3D11_RASTERIZER_DESC rasterizer = {};
    rasterizer.FillMode = D3D11_FILL_SOLID;
    rasterizer.CullMode = D3D11_CULL_NONE;
    rasterizer.DepthClipEnable = TRUE;
    rasterizer.ScissorEnable = TRUE;
    const D3D11_BUFFER_DESC culledDesc = { sizeof(CulledConstants), D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER };
    if (!culledVertexCode || !culledCode
        || FAILED(dev->CreateVertexShader(culledVertexCode->GetBufferPointer(), culledVertexCode->GetBufferSize(), nullptr,
            &CulledVertexShader))
        || FAILED(dev->CreateRasterizerState(&rasterizer, &CulledRasterizer))
        || FAILED(dev->CreateBuffer(&culledDesc, nullptr, &CulledBuffer)))
        return;
    dev->CreatePixelShader(culledCode->GetBufferPointer(), culledCode->GetBufferSize(), nullptr, &CulledPixelShader);
}

static bool CreateObjects(device *dev)
{
    if (MultiplyAlphaBlend.handle)
        return true;

    OnInitDevice(dev);
    const std::vector<ComPtr<ID3DBlob>> &compiled = CompiledShaders.get();
    for (size_t index = 0; index < LAMP_SHADER_COUNT; ++index)
        if (!CreateShader(dev, compiled[index].Get(), pipeline_subobject_type::pixel_shader, LampPixelShaders[index]))
            return false;

    CreateAddedLampObjects(reinterpret_cast<ID3D11Device *>(dev->get_native()), compiled);
    CreateShader(dev, compiled[LAMP_SHADER_COUNT + 7].Get(), pipeline_subobject_type::pixel_shader, WindowPixelShader);
    return CreateShader(dev, compiled[LAMP_SHADER_COUNT].Get(), pipeline_subobject_type::vertex_shader,
               ShadowVertexShader)
        && CreateShader(dev, compiled[LAMP_SHADER_COUNT + 1].Get(), pipeline_subobject_type::pixel_shader,
            ShadowPixelShader)
        // alpha = light amount
        && CreateBlend(dev, blend_factor::one, blend_factor::zero, blend_factor::one, blend_factor::zero, 0x8,
            AlphaOnlyBlend)
        // colour += light colour * alpha
        && CreateBlend(dev, blend_factor::dest_alpha, blend_factor::one, blend_factor::zero, blend_factor::one, 0x7,
            ColorByAlphaBlend)
        // alpha = alpha * shadow factor
        && CreateBlend(dev, blend_factor::zero, blend_factor::one, blend_factor::zero, blend_factor::source_alpha, 0x8,
            MultiplyAlphaBlend)
        // colour = shadow colour
        && CreateBlend(dev, blend_factor::one, blend_factor::zero, blend_factor::zero, blend_factor::one, 0x7,
            PaintBlend);
}

static LightPass GetLightPass(bool &isStatic)
{
    std::lock_guard guard(Lock);
    if (!PointLightShaders.contains(capture::VertexShader))
        return LightPass::None;

    isStatic = StaticLightShaders.contains(capture::VertexShader);
    if (SourceAlphaBlends.contains(capture::BlendState))
        return LightPass::SinglePass;
    if (!AdditiveBlends.contains(capture::BlendState))
        return LightPass::Alpha;
    return LightPass::None;
}

static void Issue(command_list *cmd, const shadowmaps::DrawCall &call)
{
    if (call.Indexed)
        cmd->draw_indexed(call.Count, call.Instances, call.First, call.BaseVertex, call.FirstInstance);
    else
        cmd->draw(call.Count, call.Instances, call.First, call.FirstInstance);
}

// the bound blend state with its blend factor and sample mask, which binding a blend pipeline resets
struct SavedBlend
{
    ComPtr<ID3D11BlendState> State;
    FLOAT Factor[4] = {};
    UINT SampleMask = 0;

    explicit SavedBlend(ID3D11DeviceContext *ctx) { ctx->OMGetBlendState(&State, Factor, &SampleMask); }

    void Restore(ID3D11DeviceContext *ctx) const { ctx->OMSetBlendState(State.Get(), Factor, SampleMask); }
};

// draws the shadow pass for the light in this slot, limited to the scissor rectangle when one is given
// multiplies the alpha a light pass left by the shadow of the light at a map slot, or, given a window, by that window's
// shadow
static void DrawShadow(command_list *cmd, const shadowmaps::DrawCall &call, const D3D11_RECT *scissor, int slot,
    const shadowmaps::Window *window = nullptr)
{
    auto *ctx = reinterpret_cast<ID3D11DeviceContext1 *>(cmd->get_native());
    SavedBlend blend(ctx);
    ComPtr<ID3D11RasterizerState> previousRasterizer;
    D3D11_RECT previousRects[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE];
    UINT previousRectCount = 0;
    if (scissor)
    {
        ctx->RSGetState(&previousRasterizer);
        ctx->RSGetScissorRects(&previousRectCount, nullptr);
        ctx->RSGetScissorRects(&previousRectCount, previousRects);
        D3D11_RECT rect = *scissor;
        D3D11_RASTERIZER_DESC previousDesc = {};
        if (previousRasterizer)
            previousRasterizer->GetDesc(&previousDesc);
        if (previousDesc.ScissorEnable && previousRectCount)
            rect = { std::max(rect.left, previousRects[0].left), std::max(rect.top, previousRects[0].top),
                std::min(rect.right, previousRects[0].right), std::min(rect.bottom, previousRects[0].bottom) };
        ID3D11RasterizerState *scissored = shadowmaps::GetScissorState(ctx, previousRasterizer.Get());
        if (!scissored || rect.left >= rect.right || rect.top >= rect.bottom)
            return;
        ctx->RSSetState(scissored);
        ctx->RSSetScissorRects(1, &rect);
    }

    ComPtr<ID3D11ShaderResourceView> previousMap, previousMovingMap;
    ComPtr<ID3D11SamplerState> previousSampler, previousPointSampler;
    ComPtr<ID3D11Buffer> previousTable, previousVertexTable, previousOffset;
    UINT previousFirst = 0, previousCount = 0, previousOffsetFirst = 0, previousOffsetCount = 0;
    UINT previousVertexFirst = 0, previousVertexCount = 0;
    ctx->PSGetShaderResources(MAP_SLOT, 1, &previousMap);
    ctx->PSGetShaderResources(MOVING_MAP_SLOT, 1, &previousMovingMap);
    ctx->PSGetSamplers(MAP_SLOT, 1, &previousSampler);
    ctx->PSGetSamplers(POINT_SAMPLER_SLOT, 1, &previousPointSampler);
    ctx->PSGetConstantBuffers1(TABLE_SLOT, 1, &previousTable, &previousFirst, &previousCount);
    ctx->VSGetConstantBuffers1(TABLE_SLOT, 1, &previousVertexTable, &previousVertexFirst, &previousVertexCount);
    ctx->VSGetConstantBuffers1(CAMERA_OFFSET_SLOT, 1, &previousOffset, &previousOffsetFirst, &previousOffsetCount);
    ComPtr<ID3D11ShaderResourceView> previousWindowMap;
    ComPtr<ID3D11Buffer> previousWindow;
    UINT previousWindowFirst = 0, previousWindowCount = 0;
    if (window)
    {
        ctx->PSGetShaderResources(WINDOW_MAP_SLOT, 1, &previousWindowMap);
        ctx->PSGetConstantBuffers1(WINDOW_SLOT, 1, &previousWindow, &previousWindowFirst, &previousWindowCount);
        ID3D11ShaderResourceView *windowMap = window->View.Get();
        ctx->PSSetShaderResources(WINDOW_MAP_SLOT, 1, &windowMap);
        ID3D11Buffer *windowConstants = window->Constants.Get();
        ctx->PSSetConstantBuffers(WINDOW_SLOT, 1, &windowConstants);
    }

    ID3D11ShaderResourceView *map = shadowmaps::CubeViews[shadowmaps::GetSizeIndex(slot)].Get();
    ID3D11SamplerState *sampler = shadowmaps::CompareSampler.Get();
    ID3D11Buffer *table = shadowmaps::LightTableBuffer.Get();
    ctx->PSSetShaderResources(MAP_SLOT, 1, &map);
    ID3D11ShaderResourceView *movingMap = shadowmaps::MovingView.Get();
    ctx->PSSetShaderResources(MOVING_MAP_SLOT, 1, &movingMap);
    ctx->PSSetSamplers(MAP_SLOT, 1, &sampler);
    ID3D11SamplerState *pointSampler = shadowmaps::PointSampler.Get();
    ctx->PSSetSamplers(POINT_SAMPLER_SLOT, 1, &pointSampler);
    ctx->PSSetConstantBuffers(TABLE_SLOT, 1, &table);
    ctx->VSSetConstantBuffers(TABLE_SLOT, 1, &table);
    ID3D11Buffer *offset = shadowmaps::CameraOffsetBuffer.Get();
    ctx->VSSetConstantBuffers(CAMERA_OFFSET_SLOT, 1, &offset);
    cmd->bind_pipeline(pipeline_stage::vertex_shader, ShadowVertexShader);
    cmd->bind_pipeline(pipeline_stage::pixel_shader, window ? WindowPixelShader : ShadowPixelShader);
    cmd->bind_pipeline(pipeline_stage::output_merger, shadowmaps::ShowColours ? PaintBlend : MultiplyAlphaBlend);
    Issue(cmd, call);
    if (window)
    {
        ID3D11ShaderResourceView *oldWindowMap = previousWindowMap.Get();
        ctx->PSSetShaderResources(WINDOW_MAP_SLOT, 1, &oldWindowMap);
        ID3D11Buffer *oldWindow = previousWindow.Get();
        if (previousWindowCount)
            ctx->PSSetConstantBuffers1(WINDOW_SLOT, 1, &oldWindow, &previousWindowFirst, &previousWindowCount);
        else
            ctx->PSSetConstantBuffers(WINDOW_SLOT, 1, &oldWindow);
    }

    ID3D11ShaderResourceView *oldMap = previousMap.Get();
    ID3D11SamplerState *oldSampler = previousSampler.Get();
    ID3D11Buffer *oldTable = previousTable.Get();
    ctx->PSSetShaderResources(MAP_SLOT, 1, &oldMap);
    ID3D11ShaderResourceView *oldMovingMap = previousMovingMap.Get();
    ctx->PSSetShaderResources(MOVING_MAP_SLOT, 1, &oldMovingMap);
    ctx->PSSetSamplers(MAP_SLOT, 1, &oldSampler);
    ID3D11SamplerState *oldPointSampler = previousPointSampler.Get();
    ctx->PSSetSamplers(POINT_SAMPLER_SLOT, 1, &oldPointSampler);
    if (previousCount)
        ctx->PSSetConstantBuffers1(TABLE_SLOT, 1, &oldTable, &previousFirst, &previousCount);
    else
        ctx->PSSetConstantBuffers(TABLE_SLOT, 1, &oldTable);
    ID3D11Buffer *oldVertexTable = previousVertexTable.Get();
    if (previousVertexCount)
        ctx->VSSetConstantBuffers1(TABLE_SLOT, 1, &oldVertexTable, &previousVertexFirst, &previousVertexCount);
    else
        ctx->VSSetConstantBuffers(TABLE_SLOT, 1, &oldVertexTable);
    ID3D11Buffer *oldOffset = previousOffset.Get();
    if (previousOffsetCount)
        ctx->VSSetConstantBuffers1(CAMERA_OFFSET_SLOT, 1, &oldOffset, &previousOffsetFirst, &previousOffsetCount);
    else
        ctx->VSSetConstantBuffers(CAMERA_OFFSET_SLOT, 1, &oldOffset);
    cmd->bind_pipeline(pipeline_stage::vertex_shader, { capture::VertexShader });
    cmd->bind_pipeline(pipeline_stage::pixel_shader, { capture::PixelShader });
    blend.Restore(ctx);
    if (scissor)
    {
        ctx->RSSetState(previousRasterizer.Get());
        ctx->RSSetScissorRects(previousRectCount, previousRects);
    }
}

// draws a lamp pass with the replacement for its pixel shader, which applies the light's shadow in the same draw; a slot
// of -1 draws it with no shadow
static void DrawWithLampShader(command_list *cmd, const shadowmaps::DrawCall &call, size_t index, int slot)
{
    auto *ctx = reinterpret_cast<ID3D11DeviceContext1 *>(cmd->get_native());
    ComPtr<ID3D11Buffer> previousBuffers[5];
    UINT previousFirsts[5] = {}, previousCounts[5] = {};
    ComPtr<ID3D11ShaderResourceView> previousMap, previousMovingMap;
    ComPtr<ID3D11SamplerState> previousSampler, previousPointSampler;
    ID3D11Buffer *previousBufferPointers[5] = {};
    ctx->PSGetConstantBuffers1(TABLE_SLOT, 5, previousBufferPointers, previousFirsts, previousCounts);
    for (int n = 0; n < 5; ++n)
        previousBuffers[n].Attach(previousBufferPointers[n]);
    ctx->PSGetShaderResources(MAP_SLOT, 1, &previousMap);
    ctx->PSGetShaderResources(MOVING_MAP_SLOT, 1, &previousMovingMap);
    ctx->PSGetSamplers(MAP_SLOT, 1, &previousSampler);
    ctx->PSGetSamplers(POINT_SAMPLER_SLOT, 1, &previousPointSampler);

    // b2 light table, b3 camera offset, b4 view, b6 the slot's block of the slot buffer, or its last block for no slot;
    // counts are in 16-byte constants, rounded up to the multiple of 16 the runtime requires
    static_assert(CAMERA_OFFSET_SLOT == TABLE_SLOT + 1 && VIEW_SLOT == TABLE_SLOT + 2 && DRAW_SLOT_SLOT == TABLE_SLOT + 4);
    constexpr UINT TABLE_CONSTANTS = (sizeof(shadowmaps::LightTable) / 16 + 15) / 16 * 16;
    ID3D11Buffer *buffers[5] = { shadowmaps::LightTableBuffer.Get(), shadowmaps::CameraOffsetBuffer.Get(),
        shadowmaps::ViewBuffer.Get(), nullptr, shadowmaps::SlotBuffer.Get() };
    UINT firsts[5] = { 0, 0, 0, 0, static_cast<UINT>(slot >= 0 ? slot : shadowmaps::MAX_LIGHTS) * 16 };
    UINT counts[5] = { TABLE_CONSTANTS, 16, 16, 16, 16 };
    ctx->PSSetConstantBuffers1(TABLE_SLOT, 5, buffers, firsts, counts);
    ID3D11ShaderResourceView *map = shadowmaps::CubeViews[shadowmaps::GetSizeIndex(slot)].Get();
    ID3D11ShaderResourceView *movingMap = shadowmaps::MovingView.Get();
    ID3D11SamplerState *sampler = shadowmaps::CompareSampler.Get();
    ID3D11SamplerState *pointSampler = shadowmaps::PointSampler.Get();
    ctx->PSSetShaderResources(MAP_SLOT, 1, &map);
    ctx->PSSetShaderResources(MOVING_MAP_SLOT, 1, &movingMap);
    ctx->PSSetSamplers(MAP_SLOT, 1, &sampler);
    ctx->PSSetSamplers(POINT_SAMPLER_SLOT, 1, &pointSampler);
    cmd->bind_pipeline(pipeline_stage::pixel_shader, LampPixelShaders[index]);
    Issue(cmd, call);

    cmd->bind_pipeline(pipeline_stage::pixel_shader, { capture::PixelShader });
    for (int n = 0; n < 5; ++n)
        previousBufferPointers[n] = previousBuffers[n].Get();
    ctx->PSSetConstantBuffers1(TABLE_SLOT, 5, previousBufferPointers, previousFirsts, previousCounts);
    ID3D11ShaderResourceView *oldMap = previousMap.Get();
    ID3D11ShaderResourceView *oldMovingMap = previousMovingMap.Get();
    ID3D11SamplerState *oldSampler = previousSampler.Get();
    ID3D11SamplerState *oldPointSampler = previousPointSampler.Get();
    ctx->PSSetShaderResources(MAP_SLOT, 1, &oldMap);
    ctx->PSSetShaderResources(MOVING_MAP_SLOT, 1, &oldMovingMap);
    ctx->PSSetSamplers(MAP_SLOT, 1, &oldSampler);
    ctx->PSSetSamplers(POINT_SAMPLER_SLOT, 1, &oldPointSampler);
}

static bool FindLampShader(size_t &index)
{
    std::lock_guard guard(Lock);
    auto replaced = ReplacedShaders.find(capture::PixelShader);
    if (replaced == ReplacedShaders.end())
        return false;

    index = replaced->second;
    return true;
}

// whether lamps are added to moving meshes: with point light shadows and own lamp shaders on, shadow colours off, and
// their objects made
static bool IsAddingLamps()
{
    return Enabled && OwnLampShaders && !shadowmaps::ShowColours && !CreateFailed && AddedDepth;
}

// whether a draw goes into the light buffer with the frame's camera
static bool IsLightBufferDraw(const DirectX::XMFLOAT4 (&rows)[shadowmaps::ROW_COUNT])
{
    DirectX::XMMATRIX worldToClip;
    return lightbuffer::LightBuffer && lightbuffer::CurrentTarget == lightbuffer::LightBuffer.Get()
        && lightbuffer::WorldToClipKnown && shadowmaps::FindWorldToClip(rows, worldToClip)
        && lightbuffer::IsSameCamera(worldToClip, lightbuffer::WorldToClip);
}

// copies the constants bound at a slot of the vertex or pixel stage, rows from start up to end with rows before start
// left zero, nothing when no buffer is bound; false when they cannot be read
static bool CopyConstants(ID3D11DeviceContext1 *ctx, bool pixel, UINT slot, std::vector<DirectX::XMFLOAT4> &out,
    UINT start = 0, UINT end = MAX_KEPT_CONSTANTS)
{
    ComPtr<ID3D11Buffer> constants;
    UINT first = 0, count = 0;
    if (pixel)
        ctx->PSGetConstantBuffers1(slot, 1, &constants, &first, &count);
    else
        ctx->VSGetConstantBuffers1(slot, 1, &constants, &first, &count);
    out.clear();
    if (!constants)
        return true;

    D3D11_BUFFER_DESC desc;
    constants->GetDesc(&desc);
    // a whole buffer bound has no count
    const UINT available = first < desc.ByteWidth / 16 ? desc.ByteWidth / 16 - first : 0;
    count = count ? std::min(count, available) : available;
    D3D11_MAPPED_SUBRESOURCE mapped;
    if (desc.Usage != D3D11_USAGE_DYNAMIC || !count || count > MAX_KEPT_CONSTANTS
        || FAILED(ctx->Map(constants.Get(), 0, D3D11_MAP_WRITE_NO_OVERWRITE, 0, &mapped)))
        return false;
    const auto *rows = reinterpret_cast<const DirectX::XMFLOAT4 *>(static_cast<const uint8_t *>(mapped.pData) + first * 16);
    out.assign(std::min(count, end), {});
    if (start < out.size())
        std::memcpy(out.data() + start, rows + start, (out.size() - start) * sizeof(DirectX::XMFLOAT4));
    ctx->Unmap(constants.Get(), 0);
    return true;
}

// the inverse of a draw's world rows c7 to c9, which takes camera-relative positions into its object's own space
static bool FindInverseWorld(const std::vector<DirectX::XMFLOAT4> &constants, DirectX::XMMATRIX &inverse)
{
    using namespace DirectX;
    if (constants.size() < 10)
        return false;

    const XMFLOAT4 *rows = constants.data();
    const XMMATRIX world(rows[7].x, rows[8].x, rows[9].x, 0, rows[7].y, rows[8].y, rows[9].y, 0, rows[7].z, rows[8].z,
        rows[9].z, 0, rows[7].w, rows[8].w, rows[9].w, 1);
    XMVECTOR determinant;
    inverse = XMMatrixInverse(&determinant, world);
    return std::abs(XMVectorGetX(determinant)) > 1e-12f;
}

// whether a projection row puts the light, in object space, at 0.5
static bool IsFadeRow(const DirectX::XMFLOAT4 &projection, DirectX::FXMVECTOR light)
{
    using namespace DirectX;
    const XMVECTOR gradient = XMVectorSet(projection.x, projection.y, projection.z, 0);
    return !XMVector3Equal(gradient, XMVectorZero())
        && std::abs(XMVectorGetX(XMVector3Dot(gradient, light)) + projection.w - 0.5f) <= 0.01f;
}

// a lamp draw's reach in world units, from its vertex constants: a fade row moves by 1 / (2 * reach) per world unit, and
// the rows across the light's horizontal disc come in pairs that move alike; 0 when it has none
static float FindLampReach(const std::vector<DirectX::XMFLOAT4> &constants)
{
    using namespace DirectX;
    XMMATRIX inverse;
    if (constants.size() < FADE_ROW_END || !FindInverseWorld(constants, inverse))
        return 0;

    const XMVECTOR light = XMVector3TransformCoord(XMLoadFloat4(&constants[13]), inverse);
    const XMMATRIX inverseTranspose = XMMatrixTranspose(inverse);
    // a fade row's change per world unit, else 0
    const auto findSlope = [&](const XMFLOAT4 &projection) {
        if (!IsFadeRow(projection, light))
            return 0.0f;
        return XMVectorGetX(XMVector3Length(
            XMVector3TransformNormal(XMVectorSet(projection.x, projection.y, projection.z, 0), inverseTranspose)));
    };
    float reach = 0;
    for (size_t row = FIRST_FADE_ROW; row + 1 < FADE_ROW_END; ++row)
    {
        const float slope = findSlope(constants[row]);
        if (slope > 0 && std::abs(findSlope(constants[row + 1]) - slope) <= 0.01f * slope)
            reach = std::max(reach, 0.5f / slope);
    }
    return reach;
}

// Measures each native strip once, with at most64 retained sources and one nonblocking
// read attempt per source/frame. Until ready (or on any unsupported/failure leaf),
// native fade sampling remains exact for original lamp draws; replayed lamps use
// the upstream common approximation. No Map(READ,0) GPU wait is introduced.
static float FindFadePower(ID3D11DeviceContext *ctx, int stage)
{
    if(stage<0)return 0;
    ComPtr<ID3D11ShaderResourceView> view;ctx->PSGetShaderResources(stage,1,&view);
    if(!view)return 0;
    ComPtr<ID3D11Resource> resource;view->GetResource(&resource);
    auto found=std::find_if(MeasuredFades.begin(),MeasuredFades.end(),[&](const auto &f){return f.Texture==resource;});
    if(found==MeasuredFades.end())
    {
        if(MeasuredFades.size()>=MAX_MEASURED_FADES)return COMMON_FADE_POWER;
        MeasuredFades.push_back({resource,COMMON_FADE_POWER});found=std::prev(MeasuredFades.end());
        auto &f=*found;
        ComPtr<ID3D11Texture2D> texture;D3D11_SHADER_RESOURCE_VIEW_DESC vd{};view->GetDesc(&vd);
        if(FAILED(resource.As(&texture))||vd.ViewDimension!=D3D11_SRV_DIMENSION_TEXTURE2D
            ||vd.Texture2D.MostDetailedMip!=0){f.Complete=true;return f.Power;}
        D3D11_TEXTURE2D_DESC desc{};texture->GetDesc(&desc);
        if((desc.Format!=DXGI_FORMAT_B8G8R8A8_UNORM&&desc.Format!=DXGI_FORMAT_R8G8B8A8_UNORM)
            ||desc.Width<4||desc.SampleDesc.Count!=1||desc.ArraySize!=1){f.Complete=true;return f.Power;}
        ComPtr<ID3D11Device> dev;ctx->GetDevice(&dev);
        f.Width=desc.Width;f.Height=desc.Height;
        desc.MipLevels=1;desc.Usage=D3D11_USAGE_STAGING;desc.BindFlags=0;
        desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;desc.MiscFlags=0;
        if(FAILED(dev->CreateTexture2D(&desc,nullptr,&f.Pending))){f.Complete=true;return f.Power;}
        ctx->CopySubresourceRegion(f.Pending.Get(),0,0,0,0,texture.Get(),0,nullptr);
        // Defer even the first nonblocking read until a later frame.
        f.PollFrame=shadowmaps::FrameNumber;return f.Power;
    }
    auto &f=*found;
    if(f.Complete||!f.Pending||f.PollFrame==shadowmaps::FrameNumber)return f.Power;
    f.PollFrame=shadowmaps::FrameNumber;
    D3D11_MAPPED_SUBRESOURCE mapped{};
    const HRESULT read=ctx->Map(f.Pending.Get(),0,D3D11_MAP_READ,D3D11_MAP_FLAG_DO_NOT_WAIT,&mapped);
    if(read==DXGI_ERROR_WAS_STILL_DRAWING)return f.Power;
    if(SUCCEEDED(read))
    {
        const auto *row=static_cast<const uint8_t *>(mapped.pData)+f.Height/2*mapped.RowPitch;
        const UINT q=f.Width/4;
        const float half=(row[(q-1)*4+3]+row[q*4+3]+row[(q*3-1)*4+3]+row[q*3*4+3])/(4*255.0f);
        ctx->Unmap(f.Pending.Get(),0);
        if(half>0&&half<1)f.Power=std::log(half)/std::log(.75f);
    }
    f.Pending.Reset();f.Complete=true;return f.Power;
}

// the index in FrameLamps of a lamp draw's lamp, added when new; until its reach is known it is read from the draw,
// times stretch for a draw whose fade rows StretchLight left alone, and until its fade's power is known it is measured
// from the strip after fadeStage, the draw's first fade stage, or -1 for none
static size_t NoteLamp(ID3D11DeviceContext1 *ctx, const DirectX::XMFLOAT4 (&rows)[shadowmaps::ROW_COUNT], float stretch,
    int fadeStage)
{
    const DirectX::XMFLOAT4 &position = rows[13 - shadowmaps::FIRST_ROW];
    const DirectX::XMFLOAT4 &colour = rows[11 - shadowmaps::FIRST_ROW];
    const DirectX::XMFLOAT3 hue = shadowmaps::FindHue(colour);
    size_t index = 0;
    while (index < FrameLamps.size()
        && (shadowmaps::Distance({ position.x, position.y, position.z }, { FrameLamps[index].Position.x,
                FrameLamps[index].Position.y, FrameLamps[index].Position.z }) >= shadowmaps::LIGHT_MERGE_DISTANCE
            || !shadowmaps::IsSameHue(FrameLamps[index].Hue, hue)))
        ++index;
    if (index == FrameLamps.size())
        FrameLamps.push_back({ position, colour, 0, hue, -1, 0 });
    if (FrameLamps[index].FadePower <= 0)
    {
        FrameLamps[index].FadePower = FindFadePower(ctx, fadeStage < 0 ? -1 : fadeStage + 1);
        if (FrameLamps[index].FadePower > 0 && capture::State == capture::Phase::Capturing && capture::Log)
            fprintf(capture::Log, "  lamp fade power %.3f\n", FrameLamps[index].FadePower);
    }

    // the world rows, the light's position and the fade rows
    std::vector<DirectX::XMFLOAT4> constants;
    if (FrameLamps[index].Reach <= 0 && CopyConstants(ctx, false, 0, constants, 7, FADE_ROW_END))
        FrameLamps[index].Reach = FindLampReach(constants) * stretch;
    return index;
}

// keeps everything a draw binds, with copies of its constants; false when its constants cannot be read
// ponytail: buffers are kept, not their contents, so a vertex or index buffer rewritten before the light buffer is
// finished is drawn with its new contents; the game fills them at the start of the frame
static bool KeepDraw(ID3D11DeviceContext1 *ctx, const shadowmaps::DrawCall &call, KeptDraw &kept)
{
    for (UINT n = 0; n < 4; ++n)
        if (!CopyConstants(ctx, n >= 2, n % 2, kept.Constants[n]))
            return false;

    kept.Call = call;
    ctx->VSGetShader(&kept.VertexShader, nullptr, nullptr);
    {
        std::lock_guard guard(Lock);
        auto repeated = RepeatedShaders.find(capture::VertexShader);
        kept.Repeated = repeated == RepeatedShaders.end() ? -1 : repeated->second;
    }
    ctx->IAGetInputLayout(&kept.Layout);
    ctx->IAGetPrimitiveTopology(&kept.Topology);
    ID3D11Buffer *vertexBuffers[KEPT_VERTEX_SLOTS] = {};
    ctx->IAGetVertexBuffers(0, KEPT_VERTEX_SLOTS, vertexBuffers, kept.Strides, kept.Offsets);
    for (UINT n = 0; n < KEPT_VERTEX_SLOTS; ++n)
        kept.VertexBuffers[n].Attach(vertexBuffers[n]);
    ctx->IAGetIndexBuffer(&kept.IndexBuffer, &kept.IndexFormat, &kept.IndexOffset);
    for (size_t n = 0; n < std::size(KEPT_VIEW_SLOTS); ++n)
        ctx->PSGetShaderResources(KEPT_VIEW_SLOTS[n], 1, &kept.Views[n]);
    ID3D11SamplerState *samplers[KEPT_SAMPLERS] = {};
    ctx->PSGetSamplers(0, KEPT_SAMPLERS, samplers);
    for (UINT n = 0; n < KEPT_SAMPLERS; ++n)
        kept.Samplers[n].Attach(samplers[n]);
    ctx->RSGetState(&kept.Rasterizer);
    UINT count = 1;
    ctx->RSGetViewports(&count, &kept.Viewport);
    ctx->OMGetDepthStencilState(&kept.Depth, &kept.StencilReference);
    ctx->OMGetRenderTargets(1, &kept.Target, &kept.DepthView);
    return true;
}

// the mesh a draw draws, keyed as Record keys it
static shadowmaps::Geometry FindDrawnMesh(ID3D11DeviceContext *ctx, const shadowmaps::DrawCall &call)
{
    shadowmaps::Geometry mesh = {};
    ctx->IAGetVertexBuffers(0, 1, &mesh.VertexBuffer, &mesh.Stride, &mesh.VertexOffset);
    ctx->IAGetIndexBuffer(&mesh.IndexBuffer, &mesh.IndexFormat, &mesh.IndexOffset);
    mesh.Call = call;
    mesh.Key = shadowmaps::HashMesh(mesh);
    return mesh;
}

// whether a mesh moves: one seen moving, or one whose vertices the game wrote this frame, as it does to pose characters.
// Scenery in a dynamic vertex buffer the game filled once stays as the game lights it.
static bool IsMoving(const shadowmaps::Geometry &mesh)
{
    if (shadowmaps::MovingMeshes.contains(mesh.Key))
        return true;

    D3D11_BUFFER_DESC desc = {};
    if (mesh.VertexBuffer)
        mesh.VertexBuffer->GetDesc(&desc);
    auto written = shadowmaps::BufferWrites.find(reinterpret_cast<uint64_t>(mesh.VertexBuffer.Get()));
    return desc.Usage == D3D11_USAGE_DYNAMIC && written != shadowmaps::BufferWrites.end()
        && written->second == shadowmaps::FrameNumber;
}

// a moving mesh copy's key: its mesh and the eighth-of-a-unit cell of its camera-relative origin
static uint64_t FindCopyKey(uint64_t mesh, const DirectX::XMFLOAT4 (&rows)[shadowmaps::ROW_COUNT])
{
    return shadowmaps::HashCaster(mesh, std::llround(rows[4].w * 8.0), std::llround(rows[5].w * 8.0),
        std::llround(rows[6].w * 8.0));
}

// notes a lamp pass into the light buffer, with its rows when they could be read: its lamp, and for a moving mesh that
// the game drew it for that lamp. The mesh's first single-pass lamp pass whose pixel shader has a replacement is kept, in
// place of a kept base light draw, and a diffuse one in place of a highlight one. A moving mesh whose lamp pass cannot
// be told apart from its other copies, or whose lamp cannot be read, is noted as unsure.
static void NoteLampDraw(ID3D11DeviceContext *ctx, const shadowmaps::DrawCall &call,
    const DirectX::XMFLOAT4 (*rows)[shadowmaps::ROW_COUNT], bool singlePass)
{
    ComPtr<ID3D11DeviceContext1> ctx1;
    if (!IsAddingLamps() || !lightbuffer::LightBuffer || lightbuffer::CurrentTarget != lightbuffer::LightBuffer.Get())
        return;

    const shadowmaps::Geometry mesh = FindDrawnMesh(ctx, call);
    const bool moving = IsMoving(mesh);
    if (!rows || !IsLightBufferDraw(*rows) || call.Instances != 1 || FAILED(ctx->QueryInterface(IID_PPV_ARGS(&ctx1))))
    {
        if (moving)
            UnsureMeshes.insert(mesh.Key);
        return;
    }

    size_t lampShader = 0;
    const bool replaced = FindLampShader(lampShader);
    const size_t lamp = NoteLamp(ctx1.Get(), *rows, 1, replaced ? LAMP_SHADERS[lampShader].FadeStage : -1);
    if (!moving)
        return;
    KeptDraw &kept = KeptDraws[FindCopyKey(mesh.Key, *rows)];
    if (std::find(kept.DrawnFor.begin(), kept.DrawnFor.end(), lamp) == kept.DrawnFor.end())
        kept.DrawnFor.push_back(lamp);
    if (!singlePass || !replaced || lampShader >= SINGLE_PASS_SHADERS
        || (kept.VertexShader && !kept.Base && (kept.LampShader != HIGHLIGHT_SHADER || lampShader == HIGHLIGHT_SHADER)))
        return;

    KeptDraw draw;
    if (!KeepDraw(ctx1.Get(), call, draw))
        return;
    draw.LampShader = lampShader;
    draw.Mesh = mesh.Key;
    draw.DrawnFor = std::move(kept.DrawnFor);
    kept = std::move(draw);
}

// notes a two-pass lamp's colour pass into the light buffer, with its rows, drawn with the lamp shader at lampShader,
// which carries its lamp's fade rows and textures and so its reach and fade
static void NoteColourPass(ID3D11DeviceContext *ctx, const DirectX::XMFLOAT4 (&rows)[shadowmaps::ROW_COUNT],
    size_t lampShader)
{
    ComPtr<ID3D11DeviceContext1> ctx1;
    if (IsAddingLamps() && IsLightBufferDraw(rows) && SUCCEEDED(ctx->QueryInterface(IID_PPV_ARGS(&ctx1))))
        NoteLamp(ctx1.Get(), rows, shadowmaps::LIGHT_REACH, LAMP_SHADERS[lampShader].FadeStage);
}

// whether a lamp remembered from earlier frames is still on: with a known reach, drawn by the game or with the camera
// within its reach and REMEMBER_MARGIN in the last REMEMBERED_LAMP_FRAMES, and not left undrawn for MISSES_TO_FORGET
// frames in a row while it was in view
static bool IsRemembered(const shadowmaps::Light &light)
{
    return !light.Glow && light.Reach > 0 && light.LastInRange + shadowmaps::REMEMBERED_LAMP_FRAMES >= shadowmaps::FrameNumber
        && light.MissedInView < MISSES_TO_FORGET;
}

// whether any of the sphere of this radius about an absolute position is in the light buffer's view, by the planes of
// its camera
static bool IsSphereInView(const DirectX::XMFLOAT3 &centre, float radius)
{
    using namespace DirectX;
    if (!shadowmaps::ProbeDepth)
        return false;

    XMFLOAT4X4 clip;
    XMStoreFloat4x4(&clip, shadowmaps::ProbeWorldToClip);
    const XMVECTOR x = XMVectorSet(clip._11, clip._12, clip._13, clip._14);
    const XMVECTOR y = XMVectorSet(clip._21, clip._22, clip._23, clip._24);
    const XMVECTOR z = XMVectorSet(clip._31, clip._32, clip._33, clip._34);
    const XMVECTOR w = XMVectorSet(clip._41, clip._42, clip._43, clip._44);
    const XMVECTOR point = XMVectorSet(centre.x - shadowmaps::Camera.x, centre.y - shadowmaps::Camera.y,
        centre.z - shadowmaps::Camera.z, 1);
    const XMVECTOR planes[] = { w + x, w - x, w + y, w - y, z, w - z };
    for (const XMVECTOR &plane : planes)
        if (XMVectorGetX(XMVector4Dot(plane, point)) < -radius * XMVectorGetX(XMVector3Length(plane)))
            return false;
    return true;
}

// keeps a moving mesh's base light draw when a lamp known from earlier frames reaches it, for a mesh the game draws for
// no lamp this frame
// ponytail: lamp passes are matched to their mesh only for vertex shaders that read world rows directly, as every lamp
// vertex shader seen so far does; the lamps of a mesh drawn with a skinned one would be added again from here
static void KeepBaseDraw(ID3D11DeviceContext *ctx, const shadowmaps::DrawCall &call,
    const DirectX::XMFLOAT4 (&rows)[shadowmaps::ROW_COUNT])
{
    ComPtr<ID3D11DeviceContext1> ctx1;
    if (!IsAddingLamps() || call.Instances != 1)
        return;
    const shadowmaps::Geometry mesh = FindDrawnMesh(ctx, call);
    if (!IsMoving(mesh) || FAILED(ctx->QueryInterface(IID_PPV_ARGS(&ctx1))))
        return;
    const uint64_t key = FindCopyKey(mesh.Key, rows);
    if (KeptDraws.contains(key))
        return;

    const DirectX::XMFLOAT3 origin = { rows[4].w - shadowmaps::CurrentOffset.x, rows[5].w - shadowmaps::CurrentOffset.y,
        rows[6].w - shadowmaps::CurrentOffset.z };
    const bool reached = std::any_of(shadowmaps::Lights.begin(), shadowmaps::Lights.end(), [&origin](const auto &entry) {
        const shadowmaps::Light &light = entry.second;
        return IsRemembered(light)
            && shadowmaps::Distance(light.Position, origin) < light.Reach * FADE_EXTENT + MESH_RADIUS;
    });
    KeptDraw draw;
    if (!reached || !KeepDraw(ctx1.Get(), call, draw))
        return;
    draw.Base = true;
    draw.Mesh = mesh.Key;
    KeptDraws.emplace(key, std::move(draw));
}

// draws a two-pass lamp's colour pass, which carries the lamp's fade, with the replacement for its pixel shader. A
// directional light, such as the light through a stained-glass window, has its direction in c13 with w 0; its passes
// use the lamp pixel shaders too, and are left to the game.
static bool DrawColourPass(command_list *cmd, const shadowmaps::DrawCall &call)
{
    size_t lampShader = 0;
    auto *ctx = reinterpret_cast<ID3D11DeviceContext *>(cmd->get_native());
    DirectX::XMFLOAT4 rows[shadowmaps::ROW_COUNT];
    if (!Enabled || !OwnLampShaders || shadowmaps::ShowColours || CreateFailed || !FindLampShader(lampShader)
        || lampShader < FIRST_COLOUR_PASS_SHADER || !shadowmaps::ReadDrawRows(ctx, rows)
        || rows[13 - shadowmaps::FIRST_ROW].w == 0)
        return false;
    if (!CreateObjects(cmd->get_device()))
    {
        CreateFailed = true;
        return false;
    }
    NoteColourPass(ctx, rows, lampShader);

    cmd->bind_pipeline(pipeline_stage::pixel_shader, LampPixelShaders[lampShader]);
    Issue(cmd, call);
    cmd->bind_pipeline(pipeline_stage::pixel_shader, { capture::PixelShader });
    return true;
}

// shadows a directional light the game projects through a window, from its window's map (shadowmaps::Window). Its
// picture and fade come in the two stages from the lamp shader's FadeStage, through rows in the object's own space,
// which are turned into rows on absolute positions. A single pass is split into an alpha pass, the shadow and a colour
// pass, and true returned. The colour pass of a two-pass light has the shadow multiply the alpha its first pass left,
// and the game then draws it. Over characters the game draws both straight onto the screen's image rather than into the
// light buffer. With shadow colours on, the shadow is painted in colours instead.
static bool DrawWindowShadow(command_list *cmd, const shadowmaps::DrawCall &call)
{
    using namespace DirectX;
    size_t lampShader = 0;
    bool singlePass = false;
    {
        std::lock_guard guard(Lock);
        if(!ProjectedLightShaders.contains(capture::VertexShader))return false; // Exact captured VS layout; unknown/skinned stays native.
        singlePass = SourceAlphaBlends.contains(capture::BlendState);
    }
    auto *ctx = reinterpret_cast<ID3D11DeviceContext *>(cmd->get_native());
    ComPtr<ID3D11DeviceContext1> ctx1;
    XMFLOAT4 rows[shadowmaps::ROW_COUNT];
    if (!Enabled || CreateFailed || !shadowmaps::DrawState || !shadowmaps::CameraFound
        ||call.Instances!=1||!call.Count||!FindLampShader(lampShader)
        || LAMP_SHADERS[lampShader].FadeStage < 0 || !shadowmaps::ReadDrawRows(ctx, rows)
        || rows[13 - shadowmaps::FIRST_ROW].w != 0 || FAILED(ctx->QueryInterface(IID_PPV_ARGS(&ctx1))))
        return false;

    const auto skip = [](const char *reason) {
        if (capture::State == capture::Phase::Capturing && capture::Log)
            fprintf(capture::Log, "  window light: no shadow, %s\n", reason);
        return false;
    };
    if (!shadowmaps::OffsetKnown||!std::isfinite(shadowmaps::CurrentOffset.x)
        ||!std::isfinite(shadowmaps::CurrentOffset.y)||!std::isfinite(shadowmaps::CurrentOffset.z))
        return skip("camera offset not known or finite");
    for(const auto &r:rows)if(!std::isfinite(r.x)||!std::isfinite(r.y)||!std::isfinite(r.z)||!std::isfinite(r.w))return skip("nonfinite native draw rows");
    if(std::abs(rows[4].w)>1e7f||std::abs(rows[5].w)>1e7f||std::abs(rows[6].w)>1e7f
        ||!std::isfinite(shadowmaps::Strength.load()))return skip("native coordinate or strength bounds");
    projected_shadow_policy::Direction nativeDirection;
    const auto &light=rows[13-shadowmaps::FIRST_ROW];
    if(!projected_shadow_policy::Normalize(light.x,light.y,light.z,light.w,nativeDirection))return skip("native directional row invalid");
    ComPtr<ID3D11BlendState> nativeBlend;ComPtr<ID3D11DepthStencilState> nativeDepth;
    UINT nativeMask=0;ctx->OMGetBlendState(&nativeBlend,nullptr,&nativeMask);ctx->OMGetDepthStencilState(&nativeDepth,nullptr);
    D3D11_BLEND_DESC bd{};D3D11_DEPTH_STENCIL_DESC dd{};
    if(nativeBlend)nativeBlend->GetDesc(&bd);if(nativeDepth)nativeDepth->GetDesc(&dd);
    const auto keep=[](const D3D11_DEPTH_STENCILOP_DESC &f){return f.StencilFailOp==D3D11_STENCIL_OP_KEEP
        &&f.StencilDepthFailOp==D3D11_STENCIL_OP_KEEP&&f.StencilPassOp==D3D11_STENCIL_OP_KEEP;};
    const bool readOnly=dd.DepthEnable&&dd.DepthWriteMask==D3D11_DEPTH_WRITE_MASK_ZERO
        &&(!dd.StencilEnable||dd.StencilWriteMask==0||(keep(dd.FrontFace)&&keep(dd.BackFace)));
    if(nativeMask!=0xffffffffu||!readOnly||(singlePass&&!projected_shadows::ReplaySafe(bd,dd)))
        return skip("native replay writes depth/stencil or blend unsupported");
    if (singlePass != (lampShader < SINGLE_PASS_SHADERS))
        return skip("blend does not match the lamp shader");

    // the picture's u, v and w rows, then the fade's row along the beam
    const UINT pictureRow = static_cast<UINT>(FIRST_FADE_ROW + 4 * LAMP_SHADERS[lampShader].FadeStage);
    const UINT fadeRow = pictureRow + 4;
    std::vector<XMFLOAT4> constants;
    XMMATRIX inverse;
    if (!CopyConstants(ctx1.Get(), false, 0, constants, 7, fadeRow + 1) || constants.size() <= fadeRow)
        return skip("constants not read");
    if (!FindInverseWorld(constants, inverse))
        return skip("world rows not invertible");
    const XMFLOAT4 &perspective = constants[pictureRow + 3];
    constexpr float ROUNDING = 1e-4f;
    if (std::abs(perspective.x) > ROUNDING || std::abs(perspective.y) > ROUNDING || std::abs(perspective.z) > ROUNDING
        || std::abs(perspective.w - 1) > ROUNDING)
        return skip("picture projected with perspective");

    const XMVECTOR origin = XMVector3TransformCoord(XMVectorZero(), inverse);
    const XMVECTOR axes[3] = { XMVector3TransformNormal(g_XMIdentityR0, inverse),
        XMVector3TransformNormal(g_XMIdentityR1, inverse), XMVector3TransformNormal(g_XMIdentityR2, inverse) };
    const XMFLOAT4 *objectRows[3] = { &constants[pictureRow], &constants[pictureRow + 1], &constants[fadeRow] };
    const XMFLOAT3 &offset = shadowmaps::CurrentOffset;
    XMFLOAT4 windowRows[3];
    for (int n = 0; n < 3; ++n)
    {
        const XMVECTOR row = XMLoadFloat4(objectRows[n]);
        const float x = XMVectorGetX(XMVector3Dot(row, axes[0]));
        const float y = XMVectorGetX(XMVector3Dot(row, axes[1]));
        const float z = XMVectorGetX(XMVector3Dot(row, axes[2]));
        // drawn positions are absolute ones plus the camera offset
        windowRows[n] = { x, y, z, XMVectorGetX(XMVector3Dot(row, origin)) + objectRows[n]->w + x * offset.x
            + y * offset.y + z * offset.z };
    }
    for(const auto &r:windowRows)if(!std::isfinite(r.x)||!std::isfinite(r.y)||!std::isfinite(r.z)||!std::isfinite(r.w))
        return skip("nonfinite native projection");
    const auto u=XMLoadFloat4(&windowRows[0]),v=XMLoadFloat4(&windowRows[1]),w=XMLoadFloat4(&windowRows[2]);
    const float determinant=XMVectorGetX(XMVector3Dot(u,XMVector3Cross(v,w)));
    if(!std::isfinite(determinant)||std::abs(determinant)<1e-12f)return skip("singular native projection");
    const shadowmaps::Window *window = shadowmaps::NoteWindow(windowRows);
    if (!window)
        return skip("every window map in use");
    if (!window->BuiltFrame)
        return skip("map not drawn yet");
    if (!CreateObjects(cmd->get_device()))
    {
        CreateFailed = true;
        return false;
    }
    if (!WindowPixelShader.handle)
        return skip("shader not made");
    if (capture::State == capture::Phase::Capturing && capture::Log)
        fprintf(capture::Log, "  window light: shadow drawn from its map\n");

    if (!singlePass)
    {
        DrawShadow(cmd, call, nullptr, 0, window);
        ++shadowmaps::ExtraDrawsThisSecond;
        return false;
    }

    shadowmaps::ExtraDrawsThisSecond += 2;
    const SavedBlend blend(ctx);
    cmd->bind_pipeline(pipeline_stage::output_merger, AlphaOnlyBlend);
    Issue(cmd, call);
    DrawShadow(cmd, call, nullptr, 0, window);
    cmd->bind_pipeline(pipeline_stage::output_merger, ColorByAlphaBlend);
    Issue(cmd, call);
    blend.Restore(ctx);
    return true;
}

// records an opaque mesh drawn into the light buffer's base light with the frame's camera, so objects seen outside any
// lamp's light still cast once out of view
static void RecordBaseDraw(command_list *cmd, const shadowmaps::DrawCall &call)
{
    if (!Enabled || CreateFailed || !shadowmaps::CubeViews[0] || !lightbuffer::LightBuffer
        || lightbuffer::CurrentTarget != lightbuffer::LightBuffer.Get())
        return;

    // a frame capture notes why a base light draw is not recorded
    const bool logging = capture::State == capture::Phase::Capturing && capture::Log;
    auto *ctx = reinterpret_cast<ID3D11DeviceContext *>(cmd->get_native());
    ComPtr<ID3D11BlendState> blend;
    ctx->OMGetBlendState(&blend, nullptr, nullptr);
    D3D11_BLEND_DESC blendDesc = {};
    if (blend)
        blend->GetDesc(&blendDesc);
    // blending one, zero writes the draw's colour as is, like no blending
    const D3D11_RENDER_TARGET_BLEND_DESC &target = blendDesc.RenderTarget[0];
    if (target.BlendEnable && (target.SrcBlend != D3D11_BLEND_ONE || target.DestBlend != D3D11_BLEND_ZERO))
    {
        if (logging)
            fprintf(capture::Log, "  base draw not recorded: blended\n");
        return;
    }
    const auto opaqueMesh=FindDrawnMesh(ctx,call);
    shadowmaps::NoteBaseMesh(opaqueMesh);
    if (!lightbuffer::WorldToClipKnown)
    {
        if (logging)
            fprintf(capture::Log, "  base draw not recorded: camera not known yet\n");
        return;
    }

    DirectX::XMFLOAT4 rows[shadowmaps::ROW_COUNT];
    DirectX::XMMATRIX worldToClip;
    if (!shadowmaps::ReadDrawRows(ctx, rows) || !shadowmaps::FindWorldToClip(rows, worldToClip))
    {
        if (logging)
            fprintf(capture::Log, "  base draw not recorded: constants not read\n");
        return;
    }
    if (!lightbuffer::IsSameCamera(worldToClip, lightbuffer::WorldToClip))
    {
        if (logging)
            fprintf(capture::Log, "  base draw not recorded: another camera\n");
        return;
    }
    shadowmaps::Record(ctx, call, rows, false);
    KeepBaseDraw(ctx, call, rows);
}

// Cache-only see-through classification after native lighting, never a draw suppression.
static void NoteBlendedDraw(ID3D11DeviceContext *ctx,const shadowmaps::DrawCall &call)
{
    if(!Enabled||!lightbuffer::Processed)return;
    ComPtr<ID3D11BlendState> blend;ctx->OMGetBlendState(&blend,nullptr,nullptr);
    if(!blend)return;D3D11_BLEND_DESC b{};blend->GetDesc(&b);const auto &a=b.RenderTarget[0];
    if(b.AlphaToCoverageEnable||!a.BlendEnable||a.SrcBlend!=D3D11_BLEND_SRC_ALPHA
        ||a.DestBlend!=D3D11_BLEND_INV_SRC_ALPHA||a.BlendOp!=D3D11_BLEND_OP_ADD)return;
    ComPtr<ID3D11DepthStencilState> state;ctx->OMGetDepthStencilState(&state,nullptr);
    if(!state)return;D3D11_DEPTH_STENCIL_DESC d{};state->GetDesc(&d);
    if(!d.DepthEnable||d.DepthFunc==D3D11_COMPARISON_ALWAYS)return;
    shadowmaps::Geometry mesh;ctx->IAGetVertexBuffers(0,1,&mesh.VertexBuffer,&mesh.Stride,&mesh.VertexOffset);
    ctx->IAGetIndexBuffer(&mesh.IndexBuffer,&mesh.IndexFormat,&mesh.IndexOffset);mesh.Call=call;
    mesh.Key=shadowmaps::HashMesh(mesh);shadowmaps::NoteBlendedMesh(mesh);
}

// reads the sun and ambient colours from a sun-lit base light draw into the light buffer, with point light shadows
// on or off, so what a sun shadow takes off does not depend on them
static bool ReadSunDraw(ID3D11DeviceContext *ctx,const shadowmaps::DrawCall &call,bool point,bool projected)
{
    if(!tq_world::Work.Sun())return false;
    sun_state_policy::BasePass input{};
    input.master=tq_world::Work.Sun();input.point=point;input.projected=projected;
    input.target=lightbuffer::LightBuffer&&lightbuffer::CurrentTarget==lightbuffer::LightBuffer.Get()
        &&lightbuffer::WorldToClipKnown;input.singleInstance=call.Instances==1;
    {
        std::lock_guard guard(Lock);
        input.sunVertex=SunVertexShaders.contains(capture::VertexShader);
        input.ambientPixel=SunPixelShaders.contains(capture::PixelShader);
    }
    if(!input.master||!input.target||input.point||input.projected||!input.sunVertex||!input.ambientPixel||!input.singleInstance)return false;
    D3D11_TEXTURE2D_DESC target{};lightbuffer::LightBuffer->GetDesc(&target);
    D3D11_VIEWPORT viewport{};UINT count=1;ctx->RSGetViewports(&count,&viewport);
    input.fullViewport=count==1&&viewport.TopLeftX==0&&viewport.TopLeftY==0&&viewport.Width==target.Width
        &&viewport.Height==target.Height&&viewport.MinDepth==0&&viewport.MaxDepth==1;
    ComPtr<ID3D11BlendState> blend;UINT mask=0;ctx->OMGetBlendState(&blend,nullptr,&mask);
    D3D11_BLEND_DESC desc{};desc.RenderTarget[0].RenderTargetWriteMask=15;if(blend)blend->GetDesc(&desc);
    const auto &rgb=desc.RenderTarget[0];
    input.opaque=!desc.AlphaToCoverageEnable&&!desc.IndependentBlendEnable&&mask==0xffffffffu&&(rgb.RenderTargetWriteMask&7)==7
        &&(!rgb.BlendEnable||(rgb.SrcBlend==D3D11_BLEND_ONE&&rgb.DestBlend==D3D11_BLEND_ZERO&&rgb.BlendOp==D3D11_BLEND_OP_ADD));
    if(!input.fullViewport||!input.opaque)return false;
    DirectX::XMFLOAT4 rows[shadowmaps::ROW_COUNT];DirectX::XMMATRIX worldToClip;
    if(!shadowmaps::ReadDrawRows(ctx,rows))return false;
    input.finiteRows=true;
    for(const auto &row:rows)input.finiteRows=input.finiteRows&&std::isfinite(row.x)&&std::isfinite(row.y)&&std::isfinite(row.z)&&std::isfinite(row.w);
    input.sameCamera=input.finiteRows&&shadowmaps::FindWorldToClip(rows,worldToClip)
        &&lightbuffer::IsSameCamera(worldToClip,lightbuffer::WorldToClip);
    if(!sun_state_policy::Admitted(input))return false;
    if(!lightbuffer::SunKnown)lightbuffer::ReadSun(ctx,rows);
    return true;
}

static bool HandleDraw(command_list *cmd, const shadowmaps::DrawCall &call)
{
    if(!tq_world::Work.Point()&&!tq_world::Work.Sun())
    {
        // AO still needs the pre-lamp colour. Do nothing until that one copy
        // is pending; an AO-only frame does not classify or replay lamp draws.
        if(!tq_world::Work.AO()||!lightbuffer::IsCopyPending())return false;
        bool lamp=false;
        {std::lock_guard guard(Lock);lamp=PointLightShaders.contains(capture::VertexShader);}
        if(lamp&&!lightbuffer::IsPartialView(reinterpret_cast<ID3D11DeviceContext *>(cmd->get_native())))
            lightbuffer::CopyBaseLight(reinterpret_cast<ID3D11DeviceContext *>(cmd->get_native()));
        return false;
    }
    if (lightbuffer::IsPartialView(reinterpret_cast<ID3D11DeviceContext *>(cmd->get_native())))
        return false;
    if (tq_world::Work.Point())
        shadowmaps::ReadCamera(reinterpret_cast<ID3D11DeviceContext *>(cmd->get_native()));
    bool isLamp = false, isProjected = false;
    {
        std::lock_guard guard(Lock);
        isLamp = PointLightShaders.contains(capture::VertexShader);
        isProjected = ProjectedLightShaders.contains(capture::VertexShader) && ReplacedShaders.contains(capture::PixelShader);
    }
    // The native affine window path below owns projector shadows; no legacy851 replay.
    if (isLamp && tq_world::Work.Point() && OwnLampShaders)
        shadowmaps::StretchLight(reinterpret_cast<ID3D11DeviceContext *>(cmd->get_native()));

    bool isStatic = false;
    const LightPass pass = GetLightPass(isStatic);
    if ((tq_world::Work.AO() || tq_world::Work.Sun()) && lightbuffer::IsCopyPending())
    {
        bool lamp = false;
        {
            std::lock_guard guard(Lock);
            lamp = PointLightShaders.contains(capture::VertexShader);
        }
        if (lamp)
            lightbuffer::CopyBaseLight(reinterpret_cast<ID3D11DeviceContext *>(cmd->get_native()));
    }
    if (pass == LightPass::None)
    {
        if (tq_world::Work.Point()) particle_emission::Observe(reinterpret_cast<ID3D11DeviceContext *>(cmd->get_native()),call,
            capture::VertexShader,capture::PixelShader);
        if(tq_world::Work.Point())NoteBlendedDraw(reinterpret_cast<ID3D11DeviceContext *>(cmd->get_native()),call);
        if (tq_world::Work.Point() && glow::HandleColourDraw(reinterpret_cast<ID3D11DeviceContext *>(cmd->get_native()), call,
                capture::PixelShader))
            return true;

        auto *nativeContext=reinterpret_cast<ID3D11DeviceContext *>(cmd->get_native());
        const bool sunBase=ReadSunDraw(nativeContext,call,isLamp,isProjected);
        bool sunDirection = false;
        {
            std::lock_guard guard(Lock);
            // the same vertex shaders add a window's directional light over the base light, which keeps its own
            // direction and colour
            sunDirection = SunDirectionShaders.contains(capture::VertexShader)
                && !SourceAlphaBlends.contains(capture::BlendState);
        }
        if (sunDirection&&sunBase&&(lightbuffer::SunDirectionFix||lightbuffer::SmoothIndoorTransition))
        {
            lightbuffer::ScopedSunConstants transaction;
            if(transaction.Begin(nativeContext,false,14))
            {
            auto *ctx = reinterpret_cast<ID3D11DeviceContext *>(cmd->get_native());
            lightbuffer::ReplaceSunDirection(ctx);
            shadowmaps::Geometry mesh;
            ctx->IAGetVertexBuffers(0, 1, &mesh.VertexBuffer, &mesh.Stride, &mesh.VertexOffset);
            ctx->IAGetIndexBuffer(&mesh.IndexBuffer, &mesh.IndexFormat, &mesh.IndexOffset);
            mesh.Call = call;
            lightbuffer::EaseSunColour(ctx, shadowmaps::HashMesh(mesh));
            RecordBaseDraw(cmd,call);
            // This native draw is consumed here so the private CB stays bound
            // through submission; only binding state is restored afterward.
            lightbuffer::CountCasterDraw();lightbuffer::OnAnyDraw(cmd);
            Issue(cmd,call);return true;
            }
        }
        RecordBaseDraw(cmd, call);
        if (tq_world::Work.Point() && !isLamp && DrawWindowShadow(cmd, call))
            return true;
        return !isLamp && DrawColourPass(cmd, call);
    }

    capture::SnapshotBeforeLights(cmd);
    capture::SnapshotLampTextures(cmd);
    if (!tq_world::Work.Point() || !Enabled || CreateFailed || !shadowmaps::CubeViews[0])
        return false;
    if (!CreateObjects(cmd->get_device()))
    {
        CreateFailed = true;
        return false;
    }

    auto *ctx = reinterpret_cast<ID3D11DeviceContext *>(cmd->get_native());
    DirectX::XMFLOAT4 rows[shadowmaps::ROW_COUNT];
    const bool rowsRead = shadowmaps::ReadDrawRows(ctx, rows);
    NoteLampDraw(ctx, call, rowsRead ? &rows : nullptr, pass == LightPass::SinglePass);
    if (!rowsRead)
        return false;
    if (isStatic)
        shadowmaps::Record(ctx, call, rows, true);
    // the light position is row c13 and its colour row c11
    const int slot = shadowmaps::FindShadowSlot(rows[13 - shadowmaps::FIRST_ROW], rows[11 - shadowmaps::FIRST_ROW]);
    D3D11_VIEWPORT viewport = {};
    UINT viewportCount = 1;
    ctx->RSGetViewports(&viewportCount, &viewport);
    size_t lampShader = 0;
    // a light with no map is drawn with the replacement too, for its fade
    if (OwnLampShaders && !shadowmaps::ShowColours && viewportCount && FindLampShader(lampShader))
    {
        shadowmaps::UpdateView(ctx, rows, viewport);
        DrawWithLampShader(cmd, call, lampShader, slot);
        ++shadowmaps::LampShaderDrawsThisSecond;
        return true;
    }
    if (!shadowmaps::ShowColours && slot < 0)
        return false;

    D3D11_RECT rect = {};
    bool full = true;
    if (viewportCount && !shadowmaps::FindShadowRect(rows, viewport, rect, full))
        return false;
    const D3D11_RECT *scissor = full ? nullptr : &rect;

    if (pass == LightPass::Alpha)
    {
        Issue(cmd, call);
        DrawShadow(cmd, call, scissor, slot);
        ++shadowmaps::ExtraDrawsThisSecond;
        return true;
    }

    shadowmaps::ExtraDrawsThisSecond += 2;

    const SavedBlend blend(reinterpret_cast<ID3D11DeviceContext *>(cmd->get_native()));
    cmd->bind_pipeline(pipeline_stage::output_merger, AlphaOnlyBlend);
    Issue(cmd, call);
    DrawShadow(cmd, call, scissor, slot);
    cmd->bind_pipeline(pipeline_stage::output_merger, ColorByAlphaBlend);
    Issue(cmd, call);
    blend.Restore(reinterpret_cast<ID3D11DeviceContext *>(cmd->get_native()));
    return true;
}

static bool OnDraw(command_list *cmd, uint32_t vertices, uint32_t instances, uint32_t firstVertex, uint32_t firstInstance)
{
    const shadowmaps::CpuScope scope(shadowmaps::DrawCpu);
    return HandleDraw(cmd, { false, vertices, instances, firstVertex, 0, firstInstance });
}

static bool OnDrawIndexed(command_list *cmd, uint32_t indices, uint32_t instances, uint32_t firstIndex, int32_t vertexOffset, uint32_t firstInstance)
{
    const shadowmaps::CpuScope scope(shadowmaps::DrawCpu);
    return HandleDraw(cmd, { true, indices, instances, firstIndex, vertexOffset, firstInstance });
}

// a frame capture writes the shaders and states this code has sorted, and its settings, to shader-state.txt
static void WriteState()
{
    FILE *file = nullptr;
    if (_wfopen_s(&file, (capture::Folder / L"shader-state.txt").c_str(), L"w") != 0)
        return;

    fprintf(file, "point light shadows %d, own lamp shaders %d, create failed %d\n", Enabled.load() ? 1 : 0,
        OwnLampShaders.load() ? 1 : 0, CreateFailed ? 1 : 0);
    const auto writeSet = [file](const char *name, const std::unordered_set<uint64_t> &set) {
        fprintf(file, "\n--- %s: %zu\n", name, set.size());
        for (uint64_t value : set)
            fprintf(file, "%016llx\n", static_cast<unsigned long long>(value));
    };
    writeSet("additive blends", AdditiveBlends);
    writeSet("source alpha blends", SourceAlphaBlends);
    writeSet("alpha blends", AlphaBlends);
    writeSet("point light shaders", PointLightShaders);
    writeSet("static light shaders", StaticLightShaders);
    writeSet("sun vertex shaders", SunVertexShaders);
    writeSet("sun pixel shaders", SunPixelShaders);
    writeSet("sun direction shaders", SunDirectionShaders);
    fprintf(file, "\n--- replaced shaders: pipeline, replacement\n");
    for (const auto &[shader, replacement] : ReplacedShaders)
        fprintf(file, "%016llx %zu\n", static_cast<unsigned long long>(shader), replacement);
    fprintf(file, "\n--- lamp replacement pipelines\n");
    for (size_t n = 0; n < LAMP_SHADER_COUNT; ++n)
        fprintf(file, "%zu %016llx\n", n, static_cast<unsigned long long>(LampPixelShaders[n].handle));
    fclose(file);
}

static void OnPresent(command_queue *queue, swapchain *chain, const rect *, const rect *, uint32_t, const rect *)
{
    shadowmaps::EndCpuFrame();
    if (capture::State == capture::Phase::Capturing)
        WriteState();
    if(!tq_world::Work.Tracking())return;
    const resource_desc screen = queue->get_device()->get_resource_desc(chain->get_current_back_buffer());
    shadowmaps::ScreenWidth = screen.texture.width;
    shadowmaps::ScreenHeight = screen.texture.height;
    if(!tq_world::Work.Point())return;
    FrameLamps.clear();
    KeptDraws.clear();
    UnsureMeshes.clear();
    if (!Enabled)
    {
        shadowmaps::CameraFound = false;
        projected_shadows::Invalidate();
        particle_emission::Hide();
        return;
    }

    particle_emission::ReadFrame(reinterpret_cast<ID3D11DeviceContext *>(queue->get_native()));
    glow::EndFrame(reinterpret_cast<ID3D11Device *>(queue->get_device()->get_native()),
        reinterpret_cast<ID3D11DeviceContext *>(queue->get_native()),particle_emission::ReadyCount());
    particle_emission::EndFrame(reinterpret_cast<ID3D11DeviceContext *>(queue->get_native()),glow::ParticleSlots);
    shadowmaps::EndFrame(reinterpret_cast<ID3D11Device *>(queue->get_device()->get_native()),
        reinterpret_cast<ID3D11DeviceContext *>(queue->get_native()));
}

// the AddedLamps constants of one added lamp draw
struct AddedLampConstants
{
    DirectX::XMFLOAT4 Header;
    DirectX::XMFLOAT4 Eye;
    DirectX::XMFLOAT4 Range;
};

// a kept draw to draw again, with where its constants start in AddedConstants: vertex b0 and b1, pixel b0 and b1, then
// its AddedLamps; and where its lamps start in the lamp list, in rows, and how many there are
struct PlannedDraw
{
    const KeptDraw *Kept;
    UINT First[5];
    UINT FirstLampRow;
    int LampCount;
};

// a read-only copy of the game's depth-stencil state; AddedDepth for none, or null when the copy cannot be made
static ID3D11DepthStencilState *GetReadOnlyDepthState(ID3D11DeviceContext *ctx, ID3D11DepthStencilState *state)
{
    if (!state)
        return AddedDepth.Get();
    auto cached = ReadOnlyDepthStates.find(state);
    if (cached != ReadOnlyDepthStates.end())
        return cached->second.second.Get();

    D3D11_DEPTH_STENCIL_DESC desc;
    state->GetDesc(&desc);
    desc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
    desc.StencilWriteMask = 0;
    ComPtr<ID3D11Device> dev;
    ctx->GetDevice(&dev);
    ComPtr<ID3D11DepthStencilState> copy;
    if (FAILED(dev->CreateDepthStencilState(&desc, &copy)))
        return nullptr;

    ReadOnlyDepthStates[state] = { state, copy };
    return copy.Get();
}

// the multiple of 16 constants a binding of this many constants takes
static UINT RoundConstants(size_t count)
{
    return static_cast<UINT>((count + 15) / 16 * 16);
}

// draws each kept moving mesh copy once more, adding every lamp that reaches its origin and that the game did not draw
// it for: this frame's lamps, then lamps remembered from earlier frames whose own place may be off screen. A copy whose vertex shader added_vs repeats is lit by the normal map of its kept lamp pass in
// its tangent frame, or by its surface's normal for a kept base light draw; any other copy is drawn with its own vertex
// shader and lit by its surface's normal from its depth. Everything is drawn in shadowmaps::DrawState, so the game's
// state is left as it was.
static void DrawAddedLamps(ID3D11DeviceContext *ctx)
{
    AddedMarkFrame=UINT64_MAX;
    using namespace DirectX;
    const std::unordered_map<uint64_t, KeptDraw> kept = std::move(KeptDraws);
    std::vector<FrameLamp> lamps = std::move(FrameLamps);
    const std::unordered_set<uint64_t> unsure = std::move(UnsureMeshes);
    KeptDraws.clear();
    FrameLamps.clear();
    UnsureMeshes.clear();
    CulledLamps.clear();
    ComPtr<ID3D11DeviceContext1> ctx1;
    if (!IsAddingLamps() || !shadowmaps::OffsetKnown || !shadowmaps::DrawState
        || FAILED(ctx->QueryInterface(IID_PPV_ARGS(&ctx1))))
        return;

    // this frame's lamps give their lights their colour and reach. A light stays remembered while the camera is within its
    // reach and REMEMBER_MARGIN, and one not drawn while it is in view comes closer to being forgotten.
    const XMFLOAT3 &offset = shadowmaps::CurrentOffset;
    const size_t drawnLamps = lamps.size();
    std::unordered_set<uint64_t> drawnLights;
    for (const FrameLamp &lamp : lamps)
    {
        auto light = shadowmaps::Lights.find(shadowmaps::FindNearestLight(
            { lamp.Position.x - offset.x, lamp.Position.y - offset.y, lamp.Position.z - offset.z }, lamp.Hue,
            shadowmaps::LIGHT_MATCH_DISTANCE));
        if (light == shadowmaps::Lights.end())
            continue;

        drawnLights.insert(light->first);
        light->second.MissedInView = 0;
        light->second.LastInRange = shadowmaps::FrameNumber;
        if (lamp.Reach <= 0)
            continue;
        light->second.Colour = lamp.Colour;
        light->second.Reach = lamp.Reach;
        if (lamp.FadePower > 0)
            light->second.FadePower = lamp.FadePower;
    }
    std::vector<shadowmaps::Light *> remembered;
    for (auto &[key, light] : shadowmaps::Lights)
    {
        if (drawnLights.contains(key) || light.Glow || light.Reach <= 0)
            continue;
        if (shadowmaps::CameraFound && shadowmaps::Distance(shadowmaps::Camera, light.Position) < light.Reach + REMEMBER_MARGIN)
            light.LastInRange = shadowmaps::FrameNumber;
        if (!IsRemembered(light))
            continue;
        if (IsSphereInView(light.Position, 0))
            ++light.MissedInView;
        if (IsRemembered(light))
            remembered.push_back(&light);
    }
    // a lamp whose colour flickers leaves several lights at its spot, one per hue; it is added once, as the light seen
    // last, and not at all when drawn this frame
    std::sort(remembered.begin(), remembered.end(),
        [](const shadowmaps::Light *a, const shadowmaps::Light *b) { return a->LastSeen > b->LastSeen; });
    for (shadowmaps::Light *light : remembered)
    {
        const XMFLOAT4 position = { light->Position.x + offset.x, light->Position.y + offset.y,
            light->Position.z + offset.z, 1 };
        if (std::none_of(lamps.begin(), lamps.end(), [&](const FrameLamp &lamp) {
                return shadowmaps::Distance({ lamp.Position.x, lamp.Position.y, lamp.Position.z },
                           { position.x, position.y, position.z }) < shadowmaps::LIGHT_MATCH_DISTANCE;
            }))
        {
            lamps.push_back({ position, light->Colour, light->Reach, light->Hue, -1, light->FadePower });
            light->LastAdded = shadowmaps::FrameNumber;
        }
    }
    for (FrameLamp &lamp : lamps)
        lamp.Slot = shadowmaps::FindShadowSlot(lamp.Position, lamp.Colour);
    // the remembered lamps whose reach is in view are added to the light buffer from its depth as well
    for (size_t n = drawnLamps; n < lamps.size(); ++n)
        if (IsSphereInView({ lamps[n].Position.x - offset.x, lamps[n].Position.y - offset.y, lamps[n].Position.z - offset.z },
                lamps[n].Reach * FADE_EXTENT))
            CulledLamps.push_back(lamps[n]);
    if (kept.empty())
        return;

    std::vector<XMFLOAT4> pool;
    // appends constants at a multiple of 16, as binding part of a buffer needs, and returns where they start
    const auto append = [&pool](const XMFLOAT4 *rows, size_t count) {
        const UINT first = static_cast<UINT>(pool.size());
        pool.insert(pool.end(), rows, rows + count);
        pool.resize(first + RoundConstants(count));
        return first;
    };
    std::vector<PlannedDraw> plan;
    // three rows per added lamp, as AddedLampList holds them
    std::vector<XMFLOAT4> lampRows;
    for (const auto &[key, draw] : kept)
    {
        const bool movedTexture = draw.Repeated >= 0 && REPEATED_VERTEX_SHADERS[draw.Repeated].MovedTexture;
        if (!draw.VertexShader || unsure.contains(draw.Mesh) || draw.Constants[0].size() < (movedTexture ? 24u : 15u))
            continue;

        const XMFLOAT3 origin = { draw.Constants[0][7].w, draw.Constants[0][8].w, draw.Constants[0][9].w };
        std::vector<std::pair<float, size_t>> nearby;
        for (size_t n = 0; n < lamps.size(); ++n)
        {
            const float distance = shadowmaps::Distance(origin,
                { lamps[n].Position.x, lamps[n].Position.y, lamps[n].Position.z });
            if (lamps[n].Reach > 0 && distance < lamps[n].Reach * FADE_EXTENT + MESH_RADIUS
                && std::find(draw.DrawnFor.begin(), draw.DrawnFor.end(), n) == draw.DrawnFor.end())
                nearby.push_back({ distance, n });
        }
        if (nearby.empty())
            continue;

        // a kept base light draw follows lamp_d878054c72da7a41, N.L with no lighting scale, since its pixel constants
        // are not a lamp material's
        const bool bumped = !draw.Base && draw.Repeated >= 0;
        AddedLampConstants constants = {};
        constants.Header = { static_cast<float>(nearby.size()), static_cast<float>(draw.Base ? 1 : draw.LampShader),
            movedTexture ? 1.0f : 0.0f, bumped ? 1.0f : 0.0f };
        const XMFLOAT4 &eye = draw.Constants[0][0];
        constants.Eye = { eye.x, eye.y, eye.z, bumped && REPEATED_VERTEX_SHADERS[draw.Repeated].HalfVector ? 1.0f : 0.0f };
        constants.Range = { static_cast<float>(lampRows.size()),
            draw.Repeated >= 0 && REPEATED_VERTEX_SHADERS[draw.Repeated].NormalInNormal ? 1.0f : 0.0f, 0, 0 };
        PlannedDraw planned = { &draw, {}, static_cast<UINT>(lampRows.size()), static_cast<int>(nearby.size()) };
        for (const auto &[distance, index] : nearby)
        {
            const FrameLamp &lamp = lamps[index];
            lampRows.push_back({ lamp.Position.x, lamp.Position.y, lamp.Position.z, lamp.Reach });
            lampRows.push_back(lamp.Colour);
            lampRows.push_back({ static_cast<float>(lamp.Slot), lamp.FadePower > 0 ? lamp.FadePower : COMMON_FADE_POWER, 0,
                0 });
        }

        for (size_t n = 0; n < 4; ++n)
            planned.First[n] = append(draw.Constants[n].data(), draw.Constants[n].size());
        planned.First[4] = append(&constants.Header, sizeof(constants) / sizeof(XMFLOAT4));
        plan.push_back(planned);
    }
    if (plan.empty())
        return;

    const UINT size = static_cast<UINT>(pool.size() * sizeof(XMFLOAT4));
    if (size > AddedConstantsSize)
    {
        ComPtr<ID3D11Device> dev;
        ctx->GetDevice(&dev);
        const UINT grown = std::max(size, AddedConstantsSize * 2);
        const D3D11_BUFFER_DESC desc = { grown, D3D11_USAGE_DYNAMIC, D3D11_BIND_CONSTANT_BUFFER, D3D11_CPU_ACCESS_WRITE };
        AddedConstants.Reset();
        AddedConstantsSize = 0;
        if (FAILED(dev->CreateBuffer(&desc, nullptr, &AddedConstants)))
            return;
        AddedConstantsSize = grown;
    }
    const UINT rows = static_cast<UINT>(lampRows.size());
    if (rows > AddedLampRows)
    {
        ComPtr<ID3D11Device> dev;
        ctx->GetDevice(&dev);
        const UINT grown = std::max(rows, AddedLampRows * 2);
        const D3D11_BUFFER_DESC desc = { grown * static_cast<UINT>(sizeof(XMFLOAT4)), D3D11_USAGE_DYNAMIC,
            D3D11_BIND_SHADER_RESOURCE, D3D11_CPU_ACCESS_WRITE, D3D11_RESOURCE_MISC_BUFFER_STRUCTURED, sizeof(XMFLOAT4) };
        AddedLampBuffer.Reset();
        AddedLampView.Reset();
        AddedLampRows = 0;
        if (FAILED(dev->CreateBuffer(&desc, nullptr, &AddedLampBuffer))
            || FAILED(dev->CreateShaderResourceView(AddedLampBuffer.Get(), nullptr, &AddedLampView)))
            return;
        AddedLampRows = grown;
    }
    D3D11_MAPPED_SUBRESOURCE mapped;
    if (FAILED(ctx->Map(AddedConstants.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
        return;
    std::memcpy(mapped.pData, pool.data(), size);
    ctx->Unmap(AddedConstants.Get(), 0);
    if (FAILED(ctx->Map(AddedLampBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
        return;
    std::memcpy(mapped.pData, lampRows.data(), rows * sizeof(XMFLOAT4));
    ctx->Unmap(AddedLampBuffer.Get(), 0);

    ComPtr<ID3DDeviceContextState> previous;
    ctx1->SwapDeviceContextState(shadowmaps::DrawState.Get(), &previous);
    // DrawState still holds the depth view of the last cube face drawn into, which the runtime would not let the maps
    // below be read through, binding nothing in their place; every shadow would then read as full shadow
    const bool logging = capture::State == capture::Phase::Capturing && capture::Log;
    ComPtr<ID3D11DepthStencilView> leftDepth;
    if (logging)
        ctx1->OMGetRenderTargets(0, nullptr, &leftDepth);
    ctx1->OMSetRenderTargets(0, nullptr, nullptr);
    // the light table, camera offset and view, the moving-object maps and the samplers, as DrawWithLampShader binds
    // them, and the maps of every size, since the lamps of one draw can have maps of any size
    static_assert(CAMERA_OFFSET_SLOT == TABLE_SLOT + 1 && VIEW_SLOT == TABLE_SLOT + 2);
    ID3D11Buffer *tables[] = { shadowmaps::LightTableBuffer.Get(), shadowmaps::CameraOffsetBuffer.Get(),
        shadowmaps::ViewBuffer.Get() };
    ctx1->PSSetConstantBuffers(TABLE_SLOT, 3, tables);
    ID3D11ShaderResourceView *movingMap = shadowmaps::MovingView.Get();
    ctx1->PSSetShaderResources(MOVING_MAP_SLOT, 1, &movingMap);
    ID3D11ShaderResourceView *maps[shadowmaps::SIZE_COUNT];
    for (int n = 0; n < shadowmaps::SIZE_COUNT; ++n)
        maps[n] = shadowmaps::CubeViews[n].Get();
    ctx1->PSSetShaderResources(ADDED_MAPS_SLOT, shadowmaps::SIZE_COUNT, maps);
    static_assert(ADDED_LIST_SLOT == ADDED_MAPS_SLOT + shadowmaps::SIZE_COUNT);
    ID3D11ShaderResourceView *lampList = AddedLampView.Get();
    ctx1->PSSetShaderResources(ADDED_LIST_SLOT, 1, &lampList);
    ID3D11SamplerState *compareSampler = shadowmaps::CompareSampler.Get();
    ID3D11SamplerState *pointSampler = shadowmaps::PointSampler.Get();
    ctx1->PSSetSamplers(MAP_SLOT, 1, &compareSampler);
    ctx1->PSSetSamplers(POINT_SAMPLER_SLOT, 1, &pointSampler);
    ctx1->OMSetBlendState(AddedBlend.Get(), nullptr, 0xffffffff);
    // a frame capture notes whether the maps are bound as the added draws read them
    if (logging)
    {
        ComPtr<ID3D11ShaderResourceView> boundMoving, boundMaps[shadowmaps::SIZE_COUNT];
        ctx1->PSGetShaderResources(MOVING_MAP_SLOT, 1, &boundMoving);
        ID3D11ShaderResourceView *mapViews[shadowmaps::SIZE_COUNT] = {};
        ctx1->PSGetShaderResources(ADDED_MAPS_SLOT, shadowmaps::SIZE_COUNT, mapViews);
        fprintf(capture::Log, "--- added lamps: draw state held a depth view %s; moving maps %s, maps", leftDepth ? "yes" : "no",
            boundMoving ? "bound" : "missing");
        for (int n = 0; n < shadowmaps::SIZE_COUNT; ++n)
        {
            boundMaps[n].Attach(mapViews[n]);
            fprintf(capture::Log, " %s", boundMaps[n] ? "bound" : "missing");
        }
        fprintf(capture::Log, "\n");
    }
    // the draws into the light buffer mark the pixels they relight, which DrawCulledLamps leaves out
    D3D11_TEXTURE2D_DESC bufferDesc = {};
    ComPtr<ID3D11Device> dev;
    ctx->GetDevice(&dev);
    if (lightbuffer::LightBuffer)
        lightbuffer::LightBuffer->GetDesc(&bufferDesc);
    const bool markReady=lightbuffer::LightBuffer
        && glow::CreateScreenTexture(dev.Get(), bufferDesc.Width, bufferDesc.Height, DXGI_FORMAT_R8_UNORM, AddedMarkTexture,
            AddedMarkTarget, AddedMarkView);
    if(markReady)
    {
        const FLOAT clear[4] = {};
        ctx1->ClearRenderTargetView(AddedMarkTarget.Get(), clear);
        AddedMarkFrame = shadowmaps::FrameNumber;
    }
    // Unmarked moving draws are still valid; the screen pass must not add
    // their lamps a second time when mask allocation failed.
    if(!offscreen_lamp_policy::CanRelight(!plan.empty(),markReady))CulledLamps.clear();
    ID3D11Buffer *constants = AddedConstants.Get();
    // binds count constants of AddedConstants from first, or nothing when count is 0
    const auto bind = [&](bool pixel, UINT slot, UINT first, size_t count) {
        ID3D11Buffer *bound = count ? constants : nullptr;
        const UINT rounded = RoundConstants(count);
        if (pixel)
            ctx1->PSSetConstantBuffers1(slot, 1, &bound, &first, &rounded);
        else
            ctx1->VSSetConstantBuffers1(slot, 1, &bound, &first, &rounded);
    };

    int lampsAdded = 0;
    for (size_t index = 0; index < plan.size(); ++index)
    {
        const PlannedDraw &planned = plan[index];
        const KeptDraw &draw = *planned.Kept;
        const bool repeated = draw.Repeated >= 0;
        ID3D11Buffer *vertexBuffers[KEPT_VERTEX_SLOTS];
        for (UINT n = 0; n < KEPT_VERTEX_SLOTS; ++n)
            vertexBuffers[n] = draw.VertexBuffers[n].Get();
        ctx1->IASetInputLayout(draw.Layout.Get());
        ctx1->IASetPrimitiveTopology(draw.Topology);
        ctx1->IASetVertexBuffers(0, KEPT_VERTEX_SLOTS, vertexBuffers, draw.Strides, draw.Offsets);
        ctx1->IASetIndexBuffer(draw.IndexBuffer.Get(), draw.IndexFormat, draw.IndexOffset);
        ctx1->VSSetShader(repeated ? AddedVertexShader.Get() : draw.VertexShader.Get(), nullptr, 0);
        ctx1->PSSetShader(repeated ? AddedPixelShader.Get() : ScreenPixelShader.Get(), nullptr, 0);
        bind(false, 0, planned.First[0], draw.Constants[0].size());
        bind(false, 1, planned.First[1], draw.Constants[1].size());
        bind(true, 0, planned.First[2], draw.Constants[2].size());
        bind(true, 1, planned.First[3], draw.Constants[3].size());
        bind(false, ADDED_LAMP_SLOT, planned.First[4], sizeof(AddedLampConstants) / sizeof(XMFLOAT4));
        bind(true, ADDED_LAMP_SLOT, planned.First[4], sizeof(AddedLampConstants) / sizeof(XMFLOAT4));
        for (size_t n = 0; n < std::size(KEPT_VIEW_SLOTS); ++n)
        {
            ID3D11ShaderResourceView *view = draw.Views[n].Get();
            ctx1->PSSetShaderResources(KEPT_VIEW_SLOTS[n], 1, &view);
        }
        ID3D11SamplerState *samplers[KEPT_SAMPLERS];
        for (UINT n = 0; n < KEPT_SAMPLERS; ++n)
            samplers[n] = draw.Samplers[n].Get();
        ctx1->PSSetSamplers(0, KEPT_SAMPLERS, samplers);
        ctx1->RSSetState(draw.Rasterizer.Get());
        ctx1->RSSetViewports(1, &draw.Viewport);
        // the kept pass's scissor rectangle may be the one the game gave its own lamp
        const D3D11_RECT whole = { static_cast<LONG>(std::floor(draw.Viewport.TopLeftX)),
            static_cast<LONG>(std::floor(draw.Viewport.TopLeftY)),
            static_cast<LONG>(std::ceil(draw.Viewport.TopLeftX + draw.Viewport.Width)),
            static_cast<LONG>(std::ceil(draw.Viewport.TopLeftY + draw.Viewport.Height)) };
        ctx1->RSSetScissorRects(1, &whole);
        ID3D11DepthStencilState *depth = draw.Base ? AddedDepth.Get() : GetReadOnlyDepthState(ctx, draw.Depth.Get());
        ctx1->OMSetDepthStencilState(depth ? depth : AddedDepth.Get(), draw.StencilReference);
        ComPtr<ID3D11Resource> targetResource;
        if (draw.Target)
            draw.Target->GetResource(&targetResource);
        const bool marked = AddedMarkFrame == shadowmaps::FrameNumber && targetResource
            && targetResource.Get() == lightbuffer::LightBuffer.Get();
        ID3D11RenderTargetView *targets[] = { draw.Target.Get(), AddedMarkTarget.Get() };
        ctx1->OMSetRenderTargets(marked ? 2 : 1, targets, draw.DepthView.Get());
        XMFLOAT4 drawRows[shadowmaps::ROW_COUNT];
        std::copy_n(draw.Constants[0].begin() + shadowmaps::FIRST_ROW, shadowmaps::ROW_COUNT, drawRows);
        shadowmaps::UpdateView(ctx1.Get(), drawRows, draw.Viewport);
        if (draw.Call.Indexed)
            ctx1->DrawIndexed(draw.Call.Count, draw.Call.First, draw.Call.BaseVertex);
        else
            ctx1->Draw(draw.Call.Count, draw.Call.First);
        lampsAdded += planned.LampCount;

        // a frame capture notes each added lamp draw and its lamps
        if (logging)
        {
            fprintf(capture::Log, "--- added lamps: mesh %016llx at %.3f %.3f %.3f from its %s, %s, %d lamps:",
                static_cast<unsigned long long>(draw.Mesh), draw.Constants[0][7].w - offset.x,
                draw.Constants[0][8].w - offset.y, draw.Constants[0][9].w - offset.z,
                draw.Base ? "base light draw" : "lamp pass",
                !repeated ? "lit from depth" : draw.Base ? "lit by its surface" : "lit by its normal map", planned.LampCount);
            for (int n = 0; n < planned.LampCount; ++n)
            {
                const XMFLOAT4 &lamp = lampRows[planned.FirstLampRow + n * 3];
                fprintf(capture::Log, " [%.3f %.3f %.3f reach %.2f slot %d]", lamp.x - offset.x, lamp.y - offset.y,
                    lamp.z - offset.z, lamp.w, static_cast<int>(lampRows[planned.FirstLampRow + n * 3 + 2].x));
            }
            D3D11_DEPTH_STENCIL_DESC keptDepth = {};
            if (draw.Depth)
                draw.Depth->GetDesc(&keptDepth);
            fprintf(capture::Log, ", kept depth %s func %d stencil %s (D3D11 numbering), drawn with %s\n",
                !draw.Depth ? "none" : keptDepth.DepthEnable ? "on" : "off", static_cast<int>(keptDepth.DepthFunc),
                keptDepth.StencilEnable ? "on" : "off",
                draw.Base ? "AddedDepth" : !draw.Depth ? "AddedDepth, the game had none"
                    : depth ? "a read-only copy" : "AddedDepth, no read-only copy");
        }
    }

    // DrawState keeps none of the game's resources, nor views the game may bind for writing
    static_assert(DRAW_SLOT_SLOT < KEPT_VERTEX_SLOTS);
    ID3D11Buffer *noBuffers[KEPT_VERTEX_SLOTS] = {};
    const UINT zeros[KEPT_VERTEX_SLOTS] = {};
    ctx1->IASetVertexBuffers(0, KEPT_VERTEX_SLOTS, noBuffers, zeros, zeros);
    ctx1->IASetIndexBuffer(nullptr, DXGI_FORMAT_UNKNOWN, 0);
    ctx1->IASetInputLayout(nullptr);
    ctx1->VSSetShader(nullptr, nullptr, 0);
    ctx1->VSSetConstantBuffers(0, ADDED_LAMP_SLOT + 1, noBuffers);
    ctx1->PSSetConstantBuffers(0, DRAW_SLOT_SLOT + 1, noBuffers);
    ID3D11ShaderResourceView *noViews[ADDED_LIST_SLOT + 1] = {};
    ctx1->PSSetShaderResources(0, ADDED_LIST_SLOT + 1, noViews);
    ctx1->PSSetShaderResources(KEPT_VIEW_SLOTS[4], 4, noViews);
    ID3D11SamplerState *noSamplers[KEPT_SAMPLERS] = {};
    ctx1->PSSetSamplers(0, KEPT_SAMPLERS, noSamplers);
    ctx1->OMSetRenderTargets(0, nullptr, nullptr);
    ctx1->SwapDeviceContextState(previous.Get(), nullptr);
    shadowmaps::ExtraDrawsThisSecond += static_cast<int>(plan.size());
    if (logging)
        fprintf(capture::Log, "--- added lamps: %zu moving mesh copies kept, %zu lamps known (%zu drawn this frame), %zu "
            "copies drawn again adding %d lamps in all\n", kept.size(), lamps.size(), drawnLamps, plan.size(), lampsAdded);
}

// adds the remembered lamps the game drew nothing for this frame to the light buffer from its depth, all in one pass,
// with their maps, so the surfaces in their reach keep them once the lamp itself is off screen. The pixels the added
// lamp draws marked are left out: those draws already added these lamps to the moving meshes they relight.
static void DrawCulledLamps(ID3D11DeviceContext *ctx)
{
    using namespace DirectX;
    std::vector<FrameLamp> lamps = std::move(CulledLamps);
    CulledLamps.clear();
    ComPtr<ID3D11DeviceContext1> ctx1;
    if (lamps.empty() || !IsAddingLamps() || !CulledPixelShader || !shadowmaps::DrawState || !lightbuffer::LightBuffer
        || !lightbuffer::LightBufferTarget || !lightbuffer::DepthCopyView || FAILED(ctx->QueryInterface(IID_PPV_ARGS(&ctx1))))
        return;

    // the nearest lamps first, which are kept when there are more than lamps_culled holds
    const auto nearness = [](const FrameLamp &lamp) {
        return lamp.Position.x * lamp.Position.x + lamp.Position.y * lamp.Position.y + lamp.Position.z * lamp.Position.z;
    };
    std::sort(lamps.begin(), lamps.end(), [&](const FrameLamp &a, const FrameLamp &b) { return nearness(a) < nearness(b); });
    D3D11_TEXTURE2D_DESC desc;
    lightbuffer::LightBuffer->GetDesc(&desc);
    XMFLOAT4X4 worldToClip;
    XMStoreFloat4x4(&worldToClip, lightbuffer::WorldToClip);
    const XMFLOAT3 &offset = shadowmaps::CurrentOffset;
    CulledConstants constants = {};
    D3D11_RECT area = { LONG_MAX, LONG_MAX, LONG_MIN, LONG_MIN };
    int count = 0;
    for (const FrameLamp &lamp : lamps)
    {
        D3D11_RECT rect;
        if (count == MAX_CULLED_LAMPS)
            break;
        if (!glow::FindLightRect(worldToClip, { lamp.Position.x - offset.x, lamp.Position.y - offset.y,
                lamp.Position.z - offset.z }, lamp.Reach * FADE_EXTENT, desc.Width, desc.Height, rect))
            continue;

        constants.Lamps[count * 3] = { lamp.Position.x, lamp.Position.y, lamp.Position.z, lamp.Reach };
        constants.Lamps[count * 3 + 1] = lamp.Colour;
        constants.Lamps[count * 3 + 2] = { static_cast<float>(lamp.Slot),
            lamp.FadePower > 0 ? lamp.FadePower : COMMON_FADE_POWER, 0, 0 };
        area = { std::min(area.left, rect.left), std::min(area.top, rect.top), std::max(area.right, rect.right),
            std::max(area.bottom, rect.bottom) };
        ++count;
    }
    if (!count)
        return;

    XMStoreFloat4x4(&constants.Inverse, XMMatrixInverse(nullptr, lightbuffer::WorldToClip));
    constants.Size = { static_cast<float>(desc.Width), static_cast<float>(desc.Height), 1.0f / desc.Width,
        1.0f / desc.Height };
    constants.Count = { static_cast<float>(count), 0, 0, 0 };
    const bool marked = AddedMarkFrame == shadowmaps::FrameNumber;

    ComPtr<ID3DDeviceContextState> previous;
    ctx1->SwapDeviceContextState(shadowmaps::DrawState.Get(), &previous);
    ctx1->UpdateSubresource(CulledBuffer.Get(), 0, nullptr, &constants, 0, 0);
    // the light buffer is bound first, so no cube face's depth view is left bound while the maps are read
    ID3D11RenderTargetView *target = lightbuffer::LightBufferTarget.Get();
    ctx1->OMSetRenderTargets(1, &target, nullptr);
    ctx1->OMSetBlendState(AddedBlend.Get(), nullptr, 0xffffffff);
    ctx1->OMSetDepthStencilState(nullptr, 0);
    ctx1->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx1->IASetInputLayout(nullptr);
    ctx1->VSSetShader(CulledVertexShader.Get(), nullptr, 0);
    ctx1->PSSetShader(CulledPixelShader.Get(), nullptr, 0);
    const D3D11_VIEWPORT viewport = { 0, 0, static_cast<float>(desc.Width), static_cast<float>(desc.Height), 0, 1 };
    ctx1->RSSetViewports(1, &viewport);
    ctx1->RSSetState(CulledRasterizer.Get());
    ctx1->RSSetScissorRects(1, &area);
    ID3D11Buffer *tables[] = { shadowmaps::LightTableBuffer.Get(), shadowmaps::CameraOffsetBuffer.Get() };
    ctx1->PSSetConstantBuffers(TABLE_SLOT, 2, tables);
    ID3D11Buffer *culled = CulledBuffer.Get();
    ctx1->PSSetConstantBuffers(CULLED_SLOT, 1, &culled);
    ID3D11ShaderResourceView *movingMap = shadowmaps::MovingView.Get();
    ctx1->PSSetShaderResources(MOVING_MAP_SLOT, 1, &movingMap);
    ID3D11ShaderResourceView *maps[shadowmaps::SIZE_COUNT];
    for (int n = 0; n < shadowmaps::SIZE_COUNT; ++n)
        maps[n] = shadowmaps::CubeViews[n].Get();
    ctx1->PSSetShaderResources(ADDED_MAPS_SLOT, shadowmaps::SIZE_COUNT, maps);
    static_assert(CULLED_MARK_SLOT == CULLED_DEPTH_SLOT + 1);
    // without this frame's marks no pixel is left out
    ID3D11ShaderResourceView *screen[] = { lightbuffer::DepthCopyView.Get(), marked ? AddedMarkView.Get() : nullptr };
    ctx1->PSSetShaderResources(CULLED_DEPTH_SLOT, 2, screen);
    ID3D11SamplerState *compareSampler = shadowmaps::CompareSampler.Get();
    ID3D11SamplerState *pointSampler = shadowmaps::PointSampler.Get();
    ctx1->PSSetSamplers(MAP_SLOT, 1, &compareSampler);
    ctx1->PSSetSamplers(POINT_SAMPLER_SLOT, 1, &pointSampler);
    ctx1->Draw(3, 0);

    ID3D11Buffer *noBuffers[CULLED_SLOT + 1] = {};
    ctx1->PSSetConstantBuffers(0, CULLED_SLOT + 1, noBuffers);
    ID3D11ShaderResourceView *noViews[CULLED_MARK_SLOT + 1] = {};
    ctx1->PSSetShaderResources(0, CULLED_MARK_SLOT + 1, noViews);
    ctx1->VSSetShader(nullptr, nullptr, 0);
    ctx1->OMSetRenderTargets(0, nullptr, nullptr);
    ctx1->SwapDeviceContextState(previous.Get(), nullptr);
    ++shadowmaps::ExtraDrawsThisSecond;
    // a frame capture notes the lamps drawn from depth
    if (capture::State == capture::Phase::Capturing && capture::Log)
    {
        fprintf(capture::Log, "--- culled lamps: %d of %zu drawn from depth in rect %ld %ld %ld %ld, %s:", count, lamps.size(),
            area.left, area.top, area.right, area.bottom, marked ? "marked moving meshes left out" : "no marks");
        for (int n = 0; n < count; ++n)
        {
            const XMFLOAT4 &lamp = constants.Lamps[n * 3];
            fprintf(capture::Log, " [%.3f %.3f %.3f reach %.2f slot %d]", lamp.x - offset.x, lamp.y - offset.y,
                lamp.z - offset.z, lamp.w, static_cast<int>(constants.Lamps[n * 3 + 2].x));
        }
        fprintf(capture::Log, "\n");
    }
}

// draws the lamps added to moving meshes, the remembered lamps the game drew nothing for and the glow lights into the
// light buffer while point light shadows are on
static void DrawAddedLights(ID3D11DeviceContext *ctx)
{
    if (!tq_world::Work.Point() || !Enabled)
        return;

    DrawAddedLamps(ctx);
    DrawCulledLamps(ctx);
    // a frame capture saves the light buffer with the added lamps, which its after-lights snapshot, taken before them,
    // leaves out
    if (capture::State == capture::Phase::Capturing && capture::Log && lightbuffer::LightBuffer)
        capture::AddSnapshot(ctx, lightbuffer::LightBuffer.Get(), "after-added-lamps");
    glow::DrawLights(ctx);
}

static void DrawSettings()
{
    ImGui::SeparatorText("Point light shadows");
    bool enabled = Enabled;
    if (ImGui::Checkbox("Point light shadows", &enabled))
    {
        Enabled = enabled;
        reshade::set_config_value(nullptr, CONFIG_SECTION, "PointLightShadows", enabled);
    }
    ImGui::BeginDisabled(!enabled);
    constexpr const char* presets[] = {"Min (10)", "Med (50)", "Max"};
    const int selected = tq_world::ShadowPresetIndex(shadowmaps::LightLimit.load(), shadowmaps::MAX_LIGHTS);
    if (ImGui::BeginCombo("Shadowed lights", selected < 0 ? "Custom (Advanced)" : presets[selected]))
    {
        for (int i = 0; i < 3; ++i)
            if (ImGui::Selectable(presets[i], selected == i))
            {
                const int limit = tq_world::ShadowPresetLimit(i, shadowmaps::MAX_LIGHTS);
                shadowmaps::LightLimit = limit;
                reshade::set_config_value(nullptr, CONFIG_SECTION, "ShadowedLights", limit);
            }
        ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Max uses all %d renderer-supported shadow maps. Range and eligible lights still apply.", shadowmaps::MAX_LIGHTS);
    bool moving = shadowmaps::MovingShadows;
    if (ImGui::Checkbox("Character shadows", &moving))
    {
        shadowmaps::MovingShadows = moving;
        reshade::set_config_value(nullptr, CONFIG_SECTION, "CharacterShadows", moving);
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Moving shadows for point lights and supported projected window lights.");
    bool emissive = glow::Enabled;
    if (ImGui::Checkbox("Emissive lights (experimental)", &emissive))
    {
        glow::Enabled = emissive;
        reshade::set_config_value(nullptr, CONFIG_SECTION, "EmissiveLights", emissive);
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Supported emissive surfaces and local textured particles light nearby surfaces. Other material and character glow may be unsupported.");
    ImGui::EndDisabled();
    if (emissive)
    {
        size_t emitters = 0;
        int cells = 0, drawn = 0;
        {
            std::lock_guard guard(glow::Lock);
            emitters = glow::Emitters.size() + particle_emission::Measured;
            cells = glow::CellsPickedLastFrame + static_cast<int>(particle_emission::Active);
            drawn = glow::LightsDrawnLastFrame;
        }
        if (!tq_world::Managed.Enabled()) ImGui::TextDisabled("Emissive: paused by texture toggle.");
        else if (!enabled) ImGui::TextDisabled("Emissive: requires point light shadows.");
        else if (!emitters && particle_emission::Seen) ImGui::TextDisabled("Emissive: measuring particle sources (%u reads pending).", particle_emission::Pending);
        else if (!emitters) ImGui::TextDisabled("Emissive: no supported sources detected.");
        else if (!drawn) ImGui::TextDisabled("Emissive: %zu emitters, %d cells; no active draws.", emitters, cells);
        else ImGui::TextDisabled("Emissive: %d light draws last frame.", drawn);
    }
    bool occlusion = lightbuffer::OcclusionEnabled;
    if (ImGui::Checkbox("Ambient occlusion", &occlusion))
    {
        lightbuffer::OcclusionEnabled = occlusion;
        reshade::set_config_value(nullptr, CONFIG_SECTION, "AmbientOcclusion", occlusion);
    }
    bool sunLighting=lightbuffer::SunEnabled;
    if(ImGui::Checkbox("Sun lighting",&sunLighting))
    {
        lightbuffer::SunEnabled=sunLighting;
        reshade::set_config_value(nullptr,CONFIG_SECTION,"SunLighting",sunLighting);
    }
    if(ImGui::IsItemHovered())ImGui::SetTooltip("Sun-light corrections, indoor transitions and optional soft shadows. Off stops all added sun processing.");
    ImGui::BeginDisabled(!sunLighting);
    bool sun = lightbuffer::SunSoftness.load() > 0;
    if (ImGui::Checkbox("Sun shadow softening", &sun))
    {
        float softness = 0;
        if (sun)
        {
            softness = sun_softness_settings::Default;
            reshade::get_config_value(nullptr, CONFIG_SECTION, "TextureQuestSunSoftnessPreferenceRealSun", softness);
            if (!std::isfinite(softness) || softness <= 0) softness = sun_softness_settings::Default;
            softness = sun_softness_settings::Clean(softness);
        }
        else reshade::set_config_value(nullptr, CONFIG_SECTION, "TextureQuestSunSoftnessPreferenceRealSun", lightbuffer::SunSoftness.load());
        lightbuffer::SunSoftness = softness;
        reshade::set_config_value(nullptr, CONFIG_SECTION, "SunSoftness", softness);
    }
    ImGui::EndDisabled();
}

static void DrawAdvancedSettings()
{
    bool enabled = Enabled;

    ImGui::BeginDisabled(!enabled);
    float strength = shadowmaps::Strength;
    if (ImGui::SliderFloat("Shadow strength", &strength, 0.0f, 1.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp))
    {
        shadowmaps::Strength = strength;
        reshade::set_config_value(nullptr, CONFIG_SECTION, "ShadowStrength", strength);
    }

    int lights = shadowmaps::LightLimit;
    if (ImGui::SliderInt("Shadowed lights", &lights, 1, shadowmaps::MAX_LIGHTS, "%d", ImGuiSliderFlags_AlwaysClamp))
    {
        shadowmaps::LightLimit = lights;
        reshade::set_config_value(nullptr, CONFIG_SECTION, "ShadowedLights", lights);
    }


    float softness = shadowmaps::LightSize;
    if (ImGui::SliderFloat("Shadow softness", &softness, 0.0f, 2.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp))
    {
        shadowmaps::LightSize = softness;
        reshade::set_config_value(nullptr, CONFIG_SECTION, "ShadowSoftness", softness);
    }

    float range = shadowmaps::Range;
    if (ImGui::SliderFloat("Shadow range", &range, 5.0f, 200.0f, "%.0f", ImGuiSliderFlags_AlwaysClamp))
    {
        shadowmaps::Range = range;
        shadowmaps::RebuildAll();
        reshade::set_config_value(nullptr, CONFIG_SECTION, "ShadowRange", range);
    }

    ImGui::SeparatorText("Emissive lights");
    bool glowLights = glow::Enabled;
    ImGui::BeginDisabled(!glowLights);
    float glowStrength = glow::Strength;
    if (ImGui::SliderFloat("Emissive light strength", &glowStrength, 0.0f, 32.0f, "%.2f",
            ImGuiSliderFlags_AlwaysClamp | ImGuiSliderFlags_Logarithmic))
    {
        glow::Strength = glowStrength;
        reshade::set_config_value(nullptr, CONFIG_SECTION, "EmissiveLightStrength", glowStrength);
    }
    float paintedLight=glow::PaintedLight;
    if(ImGui::SliderFloat("Painted surface light",&paintedLight,0.0f,1.0f,"%.2f",ImGuiSliderFlags_AlwaysClamp))
    {glow::PaintedLight=paintedLight;reshade::set_config_value(nullptr,CONFIG_SECTION,"PaintedEmissionLight",paintedLight);}
    float paintedKept = glow::PaintedKept;
    if (ImGui::SliderFloat("Painted emission kept", &paintedKept, 0.0f, 1.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp))
    {
        glow::PaintedKept = paintedKept;
        reshade::set_config_value(nullptr, CONFIG_SECTION, "PaintedEmissionKept", paintedKept);
    }
    ImGui::EndDisabled();
    ImGui::EndDisabled();

    ImGui::SeparatorText("Sun shadows");
    ImGui::BeginDisabled(!lightbuffer::SunEnabled);
    float sunSoftness = lightbuffer::SunSoftness;
    if (ImGui::SliderFloat("Sun softness (real-Sun multiples)", &sunSoftness, 0.0f, sun_softness_settings::Maximum, "%.2f", ImGuiSliderFlags_AlwaysClamp))
    {
        lightbuffer::SunSoftness = sunSoftness;
        reshade::set_config_value(nullptr, CONFIG_SECTION, "SunSoftness", sunSoftness);
        if (sunSoftness > 0) reshade::set_config_value(nullptr, CONFIG_SECTION, "TextureQuestSunSoftnessPreferenceRealSun", sunSoftness);
    }
    int sunMode = static_cast<int>(lightbuffer::SunShadowMode.load());
    if (ImGui::Combo("Sun shadows", &sunMode, "Full\0Split\0Darker ambient\0"))
    {
        lightbuffer::SunShadowMode = static_cast<lightbuffer::SunMode>(sunMode);
        reshade::set_config_value(nullptr, CONFIG_SECTION, "SunShadowMode", sunMode);
    }
    ImGui::BeginDisabled(sunMode == static_cast<int>(lightbuffer::SunMode::Full));
    float ambientKept = lightbuffer::AmbientKept;
    if (ImGui::SliderFloat("Ambient kept", &ambientKept, 0.0f, 1.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp))
    {
        lightbuffer::AmbientKept = ambientKept;
        reshade::set_config_value(nullptr, CONFIG_SECTION, "AmbientKept", ambientKept);
    }
    ImGui::EndDisabled();
    bool sunDirectionFix = lightbuffer::SunDirectionFix;
    if (ImGui::Checkbox("Sun shadow direction fix", &sunDirectionFix))
    {
        lightbuffer::SunDirectionFix = sunDirectionFix;
        reshade::set_config_value(nullptr, CONFIG_SECTION, "SunShadowDirectionFix", sunDirectionFix);
    }
    if(ImGui::IsItemHovered())ImGui::SetTooltip("Requires supported current native sun-shadow-map evidence. Unknown sources keep native lighting.");
    bool smoothIndoor = lightbuffer::SmoothIndoorTransition;
    if (ImGui::Checkbox("Smooth indoor transition", &smoothIndoor))
    {
        lightbuffer::SmoothIndoorTransition = smoothIndoor;
        reshade::set_config_value(nullptr, CONFIG_SECTION, "SmoothIndoorTransition", smoothIndoor);
    }

    ImGui::EndDisabled();
    ImGui::SeparatorText("Ambient occlusion");
    bool occlusion = lightbuffer::OcclusionEnabled;
    ImGui::BeginDisabled(!occlusion);
    float occlusionRadius = lightbuffer::OcclusionRadius;
    if (ImGui::SliderFloat("Occlusion radius", &occlusionRadius, 0.25f, 8.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp))
    {
        lightbuffer::OcclusionRadius = occlusionRadius;
        reshade::set_config_value(nullptr, CONFIG_SECTION, "OcclusionRadius", occlusionRadius);
    }
    float occlusionStrength = lightbuffer::OcclusionStrength;
    if (ImGui::SliderFloat("Occlusion strength", &occlusionStrength, 0.0f, 2.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp))
    {
        lightbuffer::OcclusionStrength = occlusionStrength;
        reshade::set_config_value(nullptr, CONFIG_SECTION, "OcclusionStrength", occlusionStrength);
    }
    ImGui::EndDisabled();

    ImGui::SeparatorText("Colour");
}

// the settings for finding problems: counts, timings and views of what the add-on computes
static void DrawDebugSettings()
{
    bool ownLampShaders = OwnLampShaders;
    if (ImGui::Checkbox("Rebuild lamp lighting (experimental)", &ownLampShaders))
    {
        OwnLampShaders = ownLampShaders;
        reshade::set_config_value(nullptr, CONFIG_SECTION, "OwnLampShaders", ownLampShaders);
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Replaces EQ2 lamp shaders and changes light falloff and brightness. Leave off to preserve native lamp lighting.");
    bool showColours = shadowmaps::ShowColours;
    if (ImGui::Checkbox("Show shadow colours", &showColours))
        shadowmaps::ShowColours = showColours;
    if (showColours)
        ImGui::TextUnformatted("red: in shadow, green: lit, blue: light has no map, grey: beyond range, magenta: no "
            "camera offset");
    ImGui::BeginDisabled(!lightbuffer::OcclusionEnabled);
    bool showOcclusion = lightbuffer::ShowOcclusion;
    if (ImGui::Checkbox("Show occlusion", &showOcclusion))
        lightbuffer::ShowOcclusion = showOcclusion;
    ImGui::EndDisabled();

    int drawn = 0;
    for (uint64_t key : shadowmaps::SlotLights)
        if (auto it = shadowmaps::Lights.find(key); key && it != shadowmaps::Lights.end() && it->second.BuiltFrame)
            ++drawn;
    ImGui::Text("%zu objects cached, %zu lights seen, %d maps ready", shadowmaps::Casters.size(),
        shadowmaps::Lights.size(), drawn);
    const int frames = std::max(shadowmaps::FramesLastSecond, 1);
    ImGui::Text("per frame: %d extra lighting draws, %d lamp shader draws, %d character map draws; %zu meshes marked "
        "moving", shadowmaps::ExtraDrawsLastSecond / frames, shadowmaps::LampShaderDrawsLastSecond / frames,
        shadowmaps::MovingDrawsLastSecond / frames, shadowmaps::MovingMeshes.size());
    ImGui::Text("%d maps drawn in the last second", shadowmaps::MapsDrawnLastSecond);
    ImGui::Text("%zu window maps, %d drawn in the last second", shadowmaps::Windows.size(),
        shadowmaps::WindowMapsDrawnLastSecond);
    ImGui::Text("%d new objects in the last second, camera at %.1f %.1f %.1f", shadowmaps::CastersAddedLastSecond,
        shadowmaps::Camera.x, shadowmaps::Camera.y, shadowmaps::Camera.z);
    if (ImGui::TreeNode("Maps"))
    {
        for (int slot = 0; slot < shadowmaps::MAX_LIGHTS; ++slot)
        {
            auto it = shadowmaps::Lights.find(shadowmaps::SlotLights[slot]);
            if (!shadowmaps::SlotLights[slot] || it == shadowmaps::Lights.end())
                continue;

            const shadowmaps::Light &light = it->second;
            ImGui::Text("map %d (%u): light at %.1f %.1f %.1f, %zu objects, %s", slot,
                shadowmaps::FACE_SIZES[shadowmaps::GetSizeIndex(slot)], light.Position.x, light.Position.y,
                light.Position.z, light.CasterKeys.size(),
                !shadowmaps::SlotActive[slot] ? "not picked" : light.BuiltFrame ? "drawn" : "waiting");
        }
        ImGui::TreePop();
    }
    ImGui::Text("emissive: %zu emitters, %d cells picked, %d lights drawn, %d painted passes dimmed per frame",
        glow::Emitters.size(), glow::CellsPickedLastFrame, glow::LightsDrawnLastFrame,
        glow::PaintedSkippedLastSecond / frames);
    ImGui::Text("light buffer %s", lightbuffer::LightBuffer ? "found" : "not found");
    ImGui::Text("Window projector shadows: %zu maps, native affine projection",shadowmaps::Windows.size());
    ImGui::Text("Particle emission: %u sources, %u measured, %u selected; %u reads pending",particle_emission::Seen,
        particle_emission::Measured,particle_emission::Active,particle_emission::Pending);
    ImGui::Text("Particle rejects: state %u, bounds/color %u, IO/cap %u",particle_emission::RejectedState,
        particle_emission::RejectedBounds,particle_emission::RejectedIO);
    ImGui::Text("GPU time: ambient occlusion %.2f ms, sun shadow passes %.2f ms, emissive lights %.2f ms",
        lightbuffer::OcclusionTimer.Milliseconds, lightbuffer::SunTimer.Milliseconds, lightbuffer::GlowTimer.Milliseconds);
    ImGui::Text("GPU time:");
    ImGui::Text("  ambient occlusion %.2f ms", lightbuffer::OcclusionTimer.Milliseconds);
    ImGui::Text("  sun shadow passes %.2f ms", lightbuffer::SunTimer.Milliseconds);
    ImGui::Text("  emissive lights %.2f ms", lightbuffer::GlowTimer.Milliseconds);
    ImGui::Text("  surface saturation %.2f ms", lightbuffer::SurfaceTimer.Milliseconds);
    bool cpuProfiling=shadowmaps::CpuProfileEnabled;
    if(ImGui::Checkbox("CPU profiling (diagnostic)",&cpuProfiling))shadowmaps::CpuProfileEnabled=cpuProfiling;
    if(!cpuProfiling){ImGui::TextDisabled("CPU profiling off: no per-event clock reads.");return;}
    const double frameMilliseconds = shadowmaps::CpuSecondMilliseconds / std::max(shadowmaps::CpuFramesLastSecond, 1);
    double total = 0;
    for (const shadowmaps::CpuTimer *timer : shadowmaps::CPU_TIMERS)
        if (timer != &shadowmaps::ShaderSortCpu)
            total += shadowmaps::GetCpuMilliseconds(*timer);
    ImGui::Text("CPU time on the game's render thread, per frame (frame time %.2f ms):", frameMilliseconds);
    ImGui::Text("  total %.2f ms (%.0f%% of the frame)", total, frameMilliseconds > 0 ? 100 * total / frameMilliseconds : 0);
    ImGui::Text("  game draws %.2f ms", shadowmaps::GetCpuMilliseconds(shadowmaps::DrawCpu));
    ImGui::Text("  light buffer passes %.2f ms", shadowmaps::GetCpuMilliseconds(shadowmaps::LightBufferCpu));
    ImGui::Text("  lamp shadow maps %.2f ms", shadowmaps::GetCpuMilliseconds(shadowmaps::MapCpu));
    ImGui::Text("  emissive lights %.2f ms", shadowmaps::GetCpuMilliseconds(shadowmaps::GlowCpu));
    ImGui::Text("  buffer writes and samplers %.2f ms", shadowmaps::GetCpuMilliseconds(shadowmaps::BindCpu));
    ImGui::Text("  end of frame %.2f ms", shadowmaps::GetCpuMilliseconds(shadowmaps::PresentCpu));
    ImGui::Text("  shader sorting %.2f ms in the last second", shadowmaps::ShaderSortCpu.LastSecond / 1e6);
}

static void LoadSettings()
{
    bool enabled = Enabled;
    const bool savedPoint=reshade::get_config_value(nullptr, CONFIG_SECTION, "PointLightShadows", enabled);
    Enabled = enabled;

    float strength = shadowmaps::Strength;
    reshade::get_config_value(nullptr, CONFIG_SECTION, "ShadowStrength", strength);
    shadowmaps::Strength = strength;

    int lights = shadowmaps::LightLimit;
    reshade::get_config_value(nullptr, CONFIG_SECTION, "ShadowedLights", lights);
    shadowmaps::LightLimit = lights;

    bool ownLampShaders = OwnLampShaders;
    reshade::get_config_value(nullptr, CONFIG_SECTION, "OwnLampShaders", ownLampShaders);
    OwnLampShaders = ownLampShaders;

    bool movingShadows = shadowmaps::MovingShadows;
    reshade::get_config_value(nullptr, CONFIG_SECTION, "CharacterShadows", movingShadows);
    shadowmaps::MovingShadows = movingShadows;

    float softness = shadowmaps::LightSize;
    reshade::get_config_value(nullptr, CONFIG_SECTION, "ShadowSoftness", softness);
    shadowmaps::LightSize = softness;

    float range = shadowmaps::Range;
    reshade::get_config_value(nullptr, CONFIG_SECTION, "ShadowRange", range);
    shadowmaps::Range = range;

    bool sunLighting=lightbuffer::SunEnabled;
    const bool explicitSun=reshade::get_config_value(nullptr,CONFIG_SECTION,"SunLighting",sunLighting);
    lightbuffer::SunEnabled=sunLighting;

    const auto sunSettings=sun_softness_settings::Load(
        [](const char *key,auto &value){return reshade::get_config_value(nullptr,CONFIG_SECTION,key,value);},
        [](const char *key,auto value){reshade::set_config_value(nullptr,CONFIG_SECTION,key,value);});
    const float sunSoftness=sunSettings.Active;
    const bool savedSoftness=sunSettings.Saved;
    lightbuffer::SunSoftness=sunSoftness;

    int sunMode = static_cast<int>(lightbuffer::SunShadowMode.load());
    reshade::get_config_value(nullptr, CONFIG_SECTION, "SunShadowMode", sunMode);
    lightbuffer::SunShadowMode = static_cast<lightbuffer::SunMode>(
        std::clamp(sunMode, 0, static_cast<int>(lightbuffer::SunMode::DarkerAmbient)));

    float ambientKept = lightbuffer::AmbientKept;
    reshade::get_config_value(nullptr, CONFIG_SECTION, "AmbientKept", ambientKept);
    lightbuffer::AmbientKept = std::clamp(ambientKept, 0.0f, 1.0f);

    bool sunDirectionFix = lightbuffer::SunDirectionFix;
    const bool savedDirection=reshade::get_config_value(nullptr, CONFIG_SECTION, "SunShadowDirectionFix", sunDirectionFix);
    lightbuffer::SunDirectionFix = sunDirectionFix;

    bool smoothIndoor = lightbuffer::SmoothIndoorTransition;
    const bool savedIndoor=reshade::get_config_value(nullptr, CONFIG_SECTION, "SmoothIndoorTransition", smoothIndoor);
    lightbuffer::SmoothIndoorTransition = smoothIndoor;

    bool occlusion = lightbuffer::OcclusionEnabled;
    const bool savedAO=reshade::get_config_value(nullptr, CONFIG_SECTION, "AmbientOcclusion", occlusion);
    lightbuffer::OcclusionEnabled = occlusion;
    lightbuffer::SunEnabled=tq_world::InitialSunSelection({explicitSun,sunLighting,savedPoint,enabled,
        savedAO,occlusion,savedSoftness,sunSoftness,savedDirection,savedIndoor});

    float occlusionRadius = lightbuffer::OcclusionRadius;
    reshade::get_config_value(nullptr, CONFIG_SECTION, "OcclusionRadius", occlusionRadius);
    lightbuffer::OcclusionRadius = occlusionRadius;

    float occlusionStrength = lightbuffer::OcclusionStrength;
    reshade::get_config_value(nullptr, CONFIG_SECTION, "OcclusionStrength", occlusionStrength);
    lightbuffer::OcclusionStrength = occlusionStrength;

    lightbuffer::SurfaceSaturation = 0; // Optional upstream appearance feature is not part of this update.

    bool glowLights = glow::Enabled;
    // settings saved before the rename are read first, so they carry over
    reshade::get_config_value(nullptr, CONFIG_SECTION, "GlowLights", glowLights);
    reshade::get_config_value(nullptr, CONFIG_SECTION, "EmissiveLights", glowLights);
    glow::Enabled = glowLights;

    float glowStrength = glow::Strength;
    reshade::get_config_value(nullptr, CONFIG_SECTION, "GlowLightStrength", glowStrength);
    reshade::get_config_value(nullptr, CONFIG_SECTION, "EmissiveLightStrength", glowStrength);
    glow::Strength = glowStrength;

    float paintedKept = glow::PaintedKept;
    float paintedLight=glow::PaintedLight;
    reshade::get_config_value(nullptr,CONFIG_SECTION,"PaintedEmissionLight",paintedLight);
    glow::PaintedLight=std::isfinite(paintedLight)?std::clamp(paintedLight,0.0f,1.0f):.25f;
    reshade::get_config_value(nullptr, CONFIG_SECTION, "PaintedGlowKept", paintedKept);
    reshade::get_config_value(nullptr, CONFIG_SECTION, "PaintedEmissionKept", paintedKept);
    glow::PaintedKept = paintedKept;

    int anisotropy = 16;
    reshade::get_config_value(nullptr, CONFIG_SECTION, "Anisotropy", anisotropy);
    tq_world::PresetValues presets{shadowmaps::Strength.load(), shadowmaps::LightSize.load(),
        lightbuffer::OcclusionRadius.load(), lightbuffer::OcclusionStrength.load(),
        shadowmaps::LightLimit.load(), anisotropy};
    tq_world::MigratePresets(
        [](const char* key, auto& value) { return reshade::get_config_value(nullptr, CONFIG_SECTION, key, value); },
        [](const char* key, auto value) { reshade::set_config_value(nullptr, CONFIG_SECTION, key, value); }, presets);
    shadowmaps::Strength = presets.ShadowStrength;
    shadowmaps::LightSize = presets.ShadowSoftness;
    lightbuffer::OcclusionRadius = presets.AORadius;
    lightbuffer::OcclusionStrength = presets.AOStrength;
    shadowmaps::LightLimit = presets.ShadowedLights;
    // Filtering is intentionally fixed at16x whenever its checkbox is enabled.
    reshade::set_config_value(nullptr, CONFIG_SECTION, "Anisotropy", 16);
}

static tq_world::WorldSelection SelectedWorldFeatures()
{
    return {Enabled.load(),lightbuffer::OcclusionEnabled.load(),lightbuffer::SunEnabled.load()};
}

// Frame-boundary retirement, not per-frame work. Keep shader classifications:
// already-created native shaders must still be recognised after re-enabling.
static void SuspendPointScene()
{
    projected_shadows::Reset();
    particle_emission::Reset(true);
    glow::Reset();shadowmaps::Reset();
    shadowmaps::BufferWrites.clear();shadowmaps::BufferWriteFrames.clear();shadowmaps::NewlyRefilled.clear();
    shadowmaps::LastCameraKnown=false;
    FrameLamps.clear();KeptDraws.clear();UnsureMeshes.clear();CulledLamps.clear();
    AddedMarkTexture.Reset();AddedMarkTarget.Reset();AddedMarkView.Reset();AddedMarkFrame=UINT64_MAX;
    ReadOnlyDepthStates.clear();
}

static void CommitWorldWork(std::uint32_t mask)
{
    const auto transition=tq_world::Work.Commit(mask);
    if(transition.ResetScene())lightbuffer::SuspendScene();
    if(transition.ResetPoint())SuspendPointScene();
    if(transition.TrackingChanged()&&!mask)capture::ClearBindings();
}

static void Register()
{
    LoadSettings();
    lightbuffer::DrawExtraLights = &DrawAddedLights;
    shadowmaps::DrawAdditionalMoving = nullptr; // Native affine maps are drawn by DrawWindowMaps.
    shadowmaps::SceneEpochChanged=[](){particle_emission::InvalidateScene();glow::Reset();lightbuffer::ResetSunHistory();};
    reshade::register_event<reshade::addon_event::init_pipeline>(&tq_world::Guard<&OnInitPipeline>::Call);
    reshade::register_event<reshade::addon_event::destroy_pipeline>(&tq_world::Guard<&OnDestroyPipeline>::Call);
    reshade::register_event<reshade::addon_event::init_device>(&tq_world::Guard<&OnInitDevice>::Call);
    reshade::register_event<reshade::addon_event::destroy_device>(&tq_world::Guard<&OnDestroyDevice>::Call);
    reshade::register_event<reshade::addon_event::draw>(&tq_world::WorkGuard<&OnDraw>::Call);
    reshade::register_event<reshade::addon_event::draw_indexed>(&tq_world::WorkGuard<&OnDrawIndexed>::Call);
    reshade::register_event<reshade::addon_event::present>(&tq_world::Guard<&OnPresent>::Call);
    reshade::register_event<reshade::addon_event::map_buffer_region>(&tq_world::WorkGuard<&shadowmaps::OnMapBuffer,true>::Call);
    reshade::register_event<reshade::addon_event::update_buffer_region>(&tq_world::WorkGuard<&shadowmaps::OnUpdateBuffer,true>::Call);
    reshade::register_event<reshade::addon_event::destroy_resource>(&tq_world::Guard<&shadowmaps::OnDestroyResource>::Call);
    lightbuffer::Register();
}
}
