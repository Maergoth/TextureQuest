// Glow lights. Glowing surfaces light the scene around them with soft shadows. The game draws a glowing mesh's glow as
// an extra pass that adds its vertex colour onto the colour target, and paints a faint glow onto nearby surfaces with
// textured additive passes that use no light position and cast no shadows. Each glowing mesh is read back from the GPU
// once and split into cells on a grid fixed to the world; cells that together cover every glowing mesh near the camera,
// smaller nearer it, become point lights with cube map shadows, drawn into the light buffer, and the painted glow
// layers retain a constant user-selected share on confirmed native world passes.
#pragma once
#include <reshade.hpp>
#include "emission_policy.hpp"
#include "painted_emission_policy.hpp"
#include <d3d11_1.h>
#include <d3d11shader.h>
#include <d3dcompiler.h>
#include <DirectXMath.h>
#include <wrl/client.h>
#include <algorithm>
#include <atomic>
#include <bit>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <mutex>
#include <queue>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include "shadow_maps.hpp"
#include "light_buffer.hpp"

namespace glow
{
using Microsoft::WRL::ComPtr;
using namespace DirectX;
using namespace reshade::api;

constexpr int CELL_SIZE_COUNT = 8;
constexpr float CELL_SIZES[CELL_SIZE_COUNT] = { 2, 4, 8, 16, 32, 64, 128, 256 };
constexpr uint64_t MEASURE_INTERVAL = 180;
constexpr UINT MEASURE_WIDTH = 256;
constexpr UINT MEASURE_HEIGHT = 128;
// the least texels of a measure a cell must cover for them to replace its glow, so a cell seen edge-on or from far away
// keeps what it had
constexpr int MIN_CELL_TEXELS = 4;
// a cell whose light is less than this share of the light of the cell it splits from gets no light
constexpr float DARK_SHARE = 0.01f;
using shadowmaps::READ_LATENCY;
// each triangle is spread over points about this far apart, so cells smaller than a triangle get their share
constexpr float POINT_SPACING = 1.0f;
constexpr float MAX_POINTS = 65536;
constexpr UINT MAX_TRIANGLES = 131072;
// objects whose bounds are read each frame, so glow lights find the meshes around them
constexpr int BOUNDS_PER_FRAME = 16;
// triangle reads in flight at once, each holding a staging buffer until it is read: BOUNDS_PER_FRAME for each frame a
// read waits, and a few over for reads the GPU has not finished. Bounds reads leave one of them, and MAX_TRIANGLES of
// SHAPE_STAGING_BYTES, for an emitter's shape
constexpr size_t SHAPE_BUFFER_COUNT = BOUNDS_PER_FRAME * READ_LATENCY + 4;
// the most the staging buffers may take together
constexpr size_t SHAPE_STAGING_BYTES = 64 << 20;
// an emitter that fits within this many units is one light at every distance
constexpr float SMALL_EMITTER_SIZE = 1.0f;
// a cell is split into smaller cells while it is bigger than this share of its distance from the camera
constexpr float SPLIT_DISTANCE_RATIO = 0.25f;
// a cell changes size only once its distance passes the split distance by this factor
constexpr float SIZE_MARGIN = 1.2f;
// a lit emitter's score is raised by this factor when there are more emitters than lights
constexpr float KEEP_MARGIN = 1.5f;
// the least distance cells are split and ranked by
constexpr float MIN_JUDGED_DISTANCE = 64;
// a cell whose glowing area is less than this share of its square is at the glow's edge
constexpr float EDGE_FILL = 0.75f;
// a light sits this far off its cell's surface, so the surface does not shadow its own light
constexpr float SURFACE_OFFSET = 0.25f;
// the least full-resolution pixels across each texel of the glow lights' pass; glow light is smooth, so half resolution
// loses little
constexpr float MIN_REDUCED_SCALE = 2;
// the most glow lights drawn in a frame, all in one pass; LIGHT_SHADER's light array holds this many
constexpr int MAX_GLOW_LIGHTS = static_cast<int>(emission_policy::GlowLimit);
static_assert(MAX_GLOW_LIGHTS == shadowmaps::MAX_LIGHTS);
// how far from the camera a glowing cell still gets a light. Glow lights are capped at MAX_GLOW_LIGHTS, so this does not
// change their cost; cells beyond it give no light or bounce at all, so this is the distance to raise if a large cave
// looks dark at its far end
constexpr float SEEN_DISTANCE = 400;
// an emitter's light reaches as far as its whole glow over the distance squared stays at least this, one step of an
// 8-bit colour, and no further than MAX_GLOW_RADIUS
constexpr float GLOW_CUTOFF = 1.0f / 255;
constexpr float MAX_GLOW_RADIUS = 200;
// an emitter whose light reaches less than this gets no lights
constexpr float MIN_GLOW_RADIUS = 0.5f;
// frames an emitter keeps its shown lights while a newly picked set's maps are drawn
constexpr uint64_t HANDOVER_FRAMES = 60;
// how many times a light's reach its bounce light reaches, so the bounce from a lava lake fills the cave around it
constexpr float BOUNCE_REACH = 2.5f;

constexpr const char *FULL_SCREEN_SHADER = R"(
float4 main(uint id : SV_VertexID) : SV_Position
{
    float2 uv = float2((id << 1) & 2, id & 2);
    return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
}
)";

// adds the glow lights to the light buffer, each the light from a patch of glowing surface with a soft shadow from its
// cube map, found at reduced resolution by ps_reduced_light and upsampled by ps_upsample
constexpr const char *LIGHT_SHADER = R"(
cbuffer Frame : register(b0)
{
    // the inverse of the camera-relative world-to-clip transform, as rows
    float4 InverseRows[4];
    // the light buffer's width and height and their inverses
    float4 Size;
    // x resolution scale, y material normals, z point softness spread, w legacy scale
    float4 Reduced;
};
struct GlowLight
{
    // xyz the light's absolute position; w the map-array tier, 0 to 3
    float4 Position;
    // rgb the glow's colour times the cell's area over pi; w the light's reach
    float4 Colour;
    // xyz the cell's normal, zero when it faces every way; w the cube's place in its array
    float4 Normal;
    // x 1 when the light's moving-object map is drawn, y the cell's area over pi, z that cube's place in its array, w the
    // reach of its bounce light
    float4 Moving;
    // x the far plane of its maps; y 1 for an independent particle light
    float4 Shadow;
};
cbuffer GlowLights : register(b1)
{
    // x light count, y near plane, z shadow strength, w bypass shadows for diagnostics
    float4 Counts;
    // xyz the face size of each map size, w the moving-object maps' face size
    float4 FaceSizes;
    // MAX_GLOW_LIGHTS
    GlowLight Lights[512];
};
// this frame's camera offset
cbuffer Camera : register(b3) { float4 CameraOffset; };
Texture2D<float> Depth : register(t0);
TextureCubeArray<float> Maps0 : register(t1);
TextureCubeArray<float> Maps1 : register(t2);
TextureCubeArray<float> Maps2 : register(t3);
TextureCubeArray<float> MovingMaps : register(t4);
// rgb the glow lights' light at reduced resolution, a the camera distance it was found at
Texture2D<float4> ReducedLight : register(t5);
// xyz the normal of the surface ReducedLight was found on, w the painted glow mask, at reduced resolution
Texture2D<float4> ReducedSurface : register(t6);
// Optional current-camera native material/vertex normals, RGB encoded 0..1 and coverage in alpha.
Texture2D<float4> MaterialNormals : register(t7);
// Extra same-resolution far maps preserve all existing depth/material bindings.
TextureCubeArray<float> Maps3 : register(t8);
TextureCubeArray<float> ParticleMaps0 : register(t9);
TextureCubeArray<float> ParticleMaps1 : register(t10);
TextureCubeArray<float> ParticleMaps2 : register(t11);
TextureCubeArray<float> ParticleMaps3 : register(t12);
)"
R"(
SamplerComparisonState Compare : register(s0);
SamplerState ShadowPoint : register(s1);

static const float2 POISSON[16] = {
    float2(-0.94201624, -0.39906216), float2(0.94558609, -0.76890725), float2(-0.09418410, -0.92938870),
    float2(0.34495938, 0.29387760), float2(-0.91588581, 0.45771432), float2(-0.81544232, -0.87912464),
    float2(-0.38277543, 0.27676845), float2(0.97484398, 0.75648379), float2(0.44323325, -0.97511554),
    float2(0.53742981, -0.47373420), float2(-0.26496911, -0.41893023), float2(0.79197514, 0.19090188),
    float2(-0.24188840, 0.99706507), float2(-0.81409955, 0.91437590), float2(0.19984126, 0.78641367),
    float2(0.14383161, -0.14100790) };
static const float2 SQUARE[4] = { float2(-0.5, -0.5), float2(0.5, -0.5), float2(-0.5, 0.5), float2(0.5, 0.5) };
// a light adding less than this to the light buffer is left out
static const float CUTOFF = 1.0 / 2048;
// a light with less than this share of the pixel's glow light gets the 4-tap shadow filter
static const float FULL_FILTER_SHARE = 0.1;
// a light with less than this share of the pixel's glow light takes the lit share of the others in place of a shadow
// lookup of its own
static const float MIN_SHADOW_SHARE = 1.0 / 128;
// the width of a shadow's soft edge, in texels of a 256 face, so it spans the same angle in every map size
static const float FILTER_TEXELS = 10;
// the outer share of a light's reach over which the painted glow mask fades out
static const float MASK_FADE = 0.25;
// the share of a light's light that comes back off the surfaces around it as soft bounce light. In a closed space light
// keeps bouncing between the walls, which evens it out: rock reflecting about 0.3 gives 0.3 / (1 - 0.3), about 0.43 of the
// walls' direct light, spread evenly over walls of about four times the glow's area
static const float BOUNCE = 0.2;
// the glow light's brightest channel is eased toward this, the light buffer's 1 for a surface's full colour, so surfaces
// beside the glow do not clip while the strength is raised for far ones
static const float GLOW_LIMIT = 1;
// the bounce light's falloff is softened as if it came from this far around the light, the scale of a cave around a
// lava lake, so it is nearly even across the cave
static const float BOUNCE_SOFTENING = 150;
// the share of bounce light a surface facing straight away from the light still gets
static const float BOUNCE_BACK = 0.25;

float DeviceDepth(float along, float nearPlane, float farPlane)
{
    return farPlane / (farPlane - nearPlane) - farPlane * nearPlane / ((farPlane - nearPlane) * along);
}

// the lit share of 16 or 4 taps around a direction, spread over the soft edge's width
float LinearShadowDepth(float depth,float nearPlane,float farPlane)
{return nearPlane*farPlane/(farPlane-depth*(farPlane-nearPlane));}

// Same contact-hardening world/angular softness contract as native point lamps.
// Weak lights retain the four-tap quality tier, with the same blocker/penumbra math.
float FilterShadow(TextureCubeArray<float> maps,float3 axis,float3 tangent,float3 bitangent,float2x2 rotation,
    float layer,float reference,bool full,float along,float farPlane)
{
    float3 direction=normalize(axis);float spread=max(Reduced.z,0);
    if(spread<=0)return maps.SampleCmpLevelZero(Compare,float4(direction,layer),reference);
    float searchRadius=Reduced.w>.5?min(spread*2/along,.2):min(spread*20,1)/along;
    float blockerSum=0,blockerCount=0;int count=full?16:4;
    [loop]for(int n=0;n<count;++n)
    {
        float2 sample=full?POISSON[n]:SQUARE[n];float2 offset=mul(rotation,sample)*searchRadius;
        float blocker=LinearShadowDepth(maps.SampleLevel(ShadowPoint,float4(direction+tangent*offset.x+bitangent*offset.y,layer),0),Counts.y,farPlane);
        if(blocker<along){blockerSum+=blocker;blockerCount+=1;}
    }
    if(blockerCount==0)return 1;
    float distance=blockerSum/blockerCount;
    float radius=Reduced.w>.5?clamp(spread*(along-distance)/(distance*along),.0015,.15)
        :max(min(spread*(along-distance),1)/along,.0015);
    float lit=0;[loop]for(int i=0;i<count;++i)
    {
        float2 sample=full?POISSON[i]:SQUARE[i];float2 offset=mul(rotation,sample)*radius;
        lit+=maps.SampleCmpLevelZero(Compare,float4(direction+tangent*offset.x+bitangent*offset.y,layer),reference);
    }
    return lit/count;
}

float FilterLightShadow(float size, float3 axis, float3 tangent, float3 bitangent, float2x2 rotation, float layer,
    float reference, bool full,float along,float farPlane)
{
    if (size < 0.5)
        return FilterShadow(Maps0, axis, tangent, bitangent, rotation, layer, reference, full,along,farPlane);
    if (size < 1.5)
        return FilterShadow(Maps1, axis, tangent, bitangent, rotation, layer, reference, full,along,farPlane);
    if (size < 2.5)
        return FilterShadow(Maps2, axis, tangent, bitangent, rotation, layer, reference, full,along,farPlane);
    return FilterShadow(Maps3, axis, tangent, bitangent, rotation, layer, reference, full,along,farPlane);
}

float FilterParticleShadow(float bank,float3 axis,float3 tangent,float3 bitangent,float2x2 rotation,float cube,float reference,bool full,float along,float farPlane)
{
    if(bank<.5)return FilterShadow(ParticleMaps0,axis,tangent,bitangent,rotation,cube,reference,full,along,farPlane);
    if(bank<1.5)return FilterShadow(ParticleMaps1,axis,tangent,bitangent,rotation,cube,reference,full,along,farPlane);
    if(bank<2.5)return FilterShadow(ParticleMaps2,axis,tangent,bitangent,rotation,cube,reference,full,along,farPlane);
    return FilterShadow(ParticleMaps3,axis,tangent,bitangent,rotation,cube,reference,full,along,farPlane);
}

// what a light adds to a surface before its shadow, and how far inside its reach the surface is, 1 well inside
float3 FindContribution(GlowLight light, float3 world, float3 normal, out float inside)
{
    inside = 0;
    float3 toLight = light.Position.xyz - world;
    float distanceSquared = dot(toLight, toLight);
    float lightDistance = sqrt(distanceSquared);
    if (lightDistance >= light.Colour.w)
        return 0;

    float3 direction = toLight / max(lightDistance, 1e-4);
    float surfaceAngle = saturate(dot(normal, direction));
    float emitterAngle = dot(light.Normal.xyz, light.Normal.xyz) > 0 ? saturate(-dot(light.Normal.xyz, direction)) : 1;
    if (surfaceAngle * emitterAngle <= 0)
        return 0;

    inside = saturate((light.Colour.w - lightDistance) / (MASK_FADE * light.Colour.w));
    // a glowing disc of the cell's area, which stays as bright as the glow itself up close, faded to nothing at its reach
    float reachShare = distanceSquared / (light.Colour.w * light.Colour.w);
    float window = saturate(1 - reachShare * reachShare);
    return light.Colour.rgb * surfaceAngle * emitterAngle * window * window / (distanceSquared + light.Moving.y);
}

// the light one bounce off the surfaces around a light adds, without a shadow: soft, reaching BOUNCE_REACH times as far
// as the light, and reaching surfaces facing away from it too
float3 FindBounce(GlowLight light, float3 world, float3 normal)
{
    float3 toLight = light.Position.xyz - world;
    float distanceSquared = dot(toLight, toLight);
    float reachShare = distanceSquared / (light.Moving.w * light.Moving.w);
    if (reachShare >= 1)
        return 0;

    float facing = lerp(BOUNCE_BACK, 1, 0.5 + 0.5 * dot(normal, toLight) * rsqrt(max(distanceSquared, 1e-8)));
    float window = 1 - reachShare * reachShare;
    return light.Colour.rgb * BOUNCE * facing * window * window
        / (distanceSquared + BOUNCE_SOFTENING * BOUNCE_SOFTENING);
}

struct Output
{
    float4 Light : SV_Target0;
    // how much a glow light reaches the pixel, which dims the game's painted glow there
    float Mask : SV_Target1;
};

