// The game's light buffer: a screen-sized target holding each pixel's light at half scale, built from base light (sun
// and ambient), then added lights, then lamps, which the main pass multiplies into each surface's colour. Two changes
// work on it:
// - ambient occlusion, computed from the light buffer's depth when the game is done with it, darkens only the light
//   that was there before the lamps;
// - the game darkens the finished image with one full-screen pass per sun shadow cascade (image minus a copy of the
//   image times the shadow), which darkens lamp light too. Those passes are given the image times the share of light
//   from before the lamps in place of their image copy, so lamp light fills sun shadows;
// - with softness, the same passes are drawn with replacement shaders that filter the shadow map over a wider disc
//   than the game's 2x2 filter, so sun shadow edges are soft.
// The light buffer is the first screen-sized render target that a later draw in the same frame reads as a texture.
#pragma once
#include <reshade.hpp>
#include "sun_direction_policy.hpp"
#include "sun_source_policy.hpp"
#include <d3d11_1.h>
#include <d3d11shader.h>
#include <d3dcompiler.h>
#include <DirectXMath.h>
#include <wrl/client.h>
#include <algorithm>
#include "sun_state_policy.hpp"
#include "sun_softness_settings.hpp"
#include "owned_sun_constants.hpp"
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <vector>
#include "capture.hpp"
#include "shadow_maps.hpp"

using namespace reshade::api;

namespace lightbuffer
{
using Microsoft::WRL::ComPtr;
using namespace DirectX;

// frames the light buffer may go undrawn before it is looked for again
constexpr uint32_t LIGHT_BUFFER_LIFETIME = 60;
constexpr UINT MAX_READ_SLOTS = 4;

constexpr const char *LIGHT_BUFFER_SHADER = R"(
cbuffer Occlusion : register(b0)
{
    // the inverse of the world-to-clip transform, as rows
    float4 InverseRows[4];
    // the world-to-clip row giving clip w, the distance along the view direction
    float4 ClipW;
    // the light buffer's full-resolution width and height and their inverses
    float4 Size;
    // radius as a share of the screen's width over RADIUS_SCALE, strength (up to 1 the share of occlusion kept, above
    // it a blend toward occlusion squared), half-resolution pixels per world unit at a distance of 1
    float4 Params;
    // the camera's right and up directions in the world
    float4 CameraRight;
    float4 CameraUp;
    // the frame number (x), which picks the half of the occlusion texels computed this frame, and the full-resolution
    // pixels across each occlusion texel (y), 1 up to 1440 lines and more above
    float4 Frame;
};
Texture2D<float> Depth : register(t0);
Texture2D<float2> Occlusion : register(t1);
Texture2D<float4> Image : register(t2);
Texture2D<float4> BaseLight : register(t3);
Texture2D<float4> TotalLight : register(t4);
SamplerState PointClamp : register(s0);
SamplerState LinearClamp : register(s1);

static const float PI = 3.14159265;
static const int SLICES = 2;
static const int STEPS = 8;
// the share of the screen's width one unit of the radius setting covers; a fixed share of the screen gives distant
// surfaces broad shading with nothing small enough to shimmer
static const float RADIUS_SCALE = 0.03;
static const float MAX_TEXELS = 256;
// the distance over which occlusion fades out
static const float FADE_DISTANCE = 1000;
// the share of its distance each texel is moved toward the camera before its samples are compared with it
static const float PLANE_BIAS = 0.004;
// the depth step, as a share of the texel's distance, below which a triangle around a texel counts as flat for its normal
static const float NORMAL_STEP = 0.01;
// the depth step, as a share of the texel's distance, past which a triangle around a texel counts as crossing an edge
static const float EDGE_STEP = 0.05;
static const float NO_DEPTH = 65504;

float4 vs_main(uint id : SV_VertexID) : SV_Position
{
    float2 uv = float2((id << 1) & 2, id & 2);
    return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
}

float3 FindPosition(float2 uv, float depth)
{
    float4 clip = float4(uv.x * 2 - 1, 1 - uv.y * 2, depth, 1);
    float4 p = float4(dot(InverseRows[0], clip), dot(InverseRows[1], clip), dot(InverseRows[2], clip),
        dot(InverseRows[3], clip));
    return p.xyz / p.w;
}

// the full-resolution pixel whose depth an occlusion texel holds, the one under the texel's centre
int2 FindPixel(int2 texel)
{
    return int2((texel + 0.5) * Frame.y);
}

// the view distance of one full-resolution pixel under each occlusion texel, or NO_DEPTH for the sky, so the occlusion
// reads less memory and finds positions without a divide
float ps_half_depth(float4 position : SV_Position) : SV_Target
{
    int2 pixel = FindPixel(int2(position.xy));
    float depth = Depth.Load(int3(pixel, 0));
    if (depth >= 1)
        return NO_DEPTH;
    return dot(ClipW, float4(FindPosition((pixel + 0.5) * Size.zw, depth), 1));
}

// the world direction through a full-resolution pixel, scaled to advance one unit of view distance; it changes linearly
// across the screen, so a pixel's ray is its neighbour's plus a fixed step per pixel
float3 FindRay(float2 pixel)
{
    float2 uv = pixel * Size.zw;
    float3 step = FindPosition(uv, 0.5) - FindPosition(uv, 0.0);
    return step / dot(ClipW.xyz, step);
}

// atan2 to within about 0.004 radians, far finer than the occlusion mask's 32 sectors of a half turn
float2 FindAngle(float2 y, float2 x)
{
    float2 ax = abs(x);
    float2 ay = abs(y);
    float2 a = min(ax, ay) / max(max(ax, ay), 1e-20);
    float2 angle = a * (PI / 4 + 0.273 * (1 - a));
    angle = ay > ax ? PI / 2 - angle : angle;
    angle = x < 0 ? PI - angle : angle;
    return y < 0 ? -angle : angle;
}

// ambient occlusion and the distance along the view direction, at half resolution. Each slice through the view
// direction marks the part of the hemisphere above the surface that each depth sample covers, from its front face to a
// thickness behind it, as bits of a 32-bit mask (visibility bitmask occlusion); the bits are spread evenly over the
// angle from the normal, which builds occlusion up more smoothly than spacing them for cosine-weighted light
float2 ComputeOcclusion(int2 texel, int2 size)
{
    float viewDepth = Depth.Load(int3(texel, 0));
    if (viewDepth >= NO_DEPTH)
        return float2(1, NO_DEPTH);

    // positions are relative to the camera: a texel's is its pixel's ray times its view distance, and the ray of any
    // pixel is this one's plus a step per pixel across and down
    int2 pixel = FindPixel(texel);
    float3 ray = FindRay(pixel + 0.5);
    float3 rayAcross = FindRay(pixel + float2(1.5, 0.5)) - ray;
    float3 rayDown = FindRay(pixel + float2(0.5, 1.5)) - ray;
    float3 p = ray * viewDepth;
    float3 toCamera = -normalize(ray);
    // the normal from the four triangles around the texel, each weighted by how little depth its two neighbours step,
    // so an edge does not bend it; a texel whose every triangle steps far in depth is at an edge
    float3 toNeighbour[4];
    int2 offsets[4] = { int2(-1, 0), int2(0, -1), int2(1, 0), int2(0, 1) };
    [unroll]
    for (int n = 0; n < 4; ++n)
    {
        int2 neighbour = clamp(texel + offsets[n], 0, size - 1);
        float2 neighbourStep = FindPixel(neighbour) - pixel;
        toNeighbour[n] = (ray + rayAcross * neighbourStep.x + rayDown * neighbourStep.y) * Depth.Load(int3(neighbour, 0)) - p;
    }
    float3 normal = 0;
    bool edge = true;
    [unroll]
    for (int n = 0; n < 4; ++n)
    {
        float3 a = toNeighbour[n];
        float3 b = toNeighbour[(n + 1) & 3];
        float step = (abs(dot(a, toCamera)) + abs(dot(b, toCamera))) / viewDepth;
        float3 side = cross(a, b);
        normal += side * rsqrt(max(dot(side, side), 1e-20)) / (NORMAL_STEP * NORMAL_STEP + step * step);
        edge = edge && step > EDGE_STEP;
    }
    normal = normalize(normal);
    if (dot(normal, toCamera) < 0)
        normal = -normal;
    // moved a little toward the camera, so samples on the surface's own plane, off it by the depth buffer's steps, fall
    // below it and cover nothing instead of drawing those steps as stripes
    p += toCamera * viewDepth * PLANE_BIAS;

    // occlusion texels the radius covers, and the radius and thickness in world units
    float texels = min(Params.x * RADIUS_SCALE * Size.x / Frame.y, MAX_TEXELS);
    float radius = texels * viewDepth / Params.z;
    float thickness = log(1 + radius) / 3;

    // the 16 texels of each 4x4 block take evenly spread rotations and step offsets, which the 4x4 filter averages
    uint2 tile = uint2(texel) & 3;
    float noise = ((tile.x + tile.y * 4) * 11 % 16 + 0.5) / 16;
    float jitter = frac(noise * 4);
    float visible = 0;
    float weights = 0;
    [unroll]
    for (int slice = 0; slice < SLICES; ++slice)
    {
        float angle = (slice + noise) * PI / SLICES;
        float2 direction = float2(cos(angle), sin(angle));
        // the slice's screen direction in the world, made square to the view direction
        float3 along = CameraRight.xyz * direction.x - CameraUp.xyz * direction.y;
        along = normalize(along - toCamera * dot(along, toCamera));
        float3 axis = cross(toCamera, along);
        float3 sliceNormal = normal - axis * dot(normal, axis);
        float weight = length(sliceNormal);
        // the normal projected into the slice, and the slice's direction square to it
        float3 up = sliceNormal / max(weight, 1e-6);
        float3 across = normalize(along - up * dot(along, up));
        // each sample's distance across the slice and height over the surface are linear in its view distance and
        // pixel step, so these give them with a few multiplies; the back's are the front's less the thickness toward
        // the camera
        float3 rayAlong = float3(dot(ray, across), dot(rayAcross, across), dot(rayDown, across));
        float3 rayUp = float3(dot(ray, normal), dot(rayAcross, normal), dot(rayDown, normal));
        float2 centre = float2(dot(p, across), dot(p, normal));
        float2 backStep = thickness * float2(dot(toCamera, across), dot(toCamera, normal));
        float heightScale = 1 / max(weight, 1e-6);

        uint mask = 0;
        [unroll]
        for (int side = -1; side <= 1; side += 2)
        {
            [unroll]
            for (int step = 0; step < STEPS; ++step)
            {
                float t = (step + jitter) / STEPS;
                int2 sample = texel + int2(round(direction * side * (1 + t * t * texels)));
                if (any(sample < 0) || any(sample >= size))
                    break;

                float sampleDepth = Depth.Load(int3(sample, 0));
                float2 pixelStep = FindPixel(sample) - pixel;
                float2 front = sampleDepth * float2(dot(rayAlong, float3(1, pixelStep)), dot(rayUp, float3(1, pixelStep)))
                    - centre;
                float2 back = front - backStep;

                // the signed angles of the sample's front and back from the slice's normal, toward the slice's
                // direction, as shares of a half turn from the surface's plane on one side to it on the other; both are
                // measured on the front's side, so a back below the surface stops at the end of the range instead of
                // wrapping to the other end and covering every sector. The height is taken from the surface's own
                // normal, so a sample on its plane that rounding to a texel puts slightly off the slice covers nothing
                float end = front.x < 0 ? -1 : 1;
                float2 angle = FindAngle(end * float2(front.x, back.x), float2(front.y, back.y) * heightScale);
                angle = min(angle < -PI / 2 ? angle + 2 * PI : angle, PI / 2);
                float2 covered = saturate(end * angle / PI + 0.5);
                // rounded, so a sample lying in the surface's own plane covers nothing
                uint first = uint(round(min(covered.x, covered.y) * 32));
                uint count = uint(round(max(covered.x, covered.y) * 32)) - first;
                uint bits = count >= 32 ? 0xffffffff : (1u << count) - 1;
                mask |= first < 32 ? bits << first : 0;
            }
        }
        visible += weight * (1 - countbits(mask) / 32.0);
        weights += weight;
    }
    float occlusion = weights > 0 ? visible / weights : 1;
    float far = viewDepth / FADE_DISTANCE;
    float fade = saturate(1 - far * far) * exp2(-4 * far * far);
    occlusion = lerp(1, saturate(occlusion), saturate(Params.y));
    occlusion = lerp(occlusion, occlusion * occlusion, saturate(Params.y - 1));
    // the distance is stored negated at an edge, where the filter smooths more
    return float2(lerp(1, occlusion, fade), edge ? -viewDepth : viewDepth);
}

RWTexture2D<float2> OcclusionOutput : register(u0);

// each frame computes one texel of each pair in a checkerboard, those whose x + y has the frame number's parity, the
// other keeping its last result; each thread takes the active texel of one pair along a row
[numthreads(8, 8, 1)]
void cs_occlusion(uint3 id : SV_DispatchThreadID)
{
    uint width, height;
    Depth.GetDimensions(width, height);
    int2 texel = int2(id.x * 2 + ((id.y + uint(Frame.x)) & 1), id.y);
    if (texel.x >= int(width) || texel.y >= int(height))
        return;
    OcclusionOutput[texel] = ComputeOcclusion(texel, int2(width, height));
}

)"
R"(
// a guided filter over a 4x4 block, from a texel before this one to two after it, or the mirror of that: within the
// block the occlusion is fitted as a line against depth, so it is smoothed across a surface and kept apart at an edge
float2 FilterOcclusion(float2 position, int flip)
{
    // the least depth variance the fit divides by, relative to the texel's depth: above what the depth buffer's steps
    // give on a flat surface, which the fit would otherwise take for a slope and draw as stripes along each step; larger
    // at an edge, where the normal and so the occlusion are less certain, so the fit leans toward the block's average
    static const float LEAST_VARIANCE = 1e-5;
    static const float LEAST_EDGE_VARIANCE = 1e-3;
    float2 centre = Occlusion.Load(int3(position, 0));
    float centreDepth = abs(centre.y);
    if (centreDepth >= NO_DEPTH)
        return centre;

    // sums of depth, depth squared, occlusion and occlusion times depth, with depth relative to this texel's
    float4 sums = 0;
    float count = 0;
    [unroll]
    for (int y = -1; y <= 2; ++y)
    {
        [unroll]
        for (int x = -1; x <= 2; ++x)
        {
            float2 tap = Occlusion.Load(int3(int2(position) + int2(x, y) * flip, 0));
            float tapDepth = abs(tap.y);
            if (tapDepth <= 0 || tapDepth >= NO_DEPTH)
                continue;

            float depth = tapDepth / centreDepth;
            sums += float4(depth, depth * depth, tap.x, tap.x * depth);
            count += 1;
        }
    }
    sums /= count;
    float slope = (sums.w - sums.x * sums.z)
        / max(sums.y - sums.x * sums.x, centre.y < 0 ? LEAST_EDGE_VARIANCE : LEAST_VARIANCE);
    return float2(saturate(sums.z + slope * (1 - sums.x)), centre.y);
}

float2 ps_blur_x(float4 position : SV_Position) : SV_Target
{
    return FilterOcclusion(position.xy, 1);
}

float2 ps_blur_y(float4 position : SV_Position) : SV_Target
{
    return FilterOcclusion(position.xy, -1);
}

// the occlusion at a full-resolution pixel, with Depth holding the full-resolution depth: the four nearest
// half-resolution texels, weighted by how close their distance along the view direction is to the pixel's, so occlusion
// does not spread across the edges of objects
float FindOcclusion(float2 pixel)
{
    float depth = Depth.Load(int3(pixel, 0));
    if (depth >= 1)
        return 1;

    float viewDepth = dot(ClipW, float4(FindPosition(pixel * Size.zw, depth), 1));
    uint width, height;
    Occlusion.GetDimensions(width, height);
    // the pixel's centre in occlusion texels, whose centres are at whole numbers
    float2 position = (floor(pixel) + 0.5) / Frame.y - 0.5;
    int2 corner = int2(floor(position));
    float2 fraction = position - corner;
    float sum = 0;
    float total = 0;
    float nearest = 1;
    float nearestDifference = 1e30;
    [unroll]
    for (int n = 0; n < 4; ++n)
    {
        int2 offset = int2(n & 1, n >> 1);
        float2 texel = Occlusion.Load(int3(clamp(corner + offset, 0, int2(width, height) - 1), 0));
        float2 bilinear = offset == 1 ? fraction : 1 - fraction;
        float difference = abs(viewDepth - abs(texel.y));
        float weight = bilinear.x * bilinear.y * exp(-difference / max(0.02 * viewDepth, 0.0001));
        sum += texel.x * weight;
        total += weight;
        if (difference < nearestDifference)
        {
            nearestDifference = difference;
            nearest = texel.x;
        }
    }
    return total > 0.00001 ? sum / total : nearest;
}

// what to take off the light buffer: the light from before the lamps times the occlusion
float4 ps_apply(float4 position : SV_Position) : SV_Target
{
    return float4(BaseLight.Load(int3(position.xy, 0)).rgb * (1 - FindOcclusion(position.xy)), 0);
}

// the occlusion in grey over the whole image
float4 ps_show(float4 position : SV_Position) : SV_Target
{
    return float4(FindOcclusion(position.xy).xxx, 1);
}

cbuffer Sun : register(b1)
{
    // the inverse of the world-to-clip transform, as rows
    float4 SunInverseRows[4];
    // the sun's colour; w is 1 when the sun's share is split off, with this frame's sun, ambient and depth known
    float4 SunColour;
    // the direction toward the sun
    float4 SunDirection;
    // the ambient colour the sun's share is found with: the game's, times the share of it kept
    float4 Ambient;
    // the ambient light the Darker ambient mode takes off the light buffer before the lamps
    float4 RemovedAmbient;
};
Texture2D<float> SunDepth : register(t5);

// the share of a pixel's light from before the lamps that comes from the sun rather than the ambient light. The game's
// sun-lit shaders write half of the sun's light plus the ambient colour, with the sun's direction taken from the mesh, so
// the sun's part is what twice that light holds beyond the ambient colour
float3 FindSunShare(float2 pixel)
{
    float3 base = BaseLight.Load(int3(pixel, 0)).rgb;
    if (SunColour.w <= 0)
        return 1;

    return float3(base.r > 0 ? saturate(1 - Ambient.r / (2 * base.r)) : 1,
        base.g > 0 ? saturate(1 - Ambient.g / (2 * base.g)) : 1, base.b > 0 ? saturate(1 - Ambient.b / (2 * base.b)) : 1);
}

// what the Darker ambient mode takes off the light buffer before the lamps: the ambient light it does not keep, on
// geometry
float4 ps_darken(float4 position : SV_Position) : SV_Target
{
    if (Depth.Load(int3(position.xy, 0)) >= 1)
        discard;
    return float4(RemovedAmbient.rgb, 0);
}

// the image times the share of each pixel's light its sun shadow may take off: the share that was there before the
// lamps, after occlusion, less the ambient part of it when a sun shadow mode splits that off
float4 ps_scale(float4 position : SV_Position) : SV_Target
{
    int3 pixel = int3(position.xy, 0);
    float4 image = Image.Load(pixel);
    float3 base = round(BaseLight.Load(pixel).rgb * FindOcclusion(position.xy) * 255) / 255;
    float3 total = TotalLight.Load(pixel).rgb;
    float3 share = float3(total.r > 0 ? saturate(base.r / total.r) : 1, total.g > 0 ? saturate(base.g / total.g) : 1,
        total.b > 0 ? saturate(base.b / total.b) : 1);
    return float4(image.rgb * share * FindSunShare(position.xy), image.a);
}
)";

constexpr const char *SUN_SHADOW_SHADER = R"(
// the game's three sun shadow cascade passes, each taking its cascade's share of the sun's shadow off the image (the
// pass blends image minus image times its output); with no softness each gives the game's own output, with softness
// the shadow map is filtered over a disc
cbuffer GameConstants : register(b0)
{
    // world-to-shadow-map rows of the near, middle and far cascade, four each: x, y, depth and an unused w
    float4 CascadeRows[12];
    // view space to camera-relative world space, as rows
    float4 ViewToWorld[4];
    // the near cascade's depth bias (x)
    float4 NearBias;
    float4 Unused17;
    // the distance from FadeCentre past which the shadow fades (x)
    float4 FadeStart;
    // a tenth of the shadow's strength (x)
    float4 Strength;
    float4 Unused20[3];
    // the far cascade's depth bias (x)
    float4 FarBias;
    float4 Unused24;
    // the middle cascade's depth bias (x)
    float4 MiddleBias;
    // the shadow maps' width and height in texels (x)
    float4 MapSize;
    // the distance past FadeStart over which the shadow fades out (x)
    float4 FadeLength;
    float4 Unused28[2];
    // the point the fade distance is measured from, in view space (xyz)
    float4 FadeCentre;
    // half a pixel (xy); with the screen's width and height below, the offset the depth and facing are read at and the
    // game adds to some cascade positions
    float4 HalfPixel;
    // how fast the near (x) and middle (y) cascades fade in from their edges, 1 over the fade's width in map units
    float4 EdgeFadeRate;
    float4 ScreenWidth;
    float4 ScreenHeight;
    // below 0.75 (x) the shadow fades with fog, otherwise it is scaled by 1.3 minus this
    float4 FogOverride;
};
// how far the soft edge spreads per world unit between the shadow's caster and the surface it falls on (x), 0 running the
// game's own filter; above 0, the soft shadow is upsampled from HalfShadow rather than filtered (w)
cbuffer Softness : register(b1)
{
    // x the soft edge's spread, w the full-resolution pixels across each texel of HalfShadow, or 0 when the soft
    // shadow is found in the pass itself
    float4 Softness;
};
// the distance along the view direction of each pixel's surface
Texture2D ViewDepth : register(t0);
// a copy of the image
Texture2D Image : register(t1);
// the pass's cascade shadow map, the depth nearest the sun
Texture2D ShadowMap : register(t2);
// how much of the sun's light each pixel's surface faces
Texture2D Facing : register(t3);
// the soft shadow at half resolution (x) and the view depth it was found at (y), -1 where it was skipped; a slot the
// game's sun passes do not read
Texture2D HalfShadow : register(t4);
SamplerState ViewDepthSampler : register(s0);
SamplerState ImageSampler : register(s1);
SamplerState ShadowMapSampler : register(s2);
SamplerState FacingSampler : register(s3);

// the game's full-screen vertex shader writes this layout: texcoord0.xy the screen position, texcoord1.xy the view
// direction at a depth of 1, texcoord2.yzw the fog's offset, scale and 1 over its distance
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

static const int NEAR = 0;
static const int MIDDLE = 1;
static const int FAR = 2;

// the farthest a caster is looked for, in world units between it and the surface
static const float SEARCH_DISTANCE = 20;
// the widest soft edge and blocker search, in world units. The cascades' maps are the same size over very different
// areas, so the radius is set in world units and only then turned into each cascade's texels, which keeps the soft edge
// the same in every cascade
static const float MAX_SOFT_UNITS = 1;
// the filter takes this many taps per texel of its radius, from MIN_SOFT_TAPS up to MAX_SOFT_TAPS, so a radius many
// texels wide, as in the near cascade, is not left with gaps between its taps. The blocker search only needs an average
// depth and runs on every pixel, so it keeps MIN_SOFT_TAPS
static const float SOFT_TAPS_PER_TEXEL = 1.6;
static const int MIN_SOFT_TAPS = 16;
static const int MAX_SOFT_TAPS = 32;
static const float GOLDEN_ANGLE = 2.3999632;

// 1 where the shadow map texel at the position is nearer the sun than the receiver's depth
float FindShadowed(float2 position, float depth, float bias)
{
    return ShadowMap.Sample(ShadowMapSampler, position).x + bias < depth ? 1 : 0;
}

// the cascade's shadow map position of a world position, from the map's centre with y up, plus an offset the game adds
// for some cascades
float2 FindCentredPosition(float4 world, int cascade, float2 offset)
{
    return float2(dot(world, CascadeRows[cascade * 4]), dot(world, CascadeRows[cascade * 4 + 1])) * 0.5 + offset;
}

// the shadow map position from the top left of a centred position
float2 FindMapPosition(float2 centred)
{
    return float2(centred.x + 0.5, 1 - (centred.y + 0.5));
}

float FindMapDepth(float4 world, int cascade)
{
    return dot(world, CascadeRows[cascade * 4 + 2]);
}

// FindShadowed whole texels right of and below a centred position; the game reads the position's own row at
// 1 - (y + 0.5) and the rows below at 0.5 - y, which round differently
float FindGameShadowed(float2 centred, float2 texels, float depth, float bias)
{
    float x = centred.x + 0.5 + texels.x / MapSize.x;
    float y = texels.y == 0 ? 1 - (centred.y + 0.5) : 0.5 - centred.y + texels.y / MapSize.x;
    return FindShadowed(float2(x, y), depth, bias);
}

// the game's 2x2 tap, whole texels off the position: the texel the position falls in and the next ones right and down,
// weighted by the position's place in its texel
float FindGameTap(float2 centred, float2 texels, float2 weight, float depth, float bias)
{
    float top = lerp(FindGameShadowed(centred, texels, depth, bias),
        FindGameShadowed(centred, texels + float2(1, 0), depth, bias), weight.x);
    float bottom = lerp(FindGameShadowed(centred, texels + float2(0, 1), depth, bias),
        FindGameShadowed(centred, texels + float2(1, 1), depth, bias), weight.x);
    return lerp(top, bottom, weight.y);
}

// the game's filter: a 2x2 tap at the position, weighted twice the taps two texels right and two texels down
float FindGameShadow(float2 centred, float depth, float bias)
{
    float2 weight = frac(FindMapPosition(centred) * MapSize.x);
    return (FindGameTap(centred, float2(0, 0), weight, depth, bias)
        + FindGameTap(centred, float2(2, 0), weight, depth, bias) * 0.5
        + FindGameTap(centred, float2(0, 2), weight, depth, bias) * 0.5) * 0.5;
}

// the taps a soft shadow's search or filter takes over a radius of this many texels
int FindSoftTapCount(float texels)
{
    return (int)clamp(ceil(texels * SOFT_TAPS_PER_TEXEL), MIN_SOFT_TAPS, MAX_SOFT_TAPS);
}

// tap n of count on a disc of radius 1, spread evenly in a golden-angle spiral turned by the pixel's angle
float2 FindSoftTap(int n, int count, float angle)
{
    float turn = n * GOLDEN_ANGLE + angle;
    return sqrt((n + 0.5) / count) * float2(cos(turn), sin(turn));
}

// contact-hardening soft shadow, as for lamps: finds the average depth of whatever blocks the sun around the position,
// then filters over a disc that widens with the distance from it, each tap a bilinear 2x2 compare. Both radii are in
// world units, at least a texel
// Gather order is bottom left, bottom right, top right, top left. Outside the known map
// is unavailable shadow data, so it cannot create a blocker through native border/clamp addressing.
float4 FindSoftFootprintValidity(float2 position)
{
    float2 corner = floor(position * MapSize.x - 0.5);
    bool left = corner.x >= 0 && corner.x < MapSize.x;
    bool right = corner.x + 1 >= 0 && corner.x + 1 < MapSize.x;
    bool top = corner.y >= 0 && corner.y < MapSize.x;
    bool bottom = corner.y + 1 >= 0 && corner.y + 1 < MapSize.x;
    return float4(left && bottom, right && bottom, right && top, left && top);
}

float FindSoftShadow(float2 position, float depth, float bias, int cascade, float2 pixel)
{
    // the game's filter is centred one texel right and down of the position
    float2 centre = position + 1 / MapSize.x;
    float texelsPerUnit = 0.5 * length(CascadeRows[cascade * 4].xyz) * MapSize.x;
    float unitsPerDepth = 1 / length(CascadeRows[cascade * 4 + 2].xyz);
    float angle = 6.2831853 * frac(52.9829189 * frac(dot(pixel, float2(0.06711056, 0.00583715))));

    float searchTexels = max(min(Softness.x * SEARCH_DISTANCE, MAX_SOFT_UNITS) * texelsPerUnit, 1);
    int searchTaps = MIN_SOFT_TAPS;
    float searchRadius = searchTexels / MapSize.x;
    float blockerSum = 0;
    float blockerCount = 0;
    [loop]
    for (int n = 0; n < searchTaps; ++n)
    {
        float2 tap = centre + FindSoftTap(n, searchTaps, angle) * searchRadius;
        float4 valid = FindSoftFootprintValidity(tap);
        float4 mapDepth = ShadowMap.Gather(ShadowMapSampler, tap);
        mapDepth = valid > 0 ? mapDepth : 0;
        float4 blocks = (mapDepth + bias < depth) * valid;
        blockerSum += dot(mapDepth, blocks);
        blockerCount += dot(blocks, 1);
    }
    // nothing around blocks the sun, or everything does: no soft edge to filter
    if (blockerCount == 0)
        return 0;
    if (blockerCount == 4 * searchTaps)
        return 1;

    float distance = (depth - blockerSum / blockerCount) * unitsPerDepth;
    float texels = max(min(Softness.x * distance, MAX_SOFT_UNITS) * texelsPerUnit, 1);
    int taps = FindSoftTapCount(texels);
    float radius = texels / MapSize.x;
    float sum = 0;
    [loop]
    for (int m = 0; m < taps; ++m)
    {
        float2 tap = centre + FindSoftTap(m, taps, angle) * radius;
        float4 shadowed = (ShadowMap.Gather(ShadowMapSampler, tap) + bias < depth)
            * FindSoftFootprintValidity(tap);
        float2 weight = frac(tap * MapSize.x - 0.5);
        // Gather returns the 2x2 block as bottom left, bottom right, top right, top left
        sum += lerp(lerp(shadowed.w, shadowed.z, weight.x), lerp(shadowed.x, shadowed.y, weight.x), weight.y);
    }
    return sum / taps;
}

// 1 inside the cascade's square, falling to 0 over its last 1/500
float FindInside(float2 position)
{
    return saturate(position.x * 500) * saturate((1 - position.x) * 500) * saturate(position.y * 500)
        * saturate((1 - position.y) * 500);
}

// rises from 0 at the cascade's edge to 1 at 1/rate inside it
float FindEdgeFade(float2 position, float rate)
{
    return min(min(min(position.x, position.y) * rate, 1), min((1 - max(position.x, position.y)) * rate, 1));
}

// half a pixel in screen texture units
float2 FindPixelOffset()
{
    return float2(HalfPixel.x / ScreenWidth.x, HalfPixel.y / ScreenHeight.x);
}

struct Surface
{
    // where the depth and facing are read, half a pixel off the pixel's position
    float2 screen;
    float depth;
    float3 view;
    float4 world;
    float facing;
};

Surface FindSurface(GameInterpolants i)
{
    Surface surface;
    surface.screen = i.texcoord0.xy + FindPixelOffset();
    surface.depth = ViewDepth.Sample(ViewDepthSampler, surface.screen).x;
    surface.view = float3(surface.depth * i.texcoord1.xy, surface.depth);
    float4 view = float4(surface.view, 1);
    surface.world = float4(dot(view, ViewToWorld[0]), dot(view, ViewToWorld[1]), dot(view, ViewToWorld[2]),
        dot(view, ViewToWorld[3]));
    surface.facing = Facing.Sample(FacingSampler, surface.screen).x;
    return surface;
}

)"
R"(// a surface's place in the pass's cascade
struct Receiver
{
    float2 centred;
    float depth;
    float bias;
    // what the shadow is scaled by: for the near and middle cascades 1 inside the cascade's square, falling to 0 at its
    // edge; for the far cascade 0 inside the near cascade
    float mask;
    // what the pass's output is scaled by
    float weight;
};

// the near cascade, fading out at its edges
Receiver FindNearReceiver(Surface surface)
{
    Receiver receiver;
    receiver.centred = FindCentredPosition(surface.world, NEAR, 0);
    float2 position = FindMapPosition(receiver.centred);
    receiver.depth = FindMapDepth(surface.world, NEAR);
    receiver.bias = NearBias.x;
    receiver.mask = FindInside(position);
    receiver.weight = FindEdgeFade(position, EdgeFadeRate.x);
    return receiver;
}

// the middle cascade, fading in across the near cascade's edge and out at its own
Receiver FindMiddleReceiver(Surface surface)
{
    Receiver receiver;
    float2 offset = FindPixelOffset();
    receiver.centred = FindCentredPosition(surface.world, MIDDLE, offset);
    float2 position = FindMapPosition(receiver.centred);
    float2 nearPosition = FindMapPosition(FindCentredPosition(surface.world, NEAR, offset));
    receiver.depth = FindMapDepth(surface.world, MIDDLE);
    receiver.bias = MiddleBias.x;
    receiver.mask = FindInside(position);
    receiver.weight = all(nearPosition >= 0 && nearPosition <= 1) ? 1 - FindEdgeFade(nearPosition, EdgeFadeRate.x)
        : FindEdgeFade(position, EdgeFadeRate.y);
    return receiver;
}

// the far cascade, cut off inside the near cascade and fading in across the middle cascade's edge
Receiver FindFarReceiver(Surface surface)
{
    Receiver receiver;
    float2 offset = FindPixelOffset();
    float2 middlePosition = FindMapPosition(FindCentredPosition(surface.world, MIDDLE, offset));
    receiver.centred = FindCentredPosition(surface.world, FAR, 0);
    receiver.depth = FindMapDepth(surface.world, FAR);
    receiver.bias = FarBias.x;
    receiver.mask = 1 - FindInside(FindMapPosition(FindCentredPosition(surface.world, NEAR, offset)));
    receiver.weight = 1 - FindEdgeFade(middlePosition, EdgeFadeRate.y) * FindInside(middlePosition + 0.0001);
    return receiver;
}

// what the shadow counts for in the pass's output: 0 where the output is scaled to nothing, so the soft filter can be
// skipped
float FindCoverage(Surface surface, Receiver receiver)
{
    return surface.facing != 0 ? receiver.mask * receiver.weight : 0;
}

// the soft shadow upsampled from HalfShadow: the four nearest reduced-resolution texels, weighted bilinearly and by
// how near their depth is to the pixel's, or the nearest in depth where none is near
float FindUpsampledShadow(float2 pixel, float depth)
{
    uint width, height;
    HalfShadow.GetDimensions(width, height);
    float2 position = pixel / Softness.w - 0.5;
    int2 corner = (int2)floor(position);
    float2 fraction = position - corner;
    float sum = 0;
    float total = 0;
    float nearest = 0;
    float nearestDifference = 1e30;
    [unroll]
    for (int n = 0; n < 4; ++n)
    {
        int2 offset = int2(n & 1, n >> 1);
        float2 texel = HalfShadow.Load(int3(clamp(corner + offset, 0, int2(width, height) - 1), 0)).xy;
        // Coverage skips use depth -1. They are not surfaces and cannot win the nearest-depth fallback.
        if (!all(isfinite(texel)) || texel.y < 0)
            continue;
        float2 bilinear = offset == 1 ? fraction : 1 - fraction;
        float difference = abs(depth - texel.y);
        float weight = bilinear.x * bilinear.y * exp(-difference / max(0.02 * depth, 0.0001));
        sum += texel.x * weight;
        total += weight;
        if (difference < nearestDifference)
        {
            nearestDifference = difference;
            nearest = texel.x;
        }
    }
    return total > 0.00001 ? sum / total : nearest;
}

// the filtered share in shadow; with softness, skipped where the pass's output is scaled by a coverage of 0
float FindShadow(GameInterpolants i, Surface surface, Receiver receiver, int cascade)
{
    float shadow = 0;
    [branch]
    if (Softness.x <= 0)
        shadow = FindGameShadow(receiver.centred, receiver.depth, receiver.bias);
    else if (FindCoverage(surface, receiver) > 0)
    {
        [branch]
        if (Softness.w > 0)
            shadow = FindUpsampledShadow(i.position.xy, surface.depth);
        else
            shadow = FindSoftShadow(FindMapPosition(receiver.centred), receiver.depth, receiver.bias, cascade,
                i.position.xy);
    }
    return shadow;
}

// what to take off the image: the image times the shadow's strength, faded with distance and fog, times the surface's
// facing and the cascade's weight
float4 FinishPass(GameInterpolants i, Surface surface, Receiver receiver, int cascade)
{
    float darkening = FindShadow(i, surface, receiver, cascade) * receiver.mask * Strength.x * 10;
    float distance = length(surface.view - FadeCentre.xyz) - FadeStart.x;
    darkening -= darkening / FadeLength.x * distance * saturate(distance);
    darkening *= surface.facing;
    float3 color = Image.Sample(ImageSampler, i.texcoord0.xy).rgb * darkening;

    float fogDistance = saturate(surface.depth * i.texcoord2.w);
    float fog = exp2(fogDistance * fogDistance);
    float clear = fog == 1 ? 1 : saturate((1 / fog * 2 - 2) * i.texcoord2.z - i.texcoord2.y + 1);
    color *= FogOverride.x < 0.75 ? clear : 1.3 - FogOverride.x;
    return float4(color * receiver.weight, 1);
}

// the soft shadow for the full-resolution pass to upsample, drawn at half resolution: the shadow and the view depth
// it was found at, the depth -1 where the shadow was skipped so the upsample leaves it out
float4 FindHalfShadow(GameInterpolants i, Surface surface, Receiver receiver, int cascade)
{
    float4 result = float4(0, -1, 0, 0);
    [branch]
    if (FindCoverage(surface, receiver) > 0)
        result.xy = float2(FindSoftShadow(FindMapPosition(receiver.centred), receiver.depth, receiver.bias, cascade,
            i.position.xy), surface.depth);
    return result;
}

float4 ps_sun_near(GameInterpolants i) : SV_Target
{
    Surface surface = FindSurface(i);
    return FinishPass(i, surface, FindNearReceiver(surface), NEAR);
}

float4 ps_sun_middle(GameInterpolants i) : SV_Target
{
    Surface surface = FindSurface(i);
    return FinishPass(i, surface, FindMiddleReceiver(surface), MIDDLE);
}

float4 ps_sun_far(GameInterpolants i) : SV_Target
{
    Surface surface = FindSurface(i);
    return FinishPass(i, surface, FindFarReceiver(surface), FAR);
}

float4 ps_sun_near_half(GameInterpolants i) : SV_Target
{
    Surface surface = FindSurface(i);
    return FindHalfShadow(i, surface, FindNearReceiver(surface), NEAR);
}

float4 ps_sun_middle_half(GameInterpolants i) : SV_Target
{
    Surface surface = FindSurface(i);
    return FindHalfShadow(i, surface, FindMiddleReceiver(surface), MIDDLE);
}

float4 ps_sun_far_half(GameInterpolants i) : SV_Target
{
    Surface surface = FindSurface(i);
    return FindHalfShadow(i, surface, FindFarReceiver(surface), FAR);
}
)";

// boosts the chroma of the opaque world's surface colours and puts the light back. The game's colour pass writes twice
// its surface colour times the light buffer, clamped to 1, so the surface is the image over twice the light where that
// light is reliable
constexpr const char *SURFACE_SHADER = R"(
cbuffer Surface : register(b0)
{
    // the saturation setting from 0 to 1 (x)
    float4 Saturation;
};
Texture2D<float4> Image : register(t0);
Texture2D<float4> Light : register(t1);
Texture2D<float> Depth : register(t2);

// what the game's colour pass multiplies the light buffer by
static const float LIGHT_SCALE = 2;
// the OKLab chroma at or past which a surface colour is left as it is; duller colours get up to 1 + Saturation.x times
// their chroma, twice it for grey-ish colours at full saturation
static const float FULL_CHROMA = 0.25;
static const int GAMUT_STEPS = 8;

float3 ToLinear(float3 c)
{
    return c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4);
}

float3 ToGamma(float3 c)
{
    return c <= 0.0031308 ? c * 12.92 : 1.055 * pow(c, 1 / 2.4) - 0.055;
}

float3 ToOkLab(float3 rgb)
{
    float3 lms = float3(dot(float3(0.4122214708, 0.5363325363, 0.0514459929), rgb),
        dot(float3(0.2119034982, 0.6806995451, 0.1073969566), rgb),
        dot(float3(0.0883024619, 0.2817188376, 0.6299787005), rgb));
    lms = pow(max(lms, 0), 1.0 / 3);
    return float3(dot(float3(0.2104542553, 0.7936177850, -0.0040720468), lms),
        dot(float3(1.9779984951, -2.4285922050, 0.4505937099), lms),
        dot(float3(0.0259040371, 0.7827717662, -0.8086757660), lms));
}

float3 FromOkLab(float3 lab)
{
    float3 lms = float3(dot(float3(1, 0.3963377774, 0.2158037573), lab), dot(float3(1, -0.1055613458, -0.0638541728), lab),
        dot(float3(1, -0.0894841775, -1.2914855480), lab));
    lms = lms * lms * lms;
    return float3(dot(float3(4.0767416621, -3.3077115913, 0.2309699292), lms),
        dot(float3(-1.2684380046, 2.6097574011, -0.3413193965), lms),
        dot(float3(-0.0041960863, -0.7034186147, 1.7076147010), lms));
}

float4 ps_surface(float4 position : SV_Position) : SV_Target
{
    int3 pixel = int3(position.xy, 0);
    float4 image = Image.Load(pixel);
    float3 light = Light.Load(pixel).rgb * LIGHT_SCALE;
    float3 surface = image.rgb / max(light, 1e-4);
    // the division is left alone in dim light, on the sky, where the game clamped the image, and where the image holds
    // more than its light gives, as specular, emissive and fogged pixels do
    float trust = smoothstep(0.02, 0.08, min(light.r, min(light.g, light.b)))
        * (1 - smoothstep(0.96, 0.995, max(image.r, max(image.g, image.b))))
        * (1 - smoothstep(1, 1.15, max(surface.r, max(surface.g, surface.b))));
    float amount = Saturation.x * trust;
    if (amount <= 0 || Depth.Load(pixel) >= 1)
        return image;

    float3 lab = ToOkLab(ToLinear(saturate(surface)));
    float chroma = length(lab.yz);
    if (chroma < 1e-5)
        return image;

    // the largest step toward the boosted chroma whose colour, times the light, the image can still hold
    float3 upper = ToLinear(min(1, 1 / max(light, 1e-4)));
    float low = 1;
    float high = 1 + amount * saturate(1 - chroma / FULL_CHROMA);
    [unroll]
    for (int step = 0; step < GAMUT_STEPS; ++step)
    {
        float scale = (low + high) / 2;
        float3 rgb = FromOkLab(float3(lab.x, lab.yz * scale));
        if (all(rgb >= 0) && all(rgb <= upper))
            low = scale;
        else
            high = scale;
    }
    float3 boosted = ToGamma(saturate(FromOkLab(float3(lab.x, lab.yz * low))));
    return float4(saturate(boosted * light), image.a);
}
)";