// the surface at a full-resolution pixel: its world position, its normal facing the camera, and its distance from the
// camera; false for the sky and where the neighbouring pixels give no normal
bool FindSurface(float2 pixel, out float3 world, out float3 normal, out float cameraDistance)
{
    float depth = Depth.Load(int3(pixel, 0));
    float2 uv = pixel * Size.zw;
    float4 clip = float4(uv.x * 2 - 1, 1 - uv.y * 2, depth, 1);
    float4 relative = float4(dot(InverseRows[0], clip), dot(InverseRows[1], clip), dot(InverseRows[2], clip),
        dot(InverseRows[3], clip));
    float3 offsetPosition = relative.xyz / relative.w;
    normal = cross(ddx(offsetPosition), ddy(offsetPosition));
    world = offsetPosition - CameraOffset.xyz;
    cameraDistance = length(offsetPosition);
    if (depth >= 1 || dot(normal, normal) < 1e-20)
        return false;

    normal = normalize(normal);
    // Only the geometric depth fallback needs camera-facing orientation.
    if (dot(normal, offsetPosition) > 0) normal = -normal;
    if (Reduced.y>0.5)
    {
        float4 stored=MaterialNormals.Load(int3(pixel,0));
        float3 material=stored.xyz*2-1;
        if (stored.w>0.5 && all(isfinite(material)) && dot(material,material)>1e-6)
            normal=normalize(material);
    }
    // Valid native material normals retain their world/TBN orientation.
    return true;
}

// the brightest channel of every light's contribution before its shadow, summed, in mask how far inside the reach of
// any light the surface is, and in bounce the lights' bounce light
float FindTotal(float3 world, float3 normal, out float mask, out float3 bounce)
{
    float total = 0;
    mask = 0;
    bounce = 0;
    [loop]
    for (uint i = 0; i < (uint)Counts.x; ++i)
    {
        float inside;
        float3 contribution = FindContribution(Lights[i], world, normal, inside);
        total += max(contribution.r, max(contribution.g, contribution.b));
        mask = max(mask, inside);
        bounce += FindBounce(Lights[i], world, normal);
    }
    return total;
}

// the light the glow lights add with their shadows, and in unshadowed the same lights without their shadows
float3 FindShadowedLight(float2 position, float3 world, float3 normal, float total, out float3 unshadowed)
{
    float angle = 6.2831853 * frac(52.9829189 * frac(dot(position, float2(0.06711056, 0.00583715))));
    float2x2 rotation = float2x2(cos(angle), -sin(angle), sin(angle), cos(angle));
    float3 sum = 0;
    float3 skipped = 0;
    unshadowed = 0;
    [loop]
    for (uint j = 0; j < (uint)Counts.x; ++j)
    {
        GlowLight light = Lights[j];
        float inside;
        float3 contribution = FindContribution(light, world, normal, inside);
        float strength = max(contribution.r, max(contribution.g, contribution.b));
        if (strength < CUTOFF)
            continue;
        // Particle shadows must not disappear as unrelated lights increase.
        // Keep the existing aggregate shortcut only for object surface lights.
        if (light.Shadow.y < 0.5 && strength < MIN_SHADOW_SHARE * total)
        {
            skipped += contribution;
            continue;
        }

        // compare from about a texel of the coarser map off the surface, so it does not shadow itself
        float faceSize = FaceSizes[min((uint)light.Position.w, 2u)];
        if(light.Shadow.z>0)faceSize=min(faceSize,256);
        if (light.Moving.x > 0)
            faceSize = min(faceSize, FaceSizes.w);
        float lightDistance = distance(light.Position.xyz, world);
        float texel = 2 * lightDistance / faceSize;
        float3 toPixel = world + normal * texel * 1.5 - light.Position.xyz;
        float3 axes = abs(toPixel);
        float along = max(axes.x, max(axes.y, axes.z));
        float lit = 1;
        if (along < light.Shadow.x && Counts.w <= 0 && Counts.z > 0)
        {
            float reference = DeviceDepth(max(along * 0.985 - 0.02, Counts.y), Counts.y, light.Shadow.x);
            float3 axis = toPixel / along;
            float3 up = abs(axis.y) < 0.99 * length(axis) ? float3(0, 1, 0) : float3(1, 0, 0);
            float3 tangent = normalize(cross(up, axis));
            float3 bitangent = normalize(cross(axis, tangent));
            bool full = strength >= FULL_FILTER_SHARE * total;
            lit = FilterLightShadow(light.Position.w, axis, tangent, bitangent, rotation, light.Normal.w, reference, full,max(along*.985-.02,Counts.y),light.Shadow.x);
            if (light.Moving.x > 0)
                lit = min(lit, FilterShadow(MovingMaps, axis, tangent, bitangent, rotation, light.Moving.z, reference, full,max(along*.985-.02,Counts.y),light.Shadow.x));
            if(light.Shadow.z>0)lit=min(lit,FilterParticleShadow(light.Shadow.w,axis,tangent,bitangent,rotation,light.Shadow.z-1,reference,full,max(along*.985-.02,Counts.y),light.Shadow.x));
        }
        lit=1-saturate(Counts.z)*(1-lit);
        sum += contribution * lit;
        unshadowed += contribution;
    }
    sum += skipped * (unshadowed > 0 ? sum / max(unshadowed, 1e-30) : 1);
    unshadowed += skipped;
    return sum;
}

struct ReducedOutput
{
    // rgb the glow lights' light with their shadows and bounce, a the camera distance it was found at
    float4 Light : SV_Target0;
    // xyz the surface's normal, w the mask
    float4 Surface : SV_Target1;
};

// the glow lights with their shadows and bounce at reduced resolution, with the surface they were found on, and the mask
ReducedOutput ps_reduced_light(float4 position : SV_Position)
{
    float3 world, normal;
    float cameraDistance;
    if (!FindSurface(floor(position.xy * Reduced.x) + 0.5, world, normal, cameraDistance))
        discard;

    float mask;
    float3 bounce;
    float total = FindTotal(world, normal, mask, bounce);
    ReducedOutput output;
    output.Light = float4(bounce, cameraDistance);
    [branch]
    if (mask > 0)
    {
        float3 unshadowed;
        output.Light.rgb += FindShadowedLight(position.xy, world, normal, total, unshadowed);
    }
    output.Light.rgb /= 1 + max(output.Light.r, max(output.Light.g, output.Light.b)) / GLOW_LIMIT;
    output.Surface = float4(normal, mask);
    return output;
}

// the light (rgb) and mask (a) upsampled from ReducedLight and ReducedSurface: the four nearest reduced-resolution
// texels, weighted bilinearly, by how near their camera distance is to the pixel's and by how near their normal is to
// its, or the nearest in distance where none is near
float4 FindUpsampledLight(float2 pixel, float cameraDistance, float3 normal)
{
    uint width, height;
    ReducedLight.GetDimensions(width, height);
    float2 position = pixel / Reduced.x - 0.5;
    int2 corner = (int2)floor(position);
    float2 fraction = position - corner;
    float4 sum = 0;
    float total = 0;
    float4 nearest = 0;
    float nearestDifference = 1e30;
    [unroll]
    for (int n = 0; n < 4; ++n)
    {
        int2 offset = int2(n & 1, n >> 1);
        int3 place = int3(clamp(corner + offset, 0, int2(width, height) - 1), 0);
        float4 texel = ReducedLight.Load(place);
        float4 surface = ReducedSurface.Load(place);
        float4 value = float4(texel.rgb, surface.w);
        float2 bilinear = offset == 1 ? fraction : 1 - fraction;
        float difference = abs(cameraDistance - texel.w);
        float facing = saturate(dot(normal, surface.xyz));
        facing *= facing;
        facing *= facing;
        float weight = bilinear.x * bilinear.y * exp(-difference / max(0.02 * cameraDistance, 0.0001)) * facing * facing;
        sum += value * weight;
        total += weight;
        if (difference < nearestDifference)
        {
            nearestDifference = difference;
            nearest = value;
        }
    }
    return total > 0.00001 ? sum / total : nearest;
}

// the glow lights and mask at full resolution, upsampled from ps_reduced_light's
Output ps_upsample(float4 position : SV_Position)
{
    float3 world, normal;
    float cameraDistance;
    if (!FindSurface(position.xy, world, normal, cameraDistance))
        discard;

    float4 light = FindUpsampledLight(position.xy, cameraDistance, normal);
    if (!any(light))
        discard;

    Output output;
    output.Light = float4(light.rgb, 0);
    output.Mask = light.a;
    return output;
}
)";

// Adds the known painted-world pass back with its constant retained share.
constexpr const char *COMPOSITE_SHADER = R"(
// x is the retained share, independent of optional world-light measurement.
cbuffer Composite : register(b0) { float4 Kept; };
Texture2D<float4> Painted : register(t0);

float4 main(float4 position : SV_Position) : SV_Target
{
    int3 pixel = int3(position.xy, 0);
    float4 painted = Painted.Load(pixel);
    return float4(painted.rgb * Kept.x, painted.a);
}
)";

// writes a mesh's triangles in world positions to a stream output buffer
constexpr const char *SHAPE_SHADER = R"(
cbuffer Caster : register(b0) { float4 World[3]; };

float3 main(float3 position : POSITION) : WORLD
{
    float4 p = float4(position, 1);
    return float3(dot(p, World[0]), dot(p, World[1]), dot(p, World[2]));
}
)";

enum class AddedPass { None, Emitter, Painted };

struct Cell
{
    int X, Y, Z;
    float Area;
    XMFLOAT3 Centre;
    // the unit area-weighted normal, or zero when the cell's surface faces every way
    XMFLOAT3 Normal;
    float Radius;
    // the glow's average colour over the cell, in the colour target's units; w is 1 once measured
    XMFLOAT4 Radiance;
};

struct Emitter
{
    uint64_t LastSeen = 0;
    uint64_t Ticket = 0;
    // the glow's average colour where the last measure saw it, in the colour target's units; w is 1 once measured. Cells
    // not measured yet take it
    XMFLOAT4 Radiance = {};
    uint64_t MeasuredFrame = 0;
    bool ShapeRequested = false;
    bool ShapeKnown = false;
    XMFLOAT3 Centre = {};
    // the diagonal of the emitter's bounds
    float Size = 0;
    Cell Whole = {};
    // cells at each of CELL_SIZES, keyed by their grid coordinates
    std::unordered_map<uint64_t, Cell> Cells[CELL_SIZE_COUNT];
    // the light keys drawn for the emitter, kept until all of a newly picked set have maps
    std::vector<uint64_t> ShownLights;
    // the frame the emitter began waiting for a picked set's maps, or 0
    uint64_t HandoverStart = 0;
    // set when its glow is a painted pass, whose light is scaled by PaintedLight
    bool Painted = false;
    // how far its light reaches, from FindEmitterRadius; every one of its cells' lights reaches as far
    float Radius = 0;
};

struct ShapeRead
{
    // the object's caster key, or the mesh key of a moving mesh in world positions
    uint64_t CasterKey;
    ComPtr<ID3D11Buffer> Staging;
    UINT Triangles;
    uint64_t Frame;
    // set when only the object's bounds are read
    bool BoundsOnly;
    // set when the bounds read are a moving mesh's, for shadowmaps::WorldMeshes
    bool WorldMesh;
};

// a cell's sums while its points are added
struct CellSum
{
    int X, Y, Z;
    double Area;
    double Centre[3];
    double Normal[3];
    float Low[3], High[3];
};

struct FrameConstants
{
    XMFLOAT4X4 InverseRows;
    XMFLOAT4 Size, Reduced;
};

struct GlowLightConstants
{
    XMFLOAT4 Position, Colour, Normal, Moving, Shadow;
};

struct LightConstants
{
    XMFLOAT4 Counts, FaceSizes;
    GlowLightConstants Lights[MAX_GLOW_LIGHTS];
};
static_assert(sizeof(LightConstants) == 32 + sizeof(GlowLightConstants) * emission_policy::GlowLimit);
static_assert(sizeof(LightConstants) <= D3D11_REQ_CONSTANT_BUFFER_ELEMENT_COUNT * 16);

// a cell in the cut through the emitters' cells that become this frame's lights
struct Candidate
{
    // how far the cell is past its split distance, over 1 when it splits
    float Need;
    uint64_t CasterKey;
    uint64_t CellKey;
    const Cell *Source;
    // the cell's place in CELL_SIZES, or CELL_SIZE_COUNT for the whole emitter
    int Size;
};

static std::atomic<bool> Enabled = false;
// Saved choices; the existing world/master gate determines effective work.
static std::atomic<bool> ObjectsEnabled = true, ParticlesEnabled = true;
static bool ObjectsActive(){return Enabled && ObjectsEnabled && tq_world::Work.Point();}
static bool ParticlesActive(){return Enabled && ParticlesEnabled && tq_world::Work.Point();}
static bool SourcesActive(){return ObjectsActive() || ParticlesActive();}
static std::atomic<int> LightLimit = emission_policy::DefaultGlowLimit;
static std::atomic<int> ParticleLightLimit = emission_policy::DefaultParticleLimit;
static std::atomic<float> Strength = 1.0f;
// the constant share kept for confirmed native painted-world passes
static std::atomic<float> PaintedKept = 0.5f;
// what a painted glow's light is scaled by, against an emitter pass's; at 0 painted glow gives no light
static std::atomic<float> PaintedLight = 0.25f;
static std::mutex Lock;
// the texture slots each pixel shader declares, one bit per slot, by pipeline handle
static std::unordered_map<uint64_t, uint64_t> ShaderTextures;
static std::unordered_map<uint64_t, uint64_t> PaintedVertexHashes, PaintedPixelHashes;
// glowing objects by caster key
static std::unordered_map<uint64_t, Emitter> Emitters;
// the measure draw's colour and depth, each copied whole for reading
static ComPtr<ID3D11Texture2D> MeasureTexture, MeasureDepth, MeasureReadback[READ_LATENCY],
    MeasureDepthReadback[READ_LATENCY];
static ComPtr<ID3D11RenderTargetView> MeasureTarget;
static ComPtr<ID3D11DepthStencilView> MeasureDepthTarget;
static ComPtr<ID3D11DepthStencilState> MeasureDepthState;
static ComPtr<ID3D11BlendState> MeasureBlend;
// the emitter measured into each readback texture, or 0, with its frame's clip-to-camera-relative transform as rows and
// camera offset, known when the frame's world-to-clip transform was
static uint64_t MeasuredKeys[READ_LATENCY];
static uint64_t MeasuredTickets[READ_LATENCY], NextTicket=1;
static bool MeasuredPainted[READ_LATENCY];
static unsigned ParticleSlots=0;
static bool ParticleAllocationReady=false;
static XMFLOAT4X4 MeasuredClipToWorld[READ_LATENCY];
static XMFLOAT3 MeasuredOffset[READ_LATENCY];
static bool MeasuredPlaced[READ_LATENCY];
static bool MeasuredThisFrame;
static std::vector<ShapeRead> ShapeReads;
// a staging buffer a mesh's triangles are copied into for reading, free while no ShapeRead holds it, and its size
struct ShapeStaging
{
    ComPtr<ID3D11Buffer> Buffer;
    UINT Bytes = 0;
};
static ShapeStaging ShapeStagingPool[SHAPE_BUFFER_COUNT];
// the stream output buffer every mesh's triangles are drawn into before their copy to staging, which the GPU finishes
// before the next draw into it, and MAX_TRIANGLES of zeros copied over it before each draw
static ComPtr<ID3D11Buffer> ShapeOutput, ShapeZeros;
// set when ShapeOutput and ShapeZeros could not be made, so the zeros are not built again every frame until Reset
static bool ShapeBuffersFailed;
static ComPtr<ID3D11VertexShader> ShapeVertexShader;
static ComPtr<ID3D11GeometryShader> ShapeStreamShader;
// the light key of each cell light, by cell
static std::unordered_map<uint64_t, uint64_t> CellLights;
// the cells lit last frame, which split and merge with a margin
static std::unordered_set<uint64_t> ActiveCells;
static ComPtr<ID3D11VertexShader> LightVertexShader;
static ComPtr<ID3D11PixelShader> ReducedLightShader, UpsampleShader;
static ComPtr<ID3D11BlendState> LightBlend, PaintedCompositeBlend;
static ComPtr<ID3D11RasterizerState> LightRasterizer;
static ComPtr<ID3D11Buffer> FrameBuffer, LightConstantsBuffer;
// how much glow lights reach each pixel this frame, written with the lights
static ComPtr<ID3D11Texture2D> MaskTexture;
static ComPtr<ID3D11RenderTargetView> MaskTarget;
static ComPtr<ID3D11ShaderResourceView> MaskView;
static uint64_t MaskFrame = UINT64_MAX;
// the glow lights' light with its camera distance, and the surface's normal with the mask, at reduced resolution
static ComPtr<ID3D11Texture2D> ReducedLightTexture, ReducedSurfaceTexture;
static ComPtr<ID3D11RenderTargetView> ReducedLightTarget, ReducedSurfaceTarget;
static ComPtr<ID3D11ShaderResourceView> ReducedLightView, ReducedSurfaceView;
// qualified painted glow passes drawn aside until they are added back
static ComPtr<ID3D11Texture2D> PaintedTexture;
static ComPtr<ID3D11RenderTargetView> PaintedTarget;
static ComPtr<ID3D11ShaderResourceView> PaintedView;
static ComPtr<ID3D11RenderTargetView> PaintedDestination;
static ComPtr<ID3D11PixelShader> CompositePixelShader;
static ComPtr<ID3D11Buffer> CompositeBuffer;
static int LightsDrawnThisFrame, LightsDrawnLastFrame, CellsPickedLastFrame;
static bool CreateFailed;
static int PaintedSkippedThisSecond, PaintedSkippedLastSecond;

// records the texture slots a pixel shader declares
static void ClassifyPixelShader(const void *code, size_t size, uint64_t handle)
{
    ComPtr<ID3D11ShaderReflection> reflection;
    if (FAILED(D3DReflect(code, size, IID_PPV_ARGS(&reflection))))
        return;

    D3D11_SHADER_DESC desc;
    reflection->GetDesc(&desc);
    uint64_t textures = 0;
    for (UINT i = 0; i < desc.BoundResources; ++i)
    {
        D3D11_SHADER_INPUT_BIND_DESC bind;
        if (SUCCEEDED(reflection->GetResourceBindingDesc(i, &bind)) && bind.Type == D3D_SIT_TEXTURE && bind.BindPoint < 64)
            textures |= 1ull << bind.BindPoint;
    }
    std::lock_guard guard(Lock);
    ShaderTextures[handle] = textures;
    PaintedPixelHashes[handle] = capture::HashCode(code, size);
}

static void ClassifyPaintedVertex(uint64_t hash, uint64_t handle)
{
    std::lock_guard guard(Lock);
    PaintedVertexHashes.erase(handle);
    if (painted_emission_policy::Vertex(hash)) PaintedVertexHashes[handle] = hash;
}

static void ForgetPixelShader(uint64_t handle)
{
    std::lock_guard guard(Lock);
    ShaderTextures.erase(handle);
    PaintedPixelHashes.erase(handle);
    PaintedVertexHashes.erase(handle);
}

static void ForgetPaintedDevice()
{
    std::lock_guard guard(Lock);
    ShaderTextures.clear();
    PaintedPixelHashes.clear();
    PaintedVertexHashes.clear();
}

// whether the bound state adds the draw's colour onto its target (blend one, one) with depth write on
static bool IsAddingWithDepth(shadowmaps::BoundDraw &bound)
{
    const D3D11_BLEND_DESC *blend = bound.GetBlend();
    const D3D11_DEPTH_STENCIL_DESC *depth = bound.GetDepth();
    if (!blend || !depth)
        return false;

    const D3D11_RENDER_TARGET_BLEND_DESC &target = blend->RenderTarget[0];
    return target.BlendEnable && target.SrcBlend == D3D11_BLEND_ONE && target.DestBlend == D3D11_BLEND_ONE
        && depth->DepthEnable && depth->DepthWriteMask == D3D11_DEPTH_WRITE_MASK_ALL;
}

// an emitter pass adds a glow that declares no textures; a painted pass adds textured glow without the light buffer
static AddedPass GetAddedPass(shadowmaps::BoundDraw &bound, uint64_t pixelShader)
{
    auto *ctx = bound.Context;
    uint64_t textures = 0;
    {
        std::lock_guard guard(Lock);
        auto it = ShaderTextures.find(pixelShader);
        if (it == ShaderTextures.end())
            return AddedPass::None;
        textures = it->second;
    }
    if (!IsAddingWithDepth(bound))
        return AddedPass::None;
    if (!textures)
        return AddedPass::Emitter;

    for (UINT slot = 0; slot < 64; ++slot)
    {
        if (!(textures >> slot & 1))
            continue;

        ComPtr<ID3D11ShaderResourceView> view;
        ctx->PSGetShaderResources(slot, 1, &view);
        ComPtr<ID3D11Resource> resource;
        if (view)
            view->GetResource(&resource);
        if (resource && resource.Get() == lightbuffer::LightBuffer.Get())
            return AddedPass::None;
    }
    return AddedPass::Painted;
}

// Exact native pair and reproducible additive coverage only. Never infer a world
// pass from a texture, camera transform, or a similar-looking shader alone.
static bool IsQualifiedPaintedWorld(ID3D11DeviceContext *ctx, uint64_t pixelShader)
{
    ComPtr<ID3D11VertexShader> vertex;
    ComPtr<ID3D11PixelShader> pixel;
    UINT vertexClasses = 0, pixelClasses = 0;
    ctx->VSGetShader(&vertex, nullptr, &vertexClasses);
    ctx->PSGetShader(&pixel, nullptr, &pixelClasses);
    if (!vertex || !pixel || vertexClasses || pixelClasses
        || reinterpret_cast<uint64_t>(pixel.Get()) != pixelShader) return false;
    {
        std::lock_guard guard(Lock);
        const auto v = PaintedVertexHashes.find(reinterpret_cast<uint64_t>(vertex.Get()));
        const auto p = PaintedPixelHashes.find(pixelShader);
        if (v == PaintedVertexHashes.end() || p == PaintedPixelHashes.end()
            || !painted_emission_policy::Match(v->second, p->second)) return false;
    }
    ComPtr<ID3D11GeometryShader> geometry;
    ComPtr<ID3D11HullShader> hull;
    ComPtr<ID3D11DomainShader> domain;
    ctx->GSGetShader(&geometry, nullptr, nullptr);
    ctx->HSGetShader(&hull, nullptr, nullptr);
    ctx->DSGetShader(&domain, nullptr, nullptr);
    if (geometry || hull || domain) return false;
    ID3D11RenderTargetView *targets[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
    ctx->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, targets, nullptr);
    bool soleTarget = targets[0] != nullptr;
    for (UINT i = 0; i < D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT; ++i)
        if (targets[i]) { if (i) soleTarget = false; targets[i]->Release(); }
    if (!soleTarget) return false;
    ComPtr<ID3D11BlendState> blend;
    UINT sampleMask = 0;
    ctx->OMGetBlendState(&blend, nullptr, &sampleMask);
    if (!blend || sampleMask != 0xffffffff) return false;
    D3D11_BLEND_DESC desc;
    blend->GetDesc(&desc);
    const auto &target = desc.RenderTarget[0];
    const UINT rgb = D3D11_COLOR_WRITE_ENABLE_RED | D3D11_COLOR_WRITE_ENABLE_GREEN | D3D11_COLOR_WRITE_ENABLE_BLUE;
    return !desc.AlphaToCoverageEnable && !desc.IndependentBlendEnable
        && target.BlendEnable && target.SrcBlend == D3D11_BLEND_ONE && target.DestBlend == D3D11_BLEND_ONE
        && target.BlendOp == D3D11_BLEND_OP_ADD && (target.RenderTargetWriteMask & rgb) == rgb
        && target.DestBlendAlpha == D3D11_BLEND_ONE && target.BlendOpAlpha == D3D11_BLEND_OP_ADD
        && (target.SrcBlendAlpha == D3D11_BLEND_ONE || target.SrcBlendAlpha == D3D11_BLEND_ZERO);
}

static bool CreateMeasureObjects(ID3D11Device *dev)
{
    if (MeasureTarget && MeasureBlend && MeasureDepthState) return true;
    const D3D11_TEXTURE2D_DESC desc = { MEASURE_WIDTH, MEASURE_HEIGHT, 1, 1, DXGI_FORMAT_R32G32B32A32_FLOAT, { 1, 0 },
        D3D11_USAGE_DEFAULT, D3D11_BIND_RENDER_TARGET };
    const D3D11_TEXTURE2D_DESC depthDesc = { MEASURE_WIDTH, MEASURE_HEIGHT, 1, 1, DXGI_FORMAT_R32_TYPELESS, { 1, 0 },
        D3D11_USAGE_DEFAULT, D3D11_BIND_DEPTH_STENCIL };
    D3D11_DEPTH_STENCIL_VIEW_DESC depthTargetDesc = {};
    depthTargetDesc.Format = DXGI_FORMAT_D32_FLOAT;
    depthTargetDesc.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
    if (FAILED(dev->CreateTexture2D(&desc, nullptr, &MeasureTexture))
        || FAILED(dev->CreateRenderTargetView(MeasureTexture.Get(), nullptr, &MeasureTarget))
        || FAILED(dev->CreateTexture2D(&depthDesc, nullptr, &MeasureDepth))
        || FAILED(dev->CreateDepthStencilView(MeasureDepth.Get(), &depthTargetDesc, &MeasureDepthTarget)))
        return false;

    D3D11_TEXTURE2D_DESC readDesc = desc, depthReadDesc = depthDesc;
    readDesc.Usage = depthReadDesc.Usage = D3D11_USAGE_STAGING;
    readDesc.BindFlags = depthReadDesc.BindFlags = 0;
    readDesc.CPUAccessFlags = depthReadDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    for (UINT slot = 0; slot < READ_LATENCY; ++slot)
        if (FAILED(dev->CreateTexture2D(&readDesc, nullptr, &MeasureReadback[slot]))
            || FAILED(dev->CreateTexture2D(&depthReadDesc, nullptr, &MeasureDepthReadback[slot])))
            return false;

    // the nearest of the glow's surfaces is kept at each texel, the one the camera sees
    D3D11_DEPTH_STENCIL_DESC depthStateDesc = {};
    depthStateDesc.DepthEnable = TRUE;
    depthStateDesc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    depthStateDesc.DepthFunc = D3D11_COMPARISON_LESS;
    if (FAILED(dev->CreateDepthStencilState(&depthStateDesc, &MeasureDepthState)))
        return false;

    // the colour is written as drawn, and alpha is set to 0 wherever the glow covers, whatever the shader writes there
    D3D11_BLEND_DESC blendDesc = {};
    D3D11_RENDER_TARGET_BLEND_DESC &target = blendDesc.RenderTarget[0];
    target.BlendEnable = TRUE;
    target.SrcBlend = D3D11_BLEND_ONE;
    target.DestBlend = D3D11_BLEND_ZERO;
    target.BlendOp = D3D11_BLEND_OP_ADD;
    target.SrcBlendAlpha = D3D11_BLEND_ZERO;
    target.DestBlendAlpha = D3D11_BLEND_ZERO;
    target.BlendOpAlpha = D3D11_BLEND_OP_ADD;
    target.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    return SUCCEEDED(dev->CreateBlendState(&blendDesc, &MeasureBlend));
}

static void SetKind(Emitter &emitter,bool painted)
{
    if(!emitter.Ticket || emitter.Painted!=painted)
    {
        emitter.Ticket=NextTicket++;
        emitter.Radiance={};emitter.Whole.Radiance={};emitter.MeasuredFrame=0;
        for(auto &level:emitter.Cells)for(auto &[key,cell]:level)cell.Radiance={};
    }
    emitter.Painted=painted;
}

// draws a glow again into the measure target with depth, and copies both whole for reading, with the frame's camera so
// each covered texel can be placed in the world
static void Measure(ID3D11DeviceContext *ctx, const shadowmaps::DrawCall &call, uint64_t casterKey)
{
    ID3D11RenderTargetView *targets[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
    ComPtr<ID3D11DepthStencilView> depth;
    ctx->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, targets, &depth);
    D3D11_VIEWPORT viewports[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE];
    UINT viewportCount = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
    ctx->RSGetViewports(&viewportCount, viewports);
    ComPtr<ID3D11BlendState> blend;
    FLOAT factor[4];
    UINT sampleMask = 0;
    ctx->OMGetBlendState(&blend, factor, &sampleMask);
    ComPtr<ID3D11DepthStencilState> depthState;
    UINT stencilRef = 0;
    ctx->OMGetDepthStencilState(&depthState, &stencilRef);

    const FLOAT clear[4] = { 0, 0, 0, 1 };
    ctx->ClearRenderTargetView(MeasureTarget.Get(), clear);
    ctx->ClearDepthStencilView(MeasureDepthTarget.Get(), D3D11_CLEAR_DEPTH, 1, 0);
    ID3D11RenderTargetView *measure = MeasureTarget.Get();
    ctx->OMSetRenderTargets(1, &measure, MeasureDepthTarget.Get());
    const D3D11_VIEWPORT measureViewport = { 0, 0, static_cast<float>(MEASURE_WIDTH), static_cast<float>(MEASURE_HEIGHT),
        0, 1 };
    ctx->RSSetViewports(1, &measureViewport);
    ctx->OMSetBlendState(MeasureBlend.Get(), nullptr, 0xffffffff);
    ctx->OMSetDepthStencilState(MeasureDepthState.Get(), 0);
    if (call.Indexed)
        ctx->DrawIndexed(call.Count, call.First, call.BaseVertex);
    else
        ctx->Draw(call.Count, call.First);

    ctx->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, targets, depth.Get());
    for (ID3D11RenderTargetView *target : targets)
        if (target)
            target->Release();
    ctx->RSSetViewports(viewportCount, viewports);
    ctx->OMSetBlendState(blend.Get(), factor, sampleMask);
    ctx->OMSetDepthStencilState(depthState.Get(), stencilRef);

    const UINT slot = shadowmaps::FrameNumber % READ_LATENCY;
    ctx->CopyResource(MeasureReadback[slot].Get(), MeasureTexture.Get());
    ctx->CopyResource(MeasureDepthReadback[slot].Get(), MeasureDepth.Get());
    MeasuredKeys[slot] = casterKey;
    MeasuredTickets[slot]=Emitters.at(casterKey).Ticket; MeasuredPainted[slot]=Emitters.at(casterKey).Painted;
    XMStoreFloat4x4(&MeasuredClipToWorld[slot], XMMatrixInverse(nullptr, lightbuffer::WorldToClip));
    MeasuredOffset[slot] = shadowmaps::CurrentOffset;
    MeasuredPlaced[slot] = lightbuffer::WorldToClipKnown;
    MeasuredThisFrame = true;
    Emitters[casterKey].MeasuredFrame = shadowmaps::FrameNumber;
}

static bool CreatePaintedObjects(ID3D11Device *dev);
static void CompositePainted(ID3D11DeviceContext *ctx);
static bool DrawPaintedAside(ID3D11DeviceContext *ctx, const shadowmaps::DrawCall &call, const D3D11_TEXTURE2D_DESC &desc);