// the sun shadow passes' receiver depth, the view distance of each surface the game draws as a sun shadow receiver, with
// the pixels it leaves at its clear value filled from the light buffer's depth where the game's receiver depth from the
// last frame with its usual casters had a receiver at the same point; elsewhere, as on characters, which the game never
// draws as receivers, it is left clear, so they take no kept shadow either. The game writes each receiver's facing,
// saturate(N.L), with its depth and clears it to 1; a filled pixel takes the kept facing from the same point, so a
// surface facing away from the sun stays unshadowed as it was
constexpr const char *RECEIVER_SHADER = R"(
cbuffer Receiver : register(b0)
{
    // the inverse of the world-to-clip transform, as rows
    float4 InverseRows[4];
    // the world-to-clip row giving clip w, the distance along the view direction
    float4 ClipW;
    // the screen's width and height and their inverses
    float4 Size;
    // the world-to-clip transform of the kept receiver depth's frame, as rows
    float4 KeptRows[4];
    // how far the camera has moved since that frame
    float4 CameraMoved;
};
Texture2D<float> Depth : register(t0);
Texture2D<float> GameReceiverDepth : register(t1);
Texture2D<float> KeptReceiverDepth : register(t2);
Texture2D<float> GameFacing : register(t3);
Texture2D<float> KeptFacing : register(t4);

struct ReceiverOutput
{
    float depth : SV_Target0;
    float facing : SV_Target1;
};

// the game clears its receiver depth to 1e10
static const float NO_RECEIVER = 1e9;
// how far a kept receiver may lie from the surface now at its point, as a share of its distance
static const float KEPT_TOLERANCE = 0.03;

ReceiverOutput ps_receiver_depth(float4 position : SV_Position)
{
    int3 pixel = int3(position.xy, 0);
    ReceiverOutput output;
    output.depth = GameReceiverDepth.Load(pixel);
    output.facing = GameFacing.Load(pixel);
    float depth = Depth.Load(pixel);
    if (output.depth < NO_RECEIVER || depth >= 1)
        return output;

    float2 uv = position.xy * Size.zw;
    float4 clip = float4(uv.x * 2 - 1, 1 - uv.y * 2, depth, 1);
    float4 p = float4(dot(InverseRows[0], clip), dot(InverseRows[1], clip), dot(InverseRows[2], clip),
        dot(InverseRows[3], clip));
    float4 world = float4(p.xyz / p.w, 1);
    // positions are relative to the camera, so the point is moved by as much as the camera has to find it in the kept frame
    float4 keptPoint = float4(world.xyz + CameraMoved.xyz, 1);
    float4 keptClip = float4(dot(KeptRows[0], keptPoint), dot(KeptRows[1], keptPoint), dot(KeptRows[2], keptPoint),
        dot(KeptRows[3], keptPoint));
    if (keptClip.w <= 0)
        return output;

    float2 keptUv = float2(keptClip.x / keptClip.w * 0.5 + 0.5, 0.5 - keptClip.y / keptClip.w * 0.5);
    if (any(keptUv < 0) || any(keptUv >= 1))
        return output;

    int3 keptPixel = int3(keptUv * Size.xy, 0);
    float kept = KeptReceiverDepth.Load(keptPixel);
    if (abs(kept - keptClip.w) > KEPT_TOLERANCE * keptClip.w)
        return output;

    output.depth = dot(ClipW, world);
    output.facing = KeptFacing.Load(keptPixel);
    return output;
}
)";

// the game's sun shadow cascade pixel shaders that have a replacement, by the hash of their code
struct SunShader
{
    uint64_t Hash;
    const char *Entry;
    // the entry point that finds the pass's soft shadow at half resolution
    const char *HalfEntry;
};

constexpr SunShader SUN_SHADERS[] = {
    { 0xda2115c6adf602bc, "ps_sun_near", "ps_sun_near_half" },
    { 0x3c9b0cbf40efc97d, "ps_sun_middle", "ps_sun_middle_half" },
    { 0x62815a7d83cc5713, "ps_sun_far", "ps_sun_far_half" },
};
constexpr size_t SUN_SHADER_COUNT = std::size(SUN_SHADERS);
// the pixel shader constant buffer slot of the softness, one the game's sun shaders do not read
constexpr UINT SOFTNESS_SLOT = 1;
// the pixel shader resource slot of the half-resolution sun shadow, one the game's sun shaders do not read
constexpr UINT HALF_SHADOW_SLOT = 4;

struct OcclusionConstants
{
    XMFLOAT4X4 InverseRows;
    XMFLOAT4 ClipW;
    XMFLOAT4 Size;
    XMFLOAT4 Params;
    XMFLOAT4 CameraRight;
    XMFLOAT4 CameraUp;
    XMFLOAT4 Frame;
};

struct SunConstants
{
    XMFLOAT4X4 InverseRows;
    XMFLOAT4 Colour, Direction, Ambient, RemovedAmbient;
};

// Sun softness uses real-Sun angular-radius multiples.
static std::atomic<bool> SunEnabled = false; // Visible umbrella; child settings remain saved while off.
static std::atomic<float> SunSoftness = 0.0f;
// what a sun shadow takes off: all the light from before the lamps (Full), only the sun's share of it (Split), or the
// sun's share after part of the ambient light is taken off the geometry before the lamps (DarkerAmbient)
enum class SunMode { Full, Split, DarkerAmbient };
static std::atomic<SunMode> SunShadowMode = SunMode::Full;
// the share of the game's ambient light the Split and Darker ambient modes keep
static std::atomic<float> AmbientKept = 0.25f;
// lights the game's sun-lit animated meshes from the direction toward the sun read from its sun shadow pass
static std::atomic<bool> SunDirectionFix = true;
// fades the sun's shadow out and in over about a second when the game drops or restores it indoors
static std::atomic<bool> SmoothIndoorTransition = true;
// how far a sun shadow's soft edge spreads per world unit from its caster, per unit of SunSoftness: the real sun's, the
// tangent of its angular radius of about 0.27 degrees
constexpr float SUN_SPREAD_PER_SOFTNESS = 0.00465f;
static std::atomic<bool> OcclusionEnabled = true;
static std::atomic<float> OcclusionRadius = 0.66f;
static std::atomic<float> OcclusionStrength = 1.25f;
// Retained upstream dependency, deliberately disabled in this managed build.
static std::atomic<float> SurfaceSaturation = 0.0f;
static std::atomic<bool> ShowOcclusion;
static uint32_t OcclusionFrame;

// the game's sun shadow cascade pixel shaders are classed capture::CLASS_SUN_PASS, with their pass's image copy slot
// (g_texBackBuffer) and their replacement's index in SUN_SHADERS, or -1 when they have none
struct SunPass
{
    UINT ImageSlot;
    int Replacement;
};

static ComPtr<ID3D11VertexShader> FullScreenShader;
static ComPtr<ID3D11PixelShader> HalfDepthShader, BlurXShader, BlurYShader, ApplyShader, ShowShader,
    ScaleShader, DarkenShader, SurfaceShader, ReceiverShader;
static ComPtr<ID3D11ComputeShader> OcclusionShader;
static ComPtr<ID3D11PixelShader> SoftSunShaders[SUN_SHADER_COUNT], HalfSunShaders[SUN_SHADER_COUNT];
static ComPtr<ID3D11Buffer> SoftnessBuffer, SurfaceBuffer, ReceiverBuffer;
static ComPtr<ID3D11SamplerState> PointClamp, LinearClamp;
static ComPtr<ID3D11BlendState> SubtractBlend;
static ComPtr<ID3D11Buffer> ConstantsBuffer, SunBuffer;
// this frame's sun colour and direction and ambient colour, read from the game's base light draws
static XMFLOAT4 SunColour, SunDirection, Ambient;
static bool SunKnown;
// the direction toward the sun or moon, read from the near cascade's sun shadow pass, and the frame it was read on
static XMFLOAT3 SunTowardLight;
static uint64_t SunTowardFrame;
static uint64_t SunFrame=1;
static sun_direction_policy::State DirectionCorrection;
static double SunSeconds(){return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();}
static bool SunMasterActive=false;
// how many frames after its sun shadow pass a direction toward the sun is still used
constexpr uint64_t SUN_DIRECTION_FRAMES = 4;
static ComPtr<ID3D11ShaderResourceView> WhiteView;
static ComPtr<ID3DDeviceContextState> DrawState;
static bool CreateFailed;
using tq_sun::ScopedSunConstants;

// the render target and depth target the game has bound, compared only, never held
static ID3D11Resource *CurrentTarget, *CurrentDepth;
// whether the bound render target is screen-sized
static bool ScreenTarget;
// screen-sized render targets bound this frame, the light buffer candidates
static std::vector<ID3D11Resource *> FrameTargets;
static ComPtr<ID3D11Texture2D> LightBuffer, BaseLight;
static ComPtr<ID3D11ShaderResourceView> LightBufferView, BaseLightView;
static ComPtr<ID3D11RenderTargetView> LightBufferTarget;
// the depth target bound with the light buffer
static ComPtr<ID3D11Resource> LightBufferDepth;
static uint32_t UndrawnFrames;
static ComPtr<ID3D11Texture2D> DepthCopy;
static ComPtr<ID3D11ShaderResourceView> DepthCopyView;
// the depth at half resolution
static ComPtr<ID3D11Texture2D> HalfDepth;
static ComPtr<ID3D11ShaderResourceView> HalfDepthView;
static ComPtr<ID3D11RenderTargetView> HalfDepthTarget;
// half-resolution occlusion and view distance: as computed, blurred across, blurred down. The computed one is written
// by a compute shader through OcclusionOutput, so it has no render target
static ComPtr<ID3D11Texture2D> OcclusionTextures[3];
static ComPtr<ID3D11ShaderResourceView> OcclusionViews[3];
static ComPtr<ID3D11RenderTargetView> OcclusionTargets[3];
static ComPtr<ID3D11UnorderedAccessView> OcclusionOutput;
static ComPtr<ID3D11Texture2D> Scaled;
static ComPtr<ID3D11ShaderResourceView> ScaledView;
static ComPtr<ID3D11RenderTargetView> ScaledTarget;
// the soft sun shadow and the view depth it was found at, at half resolution
static ComPtr<ID3D11Texture2D> HalfSunShadow;
static ComPtr<ID3D11ShaderResourceView> HalfSunShadowView;
static ComPtr<ID3D11RenderTargetView> HalfSunShadowTarget;
// the game's sun shadow cascade maps, in SUN_SHADERS order. Indoors the game stops drawing casters into them, so the
// sun's shadow vanishes at once as the player's character walks into a building and comes back at once outside. The
// add-on copies each map, with its pass's cascade rows, on frames the game draws about as many casters into it as it
// has lately, and fades between the copy and the game's shadow
struct CascadeMap
{
    // the map, compared only, and the frame it was first seen on
    ID3D11Resource *Resource;
    uint64_t SeenFrame;
    // Native map generation and original-draw write evidence, never derived from managed shadow share.
    uint64_t Generation, NativeWriteFrame, NativeWriteEpoch;
    uint32_t NativeWrites;
    // the game's caster draws into the map this frame, and the level they have kept lately
    int Draws;
    float DrawLevel;
    // the copy, its pass's cascade rows when it was taken and that frame's camera
    ComPtr<ID3D11Texture2D> Copy;
    ComPtr<ID3D11ShaderResourceView> CopyView;
    XMFLOAT4 Rows[12];
    XMFLOAT3 RowsCamera;
    bool RowsKnown;
};
static CascadeMap CascadeMaps[SUN_SHADER_COUNT];
static uint64_t NextCascadeGeneration=1;
// the capture snapshots of the image before and after each cascade's kept pass, in SUN_SHADERS order
constexpr const char *KEPT_PASS_SNAPSHOTS[SUN_SHADER_COUNT][2] = {
    { "before-kept-near-sun-pass", "after-kept-near-sun-pass" },
    { "before-kept-middle-sun-pass", "after-kept-middle-sun-pass" },
    { "before-kept-far-sun-pass", "after-kept-far-sun-pass" },
};
// the pixel shader resource slot of the game's sun passes' cascade map
constexpr UINT CASCADE_MAP_SLOT = 2;
// the time constant of the fade between the sun's shadow and none, in seconds
constexpr float INDOOR_EASE_SECONDS = 1.0f;
// the time constant a map's caster draw level follows its draws with, in seconds. A frame with fewer than FULL_SHARE
// of that level counts as one without the sun's shadow: on the way in the game draws only a few casters for some frames
// before it draws none, and copying those would keep a nearly empty map. A slow fall moves the level down with it; frames
// with no casters leave it as it is, so after a quick trip indoors a doorway frame is still not taken as full
constexpr float DRAW_LEVEL_SECONDS = 2.0f;
constexpr float FULL_SHARE = 0.5f;
// the share of the sun's shadow drawn, easing toward 1 while the game draws its usual casters and toward 0 while it does
// not, with the frame and time it was last eased on
static float SunShadowShare = 1;
static float SunShadowTarget = 1;
// whether the last sun passes found the game's usual casters, as on a frame well away from a door
static bool SunShadowFull = true;
static uint64_t ShareFrame;
static std::chrono::steady_clock::time_point ShareTime;
// the seconds since the frame before, as of the first sun pass this frame
static float FrameSeconds;
// a sun-lit animated mesh's sun colour, vertex row c11, on the last outdoor and indoor frames it was drawn on. The game
// gives each such mesh its own: the sun's colour, or a dim one while it stands in the sun's shadow. Indoors, with no sun
// shadow, the player's character gets the full colour, so it brightens at once on the way in; the colour is eased
// between the two with the sun shadow's share
// outdoor frames a mesh's kept outdoor sun colour lags behind
constexpr uint32_t OUTDOOR_COLOUR_FRAMES = 16;
struct MeshSunColour
{
    XMFLOAT4 Outdoor, Indoor;
    bool OutdoorKnown, IndoorKnown;
    uint64_t SeenFrame;
    // the colours of its last outdoor frames with the game's usual casters, oldest first once full; the game switches
    // the player to its indoor colour a few frames before it stops drawing casters, so the outdoor colour kept is the
    // oldest of these
    XMFLOAT4 Recent[OUTDOOR_COLOUR_FRAMES];
    uint32_t RecentCount, RecentNext;
};
static std::unordered_map<uint64_t, MeshSunColour> MeshSunColours;
// frames a mesh's kept sun colours outlast its last draw
constexpr uint64_t MESH_COLOUR_FRAMES = 300;
// the pixel shader resource slot of the game's sun passes' receiver depth
constexpr UINT RECEIVER_DEPTH_SLOT = 0;
// indoors the game also stops drawing many surfaces it draws as sun shadow receivers outside, which then take no sun
// shadow at all, kept or not; while a kept shadow fades out the passes read this copy of the receiver depth, with every
// surface in it, built once a frame
static ComPtr<ID3D11Texture2D> ReceiverDepth;
static ComPtr<ID3D11ShaderResourceView> ReceiverDepthView;
static ComPtr<ID3D11RenderTargetView> ReceiverDepthTarget;
// the facing the sun passes read with ReceiverDepth, filled the same way
static ComPtr<ID3D11Texture2D> ReceiverFacing;
static ComPtr<ID3D11ShaderResourceView> ReceiverFacingView;
static ComPtr<ID3D11RenderTargetView> ReceiverFacingTarget;
// the pixel shader resource slot of the game's sun passes' facing, written with the receiver depth
constexpr UINT FACING_SLOT = 3;
static uint64_t ReceiverFrame;
static bool ReceiverBuilt;
// the game's receiver depth and facing from the last frame with its usual casters, with that frame's world-to-clip
// transform and camera, copied once a frame
static ComPtr<ID3D11Texture2D> KeptReceivers, KeptFacing;
static ComPtr<ID3D11ShaderResourceView> KeptReceiversView, KeptFacingView;
static XMMATRIX KeptWorldToClip;
static XMFLOAT3 KeptCamera;
static uint64_t KeptReceiversFrame;
static bool KeptReceiversKnown;
// the game's colour image as the surface saturation pass reads it
static ComPtr<ID3D11Texture2D> SurfaceCopy;
static ComPtr<ID3D11ShaderResourceView> SurfaceCopyView;
// the game's colour image, the screen-sized target of the first draw that reads the finished light buffer, compared only
static ID3D11Resource *ColourImage;
static bool SurfaceDone;

// this frame's progress
static bool LightBufferDrawn, BaseCopied, Processed, OcclusionApplied, ScaledBuilt, DepthCopied;
// adds more light to the light buffer once it is finished, with its depth copied and its camera known
static void (*DrawExtraLights)(ID3D11DeviceContext *ctx);

// what happened to the light buffer in one frame, kept for the last HISTORY_FRAMES frames so a capture shows problems
// that come and go between frames
struct FrameHistory
{
    uint64_t Frame;
    ID3D11Resource *LightBuffer;
    int LightBufferSet, LightBufferReleased, Scenes, Finishes, Applications, CasterDraws, FullMaps, KeptPasses;
    bool Drawn, SunKnown, WorldToClipKnown;
    float SunShadowShare;
    uint64_t DirectionEvidenceFrame = 0, DirectionEpoch = 0;
    float DirectionWeight = 0, DirectionActivity = 0;
    XMFLOAT3 CorrectedDirection = {};
    unsigned DirectionAttempts = 0, DirectionApplied = 0;
    bool DirectionActive = false;
};
constexpr size_t HISTORY_FRAMES = 600;
static FrameHistory History[HISTORY_FRAMES];
static size_t HistoryCount;
static FrameHistory ThisFrame;

using shadowmaps::GpuTimer, shadowmaps::StartTimer, shadowmaps::StopTimer, shadowmaps::FinishTimer;
// LampTimer runs from the first lamp drawn into the light buffer to FinishLightBuffer
static GpuTimer OcclusionTimer, SunTimer, GlowTimer, SurfaceTimer, LampTimer;
// the world-to-clip transform of the light buffer's draws, known once two draws agree on it
static XMMATRIX WorldToClip, CandidateWorldToClip;
static bool WorldToClipKnown, CandidateKnown;

static void OnInitPipeline(device *, pipeline_layout, uint32_t count, const pipeline_subobject *subobjects, pipeline handle)
{
    const shadowmaps::CpuScope scope(shadowmaps::ShaderSortCpu);
    for (uint32_t i = 0; i < count; ++i)
    {
        if (subobjects[i].type != pipeline_subobject_type::pixel_shader || !subobjects[i].data)
            continue;

        const auto &desc = *static_cast<const shader_desc *>(subobjects[i].data);
        ComPtr<ID3D11ShaderReflection> reflection;
        if (FAILED(D3DReflect(desc.code, desc.code_size, IID_PPV_ARGS(&reflection))))
            continue;

        D3D11_SHADER_INPUT_BIND_DESC image, normalLight;
        if (FAILED(reflection->GetResourceBindingDescByName("g_texBackBuffer", &image))
            || FAILED(reflection->GetResourceBindingDescByName("nDotLTex", &normalLight)))
            continue;

        const uint64_t hash = capture::HashCode(desc.code, desc.code_size);
        int replacement = -1;
        for (size_t index = 0; index < SUN_SHADER_COUNT; ++index)
            if (SUN_SHADERS[index].Hash == hash)
                replacement = static_cast<int>(index);

        std::lock_guard guard(capture::ClassLock);
        capture::PipelineClass &found = capture::Classes[handle.handle];
        found.Bits |= capture::CLASS_SUN_PASS;
        found.SunImageSlot = image.BindPoint;
        found.SunReplacement = replacement;
    }
}

static void ResetSunHistory()
{
    for(auto &map:CascadeMaps)map={};
    SunTowardLight={};SunTowardFrame=0;SunKnown=false;DirectionCorrection.Clear();
    ShareFrame=0;ShareTime={};FrameSeconds=0;SunShadowShare=SunShadowTarget=1;SunShadowFull=true;
    ReceiverFrame=0;ReceiverBuilt=false;KeptReceiversFrame=0;KeptReceiversKnown=false;
    KeptReceivers.Reset();KeptReceiversView.Reset();KeptFacing.Reset();KeptFacingView.Reset();
    KeptWorldToClip=XMMatrixIdentity();KeptCamera={};MeshSunColours.clear();
}

static void OnDestroyResource(device *,resource handle)
{
    // Native cascade identities are compared only, never retained across reuse.
    for(const auto &map:CascadeMaps)
        if(map.Resource==reinterpret_cast<ID3D11Resource *>(handle.handle))
        {ResetSunHistory();break;}
}

static void ReleaseLightBuffer()
{
    if(LightBuffer&&shadowmaps::SceneEpochChanged)shadowmaps::SceneEpochChanged();
    ResetSunHistory();
    ++ThisFrame.LightBufferReleased;
    LightBuffer.Reset();
    LightBufferView.Reset();
    LightBufferTarget.Reset();
    LightBufferDepth.Reset();
    BaseLight.Reset();
    BaseLightView.Reset();
    DepthCopy.Reset();
    DepthCopyView.Reset();
    for (int n = 0; n < 3; ++n)
    {
        OcclusionTextures[n].Reset();
        OcclusionViews[n].Reset();
        OcclusionTargets[n].Reset();
    }
    OcclusionOutput.Reset();
    HalfDepth.Reset();
    HalfDepthView.Reset();
    HalfDepthTarget.Reset();
    Scaled.Reset();
    ScaledView.Reset();
    ScaledTarget.Reset();
    HalfSunShadow.Reset();
    HalfSunShadowView.Reset();
    HalfSunShadowTarget.Reset();
    SurfaceCopy.Reset();
    SurfaceCopyView.Reset();
    ReceiverDepth.Reset();
    ReceiverDepthView.Reset();
    ReceiverDepthTarget.Reset();
    ReceiverFacing.Reset();
    ReceiverFacingView.Reset();
    ReceiverFacingTarget.Reset();
    KeptReceivers.Reset();
    KeptReceiversView.Reset();
    KeptFacing.Reset();
    KeptFacingView.Reset();
    KeptReceiversKnown = false;
    UndrawnFrames = 0;
}