// handles a draw into the colour target after the light buffer is finished: notes glowing meshes, measures one glow a
// frame, and returns true for a painted glow pass, which is drawn aside to be dimmed where glow lights reach. A painted
// pass's mesh is an emitter too while PaintedLight is above 0, unless the mesh also has an emitter pass, which is
// measured alone
static bool HandleColourDraw(shadowmaps::BoundDraw &bound, uint64_t pixelShader)
{
    auto *ctx = bound.Context;
    const auto &call = bound.Call;
    if (!ObjectsActive()) { if (PaintedDestination) CompositePainted(ctx); return false; }
    D3D11_TEXTURE2D_DESC desc = {};
    const bool eligible = call.Instances == 1
        && call.FirstInstance == 0 && lightbuffer::LightBuffer
        && lightbuffer::CurrentTarget != lightbuffer::LightBuffer.Get()
        && lightbuffer::IsScreenSized(lightbuffer::CurrentTarget, desc);
    const AddedPass pass = eligible ? GetAddedPass(bound, pixelShader) : AddedPass::None;
    const bool knownPainted = pass == AddedPass::Painted && IsQualifiedPaintedWorld(ctx, pixelShader);
    // Flush before an unqualified/UI/unknown draw as well as an ordinary draw.
    if (PaintedDestination && !knownPainted) CompositePainted(ctx);
    if (!eligible || pass == AddedPass::None) return false;

    // Keeping painted light is a native-colour policy. Optional emitter measurement
    // may refuse an unknown camera, caster or readback without changing this share.
    const bool handled = knownPainted && painted_emission_policy::Kept(PaintedKept.load()) < 1.0f
        && DrawPaintedAside(ctx, call, desc);
    if (handled) ++PaintedSkippedThisSecond;
    if (CreateFailed || !lightbuffer::Processed) return handled;

    // a frame capture notes why a glow pass is not used
    const bool logging = capture::State == capture::Phase::Capturing && capture::Log;
    const char *kind = pass == AddedPass::Emitter ? "emitter" : "painted";
    if (!bound.ReadRows())
    {
        if (logging)
            fprintf(capture::Log, "  glow %s pass not used: constants not read\n", kind);
        return handled;
    }

    const auto &rows = bound.Rows;
    XMMATRIX measureCamera;
    if(!lightbuffer::WorldToClipKnown||!shadowmaps::FindWorldToClip(rows,measureCamera)
        ||!lightbuffer::IsSameCamera(measureCamera,lightbuffer::WorldToClip))return handled;
    for(const auto &row:rows)if(!std::isfinite(row.x)||!std::isfinite(row.y)||!std::isfinite(row.z)||!std::isfinite(row.w))return handled;
    const XMFLOAT3 position = { rows[4].w - shadowmaps::CurrentOffset.x, rows[5].w - shadowmaps::CurrentOffset.y,
        rows[6].w - shadowmaps::CurrentOffset.z };
    const uint64_t casterKey = shadowmaps::FindCasterKey(bound.GetMesh().Key, position);
    auto caster = shadowmaps::Casters.find(casterKey);
    if (caster == shadowmaps::Casters.end() || caster->second.FramesSeen < shadowmaps::CONFIRM_FRAMES)
    {
        if (logging)
            fprintf(capture::Log, "  glow %s pass not used: mesh %s\n", kind,
                caster == shadowmaps::Casters.end() ? "not a known object" : "not confirmed yet");
        return handled;
    }

    if (logging)
    {
        fprintf(capture::Log, "  glow %s pass, caster %016llx\n", kind, static_cast<unsigned long long>(casterKey));
        if (auto emitter = Emitters.find(casterKey); emitter != Emitters.end())
            fprintf(capture::Log, "  glow emitter radiance %.3f %.3f %.3f (%s), shape %s, area %.0f, centre %.1f %.1f %.1f, "
                "size %.1f, painted %d\n", emitter->second.Radiance.x, emitter->second.Radiance.y,
                emitter->second.Radiance.z, emitter->second.Radiance.w > 0 ? "measured" : "not measured",
                emitter->second.ShapeKnown ? "known" : "not known", emitter->second.Whole.Area, emitter->second.Centre.x,
                emitter->second.Centre.y, emitter->second.Centre.z, emitter->second.Size, emitter->second.Painted ? 1 : 0);
    }
    const bool painted = pass == AddedPass::Painted;
    auto found = Emitters.find(casterKey);
    if (!painted || (PaintedLight > 0 && (found == Emitters.end() || found->second.Painted)))
    {
        Emitter &emitter = found == Emitters.end() ? Emitters[casterKey] : found->second;
        SetKind(emitter,painted);
        emitter.LastSeen = shadowmaps::FrameNumber;
        if (MeasureTarget && !MeasuredThisFrame && !MeasuredKeys[shadowmaps::FrameNumber % READ_LATENCY]
            && (!emitter.MeasuredFrame || emitter.MeasuredFrame + MEASURE_INTERVAL <= shadowmaps::FrameNumber)
            && emission_policy::Samples.Take(shadowmaps::FrameNumber,emission_policy::Producer::Surface))
            Measure(ctx, call, casterKey);
    }
    return handled;
}

static bool CreateShapeObjects(ID3D11Device *dev)
{
    if (ShapeVertexShader && ShapeStreamShader) return true;
    ComPtr<ID3DBlob> code;
    if (!shadowmaps::CompileShader(SHAPE_SHADER, "vs_5_0", code)
        || FAILED(dev->CreateVertexShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &ShapeVertexShader)))
        return false;

    const D3D11_SO_DECLARATION_ENTRY entry = { 0, "WORLD", 0, 0, 3, 0 };
    const UINT stride = 12;
    return SUCCEEDED(dev->CreateGeometryShaderWithStreamOutput(code->GetBufferPointer(), code->GetBufferSize(), &entry, 1,
        &stride, 1, D3D11_SO_NO_RASTERIZED_STREAM, nullptr, &ShapeStreamShader));
}

// the triangles a mesh draws, or 0 when it is not a triangle list or strip
static UINT CountTriangles(const shadowmaps::Geometry &mesh)
{
    if (mesh.Topology == D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST)
        return mesh.Call.Count / 3;
    if (mesh.Topology == D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP && mesh.Call.Count > 2)
        return mesh.Call.Count - 2;
    return 0;
}

static bool IsStagingHeld(const ShapeStaging &staging)
{
    return std::any_of(ShapeReads.begin(), ShapeReads.end(),
        [&staging](const ShapeRead &read) { return read.Staging == staging.Buffer; });
}

// a staging buffer of at least this many bytes that no read holds: the smallest free one that fits, else the largest
// free one made again at the size rounded up to a power of two, within MAX_TRIANGLES. The buffers held by reads and the
// one returned stay within this many bytes, and so do the staging buffers together, the other free ones released to
// make room once that is enough; null when they cannot, or none is free, with failed set when a buffer could not be
// made
static ShapeStaging *FindStaging(ID3D11Device *dev, UINT bytes, size_t most, bool &failed)
{
    failed = false;
    ShapeStaging *fit = nullptr, *largest = nullptr;
    size_t total = 0, held = 0;
    for (ShapeStaging &staging : ShapeStagingPool)
    {
        total += staging.Bytes;
        if (IsStagingHeld(staging))
        {
            held += staging.Bytes;
            continue;
        }
        if (staging.Bytes >= bytes && (!fit || staging.Bytes < fit->Bytes))
            fit = &staging;
        if (!largest || staging.Bytes > largest->Bytes)
            largest = &staging;
    }
    if (fit)
        return held + fit->Bytes <= most ? fit : nullptr;

    const UINT size = std::min(std::bit_ceil(bytes), MAX_TRIANGLES * 36);
    if (!largest || held + size > most)
        return nullptr;
    if (total - largest->Bytes + size > most)
        for (ShapeStaging &staging : ShapeStagingPool)
            if (&staging != largest && staging.Bytes && !IsStagingHeld(staging))
            {
                total -= staging.Bytes;
                staging = {};
            }

    *largest = {};
    const D3D11_BUFFER_DESC desc = { size, D3D11_USAGE_STAGING, 0, D3D11_CPU_ACCESS_READ };
    if (FAILED(dev->CreateBuffer(&desc, nullptr, &largest->Buffer)))
    {
        failed = true;
        return nullptr;
    }
    largest->Bytes = size;
    return largest;
}

// whether ReadTriangles started a read; Busy when no staging buffer is free within the budget now, Failed when a buffer
// could not be made
enum class ReadStart { Started, Busy, Failed };

// draws a mesh with its world rows into the stream output buffer as world-position triangles, and copies them to a
// staging buffer for reading; the buffer's triangles are zeroed first, so those a strip restart leaves unwritten are
// empty. Nothing is drawn unless it returns Started. The stream output and zero buffers are not tried again after they
// fail until Reset
static ReadStart ReadTriangles(ID3D11DeviceContext1 *ctx, uint64_t casterKey, const shadowmaps::Geometry &mesh,
    const float *world, UINT triangles, bool boundsOnly, bool worldMesh)
{
    if (ShapeReads.size() + (boundsOnly ? 1 : 0) >= SHAPE_BUFFER_COUNT)
        return ReadStart::Busy;
    if (ShapeBuffersFailed)
        return ReadStart::Failed;

    ComPtr<ID3D11Device> dev;
    ctx->GetDevice(&dev);
    if (!ShapeOutput)
    {
        const D3D11_BUFFER_DESC outputDesc = { MAX_TRIANGLES * 36, D3D11_USAGE_DEFAULT, D3D11_BIND_STREAM_OUTPUT };
        const std::vector<XMFLOAT3> zeros(MAX_TRIANGLES * 3);
        const D3D11_SUBRESOURCE_DATA initial = { zeros.data() };
        if (FAILED(dev->CreateBuffer(&outputDesc, &initial, &ShapeZeros))
            || FAILED(dev->CreateBuffer(&outputDesc, nullptr, &ShapeOutput)))
        {
            ShapeZeros.Reset();
            ShapeOutput.Reset();
            ShapeBuffersFailed = true;
            return ReadStart::Failed;
        }
    }
    bool failed = false;
    ShapeStaging *staging = FindStaging(dev.Get(), triangles * 36,
        boundsOnly ? SHAPE_STAGING_BYTES - MAX_TRIANGLES * 36 : SHAPE_STAGING_BYTES, failed);
    if (!staging)
        return failed ? ReadStart::Failed : ReadStart::Busy;

    ID3D11Buffer *output = ShapeOutput.Get();
    const D3D11_BOX used = { 0, 0, 0, triangles * 36, 1, 1 };
    ctx->CopySubresourceRegion(output, 0, 0, 0, 0, ShapeZeros.Get(), 0, &used);

    ComPtr<ID3DDeviceContextState> previous;
    ctx->SwapDeviceContextState(shadowmaps::DrawState.Get(), &previous);
    shadowmaps::CasterConstants constants = {};
    std::copy(world, world + 12, constants.World);
    ctx->UpdateSubresource(shadowmaps::CasterBuffer.Get(), 0, nullptr, &constants, 0, 0);
    ID3D11Buffer *casterBuffer = shadowmaps::CasterBuffer.Get();
    ctx->VSSetConstantBuffers(0, 1, &casterBuffer);
    ctx->VSSetShader(ShapeVertexShader.Get(), nullptr, 0);
    ctx->GSSetShader(ShapeStreamShader.Get(), nullptr, 0);
    ctx->PSSetShader(nullptr, nullptr, 0);
    const UINT offset = 0;
    ctx->SOSetTargets(1, &output, &offset);
    shadowmaps::DrawCaster(ctx, mesh);
    ID3D11Buffer *none = nullptr;
    ctx->SOSetTargets(1, &none, &offset);
    ctx->GSSetShader(nullptr, nullptr, 0);
    ctx->SwapDeviceContextState(previous.Get(), nullptr);
    ctx->CopySubresourceRegion(staging->Buffer.Get(), 0, 0, 0, 0, output, 0, &used);
    ShapeReads.push_back({ casterKey, staging->Buffer, triangles, shadowmaps::FrameNumber, boundsOnly, worldMesh });
    return ReadStart::Started;
}

// reads a glowing object's triangles, asking again next frame when no staging buffer can be had, or marks its shape
// known with no cells when it is not a triangle mesh within MAX_TRIANGLES
static void RequestShape(ID3D11DeviceContext1 *ctx, uint64_t casterKey, Emitter &emitter)
{
    emitter.ShapeRequested = true;
    auto caster = shadowmaps::Casters.find(casterKey);
    if (caster == shadowmaps::Casters.end())
        return;

    const UINT triangles = CountTriangles(caster->second.Mesh);
    if (!triangles || triangles > MAX_TRIANGLES)
    {
        emitter.ShapeKnown = true;
        return;
    }
    if (ReadTriangles(ctx, casterKey, caster->second.Mesh, caster->second.World, triangles, false, false)
        != ReadStart::Started)
        emitter.ShapeRequested = false;
}

// reads the bounds of this frame's moving meshes in world positions not yet asked for, which go into every moving-object
// map until they are read, and of the held ones, which are kept until they are read; then of confirmed objects not yet
// asked for, those near a window's beam first, as its map leaves out objects until their bounds are read; up to
// BOUNDS_PER_FRAME a frame in all. One that cannot get a staging buffer now ends the frame's requests unmarked, so it
// keeps its place in line; one whose buffers cannot be made is marked as asked for, and its bounds stay unknown
static void RequestBounds(ID3D11DeviceContext1 *ctx)
{
    int requested = 0;
    // asks for a moving mesh's bounds, marking it unreadable when it is not a triangle mesh within MAX_TRIANGLES or its
    // read cannot start; false, with nothing marked, when no staging buffer can be had for it now
    const auto requestWorldMesh = [ctx](const shadowmaps::Geometry &mesh) {
        const UINT triangles = CountTriangles(mesh);
        const ReadStart read = triangles && triangles <= MAX_TRIANGLES
            ? ReadTriangles(ctx, mesh.Key, mesh, shadowmaps::WORLD_IDENTITY, triangles, true, true) : ReadStart::Failed;
        if (read == ReadStart::Busy)
            return false;
        const uint64_t vertices = reinterpret_cast<uint64_t>(mesh.VertexBuffer.Get());
        const uint64_t indices = reinterpret_cast<uint64_t>(mesh.IndexBuffer.Get());
        shadowmaps::WorldMesh &worldMesh = shadowmaps::WorldMeshes.try_emplace(mesh.Key,
            shadowmaps::WorldMesh { .Vertex = vertices, .Index = indices }).first->second;
        worldMesh.Stale = false;
        worldMesh.AskedFrame = shadowmaps::FrameNumber;
        // a stale mesh whose read cannot start keeps its old bounds
        worldMesh.Unreadable = read != ReadStart::Started && !worldMesh.BoundsKnown;
        shadowmaps::IndexMesh(mesh.Key, vertices, indices);
        return true;
    };
    // whether a moving mesh's bounds are to be asked for: it has no entry or a stale one, and its buffers have not been
    // written for READ_LATENCY frames, as a read started while they are being written would be dropped. One written
    // every frame keeps its old bounds, or stays unread like a Dynamic mesh
    const auto isWorldMeshDue = [](const shadowmaps::Geometry &mesh) {
        auto worldMesh = shadowmaps::WorldMeshes.find(mesh.Key);
        return (worldMesh == shadowmaps::WorldMeshes.end() || worldMesh->second.Stale)
            && !shadowmaps::IsWrittenWithin(mesh, READ_LATENCY);
    };
    // the moving meshes' world rows are relative to this frame's camera
    if (shadowmaps::CameraFound)
        for (const shadowmaps::MovingCaster &moving : shadowmaps::MovingCasters)
        {
            if (requested == BOUNDS_PER_FRAME)
                return;
            const shadowmaps::Geometry &mesh = moving.Mesh;
            if (!shadowmaps::IsInWorldPositions(&shadowmaps::MovingWorlds[moving.Index * 12], shadowmaps::CurrentOffset))
                continue;
            // the game rewrites the mesh's buffers, so bounds read once would not last, and it is no longer held
            if (mesh.Dynamic)
            {
                shadowmaps::WorldMeshes.erase(mesh.Key);
                shadowmaps::HeldMeshes.erase(mesh.Key);
                continue;
            }
            if (!isWorldMeshDue(mesh))
                continue;
            if (!requestWorldMesh(mesh))
                return;
            ++requested;
        }
    // a held mesh's world rows are absolute
    for (const auto &[meshKey, held] : shadowmaps::HeldMeshes)
    {
        if (requested == BOUNDS_PER_FRAME)
            return;
        if (!shadowmaps::IsHeldInWorldPositions(held) || !isWorldMeshDue(held.Mesh))
            continue;
        if (!requestWorldMesh(held.Mesh))
            return;
        ++requested;
    }
    for (const bool nearWindowsOnly : { true, false })
        for (auto &[casterKey, caster] : shadowmaps::Casters)
        {
            if (requested == BOUNDS_PER_FRAME)
                return;
            if (((caster.BoundsKnown || caster.BoundsRequested) && !caster.BoundsStale)
                || caster.FramesSeen < shadowmaps::CONFIRM_FRAMES)
                continue;
            if (nearWindowsOnly && !shadowmaps::IsNearWindows({ caster.World[3], caster.World[7], caster.World[11] }))
                continue;
            // a read started while its buffers are being written would be dropped; it is asked for once they settle
            if (shadowmaps::IsWrittenWithin(caster.Mesh, READ_LATENCY))
                continue;

            const UINT triangles = CountTriangles(caster.Mesh);
            if (triangles && triangles <= MAX_TRIANGLES
                && ReadTriangles(ctx, casterKey, caster.Mesh, caster.World, triangles, true, false) == ReadStart::Busy)
                return;
            caster.BoundsRequested = true;
            caster.BoundsStale = false;
            caster.BoundsAskedFrame = shadowmaps::FrameNumber;
            shadowmaps::IndexMesh(casterKey, reinterpret_cast<uint64_t>(caster.Mesh.VertexBuffer.Get()),
                reinterpret_cast<uint64_t>(caster.Mesh.IndexBuffer.Get()));
            ++requested;
        }
}

static void AddPoint(CellSum &sum, const float (&point)[3], const float (&normal)[3], double area)
{
    sum.Area += area;
    for (int axis = 0; axis < 3; ++axis)
    {
        sum.Centre[axis] += point[axis] * area;
        sum.Normal[axis] += normal[axis] * area;
        sum.Low[axis] = std::min(sum.Low[axis], point[axis]);
        sum.High[axis] = std::max(sum.High[axis], point[axis]);
    }
}

static CellSum StartCell(int x, int y, int z)
{
    return { x, y, z, 0, {}, {}, { FLT_MAX, FLT_MAX, FLT_MAX }, { -FLT_MAX, -FLT_MAX, -FLT_MAX } };
}