static void OnDestroyDevice(device *)
{
    ReleaseLightBuffer();
    SunMasterActive=false;
    tq_sun::ResetConstants();
    CurrentTarget = nullptr;
    CurrentDepth = nullptr;
    FrameTargets.clear();
    FullScreenShader.Reset();
    OcclusionShader.Reset();
    BlurXShader.Reset();
    BlurYShader.Reset();
    ApplyShader.Reset();
    ShowShader.Reset();
    HalfDepthShader.Reset();
    OcclusionTimer = {};
    SunTimer = {};
    GlowTimer = {};
    SurfaceTimer = {};
    LampTimer = {};
    for (CascadeMap &map : CascadeMaps)
        map = {};
    MeshSunColours.clear();
    ScaleShader.Reset();
    SurfaceShader.Reset();
    SurfaceBuffer.Reset();
    ReceiverShader.Reset();
    ReceiverBuffer.Reset();
    ColourImage = nullptr;
    for (ComPtr<ID3D11PixelShader> &shader : SoftSunShaders)
        shader.Reset();
    for (ComPtr<ID3D11PixelShader> &shader : HalfSunShaders)
        shader.Reset();
    SoftnessBuffer.Reset();
    PointClamp.Reset();
    LinearClamp.Reset();
    SubtractBlend.Reset();
    ConstantsBuffer.Reset();
    SunBuffer.Reset();
    WhiteView.Reset();
    DrawState.Reset();
    CreateFailed = false;
}

static ComPtr<ID3DBlob> CompileShader(const char *entry, const char *target, const char *source = LIGHT_BUFFER_SHADER)
{
    ComPtr<ID3DBlob> code, errors;
    if (SUCCEEDED(D3DCompile(source, strlen(source), "light_buffer", nullptr, nullptr, entry,
            target, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors)))
        return code;

    if (errors)
        reshade::log::message(reshade::log::level::error, static_cast<const char *>(errors->GetBufferPointer()));
    return nullptr;
}

static bool CreatePixelShader(ID3D11Device *dev, const char *entry, ComPtr<ID3D11PixelShader> &out,
    const char *source = LIGHT_BUFFER_SHADER)
{
    ComPtr<ID3DBlob> code = CompileShader(entry, "ps_5_0", source);
    return code && SUCCEEDED(dev->CreatePixelShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &out));
}

static bool CreateObjects(ID3D11Device *dev)
{
    if (DrawState)
        return true;
    if (CreateFailed)
        return false;

    ComPtr<ID3DBlob> vertexCode = CompileShader("vs_main", "vs_5_0");
    ComPtr<ID3DBlob> occlusionCode = CompileShader("cs_occlusion", "cs_5_0");
    D3D11_SAMPLER_DESC samplerDesc = {};
    samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    samplerDesc.AddressU = samplerDesc.AddressV = samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    samplerDesc.MaxLOD = D3D11_FLOAT32_MAX;
    D3D11_SAMPLER_DESC linearDesc = samplerDesc;
    linearDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    D3D11_BLEND_DESC blendDesc = {};
    D3D11_RENDER_TARGET_BLEND_DESC &subtract = blendDesc.RenderTarget[0];
    subtract.BlendEnable = TRUE;
    subtract.SrcBlend = subtract.DestBlend = D3D11_BLEND_ONE;
    subtract.BlendOp = D3D11_BLEND_OP_REV_SUBTRACT;
    subtract.SrcBlendAlpha = D3D11_BLEND_ZERO;
    subtract.DestBlendAlpha = D3D11_BLEND_ONE;
    subtract.BlendOpAlpha = D3D11_BLEND_OP_ADD;
    subtract.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_RED | D3D11_COLOR_WRITE_ENABLE_GREEN
        | D3D11_COLOR_WRITE_ENABLE_BLUE;
    D3D11_BUFFER_DESC constantsDesc = { sizeof(OcclusionConstants), D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER };
    D3D11_BUFFER_DESC sunDesc = { sizeof(SunConstants), D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER };
    const float white[2] = { 1, 0 };
    const D3D11_SUBRESOURCE_DATA whiteData = { white, sizeof(white) };
    D3D11_TEXTURE2D_DESC whiteDesc = { 1, 1, 1, 1, DXGI_FORMAT_R32G32_FLOAT, { 1, 0 }, D3D11_USAGE_IMMUTABLE,
        D3D11_BIND_SHADER_RESOURCE };
    ComPtr<ID3D11Texture2D> whiteTexture;
    ComPtr<ID3D11Device1> dev1;
    const D3D_FEATURE_LEVEL level = dev->GetFeatureLevel();
    D3D11_BUFFER_DESC softnessDesc = { 16, D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER };
    D3D11_BUFFER_DESC receiverDesc = { 11 * 16, D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER };
    bool sunCreated = true;
    for (size_t index = 0; index < SUN_SHADER_COUNT; ++index)
        sunCreated = sunCreated
            && CreatePixelShader(dev, SUN_SHADERS[index].Entry, SoftSunShaders[index], SUN_SHADOW_SHADER)
            && CreatePixelShader(dev, SUN_SHADERS[index].HalfEntry, HalfSunShaders[index], SUN_SHADOW_SHADER);
    const bool created = sunCreated && SUCCEEDED(dev->CreateBuffer(&softnessDesc, nullptr, &SoftnessBuffer))
        && vertexCode
        && SUCCEEDED(dev->CreateVertexShader(vertexCode->GetBufferPointer(), vertexCode->GetBufferSize(), nullptr,
            &FullScreenShader))
        && occlusionCode
        && SUCCEEDED(dev->CreateComputeShader(occlusionCode->GetBufferPointer(), occlusionCode->GetBufferSize(),
            nullptr, &OcclusionShader))
        && CreatePixelShader(dev, "ps_blur_x", BlurXShader)
        && CreatePixelShader(dev, "ps_blur_y", BlurYShader) && CreatePixelShader(dev, "ps_apply", ApplyShader)
        && CreatePixelShader(dev, "ps_show", ShowShader)
        && CreatePixelShader(dev, "ps_half_depth", HalfDepthShader)
        && CreatePixelShader(dev, "ps_scale", ScaleShader)
        && CreatePixelShader(dev, "ps_darken", DarkenShader)
        && CreatePixelShader(dev, "ps_surface", SurfaceShader, SURFACE_SHADER)
        && SUCCEEDED(dev->CreateBuffer(&softnessDesc, nullptr, &SurfaceBuffer))
        && CreatePixelShader(dev, "ps_receiver_depth", ReceiverShader, RECEIVER_SHADER)
        && SUCCEEDED(dev->CreateBuffer(&receiverDesc, nullptr, &ReceiverBuffer))
        && SUCCEEDED(dev->CreateSamplerState(&samplerDesc, &PointClamp))
        && SUCCEEDED(dev->CreateSamplerState(&linearDesc, &LinearClamp))
        && SUCCEEDED(dev->CreateBlendState(&blendDesc, &SubtractBlend))
        && SUCCEEDED(dev->CreateBuffer(&constantsDesc, nullptr, &ConstantsBuffer))
        && SUCCEEDED(dev->CreateBuffer(&sunDesc, nullptr, &SunBuffer))
        && SUCCEEDED(dev->CreateTexture2D(&whiteDesc, &whiteData, &whiteTexture))
        && SUCCEEDED(dev->CreateShaderResourceView(whiteTexture.Get(), nullptr, &WhiteView))
        && SUCCEEDED(dev->QueryInterface(IID_PPV_ARGS(&dev1)))
        && SUCCEEDED(dev1->CreateDeviceContextState(
            dev->GetCreationFlags() & D3D11_CREATE_DEVICE_SINGLETHREADED
                ? D3D11_1_CREATE_DEVICE_CONTEXT_STATE_SINGLETHREADED : 0,
            &level, 1, D3D11_SDK_VERSION, __uuidof(ID3D11Device), nullptr, &DrawState));
    if (!created)
    {
        DrawState.Reset();
        CreateFailed = true;
        reshade::log::message(reshade::log::level::error, "Could not create the light buffer objects.");
    }
    return created;
}

// the format to read a light buffer of this format through, or UNKNOWN when it is not a colour format read here
static DXGI_FORMAT GetReadFormat(DXGI_FORMAT format)
{
    switch (format)
    {
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:
    case DXGI_FORMAT_B8G8R8A8_UNORM:
        return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
    case DXGI_FORMAT_R8G8B8A8_UNORM:
        return DXGI_FORMAT_R8G8B8A8_UNORM;
    default:
        return DXGI_FORMAT_UNKNOWN;
    }
}

// the format to read a depth target of this typeless format through
static DXGI_FORMAT GetDepthReadFormat(DXGI_FORMAT typeless)
{
    switch (typeless)
    {
    case DXGI_FORMAT_R24G8_TYPELESS:
        return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
    case DXGI_FORMAT_R32_TYPELESS:
        return DXGI_FORMAT_R32_FLOAT;
    case DXGI_FORMAT_R32G8X24_TYPELESS:
        return DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
    case DXGI_FORMAT_R16_TYPELESS:
        return DXGI_FORMAT_R16_UNORM;
    default:
        return DXGI_FORMAT_UNKNOWN;
    }
}

static bool IsScreenSized(ID3D11Resource *resource, D3D11_TEXTURE2D_DESC &desc)
{
    ComPtr<ID3D11Texture2D> texture;
    if (!resource || FAILED(resource->QueryInterface(IID_PPV_ARGS(&texture))))
        return false;

    texture->GetDesc(&desc);
    return desc.Width == shadowmaps::ScreenWidth && desc.Height == shadowmaps::ScreenHeight
        && desc.SampleDesc.Count == 1 && desc.ArraySize == 1 && desc.MipLevels == 1;
}

// the full-resolution pixels across each texel of the occlusion and soft sun shadow passes, which run at the screen's
// height up to 1440 lines and at 1440 lines above it, so a larger screen never gets less detail
static float FindReducedScale(UINT height)
{
    constexpr UINT MOST_LINES = 1440;
    return height > MOST_LINES ? static_cast<float>(height) / MOST_LINES : 1.0f;
}

// the texels across a reduced-resolution texture for this many full-resolution pixels
static UINT FindReducedSize(UINT pixels, float scale)
{
    return static_cast<UINT>(std::ceil(pixels / scale));
}

// takes a render target read back by a later draw as the light buffer, with a copy for the light from before the lamps
static void SetLightBuffer(ID3D11Device *dev, ID3D11Resource *resource)
{
    ++ThisFrame.LightBufferSet;
    if (capture::State == capture::Phase::Capturing && capture::Log)
        fprintf(capture::Log, "--- light buffer set to r%d after draw %u\n",
            capture::GetResourceId(reinterpret_cast<uint64_t>(resource)), capture::DrawIndex);
    D3D11_TEXTURE2D_DESC desc;
    const DXGI_FORMAT readFormat = IsScreenSized(resource, desc) ? GetReadFormat(desc.Format) : DXGI_FORMAT_UNKNOWN;
    if (readFormat == DXGI_FORMAT_UNKNOWN || !(desc.BindFlags & D3D11_BIND_SHADER_RESOURCE)
        || !(desc.BindFlags & D3D11_BIND_RENDER_TARGET))
        return;

    ReleaseLightBuffer();
    ComPtr<ID3D11Texture2D> texture;
    resource->QueryInterface(IID_PPV_ARGS(&texture));
    D3D11_TEXTURE2D_DESC copyDesc = desc;
    copyDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    copyDesc.CPUAccessFlags = 0;
    copyDesc.MiscFlags = 0;
    D3D11_SHADER_RESOURCE_VIEW_DESC viewDesc = {};
    viewDesc.Format = readFormat;
    viewDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    viewDesc.Texture2D = { 0, 1 };
    D3D11_RENDER_TARGET_VIEW_DESC targetDesc = {};
    targetDesc.Format = readFormat;
    targetDesc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
    const float scale = FindReducedScale(desc.Height);
    D3D11_TEXTURE2D_DESC occlusionDesc = { FindReducedSize(desc.Width, scale), FindReducedSize(desc.Height, scale), 1, 1,
        DXGI_FORMAT_R16G16_FLOAT, { 1, 0 },
        D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET };
    bool created = SUCCEEDED(dev->CreateTexture2D(&copyDesc, nullptr, &BaseLight))
        && SUCCEEDED(dev->CreateShaderResourceView(BaseLight.Get(), &viewDesc, &BaseLightView))
        && SUCCEEDED(dev->CreateShaderResourceView(texture.Get(), &viewDesc, &LightBufferView))
        && SUCCEEDED(dev->CreateRenderTargetView(texture.Get(), &targetDesc, &LightBufferTarget));
    D3D11_TEXTURE2D_DESC computedDesc = occlusionDesc;
    computedDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
    D3D11_TEXTURE2D_DESC halfDepthDesc = occlusionDesc;
    halfDepthDesc.Format = DXGI_FORMAT_R32_FLOAT;
    created = created && (!tq_world::Work.AO() || (SUCCEEDED(dev->CreateTexture2D(&halfDepthDesc, nullptr, &HalfDepth))
        && SUCCEEDED(dev->CreateShaderResourceView(HalfDepth.Get(), nullptr, &HalfDepthView))
        && SUCCEEDED(dev->CreateRenderTargetView(HalfDepth.Get(), nullptr, &HalfDepthTarget))));
    for (int n = 0; n < 3 && created && tq_world::Work.AO(); ++n)
        created = SUCCEEDED(dev->CreateTexture2D(n == 0 ? &computedDesc : &occlusionDesc, nullptr,
                &OcclusionTextures[n]))
            && SUCCEEDED(dev->CreateShaderResourceView(OcclusionTextures[n].Get(), nullptr, &OcclusionViews[n]))
            && (n == 0
                ? SUCCEEDED(dev->CreateUnorderedAccessView(OcclusionTextures[n].Get(), nullptr, &OcclusionOutput))
                : SUCCEEDED(dev->CreateRenderTargetView(OcclusionTextures[n].Get(), nullptr, &OcclusionTargets[n])));
    if (!created)
    {
        ReleaseLightBuffer();
        return;
    }
    LightBuffer = texture;
}

// looks for the light buffer among this frame's earlier render targets in the textures a draw reads
static void FindLightBuffer(ID3D11DeviceContext *ctx)
{
    D3D11_TEXTURE2D_DESC depthDesc;
    if (!IsScreenSized(CurrentDepth, depthDesc))
        return;

    ID3D11ShaderResourceView *views[MAX_READ_SLOTS] = {};
    ctx->PSGetShaderResources(0, MAX_READ_SLOTS, views);
    ID3D11Resource *found = nullptr;
    for (ID3D11ShaderResourceView *view : views)
    {
        if (!view)
            continue;

        ComPtr<ID3D11Resource> resource;
        view->GetResource(&resource);
        if (!found && resource.Get() != CurrentTarget
            && std::find(FrameTargets.begin(), FrameTargets.end(), resource.Get()) != FrameTargets.end())
            found = resource.Get();
        view->Release();
    }
    if (!found)
        return;

    ComPtr<ID3D11Device> dev;
    ctx->GetDevice(&dev);
    SetLightBuffer(dev.Get(), found);
}

// whether two world-to-clip transforms are the same camera, within rounding error
static bool IsSameCamera(const XMMATRIX &first, const XMMATRIX &second)
{
    constexpr float TOLERANCE = 1e-4f;
    XMFLOAT4X4 a, b;
    XMStoreFloat4x4(&a, first);
    XMStoreFloat4x4(&b, second);
    for (int n = 0; n < 16; ++n)
        if (std::abs((&a._11)[n] - (&b._11)[n]) > TOLERANCE * (1 + std::abs((&b._11)[n])))
            return false;
    return true;
}

// the world-to-clip transform of a draw from its object-to-clip (c3 to c6) and world (c7 to c9) rows
static void ReadWorldToClip(shadowmaps::BoundDraw &bound)
{
    XMMATRIX worldToClip;
    if (!bound.ReadRows() || !shadowmaps::FindWorldToClip(bound.Rows, worldToClip))
        return;

    if (CandidateKnown)
    {
        if (IsSameCamera(worldToClip, CandidateWorldToClip))
        {
            WorldToClip = worldToClip;
            WorldToClipKnown = true;
            return;
        }
    }
    CandidateWorldToClip = worldToClip;
    CandidateKnown = true;
}

// whether a draw now could be the point to copy the light from before the lamps
static bool IsCopyPending()
{
    return LightBuffer && !BaseCopied && CurrentTarget == LightBuffer.Get();
}

static void DarkenAmbient(ID3D11DeviceContext *ctx);

static void StartLampTimer(ID3D11DeviceContext *ctx)
{
    if (LightBuffer && !Processed && CurrentTarget == LightBuffer.Get())
        StartTimer(ctx, LampTimer);
}

// copies the light buffer before the first lamp, after the Darker ambient mode takes off the ambient light it does not
// keep, so the copy and the lamps' share of the light both leave that light out
static void CopyBaseLight(ID3D11DeviceContext *ctx)
{
    if (tq_world::Work.Sun() && SunShadowMode == SunMode::DarkerAmbient && SunKnown)
        DarkenAmbient(ctx);
    ctx->CopyResource(BaseLight.Get(), LightBuffer.Get());
    BaseCopied = true;
}

static void DrawFullScreen(ID3D11DeviceContext *ctx, ID3D11RenderTargetView *const *targets, UINT targetCount, UINT width,
    UINT height, ID3D11PixelShader *shader, ID3D11ShaderResourceView *const *views, UINT viewCount)
{
    const D3D11_VIEWPORT viewport = { 0, 0, static_cast<float>(width), static_cast<float>(height), 0, 1 };
    ctx->RSSetViewports(1, &viewport);
    ctx->OMSetRenderTargets(targetCount, targets, nullptr);
    ctx->PSSetShader(shader, nullptr, 0);
    ctx->PSSetShaderResources(0, viewCount, views);
    ctx->Draw(3, 0);
    ID3D11ShaderResourceView *none[6] = {};
    ctx->PSSetShaderResources(0, 6, none);
    ctx->OMSetRenderTargets(0, nullptr, nullptr);
}

static void DrawFullScreen(ID3D11DeviceContext *ctx, ID3D11RenderTargetView *target, UINT width, UINT height,
    ID3D11PixelShader *shader, ID3D11ShaderResourceView *const *views, UINT viewCount)
{
    DrawFullScreen(ctx, &target, 1, width, height, shader, views, viewCount);
}

// copies the light buffer's depth target where it can be read
static bool CopyDepth(ID3D11DeviceContext *ctx)
{
    D3D11_TEXTURE2D_DESC desc;
    if (!IsScreenSized(LightBufferDepth.Get(), desc))
        return false;

    const DXGI_FORMAT typeless = capture::GetTypelessDepthFormat(desc.Format);
    const DXGI_FORMAT readFormat = GetDepthReadFormat(typeless);
    if (readFormat == DXGI_FORMAT_UNKNOWN)
        return false;

    D3D11_TEXTURE2D_DESC copyDesc = {};
    if (DepthCopy)
        DepthCopy->GetDesc(&copyDesc);
    if (!DepthCopy || copyDesc.Format != typeless || copyDesc.Width != desc.Width || copyDesc.Height != desc.Height)
    {
        DepthCopy.Reset();
        DepthCopyView.Reset();
        ComPtr<ID3D11Device> dev;
        ctx->GetDevice(&dev);
        copyDesc = desc;
        copyDesc.Format = typeless;
        copyDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        copyDesc.CPUAccessFlags = 0;
        copyDesc.MiscFlags = 0;
        D3D11_SHADER_RESOURCE_VIEW_DESC viewDesc = {};
        viewDesc.Format = readFormat;
        viewDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        viewDesc.Texture2D = { 0, 1 };
        if (FAILED(dev->CreateTexture2D(&copyDesc, nullptr, &DepthCopy))
            || FAILED(dev->CreateShaderResourceView(DepthCopy.Get(), &viewDesc, &DepthCopyView)))
        {
            DepthCopy.Reset();
            DepthCopyView.Reset();
            return false;
        }
    }
    ctx->CopyResource(DepthCopy.Get(), LightBufferDepth.Get());
    DepthCopied = true;
    return true;
}

// saves the depth copy's texels and the occlusion constants to the capture, to run the occlusion pass on them offline;
// waits for the GPU, so only for captures
static void SaveOcclusionInputs(ID3D11DeviceContext *ctx, const OcclusionConstants &constants)
{
    D3D11_TEXTURE2D_DESC desc;
    DepthCopy->GetDesc(&desc);
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Device> dev;
    ctx->GetDevice(&dev);
    ComPtr<ID3D11Texture2D> staging;
    if (FAILED(dev->CreateTexture2D(&desc, nullptr, &staging)))
        return;

    ctx->CopyResource(staging.Get(), DepthCopy.Get());
    D3D11_MAPPED_SUBRESOURCE mapped;
    if (FAILED(ctx->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped)))
        return;

    // a header of width, height and format, then the rows packed
    const UINT texelBytes = desc.Format == DXGI_FORMAT_R16_TYPELESS ? 2 : 4;
    std::vector<uint8_t> data(12 + static_cast<size_t>(desc.Width) * desc.Height * texelBytes);
    const UINT header[3] = { desc.Width, desc.Height, static_cast<UINT>(desc.Format) };
    std::memcpy(data.data(), header, sizeof(header));
    for (UINT y = 0; y < desc.Height; ++y)
        std::memcpy(data.data() + 12 + static_cast<size_t>(y) * desc.Width * texelBytes,
            static_cast<const uint8_t *>(mapped.pData) + static_cast<size_t>(y) * mapped.RowPitch, desc.Width * texelBytes);
    ctx->Unmap(staging.Get(), 0);
    capture::WriteBufferFile("occlusion-depth", data.data(), static_cast<UINT>(data.size()));
    capture::WriteBufferFile("occlusion-constants", &constants, sizeof(constants));
}

// the sun constants for this frame: whether and how the sun's share is split off, by the sun shadow mode
static SunConstants FindSunConstants()
{
    const SunMode mode = SunShadowMode;
    const float kept = std::clamp(AmbientKept.load(), 0.0f, 1.0f);
    // without the sun, ambient or depth, the whole share from before the lamps is the sun's
    const bool split = mode != SunMode::Full && SunKnown && DepthCopied && WorldToClipKnown;
    SunConstants sun = {};
    XMStoreFloat4x4(&sun.InverseRows, XMMatrixInverse(nullptr, WorldToClip));
    sun.Colour = { SunColour.x, SunColour.y, SunColour.z, split ? 1.0f : 0.0f };
    sun.Direction = SunDirection;
    sun.Ambient = { Ambient.x * kept, Ambient.y * kept, Ambient.z * kept, Ambient.w };
    // the game's sun-lit shaders write half of the sun and ambient light into the light buffer
    const float removed = (1 - kept) / 2;
    sun.RemovedAmbient = { Ambient.x * removed, Ambient.y * removed, Ambient.z * removed, 0 };
    return sun;
}

// takes the ambient light the Darker ambient mode does not keep off the light buffer's geometry before the lamps; the
// light buffer's unsigned format stops it at 0. The depth is copied for it, and again once the lamps are done.
static void DarkenAmbient(ID3D11DeviceContext *ctx)
{
    ComPtr<ID3D11DeviceContext1> ctx1;
    ComPtr<ID3D11Device> dev;
    ctx->GetDevice(&dev);
    if (!LightBufferTarget || FAILED(ctx->QueryInterface(IID_PPV_ARGS(&ctx1))) || !CreateObjects(dev.Get())
        || !CopyDepth(ctx))
        return;

    DepthCopied = false;
    D3D11_TEXTURE2D_DESC desc;
    LightBuffer->GetDesc(&desc);
    const SunConstants sun = FindSunConstants();
    ComPtr<ID3DDeviceContextState> previous;
    ctx1->SwapDeviceContextState(DrawState.Get(), &previous);
    ctx1->UpdateSubresource(SunBuffer.Get(), 0, nullptr, &sun, 0, 0);
    ID3D11Buffer *buffer = SunBuffer.Get();
    ctx1->PSSetConstantBuffers(1, 1, &buffer);
    ctx1->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx1->VSSetShader(FullScreenShader.Get(), nullptr, 0);
    ctx1->OMSetBlendState(SubtractBlend.Get(), nullptr, 0xffffffff);
    ID3D11ShaderResourceView *depth[] = { DepthCopyView.Get() };
    DrawFullScreen(ctx1.Get(), LightBufferTarget.Get(), desc.Width, desc.Height, DarkenShader.Get(), depth, 1);
    ctx1->OMSetBlendState(nullptr, nullptr, 0xffffffff);
    ctx1->SwapDeviceContextState(previous.Get(), nullptr);
}

// computes ambient occlusion from the light buffer's depth and takes it off the light from before the lamps
static void ApplyOcclusion(ID3D11DeviceContext1 *ctx)
{
    if (!WorldToClipKnown || !CopyDepth(ctx))
        return;
    if (!BaseCopied)
        CopyBaseLight(ctx);

    D3D11_TEXTURE2D_DESC desc;
    LightBuffer->GetDesc(&desc);
    const float scale = FindReducedScale(desc.Height);
    const UINT width = FindReducedSize(desc.Width, scale);
    const UINT height = FindReducedSize(desc.Height, scale);
    XMFLOAT4X4 worldToClip;
    XMStoreFloat4x4(&worldToClip, WorldToClip);
    OcclusionConstants constants;
    XMStoreFloat4x4(&constants.InverseRows, XMMatrixInverse(nullptr, WorldToClip));
    constants.ClipW = { worldToClip._41, worldToClip._42, worldToClip._43, worldToClip._44 };
    constants.Size = { static_cast<float>(desc.Width), static_cast<float>(desc.Height), 1.0f / desc.Width,
        1.0f / desc.Height };
    const float projection = std::sqrt(worldToClip._21 * worldToClip._21 + worldToClip._22 * worldToClip._22
        + worldToClip._23 * worldToClip._23);
    constants.Params = { OcclusionRadius.load(), OcclusionStrength.load(), 0.5f * height * projection, 0 };
    // clip x and y rows, less any part along the view direction, point right and up
    const XMVECTOR forward = XMVector3Normalize(XMVectorSet(worldToClip._41, worldToClip._42, worldToClip._43, 0));
    XMVECTOR cameraRight = XMVectorSet(worldToClip._11, worldToClip._12, worldToClip._13, 0);
    XMVECTOR cameraUp = XMVectorSet(worldToClip._21, worldToClip._22, worldToClip._23, 0);
    cameraRight = XMVector3Normalize(cameraRight - forward * XMVector3Dot(cameraRight, forward));
    cameraUp = XMVector3Normalize(cameraUp - forward * XMVector3Dot(cameraUp, forward));
    XMStoreFloat4(&constants.CameraRight, cameraRight);
    XMStoreFloat4(&constants.CameraUp, cameraUp);
    OcclusionFrame = (OcclusionFrame + 1) & 0xffff;
    constants.Frame = { static_cast<float>(OcclusionFrame), scale, 0, 0 };
    if (capture::State == capture::Phase::Capturing)
        SaveOcclusionInputs(ctx, constants);

    ComPtr<ID3DDeviceContextState> previous;
    ctx->SwapDeviceContextState(DrawState.Get(), &previous);
    ctx->UpdateSubresource(ConstantsBuffer.Get(), 0, nullptr, &constants, 0, 0);
    ID3D11Buffer *buffer = ConstantsBuffer.Get();
    ctx->PSSetConstantBuffers(0, 1, &buffer);
    ID3D11SamplerState *samplers[] = { PointClamp.Get(), LinearClamp.Get() };
    ctx->PSSetSamplers(0, 2, samplers);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->VSSetShader(FullScreenShader.Get(), nullptr, 0);

    StartTimer(ctx, OcclusionTimer);
    ID3D11ShaderResourceView *depth[] = { DepthCopyView.Get() };
    DrawFullScreen(ctx, HalfDepthTarget.Get(), width, height, HalfDepthShader.Get(), depth, 1);
    ID3D11ShaderResourceView *halfDepth = HalfDepthView.Get();
    ID3D11UnorderedAccessView *output = OcclusionOutput.Get();
    ctx->CSSetConstantBuffers(0, 1, &buffer);
    ctx->CSSetShaderResources(0, 1, &halfDepth);
    ctx->CSSetUnorderedAccessViews(0, 1, &output, nullptr);
    ctx->CSSetShader(OcclusionShader.Get(), nullptr, 0);
    // a thread for each pair of texels along a row, in groups of 8x8
    ctx->Dispatch(((width + 1) / 2 + 7) / 8, (height + 7) / 8, 1);
    ID3D11ShaderResourceView *noView = nullptr;
    ID3D11UnorderedAccessView *noOutput = nullptr;
    ctx->CSSetShaderResources(0, 1, &noView);
    ctx->CSSetUnorderedAccessViews(0, 1, &noOutput, nullptr);
    ID3D11ShaderResourceView *computed[] = { nullptr, OcclusionViews[0].Get() };
    DrawFullScreen(ctx, OcclusionTargets[1].Get(), width, height, BlurXShader.Get(), computed, 2);
    ID3D11ShaderResourceView *across[] = { nullptr, OcclusionViews[1].Get() };
    DrawFullScreen(ctx, OcclusionTargets[2].Get(), width, height, BlurYShader.Get(), across, 2);

    ctx->OMSetBlendState(SubtractBlend.Get(), nullptr, 0xffffffff);
    ID3D11ShaderResourceView *apply[] = { DepthCopyView.Get(), OcclusionViews[2].Get(), nullptr, BaseLightView.Get() };
    DrawFullScreen(ctx, LightBufferTarget.Get(), desc.Width, desc.Height, ApplyShader.Get(), apply, 4);
    ctx->OMSetBlendState(nullptr, nullptr, 0xffffffff);
    ctx->SwapDeviceContextState(previous.Get(), nullptr);
    StopTimer(ctx, OcclusionTimer);
    OcclusionApplied = true;
    ++ThisFrame.Applications;
    if (capture::State == capture::Phase::Capturing && capture::Log)
        fprintf(capture::Log, "--- occlusion applied to light buffer r%d after draw %u\n",
            capture::GetResourceId(reinterpret_cast<uint64_t>(LightBuffer.Get())),
            capture::DrawIndex);
}

static void WriteMatrix(FILE *file, const XMMATRIX &matrix)
{
    XMFLOAT4X4 values;
    XMStoreFloat4x4(&values, matrix);
    for (int row = 0; row < 4; ++row)
        fprintf(file, " | %g %g %g %g", values.m[row][0], values.m[row][1], values.m[row][2], values.m[row][3]);
}

static void WriteTexture(FILE *file, const char *name, ID3D11Texture2D *texture)
{
    if (!texture)
    {
        fprintf(file, "%s: none\n", name);
        return;
    }
    D3D11_TEXTURE2D_DESC desc;
    texture->GetDesc(&desc);
    fprintf(file, "%s: %ux%u format %d\n", name, desc.Width, desc.Height, static_cast<int>(desc.Format));
}

// a frame capture writes everything the light buffer code holds to light-state.txt in the capture's folder
static void WriteState()
{
    FILE *file = nullptr;
    if (_wfopen_s(&file, (capture::Folder / L"light-state.txt").c_str(), L"w") != 0)
        return;

    fprintf(file, "occlusion enabled %d, radius %.3f, strength %.3f, show %d, frame %u; sun softness %.3f\n",
        OcclusionEnabled.load() ? 1 : 0, OcclusionRadius.load(), OcclusionStrength.load(), ShowOcclusion.load() ? 1 : 0,
        OcclusionFrame, SunSoftness.load());
    fprintf(file, "light buffer drawn %d, base copied %d, processed %d, occlusion applied %d, scaled built %d, depth "
        "copied %d, create failed %d, undrawn frames %u, frame targets %zu\n", LightBufferDrawn ? 1 : 0,
        BaseCopied ? 1 : 0, Processed ? 1 : 0, OcclusionApplied ? 1 : 0, ScaledBuilt ? 1 : 0, DepthCopied ? 1 : 0,
        CreateFailed ? 1 : 0, UndrawnFrames, FrameTargets.size());
    fprintf(file, "sun known %d, colour %g %g %g %g, direction %g %g %g %g, ambient %g %g %g %g\n", SunKnown ? 1 : 0,
        SunColour.x, SunColour.y, SunColour.z, SunColour.w, SunDirection.x, SunDirection.y, SunDirection.z,
        SunDirection.w, Ambient.x, Ambient.y, Ambient.z, Ambient.w);
    fprintf(file, "sun shadow mode %d (0 full, 1 split, 2 darker ambient), ambient kept %.3f\n",
        static_cast<int>(SunShadowMode.load()), AmbientKept.load());
    fprintf(file, "sun direction fix %d, direction toward the sun %g %g %g, read on frame %llu, frame now %llu\n",
        SunDirectionFix.load() ? 1 : 0, SunTowardLight.x, SunTowardLight.y, SunTowardLight.z,
        static_cast<unsigned long long>(SunTowardFrame), static_cast<unsigned long long>(SunFrame));
    fprintf(file,"sun correction weight %.3f, native activity %d, evidence frame %llu, epoch %llu\n",
        DirectionCorrection.weight,DirectionCorrection.active?1:0,static_cast<unsigned long long>(DirectionCorrection.evidenceFrame),
        static_cast<unsigned long long>(DirectionCorrection.epoch));
    fprintf(file, "occlusion %.3f ms, sun %.3f ms, glow %.3f ms, surface %.3f ms, lamps %.3f ms, lamp and window "
        "shadow maps %.3f ms\n", OcclusionTimer.Milliseconds, SunTimer.Milliseconds, GlowTimer.Milliseconds,
        SurfaceTimer.Milliseconds, LampTimer.Milliseconds, shadowmaps::MapTimer.Milliseconds);
    fprintf(file, "smooth indoor transition %d, sun shadow share %.3f toward %.0f, cascade map caster draws %d %d %d of "
        "levels %.1f %.1f %.1f, copies kept %d %d %d\n", SmoothIndoorTransition.load() ? 1 : 0, SunShadowShare,
        SunShadowTarget, CascadeMaps[0].Draws, CascadeMaps[1].Draws, CascadeMaps[2].Draws, CascadeMaps[0].DrawLevel,
        CascadeMaps[1].DrawLevel, CascadeMaps[2].DrawLevel, CascadeMaps[0].RowsKnown ? 1 : 0,
        CascadeMaps[1].RowsKnown ? 1 : 0, CascadeMaps[2].RowsKnown ? 1 : 0);
    fprintf(file, "surface saturation %.3f, colour image r%d, applied %d\n", SurfaceSaturation.load(),
        ColourImage ? capture::GetResourceId(reinterpret_cast<uint64_t>(ColourImage)) : 0, SurfaceDone ? 1 : 0);
    fprintf(file, "world to clip (%d)", WorldToClipKnown ? 1 : 0);
    WriteMatrix(file, WorldToClip);
    fprintf(file, "\ncandidate world to clip (%d)", CandidateKnown ? 1 : 0);
    WriteMatrix(file, CandidateWorldToClip);
    fprintf(file, "\n");
    WriteTexture(file, "light buffer", LightBuffer.Get());
    WriteTexture(file, "base light", BaseLight.Get());
    WriteTexture(file, "depth copy", DepthCopy.Get());
    WriteTexture(file, "reduced depth", HalfDepth.Get());
    WriteTexture(file, "occlusion", OcclusionTextures[0].Get());
    WriteTexture(file, "scaled image", Scaled.Get());
    WriteTexture(file, "reduced sun shadow", HalfSunShadow.Get());
    fprintf(file, "\n--- last %zu frames: frame, light buffer, set, released, drawn, scenes, finishes, occlusion applied, "
        "sun known, camera known, sun shadow caster draws, full maps, kept passes, sun shadow share\n",
        std::min(HistoryCount, HISTORY_FRAMES));
    for (size_t n = HistoryCount > HISTORY_FRAMES ? HistoryCount - HISTORY_FRAMES : 0; n < HistoryCount; ++n)
    {
        const FrameHistory &frame = History[n % HISTORY_FRAMES];
        fprintf(file, "%llu %p %d %d %d %d %d %d %d %d %d %d %d %.3f\n", static_cast<unsigned long long>(frame.Frame),
            static_cast<void *>(frame.LightBuffer), frame.LightBufferSet, frame.LightBufferReleased, frame.Drawn ? 1 : 0,
            frame.Scenes, frame.Finishes, frame.Applications, frame.SunKnown ? 1 : 0, frame.WorldToClipKnown ? 1 : 0,
            frame.CasterDraws, frame.FullMaps, frame.KeptPasses, frame.SunShadowShare);
    }
    fprintf(file, "\n--- last %zu sun correction frames: frame, weight, active, native activity, evidence frame, epoch, direction xyz, attempts, applied\n",
        std::min(HistoryCount, HISTORY_FRAMES));
    for (size_t n = HistoryCount > HISTORY_FRAMES ? HistoryCount - HISTORY_FRAMES : 0; n < HistoryCount; ++n)
    {
        const FrameHistory &frame = History[n % HISTORY_FRAMES];
        fprintf(file, "%llu %.6f %d %.6f %llu %llu %.6f %.6f %.6f %u %u\n",
            static_cast<unsigned long long>(frame.Frame), frame.DirectionWeight, frame.DirectionActive ? 1 : 0,
            frame.DirectionActivity, static_cast<unsigned long long>(frame.DirectionEvidenceFrame),
            static_cast<unsigned long long>(frame.DirectionEpoch), frame.CorrectedDirection.x,
            frame.CorrectedDirection.y, frame.CorrectedDirection.z, frame.DirectionAttempts, frame.DirectionApplied);
    }
    fprintf(file, "\n--- sun shadow shaders: pixel shader, image slot, replacement\n");
    {
        std::lock_guard guard(capture::ClassLock);
        for (const auto &[shader, found] : capture::Classes)
            if ((found.Bits & capture::CLASS_SUN_PASS) != 0)
                fprintf(file, "%016llx %u %d\n", static_cast<unsigned long long>(shader), found.SunImageSlot,
                    found.SunReplacement);
    }
    fclose(file);
}

// runs when the game moves on from the light buffer, its lamps done and the main pass yet to read it
static void FinishLightBuffer(ID3D11DeviceContext *ctx)
{
    StopTimer(ctx, LampTimer);
    ++ThisFrame.Finishes;
    if (capture::State == capture::Phase::Capturing)
    {
        WriteState();
        if (capture::Log)
            fprintf(capture::Log, "--- light buffer r%d finished after draw %u\n",
                capture::GetResourceId(reinterpret_cast<uint64_t>(LightBuffer.Get())),
                capture::DrawIndex);
    }
    Processed = true;
    ComPtr<ID3D11DeviceContext1> ctx1;
    ComPtr<ID3D11Device> dev;
    ctx->GetDevice(&dev);
    if (tq_world::Work.AO() && OcclusionEnabled && SUCCEEDED(ctx->QueryInterface(IID_PPV_ARGS(&ctx1))) && CreateObjects(dev.Get()))
        ApplyOcclusion(ctx1.Get());
    // with no lamps the light is copied here, so the sun shadow can still be held to the share from before the lamps
    if (tq_world::Work.Sun() && !BaseCopied)
        CopyBaseLight(ctx);

    // Surface colour uses the native depth only to exclude the sky. It does
    // not request point-light probes, AO resources or a reconstructed camera.
    if (tq_world::Work.Surface() && !DepthCopied)
        CopyDepth(ctx);

    // Only point-light maps or an explicit diagnostic need the probe depth here.
    if (!tq_world::Work.Point() && capture::State != capture::Phase::Capturing)
        return;
    // the shadow maps probe this depth for objects removed from the world
    if (!WorldToClipKnown || (!DepthCopied && !CopyDepth(ctx)))
        return;

    D3D11_TEXTURE2D_DESC desc;
    DepthCopy->GetDesc(&desc);
    shadowmaps::ProbeDepth = DepthCopyView;
    shadowmaps::ProbeWorldToClip = WorldToClip;
    shadowmaps::ProbeWidth = desc.Width;
    shadowmaps::ProbeHeight = desc.Height;
    if (tq_world::Work.Point() && DrawExtraLights)
        DrawExtraLights(ctx);
}

static ID3D11Resource *GetResource(uint64_t view, ComPtr<ID3D11Resource> &resource)
{
    resource.Reset();
    if (view)
        reinterpret_cast<ID3D11View *>(view)->GetResource(&resource);
    return resource.Get();
}

// forgets what the light buffer's last pass left, for a pass that draws a scene into it again
static void StartScene(bool ordinaryFrame=false)
{
    if(!ordinaryFrame)
    {
        DirectionCorrection.Clear();
        for(auto &map:CascadeMaps){map.NativeWrites=0;map.NativeWriteFrame=map.NativeWriteEpoch=0;}
    }
    BaseCopied = false;
    Processed = false;
    OcclusionApplied = false;
    ScaledBuilt = false;
    DepthCopied = false;
    ColourImage = nullptr;
    SurfaceDone = false;
    WorldToClipKnown = false;
    CandidateKnown = false;
    SunKnown = false;
}

// notes the sun and ambient colours of a base light draw by the game's sun-lit shaders: the sun's colour and direction
// are its vertex rows c11 and c13, and the ambient colour the pixel shader's first constant
static void ReadSun(ID3D11DeviceContext *ctx, const XMFLOAT4 (&rows)[shadowmaps::ROW_COUNT])
{
    ComPtr<ID3D11DeviceContext1> ctx1;
    if (SunKnown || FAILED(ctx->QueryInterface(IID_PPV_ARGS(&ctx1))))
        return;

    ComPtr<ID3D11Buffer> constants;
    UINT firstConstant = 0, constantCount = 0;
    ctx1->PSGetConstantBuffers1(0, 1, &constants, &firstConstant, &constantCount);
    if (!constants)
        return;

    D3D11_BUFFER_DESC desc;
    constants->GetDesc(&desc);
    const UINT start = firstConstant * 16;
    D3D11_MAPPED_SUBRESOURCE mapped;
    if (desc.Usage != D3D11_USAGE_DYNAMIC || start + 16 > desc.ByteWidth
        || FAILED(ctx->Map(constants.Get(), 0, D3D11_MAP_WRITE_NO_OVERWRITE, 0, &mapped)))
        return;
    std::memcpy(&Ambient, static_cast<const uint8_t *>(mapped.pData) + start, sizeof(Ambient));
    ctx->Unmap(constants.Get(), 0);
    SunColour = rows[11 - shadowmaps::FIRST_ROW];
    SunDirection = rows[13 - shadowmaps::FIRST_ROW];
    SunKnown = true;
}