static Cell FinishCell(const CellSum &sum, float spacing)
{
    Cell cell = { sum.X, sum.Y, sum.Z, static_cast<float>(sum.Area) };
    cell.Centre = { static_cast<float>(sum.Centre[0] / sum.Area), static_cast<float>(sum.Centre[1] / sum.Area),
        static_cast<float>(sum.Centre[2] / sum.Area) };
    const double length = std::sqrt(sum.Normal[0] * sum.Normal[0] + sum.Normal[1] * sum.Normal[1] + sum.Normal[2] * sum.Normal[2]);
    if (length > 0.5 * sum.Area)
        cell.Normal = { static_cast<float>(sum.Normal[0] / length), static_cast<float>(sum.Normal[1] / length),
            static_cast<float>(sum.Normal[2] / length) };
    const float dx = sum.High[0] - sum.Low[0], dy = sum.High[1] - sum.Low[1], dz = sum.High[2] - sum.Low[2];
    cell.Radius = 0.5f * std::sqrt(dx * dx + dy * dy + dz * dz) + 0.5f * spacing;
    return cell;
}

// spreads the triangles over points and sums them into cells at every size and into the whole emitter
static void BuildCells(Emitter &emitter, const XMFLOAT3 *corners, UINT triangles)
{
    double totalArea = 0;
    for (UINT t = 0; t < triangles; ++t)
    {
        const XMVECTOR a = XMLoadFloat3(&corners[t * 3]);
        totalArea += 0.5f * XMVectorGetX(XMVector3Length(XMVector3Cross(XMLoadFloat3(&corners[t * 3 + 1]) - a,
            XMLoadFloat3(&corners[t * 3 + 2]) - a)));
    }
    const float spacing = std::max(POINT_SPACING, std::sqrt(static_cast<float>(totalArea) / MAX_POINTS));

    std::unordered_map<uint64_t, CellSum> sums[CELL_SIZE_COUNT];
    CellSum whole = StartCell(0, 0, 0);
    for (UINT t = 0; t < triangles; ++t)
    {
        const XMVECTOR a = XMLoadFloat3(&corners[t * 3]);
        const XMVECTOR ab = XMLoadFloat3(&corners[t * 3 + 1]) - a;
        const XMVECTOR ac = XMLoadFloat3(&corners[t * 3 + 2]) - a;
        const XMVECTOR cross = XMVector3Cross(ab, ac);
        const float length = XMVectorGetX(XMVector3Length(cross));
        if (length < 1e-6f)
            continue;

        const float area = 0.5f * length;
        float normal[3];
        XMStoreFloat3(reinterpret_cast<XMFLOAT3 *>(normal), cross / length);
        // a k by k grid of smaller triangles, each the same share of the area, sampled at its centre
        const int k = std::max(1, static_cast<int>(std::ceil(std::sqrt(area) / spacing)));
        const double share = static_cast<double>(area) / (k * k);
        const auto addSample = [&](float u, float v) {
            float point[3];
            XMStoreFloat3(reinterpret_cast<XMFLOAT3 *>(point), a + ab * u + ac * v);
            AddPoint(whole, point, normal, share);
            for (int size = 0; size < CELL_SIZE_COUNT; ++size)
            {
                const int x = static_cast<int>(std::floor(point[0] / CELL_SIZES[size]));
                const int y = static_cast<int>(std::floor(point[1] / CELL_SIZES[size]));
                const int z = static_cast<int>(std::floor(point[2] / CELL_SIZES[size]));
                const uint64_t key = shadowmaps::HashCaster(size, x, y, z);
                auto sum = sums[size].try_emplace(key, StartCell(x, y, z)).first;
                AddPoint(sum->second, point, normal, share);
            }
        };
        for (int i = 0; i < k; ++i)
            for (int j = 0; i + j < k; ++j)
            {
                addSample((i + 1.0f / 3) / k, (j + 1.0f / 3) / k);
                if (i + j < k - 1)
                    addSample((i + 2.0f / 3) / k, (j + 2.0f / 3) / k);
            }
    }
    emitter.ShapeKnown = true;
    if (whole.Area <= 0)
        return;

    for (int size = 0; size < CELL_SIZE_COUNT; ++size)
        for (const auto &[key, sum] : sums[size])
            emitter.Cells[size][key] = FinishCell(sum, spacing);
    emitter.Whole = FinishCell(whole, spacing);
    emitter.Centre = emitter.Whole.Centre;
    const float dx = whole.High[0] - whole.Low[0], dy = whole.High[1] - whole.Low[1], dz = whole.High[2] - whole.Low[2];
    emitter.Size = std::sqrt(dx * dx + dy * dy + dz * dz);
}

// finds the bounds of a mesh's triangles; false, leaving lowest and highest alone, when it has none
static bool FindBounds(const XMFLOAT3 *corners, UINT triangles, XMFLOAT3 &lowest, XMFLOAT3 &highest)
{
    XMVECTOR low = XMVectorReplicate(FLT_MAX), high = XMVectorReplicate(-FLT_MAX);
    for (UINT t = 0; t < triangles; ++t)
    {
        const XMVECTOR a = XMLoadFloat3(&corners[t * 3]);
        const XMVECTOR b = XMLoadFloat3(&corners[t * 3 + 1]);
        const XMVECTOR c = XMLoadFloat3(&corners[t * 3 + 2]);
        // empty triangles include the zeroed tail of the buffer, which is not part of the mesh
        if (XMVectorGetX(XMVector3LengthSq(XMVector3Cross(b - a, c - a))) <= 0)
            continue;

        low = XMVectorMin(low, XMVectorMin(a, XMVectorMin(b, c)));
        high = XMVectorMax(high, XMVectorMax(a, XMVectorMax(b, c)));
    }
    if (XMVectorGetX(low) > XMVectorGetX(high))
        return false;

    XMStoreFloat3(&lowest, low);
    XMStoreFloat3(&highest, high);
    return true;
}

// stores an object's bounds and adds it to every light whose shadow radius reaches them, other than a glow light's own
// emitter
static void AddBounds(uint64_t casterKey, shadowmaps::Caster &caster, const XMFLOAT3 *corners, UINT triangles)
{
    if (!FindBounds(corners, triangles, caster.Low, caster.High))
        return;

    caster.BoundsKnown = true;
    if (const float scale = shadowmaps::FindScale(caster.World); scale > 0 && shadowmaps::ClassifiedMeshes.contains(caster.Mesh.Key))
        shadowmaps::MeshSizes[caster.Mesh.Key] = shadowmaps::Distance(caster.Low, caster.High) / scale;
    for (auto &[key, light] : shadowmaps::Lights)
    {
        if (light.EmitterKey != casterKey && shadowmaps::FindCasterDistance(caster, light.Position) < light.Radius
            && light.CasterKeys.insert(casterKey).second)
            ++light.Version;
        // a lamp whose map holds its own model is redrawn without it
        if (!light.Glow && light.CasterKeys.contains(casterKey) && shadowmaps::HoldsLight(caster, light.Position))
            light.RedrawNow = true;
    }
}

// reads the shapes and bounds copied at least READ_LATENCY frames ago, dropping those whose object is gone
static void ReadShapes(ID3D11DeviceContext *ctx)
{
    std::erase_if(ShapeReads, [ctx](const ShapeRead &read) {
        D3D11_MAPPED_SUBRESOURCE mapped;
        if (read.WorldMesh)
        {
            auto worldMesh = shadowmaps::WorldMeshes.find(read.CasterKey);
            if (worldMesh == shadowmaps::WorldMeshes.end() || worldMesh->second.AskedFrame != read.Frame)
                return true;
            if (read.Frame + READ_LATENCY > shadowmaps::FrameNumber
                || FAILED(ctx->Map(read.Staging.Get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped)))
                return false;

            worldMesh->second.BoundsKnown = FindBounds(static_cast<const XMFLOAT3 *>(mapped.pData), read.Triangles,
                worldMesh->second.Low, worldMesh->second.High);
            worldMesh->second.Unreadable = !worldMesh->second.BoundsKnown;
            ctx->Unmap(read.Staging.Get(), 0);
            return true;
        }

        auto caster = shadowmaps::Casters.find(read.CasterKey);
        auto emitter = Emitters.find(read.CasterKey);
        if (caster == shadowmaps::Casters.end() || (!read.BoundsOnly && emitter == Emitters.end())
            || (read.BoundsOnly && caster->second.BoundsAskedFrame != read.Frame))
            return true;
        if (read.Frame + READ_LATENCY > shadowmaps::FrameNumber)
            return false;

        if (FAILED(ctx->Map(read.Staging.Get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped)))
            return false;

        const auto *corners = static_cast<const XMFLOAT3 *>(mapped.pData);
        if (read.BoundsOnly)
        {
            AddBounds(read.CasterKey, caster->second, corners, read.Triangles);
            ctx->Unmap(read.Staging.Get(), 0);
            return true;
        }

        BuildCells(emitter->second, corners, read.Triangles);
        ctx->Unmap(read.Staging.Get(), 0);
        if (capture::State == capture::Phase::Capturing && capture::Log)
        {
            fprintf(capture::Log, "--- glow emitter %016llx: %u triangles, area %.0f, cells",
                static_cast<unsigned long long>(read.CasterKey), read.Triangles, emitter->second.Whole.Area);
            for (int size = 0; size < CELL_SIZE_COUNT; ++size)
                fprintf(capture::Log, " %.0f:%zu", CELL_SIZES[size], emitter->second.Cells[size].size());
            fprintf(capture::Log, "\n");
        }
        return true;
    });
}

static uint64_t HashCell(uint64_t casterKey, int size, const Cell &cell)
{
    return shadowmaps::HashValue(casterKey, shadowmaps::HashCaster(size, cell.X, cell.Y, cell.Z));
}

static float FindLuminance(const XMFLOAT4 &colour)
{
    return 0.2126f * colour.x + 0.7152f * colour.y + 0.0722f * colour.z;
}

// what an emitter's light is scaled by: PaintedLight for a painted glow, else 1
static float FindLightScale(const Emitter &emitter)
{
    return emitter.Painted ? PaintedLight.load() : 1.0f;
}

// a cell's glow at one of CELL_SIZES, or CELL_SIZE_COUNT for the whole emitter: its own once measured, else that of the
// nearest cell around it that is, else the whole emitter's, else the last measure's average while none is
static XMFLOAT4 FindCellRadiance(const Emitter &emitter, int size, const Cell &cell)
{
    const Cell *current = &cell;
    int x = cell.X, y = cell.Y, z = cell.Z;
    while (size < CELL_SIZE_COUNT && current)
    {
        if (current->Radiance.w > 0)
            return current->Radiance;
        if (++size == CELL_SIZE_COUNT)
            break;

        x >>= 1;
        y >>= 1;
        z >>= 1;
        auto parent = emitter.Cells[size].find(shadowmaps::HashCaster(size, x, y, z));
        current = parent == emitter.Cells[size].end() ? nullptr : &parent->second;
    }
    return emitter.Whole.Radiance.w > 0 ? emitter.Whole.Radiance : emitter.Radiance;
}

// how far an emitter's light reaches: where its whole glow, as bright as its cells' lights draw it together, over the
// distance squared falls below GLOW_CUTOFF. It depends on the whole emitter only, so splitting or merging its cells does
// not change how far it lights
static float FindEmitterRadius(const Emitter &emitter)
{
    const XMFLOAT4 radiance = FindCellRadiance(emitter, CELL_SIZE_COUNT, emitter.Whole);
    const float brightness = std::max({ radiance.x, radiance.y, radiance.z }) * emitter.Whole.Area / XM_PI
        * Strength.load() * FindLightScale(emitter);
    return std::min(std::sqrt(std::max(brightness, 0.0f) / GLOW_CUTOFF), MAX_GLOW_RADIUS);
}

// finds each emitter's radius again, as a measure or the strength settings change it, and gives a changed one to the
// emitter's lights, whose objects and maps follow at the shadow maps' end of frame
static void UpdateRadii()
{
    for (auto &[casterKey, emitter] : Emitters)
    {
        const float radius = FindEmitterRadius(emitter);
        if (radius == emitter.Radius)
            continue;

        emitter.Radius = radius;
        for (auto &[key, light] : shadowmaps::Lights)
            if (light.Glow && light.EmitterKey == casterKey)
                light.GlowRange = radius;
    }
}

// whether a cell gets a light: cells further than SEEN_DISTANCE are left out, as whole emitters are
static bool IsCellSeen(const Cell &cell)
{
    return shadowmaps::Distance(cell.Centre, shadowmaps::Camera) - cell.Radius <= SEEN_DISTANCE;
}

// how far a cell is past the size its distance from the camera calls for, over 1 when it splits; a cell lit last frame
// splits once it is SIZE_MARGIN past it and a split one merges once it is SIZE_MARGIN under it
static float FindSplitNeed(const Emitter &emitter, int size, const Cell &cell, uint64_t cellKey)
{
    // cells near the camera are sized as if a little further away, so the camera's few units of swing around the player
    // do not resize them
    const float judged = std::max(shadowmaps::Distance(cell.Centre, shadowmaps::Camera), MIN_JUDGED_DISTANCE);
    float extent = 0;
    if (size == CELL_SIZE_COUNT)
        extent = emitter.Size > SMALL_EMITTER_SIZE ? emitter.Size : 0;
    else if (size > 0)
        // a cell only partly covered by the glow is at its edge, where nearby objects need lights close to them, so it
        // splits as if it were twice its size
        extent = CELL_SIZES[size] * (cell.Area < EDGE_FILL * CELL_SIZES[size] * CELL_SIZES[size] ? 2 : 1);
    return extent / (judged * SPLIT_DISTANCE_RATIO) * (ActiveCells.contains(cellKey) ? 1 / SIZE_MARGIN : SIZE_MARGIN);
}

// the seen cells a cell splits into: the top-level cells for a whole emitter, else the cells of the next size within it
static void FindChildren(const Candidate &parent, std::vector<Candidate> &children)
{
    children.clear();
    const Emitter &emitter = Emitters.at(parent.CasterKey);
    const float parentLight = FindLuminance(FindCellRadiance(emitter, parent.Size, *parent.Source)) * parent.Source->Area;
    const auto add = [&](int size, const Cell &cell) {
        // the dark parts of a large emitter, such as a rock glowing only near lava, get no light
        if (!IsCellSeen(cell) || FindLuminance(FindCellRadiance(emitter, size, cell)) * cell.Area < DARK_SHARE * parentLight)
            return;

        const uint64_t cellKey = HashCell(parent.CasterKey, size, cell);
        children.push_back({ FindSplitNeed(emitter, size, cell, cellKey), parent.CasterKey, cellKey, &cell, size });
    };
    if (parent.Size == CELL_SIZE_COUNT)
    {
        for (const auto &[key, cell] : emitter.Cells[CELL_SIZE_COUNT - 1])
            add(CELL_SIZE_COUNT - 1, cell);
        return;
    }
    const int size = parent.Size - 1;
    for (int dx = 0; dx < 2; ++dx)
        for (int dy = 0; dy < 2; ++dy)
            for (int dz = 0; dz < 2; ++dz)
            {
                auto child = emitter.Cells[size].find(shadowmaps::HashCaster(size, parent.Source->X * 2 + dx,
                    parent.Source->Y * 2 + dy, parent.Source->Z * 2 + dz));
                if (child != emitter.Cells[size].end())
                    add(size, child->second);
            }
}