// reads the direction toward the sun or moon from the near cascade's sun shadow pass, the first in SUN_SHADERS: its
// pixel shader's row c2 is the shadow map's depth row, which points away from the light
static bool ReadCameraRow(ID3D11DeviceContext1 *ctx,XMFLOAT3 &camera);
static void ReadSunTowardLight(shadowmaps::BoundDraw &bound)
{
    if ((capture::PixelClass.Bits & capture::CLASS_SUN_PASS) == 0 || capture::PixelClass.SunReplacement != 0)
        return;
    if(!WorldToClipKnown||!ScreenTarget)return;
    auto *ctx=bound.Context;
    UINT views=0;const D3D11_VIEWPORT &viewport=bound.GetViewport(views);
    if(views!=1||viewport.TopLeftX||viewport.TopLeftY||viewport.Width!=shadowmaps::ScreenWidth
        ||viewport.Height!=shadowmaps::ScreenHeight||viewport.MinDepth!=0||viewport.MaxDepth!=1)return;
    ComPtr<ID3D11DeviceContext1> ctx1;
    if (FAILED(ctx->QueryInterface(IID_PPV_ARGS(&ctx1))))
        return;

    XMFLOAT3 sourceCamera{};
    if(!shadowmaps::CameraFound||!ReadCameraRow(ctx1.Get(),sourceCamera)
        ||!sun_source_policy::SameCamera(sourceCamera.x,sourceCamera.y,sourceCamera.z,
            shadowmaps::Camera.x,shadowmaps::Camera.y,shadowmaps::Camera.z))return;

    ComPtr<ID3D11Buffer> constants;
    UINT firstConstant = 0, constantCount = 0;
    ctx1->PSGetConstantBuffers1(0, 1, &constants, &firstConstant, &constantCount);
    if (!constants)
        return;

    D3D11_BUFFER_DESC desc;
    constants->GetDesc(&desc);
    const UINT start = (firstConstant + 2) * 16;
    D3D11_MAPPED_SUBRESOURCE mapped;
    if (desc.Usage != D3D11_USAGE_DYNAMIC || !sun_state_policy::Window(firstConstant,constantCount,desc.ByteWidth,0,20)
        || FAILED(ctx->Map(constants.Get(), 0, D3D11_MAP_WRITE_NO_OVERWRITE, 0, &mapped)))
        return;
    XMFLOAT4 depthRow;float nativeStrength=0;
    std::memcpy(&depthRow, static_cast<const uint8_t *>(mapped.pData) + start, sizeof(depthRow));
    std::memcpy(&nativeStrength,static_cast<const uint8_t *>(mapped.pData)+(firstConstant+19)*16,sizeof(nativeStrength));
    ctx->Unmap(constants.Get(), 0);

    // A near shader and a nonzero strength are insufficient indoors. The exact native map must
    // already be known and have received original native writes in this scene and frame. Unknown
    // unshadowed/outdoor cases stay native; this is not a universal no-sun classification.
    ComPtr<ID3D11ShaderResourceView> sourceView;
    ctx->PSGetShaderResources(CASCADE_MAP_SLOT,1,&sourceView);
    ComPtr<ID3D11Resource> sourceResource;
    D3D11_SHADER_RESOURCE_VIEW_DESC sourceDesc{};
    if(sourceView){sourceView->GetDesc(&sourceDesc);sourceView->GetResource(&sourceResource);}
    auto &nativeMap=CascadeMaps[0];
    if(sourceResource.Get()!=nativeMap.Resource)
    {
        // A new/replaced native map cannot keep the prior map's correction alive, even at the
        // same dimensions. EaseSunShadow records its new generation after this observation.
        if(DirectionCorrection.evidence||DirectionCorrection.published)DirectionCorrection.Clear();
        return;
    }
    const sun_source_policy::Evidence evidence{nativeMap.Generation,nativeMap.SeenFrame,nativeMap.NativeWriteFrame,
        nativeMap.NativeWriteEpoch,nativeMap.NativeWrites,sourceResource.Get()!=nullptr,
        sourceView&&sourceDesc.ViewDimension==D3D11_SRV_DIMENSION_TEXTURE2D
            &&sourceDesc.Texture2D.MostDetailedMip==0&&sourceDesc.Texture2D.MipLevels==1};
    const bool admitted=sun_source_policy::Admit(evidence,SunFrame,DirectionCorrection.epoch,nativeStrength);
    if(capture::State==capture::Phase::Capturing&&capture::Log)
        fprintf(capture::Log,"  sun native source: generation %llu, writes %u, write frame %llu, epoch %llu, admitted %d\n",
            static_cast<unsigned long long>(nativeMap.Generation),nativeMap.NativeWrites,
            static_cast<unsigned long long>(nativeMap.NativeWriteFrame),static_cast<unsigned long long>(nativeMap.NativeWriteEpoch),
            admitted?1:0);
    if(!admitted)
    {
        if(sun_source_policy::Proven(evidence,SunFrame,DirectionCorrection.epoch)
            &&std::isfinite(nativeStrength)&&nativeStrength<=0)
            DirectionCorrection.Observe(SunFrame,SunSeconds(),{},0);
        return;
    }
    const sun_direction_policy::Vector toward{-depthRow.x,-depthRow.y,-depthRow.z};
    if(!DirectionCorrection.Observe(SunFrame,SunSeconds(),toward,nativeStrength))return;
    if(nativeStrength>0)
    {
        const auto direction=DirectionCorrection.observed;
        SunTowardLight={direction.x,direction.y,direction.z};SunTowardFrame=SunFrame;
    }
    if(capture::State==capture::Phase::Capturing&&capture::Log)
        fprintf(capture::Log,"  sun direction evidence: native strength %g, active %d, weight %.3f, frame %llu\n",
            nativeStrength,nativeStrength>0?1:0,DirectionCorrection.weight,static_cast<unsigned long long>(SunFrame));

}

// eases a sun-lit animated mesh's sun colour in its base light draw with the sun shadow's share: while the shadow fades
// out indoors from the colour the mesh had outdoors to the game's, and while it fades in outdoors from the colour the
// mesh had indoors. A mesh with no kept colour from the other side keeps the game's. The row is written in the draw's
// own window, as the sun direction is
static void EaseSunColour(ID3D11DeviceContext *ctx, uint64_t meshKey)
{
    if (!SmoothIndoorTransition || !ShareFrame || !LightBuffer || CurrentTarget != LightBuffer.Get() || !WorldToClipKnown)
        return;

    ComPtr<ID3D11DeviceContext1> ctx1;
    if (FAILED(ctx->QueryInterface(IID_PPV_ARGS(&ctx1))))
        return;

    ComPtr<ID3D11Buffer> constants;
    UINT firstConstant = 0, constantCount = 0;
    ctx1->VSGetConstantBuffers1(0, 1, &constants, &firstConstant, &constantCount);
    if (!constants)
        return;

    D3D11_BUFFER_DESC desc;
    constants->GetDesc(&desc);
    const UINT start = (firstConstant + 11) * 16;
    D3D11_MAPPED_SUBRESOURCE mapped;
    if (desc.Usage != D3D11_USAGE_DYNAMIC || !sun_state_policy::Window(firstConstant,constantCount,desc.ByteWidth,11,1)
        || FAILED(ctx->Map(constants.Get(), 0, D3D11_MAP_WRITE_NO_OVERWRITE, 0, &mapped)))
        return;

    auto *row = reinterpret_cast<XMFLOAT4 *>(static_cast<uint8_t *>(mapped.pData) + start);
    const XMFLOAT4 game = *row;
    if(!std::isfinite(game.x)||!std::isfinite(game.y)||!std::isfinite(game.z)||!std::isfinite(game.w))
    {ctx->Unmap(constants.Get(),0);return;}
    MeshSunColour &kept = MeshSunColours[meshKey];
    kept.SeenFrame = SunFrame;
    const bool indoors = SunShadowTarget < 0.5f;
    const XMFLOAT4 *from = indoors ? (kept.OutdoorKnown ? &kept.Outdoor : nullptr)
        : (kept.IndoorKnown ? &kept.Indoor : nullptr);
    const float weight = indoors ? SunShadowShare : 1 - SunShadowShare;
    const bool eased = from && weight > 0.001f;
    if (eased)
        *row = { game.x + (from->x - game.x) * weight, game.y + (from->y - game.y) * weight,
            game.z + (from->z - game.z) * weight, game.w };
    const XMFLOAT4 written = *row;
    if (indoors)
    {
        kept.Indoor = game;
        kept.IndoorKnown = true;
    }
    else if (SunShadowFull)
    {
        kept.Recent[kept.RecentNext] = game;
        kept.RecentNext = (kept.RecentNext + 1) % OUTDOOR_COLOUR_FRAMES;
        kept.RecentCount = std::min(kept.RecentCount + 1, OUTDOOR_COLOUR_FRAMES);
        kept.Outdoor = kept.Recent[kept.RecentCount < OUTDOOR_COLOUR_FRAMES ? 0 : kept.RecentNext];
        kept.OutdoorKnown = true;
    }
    ctx->Unmap(constants.Get(), 0);
    if (eased && capture::State == capture::Phase::Capturing && capture::Log)
        fprintf(capture::Log, "  sun colour eased: mesh %016llx, game %g %g %g, eased %g %g %g\n",
            static_cast<unsigned long long>(meshKey), game.x, game.y, game.z, written.x, written.y, written.z);
}

// lights a base light draw from the direction toward the sun or moon read from a recent sun shadow pass, so it matches
// the sun's shadows. The game's sun-lit animated mesh shaders light from vertex row c13, which it leaves at one fixed
// direction. The row is written in the draw's own window of the game's dynamic buffer, as each draw has its own.
static void ReplaceSunDirection(ID3D11DeviceContext *ctx)
{
    ++ThisFrame.DirectionAttempts;
    if (!SunDirectionFix || !LightBuffer || CurrentTarget != LightBuffer.Get() || !WorldToClipKnown || DirectionCorrection.weight<=0)
        return;

    ComPtr<ID3D11DeviceContext1> ctx1;
    if (FAILED(ctx->QueryInterface(IID_PPV_ARGS(&ctx1))))
        return;

    ComPtr<ID3D11Buffer> constants;
    UINT firstConstant = 0, constantCount = 0;
    ctx1->VSGetConstantBuffers1(0, 1, &constants, &firstConstant, &constantCount);
    if (!constants)
        return;

    D3D11_BUFFER_DESC desc;
    constants->GetDesc(&desc);
    const UINT start = (firstConstant + 13) * 16;
    D3D11_MAPPED_SUBRESOURCE mapped;
    if (desc.Usage != D3D11_USAGE_DYNAMIC || !sun_state_policy::Window(firstConstant,constantCount,desc.ByteWidth,13,1)
        || FAILED(ctx->Map(constants.Get(), 0, D3D11_MAP_WRITE_NO_OVERWRITE, 0, &mapped)))
        return;
    XMFLOAT4 native;std::memcpy(&native,static_cast<uint8_t *>(mapped.pData)+start,sizeof(native));
    sun_direction_policy::Vector blended;
    if(!DirectionCorrection.Apply({native.x,native.y,native.z},native.w,blended))
    {ctx->Unmap(constants.Get(),0);return;}
    const XMFLOAT4 direction{blended.x,blended.y,blended.z,native.w};
    ++ThisFrame.DirectionApplied;
    std::memcpy(static_cast<uint8_t *>(mapped.pData) + start, &direction, sizeof(direction));
    ctx->Unmap(constants.Get(), 0);
    if (capture::State == capture::Phase::Capturing && capture::Log)
        fprintf(capture::Log, "  sun direction replaced at weight %.3f\n",DirectionCorrection.weight);
}

// reads the camera position, row c16 of a draw's vertex constants
static bool ReadCameraRow(ID3D11DeviceContext1 *ctx, XMFLOAT3 &camera)
{
    ComPtr<ID3D11Buffer> constants;
    UINT firstConstant = 0, constantCount = 0;
    ctx->VSGetConstantBuffers1(0, 1, &constants, &firstConstant, &constantCount);
    if (!constants)
        return false;

    D3D11_BUFFER_DESC desc;
    constants->GetDesc(&desc);
    const UINT start = (firstConstant + 16) * 16;
    D3D11_MAPPED_SUBRESOURCE mapped;
    if (desc.Usage != D3D11_USAGE_DYNAMIC || !sun_state_policy::Window(firstConstant,constantCount,desc.ByteWidth,16,1)
        || FAILED(ctx->Map(constants.Get(), 0, D3D11_MAP_WRITE_NO_OVERWRITE, 0, &mapped)))
        return false;
    std::memcpy(&camera, static_cast<const uint8_t *>(mapped.pData) + start, sizeof(camera));
    ctx->Unmap(constants.Get(), 0);
    return true;
}

// fills the game's receiver depth and facing for the sun passes into ReceiverDepth and ReceiverFacing, once a frame
static bool BuildReceiverDepth(ID3D11DeviceContext *ctx, ID3D11ShaderResourceView *gameView,
    ID3D11ShaderResourceView *gameFacingView, const XMFLOAT3 &camera)
{
    if (ReceiverFrame == SunFrame)
        return ReceiverBuilt;
    ReceiverFrame = SunFrame;
    ReceiverBuilt = false;
    ComPtr<ID3D11DeviceContext1> ctx1;
    ComPtr<ID3D11Device> dev;
    ctx->GetDevice(&dev);
    if (!gameView || !gameFacingView || !DepthCopied || !DepthCopyView || !WorldToClipKnown
        || FAILED(ctx->QueryInterface(IID_PPV_ARGS(&ctx1)))
        || !CreateObjects(dev.Get()))
        return false;

    D3D11_TEXTURE2D_DESC desc;
    DepthCopy->GetDesc(&desc);
    D3D11_TEXTURE2D_DESC receiverDesc = {};
    if (ReceiverDepth)
        ReceiverDepth->GetDesc(&receiverDesc);
    if (!ReceiverDepth || receiverDesc.Width != desc.Width || receiverDesc.Height != desc.Height)
    {
        ReceiverDepth.Reset();
        ReceiverDepthView.Reset();
        ReceiverDepthTarget.Reset();
        ReceiverFacing.Reset();
        ReceiverFacingView.Reset();
        ReceiverFacingTarget.Reset();
        receiverDesc = { desc.Width, desc.Height, 1, 1, DXGI_FORMAT_R32_FLOAT, { 1, 0 }, D3D11_USAGE_DEFAULT,
            D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET };
        if (FAILED(dev->CreateTexture2D(&receiverDesc, nullptr, &ReceiverDepth))
            || FAILED(dev->CreateShaderResourceView(ReceiverDepth.Get(), nullptr, &ReceiverDepthView))
            || FAILED(dev->CreateRenderTargetView(ReceiverDepth.Get(), nullptr, &ReceiverDepthTarget))
            || FAILED(dev->CreateTexture2D(&receiverDesc, nullptr, &ReceiverFacing))
            || FAILED(dev->CreateShaderResourceView(ReceiverFacing.Get(), nullptr, &ReceiverFacingView))
            || FAILED(dev->CreateRenderTargetView(ReceiverFacing.Get(), nullptr, &ReceiverFacingTarget)))
        {
            ReceiverDepth.Reset();
            ReceiverDepthView.Reset();
            ReceiverDepthTarget.Reset();
            ReceiverFacing.Reset();
            ReceiverFacingView.Reset();
            ReceiverFacingTarget.Reset();
            return false;
        }
    }

    XMFLOAT4X4 worldToClip;
    XMStoreFloat4x4(&worldToClip, WorldToClip);
    XMFLOAT4 constants[11];
    XMStoreFloat4x4(reinterpret_cast<XMFLOAT4X4 *>(constants), XMMatrixInverse(nullptr, WorldToClip));
    constants[4] = { worldToClip._41, worldToClip._42, worldToClip._43, worldToClip._44 };
    constants[5] = { static_cast<float>(desc.Width), static_cast<float>(desc.Height), 1.0f / desc.Width,
        1.0f / desc.Height };
    XMStoreFloat4x4(reinterpret_cast<XMFLOAT4X4 *>(constants + 6), KeptWorldToClip);
    constants[10] = { camera.x - KeptCamera.x, camera.y - KeptCamera.y, camera.z - KeptCamera.z, 0 };
    ComPtr<ID3DDeviceContextState> previous;
    ctx1->SwapDeviceContextState(DrawState.Get(), &previous);
    ctx1->UpdateSubresource(ReceiverBuffer.Get(), 0, nullptr, constants, 0, 0);
    ID3D11Buffer *buffer = ReceiverBuffer.Get();
    ctx1->PSSetConstantBuffers(0, 1, &buffer);
    ctx1->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx1->VSSetShader(FullScreenShader.Get(), nullptr, 0);
    // without a kept receiver depth the shader reads 0 there, which matches no surface
    ID3D11ShaderResourceView *views[] = { DepthCopyView.Get(), gameView,
        KeptReceiversKnown ? KeptReceiversView.Get() : nullptr, gameFacingView,
        KeptReceiversKnown ? KeptFacingView.Get() : nullptr };
    ID3D11RenderTargetView *targets[] = { ReceiverDepthTarget.Get(), ReceiverFacingTarget.Get() };
    DrawFullScreen(ctx1.Get(), targets, 2, desc.Width, desc.Height, ReceiverShader.Get(), views, 5);
    ctx1->SwapDeviceContextState(previous.Get(), nullptr);
    ReceiverBuilt = true;
    if (capture::State == capture::Phase::Capturing && capture::Log)
    {
        ComPtr<ID3D11Resource> gameResource;
        gameView->GetResource(&gameResource);
        ComPtr<ID3D11Texture2D> gameTexture;
        if (SUCCEEDED(gameResource.As(&gameTexture)))
            capture::AddSnapshot(ctx, gameTexture.Get(), "game-receiver-depth");
        ComPtr<ID3D11Resource> gameFacingResource;
        gameFacingView->GetResource(&gameFacingResource);
        ComPtr<ID3D11Texture2D> gameFacingTexture;
        if (SUCCEEDED(gameFacingResource.As(&gameFacingTexture)))
            capture::AddSnapshot(ctx, gameFacingTexture.Get(), "game-facing");
        if (KeptReceiversKnown)
        {
            capture::AddSnapshot(ctx, KeptReceivers.Get(), "kept-receiver-depth");
            capture::AddSnapshot(ctx, KeptFacing.Get(), "kept-facing");
        }
        capture::AddSnapshot(ctx, ReceiverDepth.Get(), "filled-receiver-depth");
        capture::AddSnapshot(ctx, ReceiverFacing.Get(), "filled-facing");
    }
    return true;
}

// counts the game's caster draws into each cascade map
static void CountCasterDraw(shadowmaps::BoundDraw &bound)
{
    for (CascadeMap &map : CascadeMaps)
    {
        // Keep the upstream caster/share accounting unchanged. Direction-source evidence has
        // a separate exact actual binding + write-state proof and supports depth-only maps too.
        if(map.Resource&&map.Resource==CurrentTarget)++map.Draws;
        if(!map.Resource||(map.Resource!=CurrentTarget&&map.Resource!=CurrentDepth))continue;
        ComPtr<ID3D11RenderTargetView> rtv;ComPtr<ID3D11DepthStencilView>dsv;
        bound.Context->OMGetRenderTargets(1,&rtv,&dsv);
        ComPtr<ID3D11Resource> colour,depth;
        if(rtv)rtv->GetResource(&colour);if(dsv)dsv->GetResource(&depth);
        bool colourWrite=false,depthWrite=false;
        if(colour.Get()==map.Resource)
        {
            UINT mask=0;const D3D11_BLEND_DESC *blend=bound.GetBlend(&mask);
            D3D11_RENDER_TARGET_VIEW_DESC view{};rtv->GetDesc(&view);
            colourWrite=mask!=0&&view.ViewDimension==D3D11_RTV_DIMENSION_TEXTURE2D&&view.Texture2D.MipSlice==0
                &&(!blend||(blend->RenderTarget[0].RenderTargetWriteMask&D3D11_COLOR_WRITE_ENABLE_RED));
        }
        if(depth.Get()==map.Resource)
        {
            const D3D11_DEPTH_STENCIL_DESC *state=bound.GetDepth();
            D3D11_DEPTH_STENCIL_VIEW_DESC view{};dsv->GetDesc(&view);
            depthWrite=view.ViewDimension==D3D11_DSV_DIMENSION_TEXTURE2D&&view.Texture2D.MipSlice==0
                &&!(view.Flags&D3D11_DSV_READ_ONLY_DEPTH)
                &&(!state||(state->DepthEnable&&state->DepthWriteMask==D3D11_DEPTH_WRITE_MASK_ALL));
        }
        if(!sun_source_policy::NativeWrite(colour.Get()==map.Resource,colourWrite,depth.Get()==map.Resource,depthWrite))continue;
        if(map.NativeWriteFrame!=SunFrame||map.NativeWriteEpoch!=DirectionCorrection.epoch)
        {map.NativeWriteFrame=SunFrame;map.NativeWriteEpoch=DirectionCorrection.epoch;map.NativeWrites=0;}
        if(map.NativeWrites!=UINT32_MAX)++map.NativeWrites;
    }
}

// whether the game drew about as many casters into a map this frame as it has lately
static bool IsFull(const CascadeMap &map)
{
    return map.Draws > 0 && map.Draws >= FULL_SHARE * map.DrawLevel;
}

// copies one of the game's textures with one level into copy, made again to match it, with a view like the game's
static bool CopyGameTexture(ID3D11DeviceContext *ctx, ID3D11Resource *resource, ID3D11ShaderResourceView *view,
    ComPtr<ID3D11Texture2D> &copy, ComPtr<ID3D11ShaderResourceView> &copyView)
{
    if(!resource||!view)return false;
    ComPtr<ID3D11Texture2D> texture;
    if (FAILED(resource->QueryInterface(IID_PPV_ARGS(&texture))))
        return false;

    D3D11_TEXTURE2D_DESC desc;
    texture->GetDesc(&desc);
    D3D11_SHADER_RESOURCE_VIEW_DESC sourceView{};view->GetDesc(&sourceView);
    ComPtr<ID3D11Resource> viewed;view->GetResource(&viewed);
    const sun_state_policy::CopyShape shape{desc.Width,desc.Height,desc.MipLevels,desc.ArraySize,
        desc.SampleDesc.Count,desc.SampleDesc.Quality,sourceView.ViewDimension==D3D11_SRV_DIMENSION_TEXTURE2D,
        sourceView.Texture2D.MostDetailedMip,sourceView.Texture2D.MipLevels};
    if(viewed.Get()!=resource||!sun_state_policy::Copyable(shape))
    {copy.Reset();copyView.Reset();return false;}
    D3D11_TEXTURE2D_DESC copyDesc = {};D3D11_SHADER_RESOURCE_VIEW_DESC savedView{};
    if (copy) copy->GetDesc(&copyDesc);
    if (copyView)copyView->GetDesc(&savedView);
    if (!copy || !copyView || copyDesc.Width != desc.Width || copyDesc.Height != desc.Height || copyDesc.Format != desc.Format
        ||copyDesc.MipLevels!=1||copyDesc.ArraySize!=1||copyDesc.SampleDesc.Count!=1||copyDesc.SampleDesc.Quality
        ||savedView.Format!=sourceView.Format||savedView.ViewDimension!=sourceView.ViewDimension
        ||savedView.Texture2D.MostDetailedMip!=sourceView.Texture2D.MostDetailedMip
        ||savedView.Texture2D.MipLevels!=sourceView.Texture2D.MipLevels)
    {
        copy.Reset();
        copyView.Reset();
        ComPtr<ID3D11Device> dev;
        ctx->GetDevice(&dev);
        copyDesc = { desc.Width, desc.Height, 1, 1, desc.Format, { 1, 0 }, D3D11_USAGE_DEFAULT,
            D3D11_BIND_SHADER_RESOURCE };
        D3D11_SHADER_RESOURCE_VIEW_DESC viewDesc;
        view->GetDesc(&viewDesc);
        if (desc.MipLevels != 1 || desc.ArraySize != 1 || desc.SampleDesc.Count != 1
            || FAILED(dev->CreateTexture2D(&copyDesc, nullptr, &copy))
            || FAILED(dev->CreateShaderResourceView(copy.Get(), &viewDesc, &copyView)))
        {
            copy.Reset();
            copyView.Reset();
            return false;
        }
    }
    ctx->CopyResource(copy.Get(), resource);
    return true;
}

// copies a cascade map, with its pass's rows and the camera, so the shadow can be faded out once the game stops drawing it
static void CopyCascadeMap(ID3D11DeviceContext *ctx, CascadeMap &map, ID3D11Resource *resource,
    ID3D11ShaderResourceView *view, const XMFLOAT4 *rows, const XMFLOAT3 &camera)
{
    if (!CopyGameTexture(ctx, resource, view, map.Copy, map.CopyView))
    {
        map.RowsKnown = false;
        return;
    }
    std::memcpy(map.Rows, rows, sizeof(map.Rows));
    map.RowsCamera = camera;
    map.RowsKnown = true;
}

// fades a sun shadow cascade pass between the game's shadow and none. On a frame the game draws its usual casters the
// pass's strength (row c19) is scaled by the eased share, and the map is copied with the pass's cascade rows, one map a
// frame in turn. On a frame it does not, the pass draws the copy with the kept rows, moved for the camera since, and the
// filled receiver depth and facing, at the eased share, until the share reaches 0. True when it bound the copy and the
// filled receiver depth and facing, with the game's views to put back after the pass and the pass's cascade
static bool EaseSunShadow(ID3D11DeviceContext *ctx, ComPtr<ID3D11ShaderResourceView> &gameMap,
    ComPtr<ID3D11ShaderResourceView> &gameReceivers, ComPtr<ID3D11ShaderResourceView> &gameFacing, int &keptCascade)
{
    if ((capture::PixelClass.Bits & capture::CLASS_SUN_PASS) == 0)
        return false;
    const int cascade = capture::PixelClass.SunReplacement;
    ComPtr<ID3D11DeviceContext1> ctx1;
    if (cascade < 0 || FAILED(ctx->QueryInterface(IID_PPV_ARGS(&ctx1))))
        return false;

    CascadeMap &map = CascadeMaps[cascade];
    ComPtr<ID3D11ShaderResourceView> mapView;
    ctx->PSGetShaderResources(CASCADE_MAP_SLOT, 1, &mapView);
    ComPtr<ID3D11Resource> mapResource;
    if (mapView)
        mapView->GetResource(&mapResource);
    if (map.Resource != mapResource.Get())
    {
        map = {};
        map.Resource = mapResource.Get();
        map.SeenFrame = SunFrame;
        map.Generation=NextCascadeGeneration++;
        if(cascade==0)DirectionCorrection.Clear();
    }

    // maps first seen this frame were drawn into before they were known, so only the others tell whether casters are
    // drawn; with none of those the share is left as it is
    if (ShareFrame != SunFrame)
    {
        const auto now = std::chrono::steady_clock::now();
        const float seconds = ShareFrame ? std::min(std::chrono::duration<float>(now - ShareTime).count(), 0.25f) : 0;
        ShareFrame = SunFrame;
        ShareTime = now;
        FrameSeconds = seconds;
        // only a frame with no casters at all is indoors; one with a few, as when the camera turns to an open view or at
        // the door, keeps the shadow at its strength from the last full copy
        bool known = false, drawn = false, full = false;
        for (CascadeMap &other : CascadeMaps)
            if (other.Resource && other.SeenFrame != SunFrame)
            {
                known = true;
                drawn = drawn || other.Draws > 0;
                full = full || IsFull(other);
            }
        if (known)
        {
            SunShadowTarget = drawn ? 1.0f : 0.0f;
            SunShadowFull = full;
        }
        SunShadowShare = SmoothIndoorTransition
            ? SunShadowShare + (SunShadowTarget - SunShadowShare) * (1 - std::exp(-seconds / INDOOR_EASE_SECONDS))
            : SunShadowTarget;
    }
    const bool full = IsFull(map);
    const float drawLevel = map.DrawLevel;
    if (map.SeenFrame != SunFrame && map.Draws > 0)
        map.DrawLevel += (map.Draws - map.DrawLevel)
            * (1 - std::exp(-FrameSeconds / DRAW_LEVEL_SECONDS));

    ComPtr<ID3D11Buffer> constants;
    UINT firstConstant = 0, constantCount = 0;
    ctx1->PSGetConstantBuffers1(0, 1, &constants, &firstConstant, &constantCount);
    XMFLOAT3 camera;
    if (!constants || !mapView || !ReadCameraRow(ctx1.Get(), camera))
        return false;

    D3D11_BUFFER_DESC desc;
    constants->GetDesc(&desc);
    const UINT start = firstConstant * 16;
    const bool keptRows = !full && map.RowsKnown && map.CopyView && SmoothIndoorTransition && SunShadowShare > 0.001f;
    D3D11_MAPPED_SUBRESOURCE mapped;
    if (desc.Usage != D3D11_USAGE_DYNAMIC || !sun_state_policy::Window(firstConstant,constantCount,desc.ByteWidth,0,20)
        || FAILED(ctx->Map(constants.Get(), 0, D3D11_MAP_WRITE_NO_OVERWRITE, 0, &mapped)))
        return false;

    auto *rows = reinterpret_cast<XMFLOAT4 *>(static_cast<uint8_t *>(mapped.pData) + start);
    bool copied = false;
    // ponytail: one map copied a frame in turn keeps the copy cost to one map's size; a copy up to two frames old is
    // kept at the start of the fade
    if (full && SmoothIndoorTransition
        && (SunFrame % SUN_SHADER_COUNT == static_cast<uint64_t>(cascade) || !map.RowsKnown))
    {
        CopyCascadeMap(ctx, map, mapResource.Get(), mapView.Get(), rows, camera);
        copied = map.RowsKnown;
    }
    if (full && SmoothIndoorTransition && WorldToClipKnown && KeptReceiversFrame != SunFrame)
    {
        KeptReceiversFrame = SunFrame;
        ComPtr<ID3D11ShaderResourceView> receiverView, facingView;
        ctx->PSGetShaderResources(RECEIVER_DEPTH_SLOT, 1, &receiverView);
        ctx->PSGetShaderResources(FACING_SLOT, 1, &facingView);
        ComPtr<ID3D11Resource> receiverResource, facingResource;
        if (receiverView)
            receiverView->GetResource(&receiverResource);
        if (facingView)
            facingView->GetResource(&facingResource);
        KeptReceiversKnown = receiverResource && facingResource
            && CopyGameTexture(ctx, receiverResource.Get(), receiverView.Get(), KeptReceivers, KeptReceiversView)
            && CopyGameTexture(ctx, facingResource.Get(), facingView.Get(), KeptFacing, KeptFacingView);
        KeptWorldToClip = WorldToClip;
        KeptCamera = camera;
    }
    // the rows take camera-relative positions, so a camera that has moved since moves the kept map by as much
    if (keptRows)
        for (UINT row = 0; row < std::size(map.Rows); ++row)
        {
            const XMFLOAT4 &kept = map.Rows[row];
            rows[row] = { kept.x, kept.y, kept.z, kept.w + kept.x * (camera.x - map.RowsCamera.x)
                + kept.y * (camera.y - map.RowsCamera.y) + kept.z * (camera.z - map.RowsCamera.z) };
        }
    const float gameStrength = rows[19].x;
    // every pass is drawn at the share, kept or not, so a pass that switches between the game's map and the copy keeps
    // its strength
    const float share = SunShadowShare;
    rows[19].x = gameStrength * share;
    const XMFLOAT4 firstRow = rows[0];
    ctx->Unmap(constants.Get(), 0);
    const bool logging = capture::State == capture::Phase::Capturing && capture::Log;
    if (logging)
        fprintf(capture::Log, "  sun shadow eased: game strength %g, eased %g (share %.3f toward %.0f), caster draws %d of "
            "level %.1f (%s), %s, %s rows, row c0 %g %g %g %g, camera moved %g %g %g since the rows\n", gameStrength,
            gameStrength * share, SunShadowShare, SunShadowTarget, map.Draws, drawLevel, full ? "full" : "not full",
            copied ? "copied" : "not copied", keptRows ? "kept" : "game's", firstRow.x, firstRow.y, firstRow.z,
            firstRow.w, camera.x - map.RowsCamera.x, camera.y - map.RowsCamera.y, camera.z - map.RowsCamera.z);
    if (!keptRows)
        return false;

    ++ThisFrame.KeptPasses;
    if (logging)
    {
        capture::AddSnapshot(ctx, map.Copy.Get(), "kept-cascade-map");
        ComPtr<ID3D11Texture2D> mapTexture;
        if (SUCCEEDED(mapResource.As(&mapTexture)))
            capture::AddSnapshot(ctx, mapTexture.Get(), "game-cascade-map");
    }
    ComPtr<ID3D11ShaderResourceView> receiverView, facingView;
    ctx->PSGetShaderResources(RECEIVER_DEPTH_SLOT, 1, &receiverView);
    ctx->PSGetShaderResources(FACING_SLOT, 1, &facingView);
    if (!BuildReceiverDepth(ctx, receiverView.Get(), facingView.Get(), camera))
        return false;

    gameMap = mapView;
    gameReceivers = receiverView;
    gameFacing = facingView;
    keptCascade = cascade;
    ID3D11ShaderResourceView *views[] = { ReceiverDepthView.Get(), map.CopyView.Get(), ReceiverFacingView.Get() };
    ctx->PSSetShaderResources(RECEIVER_DEPTH_SLOT, 1, &views[0]);
    ctx->PSSetShaderResources(CASCADE_MAP_SLOT, 1, &views[1]);
    ctx->PSSetShaderResources(FACING_SLOT, 1, &views[2]);
    if (logging)
    {
        ComPtr<ID3D11Texture2D> image;
        if (CurrentTarget && SUCCEEDED(CurrentTarget->QueryInterface(IID_PPV_ARGS(&image))))
            capture::AddSnapshot(ctx, image.Get(), KEPT_PASS_SNAPSHOTS[cascade][0]);
    }
    return true;
}

static void OnBindRenderTargets(command_list *cmd, uint32_t count, const resource_view *targets, resource_view depth)
{
    if(!tq_world::Work.Tracking())return;
    const shadowmaps::CpuScope scope(shadowmaps::LightBufferCpu);
    ComPtr<ID3D11Resource> target, depthTarget;
    GetResource(count ? targets[0].handle : 0, target);
    GetResource(depth.handle, depthTarget);
    if (LightBuffer && LightBufferDrawn && !Processed && CurrentTarget == LightBuffer.Get() && target
        && target.Get() != LightBuffer.Get())
        FinishLightBuffer(reinterpret_cast<ID3D11DeviceContext *>(cmd->get_native()));

    D3D11_TEXTURE2D_DESC desc, depthDesc;
    ScreenTarget = target && IsScreenSized(target.Get(), desc);
    if (ScreenTarget && IsScreenSized(depthTarget.Get(), depthDesc)
        && std::find(FrameTargets.begin(), FrameTargets.end(), target.Get()) == FrameTargets.end())
        FrameTargets.push_back(target.Get());
    if (target && target.Get() == LightBuffer.Get())
        LightBufferDepth = depthTarget;
    CurrentTarget = target.Get();
    CurrentDepth = depthTarget.Get();
}

// builds the image times the share of light from before the lamps into Scaled, in the image's own format and size
static bool BuildScaled(ID3D11DeviceContext1 *ctx, ID3D11ShaderResourceView *image)
{
    ComPtr<ID3D11Resource> resource;
    image->GetResource(&resource);
    D3D11_TEXTURE2D_DESC imageDesc;
    D3D11_SHADER_RESOURCE_VIEW_DESC imageViewDesc;
    image->GetDesc(&imageViewDesc);
    if (!IsScreenSized(resource.Get(), imageDesc) || imageViewDesc.ViewDimension != D3D11_SRV_DIMENSION_TEXTURE2D)
        return false;

    D3D11_TEXTURE2D_DESC scaledDesc = {};
    if (Scaled)
        Scaled->GetDesc(&scaledDesc);
    if (!Scaled || scaledDesc.Width != imageDesc.Width || scaledDesc.Height != imageDesc.Height
        || scaledDesc.Format != imageViewDesc.Format)
    {
        Scaled.Reset();
        ScaledView.Reset();
        ScaledTarget.Reset();
        ComPtr<ID3D11Device> dev;
        ctx->GetDevice(&dev);
        scaledDesc = { imageDesc.Width, imageDesc.Height, 1, 1, imageViewDesc.Format, { 1, 0 }, D3D11_USAGE_DEFAULT,
            D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET };
        if (FAILED(dev->CreateTexture2D(&scaledDesc, nullptr, &Scaled))
            || FAILED(dev->CreateShaderResourceView(Scaled.Get(), nullptr, &ScaledView))
            || FAILED(dev->CreateRenderTargetView(Scaled.Get(), nullptr, &ScaledTarget)))
        {
            Scaled.Reset();
            ScaledView.Reset();
            ScaledTarget.Reset();
            return false;
        }
    }

    ComPtr<ID3DDeviceContextState> previous;
    ctx->SwapDeviceContextState(DrawState.Get(), &previous);
    ID3D11SamplerState *samplers[] = { PointClamp.Get(), LinearClamp.Get() };
    ctx->PSSetSamplers(0, 2, samplers);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->VSSetShader(FullScreenShader.Get(), nullptr, 0);
    const SunConstants sun = FindSunConstants();
    ctx->UpdateSubresource(SunBuffer.Get(), 0, nullptr, &sun, 0, 0);
    ID3D11Buffer *constants[] = { ConstantsBuffer.Get(), SunBuffer.Get() };
    ctx->PSSetConstantBuffers(0, 2, constants);
    ID3D11ShaderResourceView *views[] = { OcclusionApplied ? DepthCopyView.Get() : nullptr,
        OcclusionApplied ? OcclusionViews[2].Get() : WhiteView.Get(), image, BaseLightView.Get(), LightBufferView.Get(),
        sun.Colour.w > 0 ? DepthCopyView.Get() : nullptr };
    DrawFullScreen(ctx, ScaledTarget.Get(), imageDesc.Width, imageDesc.Height, ScaleShader.Get(), views, 6);
    ctx->SwapDeviceContextState(previous.Get(), nullptr);
    return true;
}

// draws a sun shadow cascade pass with the scaled image in place of its image copy; false leaves the draw to the game
// makes HalfSunShadow the screen's size at the reduced resolution, rounded up
static bool PrepareHalfSunShadow(ID3D11DeviceContext *ctx)
{
    const float scale = FindReducedScale(shadowmaps::ScreenHeight);
    const UINT width = FindReducedSize(shadowmaps::ScreenWidth, scale);
    const UINT height = FindReducedSize(shadowmaps::ScreenHeight, scale);
    D3D11_TEXTURE2D_DESC desc = {};
    if (HalfSunShadow)
        HalfSunShadow->GetDesc(&desc);
    if (HalfSunShadow && desc.Width == width && desc.Height == height)
        return true;

    HalfSunShadow.Reset();
    HalfSunShadowView.Reset();
    HalfSunShadowTarget.Reset();
    if (width == 0 || height == 0)
        return false;
    ComPtr<ID3D11Device> dev;
    ctx->GetDevice(&dev);
    desc = { width, height, 1, 1, DXGI_FORMAT_R16G16_FLOAT, { 1, 0 }, D3D11_USAGE_DEFAULT,
        D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET };
    if (SUCCEEDED(dev->CreateTexture2D(&desc, nullptr, &HalfSunShadow))
        && SUCCEEDED(dev->CreateShaderResourceView(HalfSunShadow.Get(), nullptr, &HalfSunShadowView))
        && SUCCEEDED(dev->CreateRenderTargetView(HalfSunShadow.Get(), nullptr, &HalfSunShadowTarget)))
        return true;
    HalfSunShadow.Reset();
    HalfSunShadowView.Reset();
    HalfSunShadowTarget.Reset();
    return false;
}

// draws the pass's soft shadow into HalfSunShadow with the game's draw, at half its viewports, then puts back the
// render targets, viewports, scissor rects, blend state and pixel shader
static void DrawHalfSunShadow(command_list *cmd, ID3D11DeviceContext *ctx, int replacement, uint32_t vertices,
    uint32_t instances, uint32_t firstVertex, uint32_t firstInstance)
{
    ID3D11RenderTargetView *targets[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
    ID3D11DepthStencilView *depth = nullptr;
    ctx->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, targets, &depth);
    UINT targetCount = 0;
    for (UINT n = 0; n < D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT; ++n)
        targetCount = targets[n] ? n + 1 : targetCount;
    D3D11_VIEWPORT viewports[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE];
    UINT viewportCount = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
    ctx->RSGetViewports(&viewportCount, viewports);
    D3D11_RECT scissors[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE];
    UINT scissorCount = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
    ctx->RSGetScissorRects(&scissorCount, scissors);
    ComPtr<ID3D11BlendState> blend;
    FLOAT blendFactor[4];
    UINT sampleMask;
    ctx->OMGetBlendState(&blend, blendFactor, &sampleMask);
    ComPtr<ID3D11PixelShader> shader;
    ctx->PSGetShader(&shader, nullptr, nullptr);

    const float scale = FindReducedScale(shadowmaps::ScreenHeight);
    D3D11_VIEWPORT halfViewports[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE];
    for (UINT n = 0; n < viewportCount; ++n)
        halfViewports[n] = { viewports[n].TopLeftX / scale, viewports[n].TopLeftY / scale, viewports[n].Width / scale,
            viewports[n].Height / scale, viewports[n].MinDepth, viewports[n].MaxDepth };
    D3D11_RECT halfScissors[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE];
    for (UINT n = 0; n < scissorCount; ++n)
        halfScissors[n] = { static_cast<LONG>(std::floor(scissors[n].left / scale)),
            static_cast<LONG>(std::floor(scissors[n].top / scale)), static_cast<LONG>(std::ceil(scissors[n].right / scale)),
            static_cast<LONG>(std::ceil(scissors[n].bottom / scale)) };
    ID3D11RenderTargetView *target = HalfSunShadowTarget.Get();
    ctx->OMSetRenderTargetsAndUnorderedAccessViews(1, &target, nullptr, 0, D3D11_KEEP_UNORDERED_ACCESS_VIEWS, nullptr,
        nullptr);
    ctx->RSSetViewports(viewportCount, halfViewports);
    ctx->RSSetScissorRects(scissorCount, halfScissors);
    ctx->OMSetBlendState(nullptr, nullptr, 0xffffffff);
    ctx->PSSetShader(HalfSunShaders[replacement].Get(), nullptr, 0);
    cmd->draw(vertices, instances, firstVertex, firstInstance);

    ctx->OMSetRenderTargetsAndUnorderedAccessViews(targetCount, targets, depth, 0, D3D11_KEEP_UNORDERED_ACCESS_VIEWS,
        nullptr, nullptr);
    ctx->RSSetViewports(viewportCount, viewports);
    ctx->RSSetScissorRects(scissorCount, scissors);
    ctx->OMSetBlendState(blend.Get(), blendFactor, sampleMask);
    ctx->PSSetShader(shader.Get(), nullptr, 0);
    for (ID3D11RenderTargetView *view : targets)
        if (view)
            view->Release();
    if (depth)
        depth->Release();
}