// the cells that become this frame's lights: a cut through each seen emitter's cells, so together they hold all of its
// glow but its dark parts. Each emitter starts as one cell, the brightest kept when there are more emitters than lights,
// and the cell whose split matters most, the one holding the most light times how far it is past its split distance,
// splits until
// none needs to or the lights run out; a split that would pass the budget is left for cells with fewer children. A
// painted glow's brightness and split priority are scaled by its light's scale, so the weaker painted lights split
// later than the emitters'
static std::vector<Candidate> FindCut(int budget)
{
    std::vector<std::pair<float, Candidate>> wholes;
    for (const auto &[casterKey, emitter] : Emitters)
    {
        const float scale = FindLightScale(emitter);
        if (!emitter.ShapeKnown || emitter.Radiance.w <= 0 || emitter.Whole.Area <= 0 || scale <= 0
            || emitter.Radius < MIN_GLOW_RADIUS
            || shadowmaps::Distance(emitter.Centre, shadowmaps::Camera) - emitter.Size / 2 > SEEN_DISTANCE)
            continue;

        const uint64_t cellKey = shadowmaps::HashValue(casterKey, CELL_SIZE_COUNT);
        const float distance = shadowmaps::Distance(emitter.Whole.Centre, shadowmaps::Camera);
        // an emitter lit last frame keeps its lights until another is clearly brighter
        const float score = scale * FindLuminance(FindCellRadiance(emitter, CELL_SIZE_COUNT, emitter.Whole))
            * emitter.Whole.Area / std::max(distance * distance, 1.0f)
            * (emitter.ShownLights.empty() ? 1 : KEEP_MARGIN);
        wholes.push_back({ score, { FindSplitNeed(emitter, CELL_SIZE_COUNT, emitter.Whole, cellKey), casterKey, cellKey,
            &emitter.Whole, CELL_SIZE_COUNT } });
    }
    std::sort(wholes.begin(), wholes.end(), [](const auto &a, const auto &b) { return a.first > b.first; });
    if (static_cast<int>(wholes.size()) > budget)
        wholes.resize(budget);

    std::vector<Candidate> cut;
    // split priorities, by place in the cut
    std::priority_queue<std::pair<float, size_t>> splits;
    const auto add = [&](size_t index, const Candidate &candidate) {
        if (index == cut.size())
            cut.push_back(candidate);
        else
            cut[index] = candidate;
        if (candidate.Need > 1)
        {
            const Emitter &emitter = Emitters.at(candidate.CasterKey);
            splits.push({ FindLightScale(emitter)
                * FindLuminance(FindCellRadiance(emitter, candidate.Size, *candidate.Source)) * candidate.Source->Area
                * candidate.Need, index });
        }
    };
    for (const auto &[score, whole] : wholes)
        add(cut.size(), whole);
    std::vector<Candidate> children;
    while (!splits.empty())
    {
        const size_t index = splits.top().second;
        splits.pop();
        FindChildren(cut[index], children);
        if (children.empty() || static_cast<int>(cut.size() + children.size()) - 1 > budget)
            continue;

        add(index, children[0]);
        for (size_t n = 1; n < children.size(); ++n)
            add(cut.size(), children[n]);
    }
    return cut;
}

// adds a light for a cell, holding the confirmed objects within its range other than its emitter
static uint64_t AddLight(uint64_t casterKey, const Cell &cell)
{
    const uint64_t key = shadowmaps::NextLightKey++;
    shadowmaps::Light &light = shadowmaps::Lights[key];
    light.Glow = true;
    light.EmitterKey = casterKey;
    light.Position = { cell.Centre.x + cell.Normal.x * SURFACE_OFFSET, cell.Centre.y + cell.Normal.y * SURFACE_OFFSET,
        cell.Centre.z + cell.Normal.z * SURFACE_OFFSET };
    light.GlowNormal = cell.Normal;
    // every cell reaches its emitter's radius, so a cell split into smaller ones still lights as far as it did
    light.GlowRange = Emitters.at(casterKey).Radius;
    light.GlowArea = cell.Area;
    shadowmaps::GatherCasters(light);
    return key;
}

// picks the cells that become lights, within the slots the game's lamps leave, and marks their lights as seen
static void PickLights(unsigned particleDemand)
{
    int lamps = 0;
    for (const auto &[key, light] : shadowmaps::Lights)
        if (!light.Glow && (light.LastSeen + shadowmaps::RECENT_FRAMES >= shadowmaps::FrameNumber || light.LastAdded==shadowmaps::FrameNumber))
            ++lamps;
    const auto budget=emission_policy::Split(shadowmaps::LightLimit.load(),static_cast<unsigned>(lamps),particleDemand,
        LightLimit.load(),ParticleLightLimit.load());
    ParticleSlots=budget.particles;
    const std::vector<Candidate> candidates = FindCut(static_cast<int>(budget.surfaces));
    unsigned heldLeft=emission_policy::Held(budget.surfaces,static_cast<unsigned>(candidates.size()));

    ActiveCells.clear();
    CellsPickedLastFrame = static_cast<int>(candidates.size());
    const float strength = Strength.load();
    // this frame's cell lights by emitter
    std::unordered_map<uint64_t, std::vector<uint64_t>> picked;
    for (const Candidate &candidate : candidates)
    {
        auto found = CellLights.find(candidate.CellKey);
        if (found == CellLights.end() || !shadowmaps::Lights.contains(found->second))
            found = CellLights.insert_or_assign(candidate.CellKey, AddLight(candidate.CasterKey, *candidate.Source)).first;
        shadowmaps::Light &light = shadowmaps::Lights[found->second];
        const Emitter &emitter = Emitters[candidate.CasterKey];
        const XMFLOAT4 radiance = FindCellRadiance(emitter, candidate.Size, *candidate.Source);
        const float scale = strength * FindLightScale(emitter) * candidate.Source->Area / XM_PI;
        light.GlowColour = { radiance.x * scale, radiance.y * scale, radiance.z * scale };
        light.LastSeen = shadowmaps::FrameNumber;
        ActiveCells.insert(candidate.CellKey);
        picked[candidate.CasterKey].push_back(found->second);
    }

    for (auto &[key, light] : shadowmaps::Lights)
        light.GlowShown = light.GlowHeld = false;
    // an emitter's picked lights replace the ones it shows once all of them have maps, so its glow does not go dark
    // while the new maps are drawn; the lights shown until then and not picked are held, taking only slots left over
    for (auto &[casterKey, emitter] : Emitters)
    {
        auto lights = picked.find(casterKey);
        if (lights == picked.end())
        {
            emitter.ShownLights.clear();
            continue;
        }
        const auto built = std::count_if(lights->second.begin(), lights->second.end(),
            [](uint64_t key) { return shadowmaps::Lights[key].BuiltFrame != 0; });
        if (capture::State == capture::Phase::Capturing && capture::Log)
        {
            float area = 0;
            for (uint64_t key : lights->second)
                area += shadowmaps::Lights[key].GlowArea;
            fprintf(capture::Log, "--- glow emitter %016llx: %zu lights picked covering area %.0f of %.0f, %lld with maps, "
                "%lld without a slot, %zu shown, waiting since frame %llu\n", static_cast<unsigned long long>(casterKey),
                lights->second.size(), area, emitter.Whole.Area, static_cast<long long>(built),
                static_cast<long long>(std::count_if(lights->second.begin(), lights->second.end(),
                    [](uint64_t key) { return shadowmaps::Lights[key].Slot < 0; })),
                emitter.ShownLights.size(), static_cast<unsigned long long>(emitter.HandoverStart));
        }
        // a set whose maps take too long is shown as it is, its lights each drawn once their map is
        if (!emitter.HandoverStart)
            emitter.HandoverStart = shadowmaps::FrameNumber;
        if (built == static_cast<std::ptrdiff_t>(lights->second.size())
            || emitter.HandoverStart + HANDOVER_FRAMES <= shadowmaps::FrameNumber)
        {
            emitter.ShownLights = lights->second;
            emitter.HandoverStart = 0;
            continue;
        }

        std::erase_if(emitter.ShownLights, [](uint64_t key) { return !shadowmaps::Lights.contains(key); });
        for (uint64_t key : emitter.ShownLights)
        {
            // picked lights were marked seen above
            shadowmaps::Light &light = shadowmaps::Lights[key];
            light.GlowHeld = light.LastSeen != shadowmaps::FrameNumber;
            if(light.GlowHeld){if(!heldLeft){light.LastSeen=0;continue;}--heldLeft;}
            light.LastSeen = shadowmaps::FrameNumber;
        }
    }
    for (const auto &[casterKey, emitter] : Emitters)
        for (uint64_t key : emitter.ShownLights)
            if(shadowmaps::Lights[key].LastSeen==shadowmaps::FrameNumber)shadowmaps::Lights[key].GlowShown = true;
    ParticleAllocationReady=true;
}

// the screen rectangle that holds the cube of this half-width about an absolute position, or the whole screen when it
// reaches behind the camera
static bool FindLightRect(const XMFLOAT4X4 &worldToClip, const XMFLOAT3 &position, float range, UINT width, UINT height,
    D3D11_RECT &rect)
{
    float low[2] = { FLT_MAX, FLT_MAX }, high[2] = { -FLT_MAX, -FLT_MAX };
    for (int corner = 0; corner < 8; ++corner)
    {
        const float x = position.x + shadowmaps::CurrentOffset.x + (corner & 1 ? range : -range);
        const float y = position.y + shadowmaps::CurrentOffset.y + (corner & 2 ? range : -range);
        const float z = position.z + shadowmaps::CurrentOffset.z + (corner & 4 ? range : -range);
        const float w = worldToClip._41 * x + worldToClip._42 * y + worldToClip._43 * z + worldToClip._44;
        if (w <= shadowmaps::NEAR_PLANE)
        {
            rect = { 0, 0, static_cast<LONG>(width), static_cast<LONG>(height) };
            return true;
        }
        const float clipX = (worldToClip._11 * x + worldToClip._12 * y + worldToClip._13 * z + worldToClip._14) / w;
        const float clipY = (worldToClip._21 * x + worldToClip._22 * y + worldToClip._23 * z + worldToClip._24) / w;
        low[0] = std::min(low[0], clipX);
        low[1] = std::min(low[1], clipY);
        high[0] = std::max(high[0], clipX);
        high[1] = std::max(high[1], clipY);
    }
    if (high[0] < -1 || low[0] > 1 || high[1] < -1 || low[1] > 1)
        return false;

    rect.left = static_cast<LONG>(std::floor((std::max(low[0], -1.0f) + 1) * 0.5f * width));
    rect.right = static_cast<LONG>(std::ceil((std::min(high[0], 1.0f) + 1) * 0.5f * width));
    rect.top = static_cast<LONG>(std::floor((1 - std::min(high[1], 1.0f)) * 0.5f * height));
    rect.bottom = static_cast<LONG>(std::ceil((1 - std::max(low[1], -1.0f)) * 0.5f * height));
    return rect.left < rect.right && rect.top < rect.bottom;
}

// makes a screen-sized texture with a render target and a view, unless the one held already fits
static bool CreateScreenTexture(ID3D11Device *dev, UINT width, UINT height, DXGI_FORMAT format, ComPtr<ID3D11Texture2D> &texture,
    ComPtr<ID3D11RenderTargetView> &target, ComPtr<ID3D11ShaderResourceView> &view)
{
    D3D11_TEXTURE2D_DESC desc = {};
    if (texture)
        texture->GetDesc(&desc);
    if (texture && desc.Width == width && desc.Height == height)
        return true;

    texture.Reset();
    target.Reset();
    view.Reset();
    desc = { width, height, 1, 1, format, { 1, 0 }, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET };
    return SUCCEEDED(dev->CreateTexture2D(&desc, nullptr, &texture))
        && SUCCEEDED(dev->CreateRenderTargetView(texture.Get(), nullptr, &target))
        && SUCCEEDED(dev->CreateShaderResourceView(texture.Get(), nullptr, &view));
}

// adds the glow lights with drawn maps to the light buffer, once the game's lamps are in it, and marks where they reach
static void DrawLights(ID3D11DeviceContext *ctx,ID3D11ShaderResourceView *materialNormals=nullptr)
{
    LightsDrawnThisFrame = 0;
    ComPtr<ID3D11DeviceContext1> ctx1;
    if (!SourcesActive() || CreateFailed || !UpsampleShader || !shadowmaps::CubeViews[0] || !shadowmaps::OffsetKnown
        || !lightbuffer::LightBufferTarget || !lightbuffer::DepthCopyView || FAILED(ctx->QueryInterface(IID_PPV_ARGS(&ctx1))))
        return;

    D3D11_TEXTURE2D_DESC desc;
    lightbuffer::LightBuffer->GetDesc(&desc);
    ComPtr<ID3D11Device> dev;
    ctx->GetDevice(&dev);
    if (!CreateScreenTexture(dev.Get(), desc.Width, desc.Height, DXGI_FORMAT_R8_UNORM, MaskTexture, MaskTarget, MaskView))
        return;

    const FLOAT clear[4] = {};
    ctx1->ClearRenderTargetView(MaskTarget.Get(), clear);
    MaskFrame = shadowmaps::FrameNumber;

    XMFLOAT4X4 worldToClip;
    XMStoreFloat4x4(&worldToClip, lightbuffer::WorldToClip);
    LightConstants constants = {};
    constants.FaceSizes = { static_cast<float>(shadowmaps::FACE_SIZES[0]), static_cast<float>(shadowmaps::FACE_SIZES[1]),
        static_cast<float>(shadowmaps::FACE_SIZES[2]), static_cast<float>(shadowmaps::MOVING_FACE_SIZE) };
    D3D11_RECT area = { LONG_MAX, LONG_MAX, LONG_MIN, LONG_MIN };
    int count = 0;
    const bool logging = capture::State == capture::Phase::Capturing && capture::Log;
    const int drawLimit=static_cast<int>(emission_policy::ClampGlowLimit(LightLimit.load()));
    for (int slot = 0; slot < shadowmaps::MAX_LIGHTS && count < drawLimit; ++slot)
    {
        auto it = shadowmaps::Lights.find(shadowmaps::SlotLights[slot]);
        D3D11_RECT rect;
        if (!shadowmaps::SlotActive[slot] || it == shadowmaps::Lights.end() || !it->second.GlowShown || !it->second.BuiltFrame
            || (Emitters.contains(it->second.EmitterKey) ? !ObjectsActive() : !ParticlesActive())
            || !FindLightRect(worldToClip, it->second.BuiltPosition, shadowmaps::FindShadowRadius(it->second) * BOUNCE_REACH,
                desc.Width, desc.Height, rect))
            continue;

        const shadowmaps::Light &light = it->second;
        const float reach = shadowmaps::FindShadowRadius(light);
        const int size = shadowmaps::GetSizeIndex(slot);
        // full-size slots also have a moving-object map, whose cube is the slot's place in its array
        const int particleMap=shadowmaps::particle_moving::Binding(slot,light);
        const bool movingUsed = particleMap<0 && slot < shadowmaps::SIZE_STARTS[1] && shadowmaps::MovingMapUsed[slot];
        constants.Lights[count++] = { { light.BuiltPosition.x, light.BuiltPosition.y, light.BuiltPosition.z, static_cast<float>(size) },
            { light.GlowColour.x, light.GlowColour.y, light.GlowColour.z, reach },
            { light.GlowNormal.x, light.GlowNormal.y, light.GlowNormal.z, static_cast<float>(slot - shadowmaps::SIZE_STARTS[size]) },
            { movingUsed ? 1.0f : 0.0f, light.GlowArea / XM_PI, static_cast<float>(slot), reach * BOUNCE_REACH },
            { light.Radius, light.Particle ? 1.0f : 0.0f, particleMap<0?0.0f:float(particleMap%32+1),particleMap<0?0.0f:float(particleMap/32) } };
        area = { std::min(area.left, rect.left), std::min(area.top, rect.top), std::max(area.right, rect.right),
            std::max(area.bottom, rect.bottom) };
        if (logging)
            fprintf(capture::Log, "--- glow light slot %d at %.2f %.2f %.2f, colour %.3f %.3f %.3f, reach %.1f, normal %.2f %.2f "
                "%.2f, built frame %llu, rect %ld %ld %ld %ld, camera offset %.2f %.2f %.2f\n", slot, light.BuiltPosition.x,
                light.BuiltPosition.y, light.BuiltPosition.z, light.GlowColour.x, light.GlowColour.y, light.GlowColour.z, reach,
                light.GlowNormal.x, light.GlowNormal.y, light.GlowNormal.z, static_cast<unsigned long long>(light.BuiltFrame),
                rect.left, rect.top, rect.right, rect.bottom, shadowmaps::CurrentOffset.x, shadowmaps::CurrentOffset.y,
                shadowmaps::CurrentOffset.z);
    }
    if (!count)
        return;

    // at most half resolution, and no more than 1440 lines, as the soft sun shadow
    const float scale = std::max(MIN_REDUCED_SCALE, lightbuffer::FindReducedScale(desc.Height));
    const UINT reducedWidth = lightbuffer::FindReducedSize(desc.Width, scale);
    const UINT reducedHeight = lightbuffer::FindReducedSize(desc.Height, scale);
    if (!CreateScreenTexture(dev.Get(), reducedWidth, reducedHeight, DXGI_FORMAT_R16G16B16A16_FLOAT, ReducedLightTexture,
            ReducedLightTarget, ReducedLightView)
        || !CreateScreenTexture(dev.Get(), reducedWidth, reducedHeight, DXGI_FORMAT_R8G8B8A8_SNORM, ReducedSurfaceTexture,
            ReducedSurfaceTarget, ReducedSurfaceView))
        return;

    constants.Counts = { static_cast<float>(count), shadowmaps::NEAR_PLANE,
        std::isfinite(shadowmaps::Strength.load())?std::clamp(shadowmaps::Strength.load(),0.0f,1.0f):1.0f, shadowmaps::ShowColours ? 1.0f : 0.0f };
    FrameConstants frame;
    XMStoreFloat4x4(&frame.InverseRows, XMMatrixInverse(nullptr, lightbuffer::WorldToClip));
    frame.Size = { static_cast<float>(desc.Width), static_cast<float>(desc.Height), 1.0f / desc.Width, 1.0f / desc.Height };
    const bool legacySoftness=shadowmaps::LegacyPointSoftness.load();
    const float savedSoftness=shadowmaps::PointSoftness.load();
    const float pointSoftness=legacySoftness?(std::isfinite(savedSoftness)?std::max(savedSoftness,0.0f):0.0f):shadowmaps::CleanPointSoftness(savedSoftness);
    frame.Reduced = { scale, materialNormals?1.0f:0.0f,pointSoftness*(legacySoftness?1.0f:sun_softness_settings::Spread),legacySoftness?1.0f:0.0f };

    ComPtr<ID3DDeviceContextState> previous;
    ctx1->SwapDeviceContextState(shadowmaps::DrawState.Get(), &previous);
    ctx1->UpdateSubresource(FrameBuffer.Get(), 0, nullptr, &frame, 0, 0);
    ctx1->UpdateSubresource(LightConstantsBuffer.Get(), 0, nullptr, &constants, 0, 0);
    ctx1->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx1->VSSetShader(LightVertexShader.Get(), nullptr, 0);
    ctx1->RSSetState(LightRasterizer.Get());
    ID3D11Buffer *buffers[] = { FrameBuffer.Get(), LightConstantsBuffer.Get() };
    ctx1->PSSetConstantBuffers(0, 2, buffers);
    ID3D11Buffer *offset = shadowmaps::CameraOffsetBuffer.Get();
    ctx1->PSSetConstantBuffers(3, 1, &offset);
    ID3D11SamplerState *samplers[] = {shadowmaps::CompareSampler.Get(),shadowmaps::PointSampler.Get()};
    ctx1->PSSetSamplers(0, 2, samplers);
    ID3D11ShaderResourceView *views[] = { lightbuffer::DepthCopyView.Get(), shadowmaps::CubeViews[0].Get(),
        shadowmaps::CubeViews[1].Get(), shadowmaps::CubeViews[2].Get(), shadowmaps::MovingView.Get(),
        ReducedLightView.Get(), ReducedSurfaceView.Get(), materialNormals, shadowmaps::CubeViews[3].Get(),
        shadowmaps::particle_moving::Pool[0].view.Get(),shadowmaps::particle_moving::Pool[1].view.Get(),
        shadowmaps::particle_moving::Pool[2].view.Get(),shadowmaps::particle_moving::Pool[3].view.Get() };
    lightbuffer::StartTimer(ctx1.Get(), lightbuffer::GlowTimer);
    // texels outside the lights' rectangle are left unlit
    ctx1->ClearRenderTargetView(ReducedLightTarget.Get(), clear);
    ctx1->ClearRenderTargetView(ReducedSurfaceTarget.Get(), clear);
    ID3D11RenderTargetView *reducedTargets[] = { ReducedLightTarget.Get(), ReducedSurfaceTarget.Get() };
    ctx1->OMSetRenderTargets(2, reducedTargets, nullptr);
    const D3D11_VIEWPORT reducedViewport = { 0, 0, static_cast<float>(reducedWidth), static_cast<float>(reducedHeight), 0,
        1 };
    ctx1->RSSetViewports(1, &reducedViewport);
    const D3D11_RECT reducedArea = { static_cast<LONG>(std::floor(area.left / scale)),
        static_cast<LONG>(std::floor(area.top / scale)), static_cast<LONG>(std::ceil(area.right / scale)),
        static_cast<LONG>(std::ceil(area.bottom / scale)) };
    ctx1->RSSetScissorRects(1, &reducedArea);
    ctx1->OMSetBlendState(nullptr, nullptr, 0xffffffff);
    ctx1->PSSetShader(ReducedLightShader.Get(), nullptr, 0);
    ctx1->PSSetShaderResources(0, 13, views);
    ctx1->Draw(3, 0);
    ctx1->PSSetShader(UpsampleShader.Get(), nullptr, 0);
    ID3D11RenderTargetView *targets[] = { lightbuffer::LightBufferTarget.Get(), MaskTarget.Get() };
    ctx1->OMSetRenderTargets(2, targets, nullptr);
    const D3D11_VIEWPORT viewport = { 0, 0, static_cast<float>(desc.Width), static_cast<float>(desc.Height), 0, 1 };
    ctx1->RSSetViewports(1, &viewport);
    ctx1->RSSetScissorRects(1, &area);
    ctx1->OMSetBlendState(LightBlend.Get(), nullptr, 0xffffffff);
    ctx1->PSSetShaderResources(0, 13, views);
    ctx1->Draw(3, 0);
    lightbuffer::StopTimer(ctx1.Get(), lightbuffer::GlowTimer);
    LightsDrawnThisFrame = count;

    ID3D11ShaderResourceView *noViews[13] = {};
    ctx1->PSSetShaderResources(0, 13, noViews);
    ID3D11SamplerState *noShadowSamplers[2]{};ctx1->PSSetSamplers(0,2,noShadowSamplers);
    ctx1->IASetInputLayout(nullptr);
    ctx1->SwapDeviceContextState(previous.Get(), nullptr);
}

// Adds qualified painted glow with a constant retained share, independent of the light mask.
static void CompositePainted(ID3D11DeviceContext *ctx)
{
    ComPtr<ID3D11RenderTargetView> destination = std::move(PaintedDestination);
    ComPtr<ID3D11DeviceContext1> ctx1;
    if (!destination || FAILED(ctx->QueryInterface(IID_PPV_ARGS(&ctx1))))
        return;

    D3D11_TEXTURE2D_DESC desc;
    PaintedTexture->GetDesc(&desc);
    const XMFLOAT4 kept = { ObjectsActive() ? painted_emission_policy::Kept(PaintedKept.load()) : 1.0f, 0, 0, 0 };
    ComPtr<ID3DDeviceContextState> previous;
    ctx1->SwapDeviceContextState(shadowmaps::DrawState.Get(), &previous);
    ctx1->UpdateSubresource(CompositeBuffer.Get(), 0, nullptr, &kept, 0, 0);
    ctx1->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx1->VSSetShader(LightVertexShader.Get(), nullptr, 0);
    ctx1->PSSetShader(CompositePixelShader.Get(), nullptr, 0);
    ID3D11RenderTargetView *target = destination.Get();
    ctx1->OMSetRenderTargets(1, &target, nullptr);
    const D3D11_VIEWPORT viewport = { 0, 0, static_cast<float>(desc.Width), static_cast<float>(desc.Height), 0, 1 };
    ctx1->RSSetViewports(1, &viewport);
    ctx1->RSSetState(LightRasterizer.Get());
    const D3D11_RECT whole = { 0, 0, static_cast<LONG>(desc.Width), static_cast<LONG>(desc.Height) };
    ctx1->RSSetScissorRects(1, &whole);
    ctx1->OMSetBlendState(PaintedCompositeBlend.Get(), nullptr, 0xffffffff);
    ID3D11Buffer *buffer = CompositeBuffer.Get();
    ctx1->PSSetConstantBuffers(0, 1, &buffer);
    ID3D11ShaderResourceView *painted = PaintedView.Get();
    ctx1->PSSetShaderResources(0, 1, &painted);
    ctx1->Draw(3, 0);
    ID3D11ShaderResourceView *noViews[2] = {};
    ctx1->PSSetShaderResources(0, 2, noViews);
    ctx1->SwapDeviceContextState(previous.Get(), nullptr);
}

// Draws the qualified native pass aside; false leaves its original game draw untouched.
static bool DrawPaintedAside(ID3D11DeviceContext *ctx, const shadowmaps::DrawCall &call, const D3D11_TEXTURE2D_DESC &desc)
{
    ComPtr<ID3D11RenderTargetView> target;
    ComPtr<ID3D11DepthStencilView> depth;
    ctx->OMGetRenderTargets(1, &target, &depth);
    if (!target)
        return false;
    ComPtr<ID3D11Device> device;
    ctx->GetDevice(&device);
    if (!shadowmaps::DrawState || !CreatePaintedObjects(device.Get())) return false;
    if (PaintedDestination && PaintedDestination != target)
        CompositePainted(ctx);
    if (!PaintedDestination)
    {
        ComPtr<ID3D11Device> dev;
        ctx->GetDevice(&dev);
        if (!CreateScreenTexture(dev.Get(), desc.Width, desc.Height, DXGI_FORMAT_R16G16B16A16_FLOAT, PaintedTexture,
                PaintedTarget, PaintedView))
            return false;

        const FLOAT clear[4] = {};
        ctx->ClearRenderTargetView(PaintedTarget.Get(), clear);
        PaintedDestination = target;
    }

    ID3D11RenderTargetView *aside = PaintedTarget.Get();
    ctx->OMSetRenderTargets(1, &aside, depth.Get());
    if (call.Indexed)
        ctx->DrawIndexed(call.Count, call.First, call.BaseVertex);
    else
        ctx->Draw(call.Count, call.First);
    ID3D11RenderTargetView *restored = target.Get();
    ctx->OMSetRenderTargets(1, &restored, depth.Get());
    return true;
}

// Painted retention does not need an admitted camera, emitter, measure target,
// light list or mask. Commit this small set only after every object succeeds.
static bool CreatePaintedObjects(ID3D11Device *dev)
{
    if (CompositePixelShader && LightVertexShader && PaintedCompositeBlend && CompositeBuffer && LightRasterizer)
        return true;
    ComPtr<ID3DBlob> vertexCode, compositeCode;
    ComPtr<ID3D11VertexShader> vertex;
    ComPtr<ID3D11PixelShader> pixel;
    ComPtr<ID3D11BlendState> blend;
    ComPtr<ID3D11RasterizerState> rasterizer;
    ComPtr<ID3D11Buffer> buffer;
    D3D11_BLEND_DESC blendDesc = {};
    auto &target = blendDesc.RenderTarget[0];
    target.BlendEnable = TRUE;
    target.SrcBlend = target.DestBlend = target.SrcBlendAlpha = target.DestBlendAlpha = D3D11_BLEND_ONE;
    target.BlendOp = target.BlendOpAlpha = D3D11_BLEND_OP_ADD;
    target.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    D3D11_RASTERIZER_DESC rasterizerDesc = {};
    rasterizerDesc.FillMode = D3D11_FILL_SOLID;
    rasterizerDesc.CullMode = D3D11_CULL_NONE;
    rasterizerDesc.DepthClipEnable = TRUE;
    rasterizerDesc.ScissorEnable = TRUE;
    const D3D11_BUFFER_DESC bufferDesc = { sizeof(XMFLOAT4), D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER };
    if (!shadowmaps::CompileShader(FULL_SCREEN_SHADER, "vs_5_0", vertexCode)
        || !shadowmaps::CompileShader(COMPOSITE_SHADER, "ps_5_0", compositeCode)
        || FAILED(dev->CreatePixelShader(compositeCode->GetBufferPointer(), compositeCode->GetBufferSize(), nullptr, &pixel))
        || FAILED(dev->CreateVertexShader(vertexCode->GetBufferPointer(), vertexCode->GetBufferSize(), nullptr, &vertex))
        || FAILED(dev->CreateBlendState(&blendDesc, &blend))
        || FAILED(dev->CreateRasterizerState(&rasterizerDesc, &rasterizer))
        || FAILED(dev->CreateBuffer(&bufferDesc, nullptr, &buffer))) return false;
    CompositePixelShader = std::move(pixel);
    LightVertexShader = std::move(vertex);
    PaintedCompositeBlend = std::move(blend);
    LightRasterizer = std::move(rasterizer);
    CompositeBuffer = std::move(buffer);
    return true;
}

static bool CreateObjects(ID3D11Device *dev)
{
    if (UpsampleShader) return true;
    if (!CreatePaintedObjects(dev)) return false;
    // The light adds to the light buffer, and the mask is written as it is.
    D3D11_BLEND_DESC blendDesc = {};
    blendDesc.IndependentBlendEnable = TRUE;
    blendDesc.RenderTarget[1].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_RED;
    auto &target = blendDesc.RenderTarget[0];
    target.BlendEnable = TRUE;
    target.SrcBlend = target.DestBlend = target.SrcBlendAlpha = target.DestBlendAlpha = D3D11_BLEND_ONE;
    target.BlendOp = target.BlendOpAlpha = D3D11_BLEND_OP_ADD;
    target.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_RED | D3D11_COLOR_WRITE_ENABLE_GREEN | D3D11_COLOR_WRITE_ENABLE_BLUE;
    const D3D11_BUFFER_DESC frameDesc = { sizeof(FrameConstants), D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER };
    const D3D11_BUFFER_DESC lightDesc = { sizeof(LightConstants), D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER };
    if (FAILED(dev->CreateBlendState(&blendDesc, &LightBlend))
        || FAILED(dev->CreateBuffer(&frameDesc, nullptr, &FrameBuffer))
        || FAILED(dev->CreateBuffer(&lightDesc, nullptr, &LightConstantsBuffer))
        || !CreateShapeObjects(dev)) return false;
    return lightbuffer::CreatePixelShader(dev, "ps_reduced_light", ReducedLightShader, LIGHT_SHADER)
        && lightbuffer::CreatePixelShader(dev, "ps_upsample", UpsampleShader, LIGHT_SHADER);
}

// keepShapeBuffers keeps the staging, stream output and zero buffers, which belong to the device, not the scene
static void Reset(bool keepShapeBuffers = false)
{
    Emitters.clear();
    CellLights.clear();
    ActiveCells.clear();
    // the objects and moving meshes whose bounds were being read lose their reads, and go stale to be asked for again
    for (const ShapeRead &read : ShapeReads)
        if (read.WorldMesh)
        {
            if (auto worldMesh = shadowmaps::WorldMeshes.find(read.CasterKey);
                worldMesh != shadowmaps::WorldMeshes.end() && worldMesh->second.AskedFrame == read.Frame)
                worldMesh->second.Stale = true;
        }
        else if (read.BoundsOnly)
        {
            if (auto caster = shadowmaps::Casters.find(read.CasterKey);
                caster != shadowmaps::Casters.end() && caster->second.BoundsAskedFrame == read.Frame)
                caster->second.BoundsStale = true;
        }
    ShapeReads.clear();
    // a moving mesh with no bounds yet loses its entry with the shape buffers kept, as the scene goes on and it is
    // asked for again; otherwise the glow objects could not be made, nothing asks again, and it is judged by its origin
    if (keepShapeBuffers)
        std::erase_if(shadowmaps::WorldMeshes,
            [](const auto &entry) { return !entry.second.BoundsKnown && !entry.second.Unreadable; });
    else
        for (auto &[meshKey, worldMesh] : shadowmaps::WorldMeshes)
            if (!worldMesh.BoundsKnown)
                worldMesh.Unreadable = true;
    ShapeBuffersFailed = false;
    if (!keepShapeBuffers)
    {
        for (ShapeStaging &staging : ShapeStagingPool)
            staging = {};
        ShapeOutput.Reset();
        ShapeZeros.Reset();
    }
    std::fill(std::begin(MeasuredKeys), std::end(MeasuredKeys), 0);
    std::fill(std::begin(MeasuredTickets),std::end(MeasuredTickets),0);
    ParticleSlots=0;ParticleAllocationReady=false;emission_policy::Samples.Clear();
    MeasureTexture.Reset();
    MeasureDepth.Reset();
    for (auto &readback : MeasureReadback)
        readback.Reset();
    for (auto &readback : MeasureDepthReadback)
        readback.Reset();
    MeasureTarget.Reset();
    MeasureDepthTarget.Reset();
    MeasureDepthState.Reset();
    MeasureBlend.Reset();
    ShapeVertexShader.Reset();
    ShapeStreamShader.Reset();
    LightVertexShader.Reset();
    ReducedLightShader.Reset();
    UpsampleShader.Reset();
    LightBlend.Reset();
    PaintedCompositeBlend.Reset();
    LightRasterizer.Reset();
    FrameBuffer.Reset();
    LightConstantsBuffer.Reset();
    MaskTexture.Reset();
    MaskTarget.Reset();
    MaskView.Reset();
    MaskFrame = UINT64_MAX;
    ReducedLightTexture.Reset();
    ReducedLightTarget.Reset();
    ReducedLightView.Reset();
    ReducedSurfaceTexture.Reset();
    ReducedSurfaceTarget.Reset();
    ReducedSurfaceView.Reset();
    PaintedTexture.Reset();
    PaintedTarget.Reset();
    PaintedView.Reset();
    PaintedDestination.Reset();
    CompositePixelShader.Reset();
    CompositeBuffer.Reset();
    MeasuredThisFrame = false;
    CreateFailed = false;
}