static bool DrawSunPass(command_list *cmd, uint32_t vertices, uint32_t instances, uint32_t firstVertex,
    uint32_t firstInstance)
{
    if(!tq_world::Work.Sun())return false;
    const float softness = SunSoftness;
    const bool fixWanted = BaseCopied && CurrentTarget != LightBuffer.Get();
    if (!fixWanted && softness <= 0)
        return false;

    if ((capture::PixelClass.Bits & capture::CLASS_SUN_PASS) == 0)
        return false;
    const SunPass pass = { capture::PixelClass.SunImageSlot, capture::PixelClass.SunReplacement };
    const bool soften = softness > 0 && pass.Replacement >= 0;
    if (!fixWanted && !soften)
        return false;

    ComPtr<ID3D11DeviceContext1> ctx;
    if (FAILED(reinterpret_cast<ID3D11DeviceContext *>(cmd->get_native())->QueryInterface(IID_PPV_ARGS(&ctx))))
        return false;
    ComPtr<ID3D11Device> dev;
    ctx->GetDevice(&dev);
    if (!CreateObjects(dev.Get()))
        return false;

    ComPtr<ID3D11ShaderResourceView> image;
    if (fixWanted)
    {
        ctx->PSGetShaderResources(pass.ImageSlot, 1, &image);
        if (image && !ScaledBuilt)
            ScaledBuilt = BuildScaled(ctx.Get(), image.Get());
    }
    const bool fix = image && ScaledBuilt;
    if (!fix && !soften)
        return false;

    ComPtr<ID3D11PixelShader> gameShader;
    ComPtr<ID3D11Buffer> gameBuffer;
    UINT gameFirst = 0, gameCount = 0;
    // up to 1440 lines the soft shadow is found at full resolution in the pass itself
    const float scale = FindReducedScale(shadowmaps::ScreenHeight);
    const bool half = soften && scale > 1 && PrepareHalfSunShadow(ctx.Get());
    if (soften)
    {
        ctx->PSGetShader(&gameShader, nullptr, nullptr);
        ctx->PSGetConstantBuffers1(SOFTNESS_SLOT, 1, &gameBuffer, &gameFirst, &gameCount);
        const float constants[4] = { softness * SUN_SPREAD_PER_SOFTNESS, 0, 0, half ? scale : 0.0f };
        ctx->UpdateSubresource(SoftnessBuffer.Get(), 0, nullptr, constants, 0, 0);
        ID3D11Buffer *buffer = SoftnessBuffer.Get();
        ctx->PSSetConstantBuffers(SOFTNESS_SLOT, 1, &buffer);
        ctx->PSSetShader(SoftSunShaders[pass.Replacement].Get(), nullptr, 0);
    }
    if (fix)
    {
        ID3D11ShaderResourceView *scaled = ScaledView.Get();
        ctx->PSSetShaderResources(pass.ImageSlot, 1, &scaled);
    }
    StartTimer(ctx.Get(), SunTimer);
    ComPtr<ID3D11ShaderResourceView> gameHalfView;
    if (half)
    {
        DrawHalfSunShadow(cmd, ctx.Get(), pass.Replacement, vertices, instances, firstVertex, firstInstance);
        ctx->PSGetShaderResources(HALF_SHADOW_SLOT, 1, &gameHalfView);
        ID3D11ShaderResourceView *view = HalfSunShadowView.Get();
        ctx->PSSetShaderResources(HALF_SHADOW_SLOT, 1, &view);
    }
    cmd->draw(vertices, instances, firstVertex, firstInstance);
    StopTimer(ctx.Get(), SunTimer);
    if (half)
    {
        ID3D11ShaderResourceView *original = gameHalfView.Get();
        ctx->PSSetShaderResources(HALF_SHADOW_SLOT, 1, &original);
    }
    if (fix)
    {
        ID3D11ShaderResourceView *original = image.Get();
        ctx->PSSetShaderResources(pass.ImageSlot, 1, &original);
    }
    if (soften)
    {
        ctx->PSSetShader(gameShader.Get(), nullptr, 0);
        ID3D11Buffer *original = gameBuffer.Get();
        ctx->PSSetConstantBuffers1(SOFTNESS_SLOT, 1, &original, &gameFirst, &gameCount);
    }
    return true;
}

// follows the light buffer through the frame: finds it, and reads its camera from its first draws
// boosts the colour of the opaque world's surfaces, with its light left as it is, once the game has drawn it: before its
// first draw after the light buffer is finished that reads the colour image or draws into it without writing depth.
// That comes before the copy the sun shadow passes take of the image, so they take their shadow off the boosted image,
// and before anything transparent
static void DrawSurfaceColour(shadowmaps::BoundDraw &bound)
{
    if (!tq_world::Work.Surface()) return;
    auto *ctx = bound.Context;
    const float saturation = SurfaceSaturation;
    if (saturation <= 0 || !Processed || SurfaceDone || !LightBuffer || !LightBufferView || !DepthCopied || CreateFailed
        || CurrentTarget == LightBuffer.Get())
        return;

    ID3D11ShaderResourceView *views[8] = {};
    ctx->PSGetShaderResources(0, 8, views);
    bool readsLight = false, readsImage = false;
    for (ID3D11ShaderResourceView *view : views)
    {
        if (!view)
            continue;
        ComPtr<ID3D11Resource> resource;
        view->GetResource(&resource);
        readsLight = readsLight || resource.Get() == LightBuffer.Get();
        readsImage = readsImage || (ColourImage && resource.Get() == ColourImage);
        view->Release();
    }
    if (!ColourImage)
    {
        if (readsLight && ScreenTarget)
            ColourImage = CurrentTarget;
        return;
    }
    if (!readsImage)
    {
        if (CurrentTarget != ColourImage)
            return;
        D3D11_DEPTH_STENCIL_DESC depthDesc = { TRUE, D3D11_DEPTH_WRITE_MASK_ALL };
        if (const D3D11_DEPTH_STENCIL_DESC *depthState = bound.GetDepth())
            depthDesc = *depthState;
        if (CurrentDepth && depthDesc.DepthEnable && depthDesc.DepthWriteMask == D3D11_DEPTH_WRITE_MASK_ALL)
            return;
    }
    SurfaceDone = true;

    ComPtr<ID3D11DeviceContext1> ctx1;
    ComPtr<ID3D11Device> dev;
    ctx->GetDevice(&dev);
    ComPtr<ID3D11Texture2D> image;
    if (FAILED(ctx->QueryInterface(IID_PPV_ARGS(&ctx1))) || !CreateObjects(dev.Get())
        || FAILED(ColourImage->QueryInterface(IID_PPV_ARGS(&image))))
        return;

    D3D11_TEXTURE2D_DESC desc;
    image->GetDesc(&desc);
    const DXGI_FORMAT readFormat = GetReadFormat(desc.Format);
    if (readFormat == DXGI_FORMAT_UNKNOWN)
        return;

    D3D11_TEXTURE2D_DESC copyDesc = {};
    if (SurfaceCopy)
        SurfaceCopy->GetDesc(&copyDesc);
    if (!SurfaceCopy || copyDesc.Width != desc.Width || copyDesc.Height != desc.Height || copyDesc.Format != desc.Format)
    {
        SurfaceCopy.Reset();
        SurfaceCopyView.Reset();
        copyDesc = { desc.Width, desc.Height, 1, 1, desc.Format, { 1, 0 }, D3D11_USAGE_DEFAULT,
            D3D11_BIND_SHADER_RESOURCE };
        D3D11_SHADER_RESOURCE_VIEW_DESC viewDesc = {};
        viewDesc.Format = readFormat;
        viewDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        viewDesc.Texture2D = { 0, 1 };
        if (FAILED(dev->CreateTexture2D(&copyDesc, nullptr, &SurfaceCopy))
            || FAILED(dev->CreateShaderResourceView(SurfaceCopy.Get(), &viewDesc, &SurfaceCopyView)))
        {
            SurfaceCopy.Reset();
            SurfaceCopyView.Reset();
            return;
        }
    }
    D3D11_RENDER_TARGET_VIEW_DESC targetDesc = {};
    targetDesc.Format = readFormat;
    targetDesc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
    ComPtr<ID3D11RenderTargetView> target;
    if (FAILED(dev->CreateRenderTargetView(image.Get(), &targetDesc, &target)))
        return;

    if (capture::State == capture::Phase::Capturing && capture::Log)
        fprintf(capture::Log, "--- surface saturation %.2f applied to r%d before draw %u\n", saturation,
            capture::GetResourceId(reinterpret_cast<uint64_t>(ColourImage)), capture::DrawIndex);
    StartTimer(ctx, SurfaceTimer);
    ctx->CopyResource(SurfaceCopy.Get(), image.Get());
    const float constants[4] = { saturation, 0, 0, 0 };
    ctx->UpdateSubresource(SurfaceBuffer.Get(), 0, nullptr, constants, 0, 0);
    ComPtr<ID3DDeviceContextState> previous;
    ctx1->SwapDeviceContextState(DrawState.Get(), &previous);
    ctx1->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx1->VSSetShader(FullScreenShader.Get(), nullptr, 0);
    ID3D11Buffer *buffer = SurfaceBuffer.Get();
    ctx1->PSSetConstantBuffers(0, 1, &buffer);
    ID3D11ShaderResourceView *inputs[] = { SurfaceCopyView.Get(), LightBufferView.Get(), DepthCopyView.Get() };
    DrawFullScreen(ctx1.Get(), target.Get(), desc.Width, desc.Height, SurfaceShader.Get(), inputs, 3);
    ctx1->SwapDeviceContextState(previous.Get(), nullptr);
    StopTimer(ctx, SurfaceTimer);
}

static void OnAnyDraw(shadowmaps::BoundDraw &bound)
{
    if(!tq_world::Work.Tracking()||bound.LightBufferNoted)return;
    bound.LightBufferNoted = true;
    auto *ctx = bound.Context;
    if (!LightBuffer)
    {
        if (!FrameTargets.empty())
            FindLightBuffer(ctx);
        return;
    }
    if (CurrentTarget != LightBuffer.Get())
        return;

    // with the character window open the game draws the character's scene into the light buffer before the world's;
    // the world's pass comes last, so a draw into the finished light buffer starts a pass that replaces the one before
    if (Processed)
    {
        ++ThisFrame.Scenes;
        if (capture::State == capture::Phase::Capturing && capture::Log)
            fprintf(capture::Log, "--- draw into the finished light buffer starts another scene\n");
        StartScene();
        shadowmaps::CurrentFrame.Rows.clear();
        shadowmaps::CurrentFrame.Records.clear();
        shadowmaps::CurrentFrame.Lamp.clear();
        shadowmaps::CurrentFrame.BaseRecords = 0;
    }
    LightBufferDrawn = true;
    if (!WorldToClipKnown && (tq_world::Work.Point() || tq_world::Work.AO() || tq_world::Work.Sun()
            || (tq_world::Work.Mask() & tq_world::CaptureWork) != 0))
        ReadWorldToClip(bound);
}

// whether a draw fills only part of a screen-sized target. With the character window open the game draws the window's
// scene into the world's own targets before the world, through a viewport the window's size, with its own camera at
// the origin and its own lamps; the add-on leaves those draws alone
static bool IsPartialView(shadowmaps::BoundDraw &bound)
{
    UINT count = 0;
    const D3D11_VIEWPORT &viewport = bound.GetViewport(count);
    return ScreenTarget && count
        && (viewport.Width < shadowmaps::ScreenWidth || viewport.Height < shadowmaps::ScreenHeight);
}

// the light buffer's part of a game draw, run after shadows::HandleDraw's for the same draw
static bool HandleDraw(command_list *cmd, shadowmaps::BoundDraw &bound)
{
    if(!tq_world::Work.Tracking())return false;
    if (IsPartialView(bound))
        return false;

    const shadowmaps::DrawCall &call = bound.Call;
    if(tq_world::Work.Sun()&&call.Count&&call.Instances)CountCasterDraw(bound);
    DrawSurfaceColour(bound);
    OnAnyDraw(bound);
    if(call.Indexed||!tq_world::Work.Sun())return false;
    const uint32_t vertices = call.Count, instances = call.Instances, firstVertex = call.First,
        firstInstance = call.FirstInstance;
    auto *ctx = bound.Context;
    ComPtr<ID3D11ShaderResourceView> gameMap, gameReceivers, gameFacing;
    int keptCascade = 0;
    ReadSunTowardLight(bound);
    const bool cascade=(capture::PixelClass.Bits&capture::CLASS_SUN_PASS)!=0&&capture::PixelClass.SunReplacement>=0;
    ScopedSunConstants transaction;
    const bool owned=cascade&&transaction.Begin(ctx,true,20);
    const bool kept = owned&&EaseSunShadow(ctx, gameMap, gameReceivers, gameFacing, keptCascade);
    const bool drawn = DrawSunPass(cmd, vertices, instances, firstVertex, firstInstance);
    if (!kept)
    {
        if(owned&&!drawn)cmd->draw(vertices,instances,firstVertex,firstInstance);
        return owned||drawn;
    }

    // the pass is drawn here, so the game's cascade map and receiver depth can be put back after it
    if (!drawn)
        cmd->draw(vertices, instances, firstVertex, firstInstance);
    ID3D11ShaderResourceView *views[] = { gameReceivers.Get(), gameMap.Get(), gameFacing.Get() };
    ctx->PSSetShaderResources(RECEIVER_DEPTH_SLOT, 1, &views[0]);
    ctx->PSSetShaderResources(CASCADE_MAP_SLOT, 1, &views[1]);
    ctx->PSSetShaderResources(FACING_SLOT, 1, &views[2]);
    if (capture::State == capture::Phase::Capturing && capture::Log)
    {
        ComPtr<ID3D11Texture2D> image;
        if (CurrentTarget && SUCCEEDED(CurrentTarget->QueryInterface(IID_PPV_ARGS(&image))))
            capture::AddSnapshot(ctx, image.Get(), KEPT_PASS_SNAPSHOTS[keptCascade][1]);
    }
    return true;
}

// draws this frame's occlusion over the finished image in grey
static void DrawOcclusionImage(swapchain *chain)
{
    if (!ShowOcclusion || !OcclusionApplied)
        return;

    auto *dev = reinterpret_cast<ID3D11Device *>(chain->get_device()->get_native());
    auto *backBuffer = reinterpret_cast<ID3D11Resource *>(chain->get_current_back_buffer().handle);
    D3D11_TEXTURE2D_DESC desc;
    if (!IsScreenSized(backBuffer, desc))
        return;

    D3D11_RENDER_TARGET_VIEW_DESC targetDesc = {};
    targetDesc.Format = GetReadFormat(desc.Format);
    targetDesc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
    ComPtr<ID3D11RenderTargetView> target;
    ComPtr<ID3D11DeviceContext> ctx;
    ComPtr<ID3D11DeviceContext1> ctx1;
    dev->GetImmediateContext(&ctx);
    if (FAILED(ctx.As(&ctx1)) || FAILED(dev->CreateRenderTargetView(backBuffer,
            targetDesc.Format == DXGI_FORMAT_UNKNOWN ? nullptr : &targetDesc, &target)))
        return;

    ComPtr<ID3DDeviceContextState> previous;
    ctx1->SwapDeviceContextState(DrawState.Get(), &previous);
    ID3D11SamplerState *samplers[] = { PointClamp.Get(), LinearClamp.Get() };
    ctx1->PSSetSamplers(0, 2, samplers);
    ctx1->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx1->VSSetShader(FullScreenShader.Get(), nullptr, 0);
    ID3D11Buffer *constants = ConstantsBuffer.Get();
    ctx1->PSSetConstantBuffers(0, 1, &constants);
    ID3D11ShaderResourceView *views[] = { DepthCopyView.Get(), OcclusionViews[2].Get(), nullptr, BaseLightView.Get() };
    DrawFullScreen(ctx1.Get(), target.Get(), desc.Width, desc.Height, ShowShader.Get(), views, 4);
    ctx1->SwapDeviceContextState(previous.Get(), nullptr);
}

static void OnPresent(command_queue *, swapchain *chain, const rect *, const rect *, uint32_t, const rect *)
{
    ++SunFrame;
    if(!tq_world::Work.Tracking())return;
    const bool active=tq_world::Work.Sun();
    if(!active&&SunMasterActive)ResetSunHistory();
    SunMasterActive=active;
    const shadowmaps::CpuScope scope(shadowmaps::PresentCpu);
    if(active&&SunDirectionFix)DirectionCorrection.Advance(SunFrame,SunSeconds());
    else if(DirectionCorrection.evidence||DirectionCorrection.published)DirectionCorrection.Clear();
    if(tq_world::Work.AO())DrawOcclusionImage(chain);
    ComPtr<ID3D11DeviceContext> immediate;
    reinterpret_cast<ID3D11Device *>(chain->get_device()->get_native())->GetImmediateContext(&immediate);
    FinishTimer(immediate.Get(), OcclusionTimer);
    FinishTimer(immediate.Get(), SunTimer);
    FinishTimer(immediate.Get(), GlowTimer);
    FinishTimer(immediate.Get(), SurfaceTimer);
    FinishTimer(immediate.Get(), LampTimer);
    FinishTimer(immediate.Get(), shadowmaps::MapTimer);
    shadowmaps::TimerFrame = (shadowmaps::TimerFrame + 1) % GpuTimer::FRAMES;
    D3D11_TEXTURE2D_DESC desc;
    if (LightBuffer && ((!LightBufferDrawn && ++UndrawnFrames > LIGHT_BUFFER_LIFETIME)
        || !IsScreenSized(LightBuffer.Get(), desc)))
        ReleaseLightBuffer();
    if (LightBufferDrawn)
        UndrawnFrames = 0;
    ThisFrame.Frame = SunFrame;
    ThisFrame.LightBuffer = LightBuffer.Get();
    ThisFrame.Drawn = LightBufferDrawn;
    ThisFrame.SunKnown = SunKnown;
    ThisFrame.WorldToClipKnown = WorldToClipKnown;
    for (const CascadeMap &map : CascadeMaps)
    {
        ThisFrame.CasterDraws += map.Draws;
        ThisFrame.FullMaps += IsFull(map) ? 1 : 0;
    }
    ThisFrame.SunShadowShare = SunShadowShare;
    ThisFrame.DirectionEvidenceFrame = DirectionCorrection.evidenceFrame;
    ThisFrame.DirectionEpoch = DirectionCorrection.epoch;
    ThisFrame.DirectionWeight = static_cast<float>(DirectionCorrection.weight);
    ThisFrame.DirectionActivity = static_cast<float>(DirectionCorrection.nativeActivity);
    ThisFrame.DirectionActive = DirectionCorrection.active;
    ThisFrame.CorrectedDirection = {DirectionCorrection.direction.x, DirectionCorrection.direction.y, DirectionCorrection.direction.z};
    History[HistoryCount++ % HISTORY_FRAMES] = ThisFrame;
    ThisFrame = {};
    FrameTargets.clear();
    LightBufferDrawn = false;
    for (CascadeMap &map : CascadeMaps)
        {map.Draws = 0;map.NativeWrites=0;map.NativeWriteFrame=map.NativeWriteEpoch=0;}
    std::erase_if(MeshSunColours, [](const auto &entry)
        { return !sun_state_policy::Fresh(SunFrame,entry.second.SeenFrame,MESH_COLOUR_FRAMES); });
    StartScene(true);
}

static void SuspendScene()
{
    ReleaseLightBuffer();
    tq_sun::ResetConstants();
    CurrentTarget=nullptr;CurrentDepth=nullptr;ScreenTarget=false;
    FrameTargets.clear();LightBufferDrawn=false;SunMasterActive=false;
    shadowmaps::ProbeDepth.Reset();
    OcclusionTimer={};SunTimer={};GlowTimer={};SurfaceTimer={};LampTimer={};
    ThisFrame={};StartScene();
}

static void Register()
{
    reshade::register_event<reshade::addon_event::init_pipeline>(&tq_world::Guard<&OnInitPipeline>::Call);
    reshade::register_event<reshade::addon_event::destroy_device>(&tq_world::Guard<&OnDestroyDevice>::Call);
    reshade::register_event<reshade::addon_event::destroy_resource>(&tq_world::Guard<&OnDestroyResource>::Call);
    reshade::register_event<reshade::addon_event::bind_render_targets_and_depth_stencil>(&tq_world::WorkGuard<&OnBindRenderTargets>::Call);
    reshade::register_event<reshade::addon_event::present>(&tq_world::Guard<&OnPresent>::Call);
}
}