// sums one measure's covered texels into the emitter's average and, with its frame's camera known, into the cells
// they fall in. A cell covered by at least MIN_CELL_TEXELS takes their average; the others keep theirs. Each cell above
// a changed one then becomes the area-weighted average of its cells, those never measured counting as it was, and the
// whole emitter the area-weighted average of its measured top cells
static void MeasureCells(uint64_t casterKey, Emitter &emitter, const D3D11_MAPPED_SUBRESOURCE &colours,
    const D3D11_MAPPED_SUBRESOURCE &depths, UINT slot)
{
    struct TexelSum
    {
        int X, Y, Z;
        double Colour[3];
        int Count;
    };
    std::unordered_map<uint64_t, TexelSum> sums;
    double total[3] = {};
    int covered = 0;
    const XMFLOAT4X4 &m = MeasuredClipToWorld[slot];
    const XMFLOAT3 &offset = MeasuredOffset[slot];
    const bool placed = MeasuredPlaced[slot] && emitter.ShapeKnown;
    for (UINT y = 0; y < MEASURE_HEIGHT; ++y)
    {
        const auto *colourRow = reinterpret_cast<const float *>(static_cast<const uint8_t *>(colours.pData)
            + y * colours.RowPitch);
        const auto *depthRow = reinterpret_cast<const float *>(static_cast<const uint8_t *>(depths.pData)
            + y * depths.RowPitch);
        for (UINT x = 0; x < MEASURE_WIDTH; ++x)
        {
            // covered texels have alpha 0 and a depth before the clear's 1
            const float *texel = colourRow + x * 4;
            const float depth = depthRow[x];
            if (!emission_policy::ValidPixel(texel,depth))
                continue;

            ++covered;
            for (int channel = 0; channel < 3; ++channel)
                total[channel] += texel[channel];
            if (!placed)
                continue;

            const float clipX = (x + 0.5f) / MEASURE_WIDTH * 2 - 1;
            const float clipY = 1 - (y + 0.5f) / MEASURE_HEIGHT * 2;
            float point[3];int cellX=0,cellY=0,cellZ=0;
            if(!emission_policy::Unproject(&m._11,clipX,clipY,depth,&offset.x,point)
                ||!emission_policy::Cell(point[0],CELL_SIZES[0],cellX)
                ||!emission_policy::Cell(point[1],CELL_SIZES[0],cellY)
                ||!emission_policy::Cell(point[2],CELL_SIZES[0],cellZ))continue;
            TexelSum &sum = sums.try_emplace(shadowmaps::HashCaster(0, cellX, cellY, cellZ),
                TexelSum{ cellX, cellY, cellZ }).first->second;
            for (int channel = 0; channel < 3; ++channel)
                sum.Colour[channel] += texel[channel];
            ++sum.Count;
        }
    }
    if (!covered)
        return;

    emitter.Radiance = { static_cast<float>(total[0] / covered), static_cast<float>(total[1] / covered),
        static_cast<float>(total[2] / covered), 1 };
    if (!placed)
        return;

    // cells at each size whose glow changed
    std::unordered_set<uint64_t> changed[CELL_SIZE_COUNT];
    int measured[CELL_SIZE_COUNT] = {};
    for (int size = 0; size < CELL_SIZE_COUNT; ++size)
    {
        std::unordered_map<uint64_t, TexelSum> parents;
        for (const auto &[key, sum] : sums)
        {
            auto cell = emitter.Cells[size].find(key);
            if (cell != emitter.Cells[size].end() && sum.Count >= MIN_CELL_TEXELS)
            {
                cell->second.Radiance = { static_cast<float>(sum.Colour[0] / sum.Count),
                    static_cast<float>(sum.Colour[1] / sum.Count), static_cast<float>(sum.Colour[2] / sum.Count), 1 };
                changed[size].insert(key);
                ++measured[size];
            }
            if (size + 1 == CELL_SIZE_COUNT)
                continue;

            const int x = sum.X >> 1, y = sum.Y >> 1, z = sum.Z >> 1;
            TexelSum &parent = parents.try_emplace(shadowmaps::HashCaster(size + 1, x, y, z), TexelSum{ x, y, z })
                .first->second;
            for (int channel = 0; channel < 3; ++channel)
                parent.Colour[channel] += sum.Colour[channel];
            parent.Count += sum.Count;
        }
        sums = std::move(parents);
    }

    for (int size = 1; size < CELL_SIZE_COUNT; ++size)
    {
        for (uint64_t childKey : changed[size - 1])
        {
            const Cell &child = emitter.Cells[size - 1].at(childKey);
            const uint64_t key = shadowmaps::HashCaster(size, child.X >> 1, child.Y >> 1, child.Z >> 1);
            if (emitter.Cells[size].contains(key))
                changed[size].insert(key);
        }
        for (uint64_t key : changed[size])
        {
            Cell &cell = emitter.Cells[size].at(key);
            double sum[3] = {};
            double area = 0;
            for (int dx = 0; dx < 2; ++dx)
                for (int dy = 0; dy < 2; ++dy)
                    for (int dz = 0; dz < 2; ++dz)
                    {
                        auto child = emitter.Cells[size - 1].find(shadowmaps::HashCaster(size - 1, cell.X * 2 + dx,
                            cell.Y * 2 + dy, cell.Z * 2 + dz));
                        if (child == emitter.Cells[size - 1].end())
                            continue;

                        const XMFLOAT4 radiance = FindCellRadiance(emitter, size - 1, child->second);
                        sum[0] += radiance.x * child->second.Area;
                        sum[1] += radiance.y * child->second.Area;
                        sum[2] += radiance.z * child->second.Area;
                        area += child->second.Area;
                    }
            if (area > 0)
                cell.Radiance = { static_cast<float>(sum[0] / area), static_cast<float>(sum[1] / area),
                    static_cast<float>(sum[2] / area), 1 };
        }
    }

    double sum[3] = {};
    double area = 0;
    for (const auto &[key, cell] : emitter.Cells[CELL_SIZE_COUNT - 1])
        if (cell.Radiance.w > 0)
        {
            sum[0] += cell.Radiance.x * cell.Area;
            sum[1] += cell.Radiance.y * cell.Area;
            sum[2] += cell.Radiance.z * cell.Area;
            area += cell.Area;
        }
    if (area > 0)
        emitter.Whole.Radiance = { static_cast<float>(sum[0] / area), static_cast<float>(sum[1] / area),
            static_cast<float>(sum[2] / area), 1 };

    if (capture::State == capture::Phase::Capturing && capture::Log)
    {
        fprintf(capture::Log, "--- glow measure %016llx: %d texels covered, average %.3f %.3f %.3f, whole %.3f %.3f %.3f, "
            "cells measured", static_cast<unsigned long long>(casterKey), covered, emitter.Radiance.x, emitter.Radiance.y,
            emitter.Radiance.z, emitter.Whole.Radiance.x, emitter.Whole.Radiance.y, emitter.Whole.Radiance.z);
        for (int size = 0; size < CELL_SIZE_COUNT; ++size)
            fprintf(capture::Log, " %.0f:%d", CELL_SIZES[size], measured[size]);
        fprintf(capture::Log, "\n");
    }
}

// reads the measurements copied on earlier frames that the GPU has finished, without waiting for it; the others are
// tried again next frame
static void ReadMeasure(ID3D11DeviceContext *ctx)
{
    for (UINT slot = 0; slot < READ_LATENCY; ++slot)
    {
        const uint64_t casterKey = MeasuredKeys[slot];
        if (!casterKey || slot == shadowmaps::FrameNumber % READ_LATENCY)
            continue;

        D3D11_MAPPED_SUBRESOURCE colours, depths;
        if (FAILED(ctx->Map(MeasureReadback[slot].Get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &colours)))
            continue;
        if (FAILED(ctx->Map(MeasureDepthReadback[slot].Get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &depths)))
        {
            ctx->Unmap(MeasureReadback[slot].Get(), 0);
            continue;
        }

        MeasuredKeys[slot] = 0;
        auto emitter = Emitters.find(casterKey);
        if (emitter != Emitters.end() && emission_policy::Measurement(emitter->second.Ticket,emitter->second.Painted,MeasuredTickets[slot],MeasuredPainted[slot]))
            MeasureCells(casterKey, emitter->second, colours, depths, slot);
        ctx->Unmap(MeasureDepthReadback[slot].Get(), 0);
        ctx->Unmap(MeasureReadback[slot].Get(), 0);
    }
}

// runs before the shadow maps' end of frame: reads back measurements and shapes, then picks this frame's glow lights
// a frame capture writes everything the glow code holds to glow-state.txt in the capture's folder: settings, counters,
// every glowing object with every cell, the cell lights, and the reads in flight
static void WriteState()
{
    FILE *file = nullptr;
    if (_wfopen_s(&file, (capture::Folder / L"glow-state.txt").c_str(), L"w") != 0)
        return;

    const auto ago = [](uint64_t frame) { return static_cast<long long>(shadowmaps::FrameNumber - frame); };
    fprintf(file, "enabled %d, strength %.3f, painted kept %.3f, painted light %.3f, create failed %d\n",
        Enabled.load() ? 1 : 0, Strength.load(), PaintedKept.load(), PaintedLight.load(), CreateFailed ? 1 : 0);
    fprintf(file, "emissive light limit %u, particle light limit %u, global shadowed light limit %d, particle slots %u\n",
        emission_policy::ClampGlowLimit(LightLimit.load()), emission_policy::ClampParticleLimit(ParticleLightLimit.load()),
        shadowmaps::LightLimit.load(), ParticleSlots);
    fprintf(file, "lights drawn this frame %d, last frame %d, cells picked last frame %d, painted skipped last second %d, "
        "this second %d, measured this frame %d, mask frame %lld ago\n", LightsDrawnThisFrame, LightsDrawnLastFrame,
        CellsPickedLastFrame, PaintedSkippedLastSecond, PaintedSkippedThisSecond, MeasuredThisFrame ? 1 : 0,
        MaskFrame == UINT64_MAX ? -1ll : ago(MaskFrame));
    fprintf(file, "measured keys:");
    for (UINT n = 0; n < READ_LATENCY; ++n)
        fprintf(file, " %016llx", static_cast<unsigned long long>(MeasuredKeys[n]));
    fprintf(file, "\n%zu emitters, %zu cell lights, %zu active cells, %zu shape reads pending, %zu shaders' texture slots\n",
        Emitters.size(), CellLights.size(), ActiveCells.size(), ShapeReads.size(), ShaderTextures.size());

    for (const auto &[key, emitter] : Emitters)
    {
        fprintf(file, "\nemitter %016llx%s: last seen %lld frames ago, radiance %.4f %.4f %.4f %.4f, measured %lld "
            "frames ago, shape %s, centre %.3f %.3f %.3f, size %.3f, light radius %.2f, handover from frame %llu, whole "
            "cell %d %d %d area %.3f radius %.3f\n  lights shown:", static_cast<unsigned long long>(key),
            emitter.Painted ? " (painted)" : "", ago(emitter.LastSeen),
            emitter.Radiance.x, emitter.Radiance.y, emitter.Radiance.z, emitter.Radiance.w, ago(emitter.MeasuredFrame),
            emitter.ShapeKnown ? "known" : emitter.ShapeRequested ? "requested" : "unknown", emitter.Centre.x,
            emitter.Centre.y, emitter.Centre.z, emitter.Size, emitter.Radius,
            static_cast<unsigned long long>(emitter.HandoverStart),
            emitter.Whole.X, emitter.Whole.Y, emitter.Whole.Z, emitter.Whole.Area, emitter.Whole.Radius);
        for (uint64_t light : emitter.ShownLights)
            fprintf(file, " %llu", static_cast<unsigned long long>(light));
        fprintf(file, "\n");
        for (int size = 0; size < CELL_SIZE_COUNT; ++size)
            for (const auto &[cellKey, cell] : emitter.Cells[size])
            {
                auto light = CellLights.find(HashCell(key, size, cell));
                fprintf(file, "  cell size %.0f at %d %d %d: area %.3f, centre %.3f %.3f %.3f, normal %.3f %.3f %.3f, "
                    "radius %.3f, radiance %.4f %.4f %.4f %.0f, light %llu%s\n", CELL_SIZES[size], cell.X, cell.Y, cell.Z,
                    cell.Area, cell.Centre.x, cell.Centre.y, cell.Centre.z, cell.Normal.x, cell.Normal.y, cell.Normal.z,
                    cell.Radius, cell.Radiance.x, cell.Radiance.y, cell.Radiance.z, cell.Radiance.w,
                    light == CellLights.end() ? 0ull : static_cast<unsigned long long>(light->second),
                    ActiveCells.contains(HashCell(key, size, cell)) ? ", active" : "");
            }
    }

    fprintf(file, "\n--- cell lights: cell, light\n");
    for (const auto &[cell, light] : CellLights)
        fprintf(file, "%016llx %llu\n", static_cast<unsigned long long>(cell), static_cast<unsigned long long>(light));
    fprintf(file, "\n--- shape reads: object, triangles, frames ago, bounds only\n");
    for (const ShapeRead &read : ShapeReads)
        fprintf(file, "%016llx %u %lld %d\n", static_cast<unsigned long long>(read.CasterKey), read.Triangles,
            ago(read.Frame), read.BoundsOnly ? 1 : 0);
    fprintf(file, "\n--- shader texture slots: pipeline, slot bits\n");
    for (const auto &[shader, slots] : ShaderTextures)
        fprintf(file, "%016llx %016llx\n", static_cast<unsigned long long>(shader), static_cast<unsigned long long>(slots));
    fclose(file);
}

// Retire only surface-owned lights. Particle keys are owned by their producer.
static void RetireObjects()
{
    if (!Emitters.empty())
        std::erase_if(shadowmaps::Lights, [](const auto &entry) {
            return entry.second.Glow && Emitters.contains(entry.second.EmitterKey);
        });
    Emitters.clear();CellLights.clear();ActiveCells.clear();
    std::erase_if(ShapeReads, [](const ShapeRead &read){return !read.BoundsOnly;});
    std::fill(std::begin(MeasuredKeys),std::end(MeasuredKeys),0);
    std::fill(std::begin(MeasuredTickets),std::end(MeasuredTickets),0);
}

static void EndFrame(ID3D11Device *dev, ID3D11DeviceContext *ctx,unsigned particleDemand=0)
{
    const shadowmaps::CpuScope scope(shadowmaps::GlowCpu);
    ParticleSlots=0;ParticleAllocationReady=false;
    if (capture::State == capture::Phase::Capturing) WriteState();
    if (PaintedDestination) CompositePainted(ctx);
    if (!ObjectsActive()) RetireObjects();
    MeasuredThisFrame=false;LightsDrawnLastFrame=LightsDrawnThisFrame;LightsDrawnThisFrame=0;CellsPickedLastFrame=0;
    if (std::chrono::steady_clock::now()-shadowmaps::SecondStart >= std::chrono::seconds(1))
    {PaintedSkippedLastSecond=PaintedSkippedThisSecond;PaintedSkippedThisSecond=0;}
    if (CreateFailed || !shadowmaps::DrawState) return;
    // Native point shadows still need mesh bounds to exclude a lamp's holder.
    // This does not sample source colour, create emission depth or capture particles.
    if (!CreateShapeObjects(dev)) {CreateFailed=true;return;}
    ReadShapes(ctx);
    ComPtr<ID3D11DeviceContext1> ctx1;
    if (FAILED(ctx->QueryInterface(IID_PPV_ARGS(&ctx1)))) return;
    RequestBounds(ctx1.Get());
    if (!SourcesActive()) return;
    if (!CreateObjects(dev) || (ObjectsActive() && !CreateMeasureObjects(dev)))
    {
        reshade::log::message(reshade::log::level::error,"Could not create the glow light objects.");
        Reset();CreateFailed=true;return;
    }
    if (ObjectsActive())
    {
        ReadMeasure(ctx);
        std::erase_if(Emitters, [](const auto &entry){return !shadowmaps::Casters.contains(entry.first);});
        std::erase_if(CellLights, [](const auto &entry){return !shadowmaps::Lights.contains(entry.second);});
        for (auto &[casterKey,emitter] : Emitters)
            if (!emitter.ShapeRequested) {RequestShape(ctx1.Get(),casterKey,emitter);break;}
        UpdateRadii();
    }
    if (shadowmaps::CameraFound) PickLights(ParticlesActive()?particleDemand:0);
}
}
