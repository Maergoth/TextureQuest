// Shadow cube maps for point lights. Each light draw on static geometry is recorded with the geometry it draws and its
// constants, read on the CPU. At the end of the frame the records give each object's world matrix and the light's
// position.
// The game draws light passes relative to the camera; the camera's world position is read from the game's constants each
// frame, and objects and lights are kept in world positions. Each light keeps the objects
// it was drawn with, and the nearest lights get a depth cube map drawn from those objects. A map stays in its slot until
// the slot is needed for a nearer light, and is redrawn only when its light is new to the slot or has new objects.
// The nearest lights get full-size maps and further ones smaller maps, each size in its own cube map array.
// Meshes seen moving, such as characters, go into a second, smaller cube map per light that is redrawn every frame, for
// the lights with full-size maps only.
#pragma once
#include <reshade.hpp>
#include <d3d11_1.h>
#include <d3dcompiler.h>
#include <DirectXMath.h>
#include <wrl/client.h>
#include <algorithm>
#include <atomic>
#include <bitset>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <cstring>
#include <optional>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include "lamp_holder_policy.hpp"
#include "emission_policy.hpp"
#include "sun_softness_settings.hpp"
#include "shadow_face_culling.hpp"

namespace shadowmaps
{
using Microsoft::WRL::ComPtr;
using namespace DirectX;

constexpr int MAX_LIGHTS = static_cast<int>(emission_policy::GlowLimit);
// the first slot of each map size, nearest lights first; a size's slots run up to the next size's first slot
constexpr int SIZE_STARTS[] = { 0, 16, 64, 256 };
constexpr UINT FACE_SIZES[] = { 1024, 512, 256, 256 };
constexpr int SIZE_COUNT = 4;
static_assert((SIZE_STARTS[1]-SIZE_STARTS[0])*6 <= D3D11_REQ_TEXTURE2D_ARRAY_AXIS_DIMENSION);
static_assert((SIZE_STARTS[2]-SIZE_STARTS[1])*6 <= D3D11_REQ_TEXTURE2D_ARRAY_AXIS_DIMENSION);
static_assert((SIZE_STARTS[3]-SIZE_STARTS[2])*6 <= D3D11_REQ_TEXTURE2D_ARRAY_AXIS_DIMENSION);
static_assert((MAX_LIGHTS-SIZE_STARTS[3])*6 <= D3D11_REQ_TEXTURE2D_ARRAY_AXIS_DIMENSION);
constexpr UINT MOVING_FACE_SIZE = 512;
constexpr UINT MAX_MOVING = 256;
// the moving maps' faces, six for each of the nearest size's slots
constexpr UINT MOVING_SLICES = SIZE_STARTS[1] * 6;
constexpr UINT MAX_RECORDS = 4096;
// constants c[3..13]: object-to-clip rows, world rows, then the light position
constexpr UINT FIRST_ROW = 3;
constexpr UINT ROW_COUNT = 11;
constexpr UINT RECORD_SIZE = ROW_COUNT * 16;
constexpr float NEAR_PLANE = 0.05f;
constexpr uint64_t CASTER_LIFETIME = 1800;
constexpr uint64_t LIGHT_LIFETIME = 600;
// a lamp remembered for moving meshes stays while it was drawn, or the camera was within its reach, this recently
constexpr uint64_t REMEMBERED_LAMP_FRAMES = 1800;
constexpr uint64_t REFRESH_INTERVAL = 120;
// an object counts toward a light once it has been seen in this many frames, which skips one-off dynamic geometry
constexpr uint32_t CONFIRM_FRAMES = 3;
// objects whose bounds are smaller across than this, such as particles, cast no shadow worth drawing and are not cached
constexpr float MIN_CASTER_SIZE = 0.1f;
// frames in a row without the world drawn that count as a loading screen, after which the cache starts over
constexpr uint32_t LOADING_FRAMES = 60;
// light positions closer than this are the same light; flickering lights move a little
constexpr float LIGHT_MERGE_DISTANCE = 0.5f;
// lamp lights closer than this are the same light whatever their hue; a lamp's colour can drift as it flickers
constexpr float SAME_SPOT_DISTANCE = 0.05f;
// a light drawn this close to a mapped light is taken to be that light
constexpr float LIGHT_MATCH_DISTANCE = 1.0f;
// a light that has moved further than this from where its map was drawn, such as a carried torch, is redrawn every frame
constexpr float MOVED_LIGHT_DISTANCE = 0.1f;
// a cached object closer than this to a new sighting of its mesh is the same object, across hash grid cells
constexpr float CASTER_MATCH_DISTANCE = 0.1f;
constexpr UINT MAX_PROBES = 256;
// a light drawn within this many frames counts as in view
constexpr uint64_t RECENT_FRAMES = 10;
// how far a moving mesh may reach from its origin; characters are drawn in pieces smaller than this
constexpr float MESH_RADIUS = 2.0f;
// cached objects further than this from the camera are not probed
constexpr float PROBE_DISTANCE = 200;
// cached objects not seen for a while are dropped once the camera is further than this from them, and a camera jump this
// far is a zone change or a teleport
constexpr float KEEP_DISTANCE = 400;
// a cached object not seen for this long is dropped wherever it is, unless a light holds it
constexpr std::chrono::minutes CASTER_AGE_LIMIT { 10 };
// how often, in frames, objects are checked against CASTER_AGE_LIMIT
constexpr uint64_t AGE_CHECK_FRAMES = 60;
// a moving mesh the game stopped drawing is held while it is within this distance of the camera
constexpr float HELD_DISTANCE = 100;
// the game's fade covers a cylinder of a lamp's reach in radius and half height, whose rim is this many reaches away
constexpr float FADE_EXTENT = 1.41421356f;
// the shadow radius of a lamp whose reach cannot be read; the largest lamp reach seen is about 71
constexpr float FALLBACK_LAMP_RADIUS = 100.0f;
// a light's objects are gathered again once its shadow radius grows past the radius they were gathered within by more
// than rounding and flicker, or shrinks well below it
constexpr float RADIUS_GROWTH = 1.01f;
constexpr float RADIUS_SHRINK = 0.75f;
// the window maps: their size, the most kept at once, and how long one stays once the game stops drawing its light
constexpr UINT WINDOW_MAP_SIZE = 1024;
constexpr size_t MAX_WINDOWS = 8;
constexpr uint64_t WINDOW_LIFETIME = 600;
// a window map holds distance along the beam up to this many times the length of the beam's fade
constexpr float WINDOW_DEPTH_RANGE = 2.0f;
// how far in front of the window a window map starts, in world units, which leaves the window's own frame and glass out
constexpr float WINDOW_MARGIN = 0.25f;
// how far behind the nearest object along the beam a surface must be to be in its shadow, in world units
constexpr float WINDOW_BIAS = 0.05f;
// the radius of the window shadow's filter, in map texels
constexpr float WINDOW_FILTER_TEXELS = 1.5f;
// windows whose rows give the same picture and beam to within this distance, in world units, are the same window
constexpr float WINDOW_MATCH_DISTANCE = 0.25f;
// frames between sending probes and reading their results
constexpr UINT PROBE_LATENCY = 3;
// frames between a GPU copy of a mesh's triangles or a glow measure and reading it on the CPU
constexpr UINT READ_LATENCY = 3;

static const XMVECTORF32 FACE_DIRECTIONS[6] = { { 1, 0, 0 }, { -1, 0, 0 }, { 0, 1, 0 }, { 0, -1, 0 }, { 0, 0, 1 },
    { 0, 0, -1 } };
static const XMVECTORF32 FACE_UPS[6] = { { 0, 1, 0 }, { 0, 1, 0 }, { 0, 0, -1 }, { 0, 0, 1 }, { 0, 1, 0 }, { 0, 1, 0 } };

constexpr const char *CASTER_SHADER = R"(
cbuffer Caster : register(b0) { float4 World[3]; float4 ViewProjection[4]; };

float4 main(float3 position : POSITION) : SV_Position
{
    float4 p = float4(position, 1);
    float4 w = float4(dot(p, World[0]), dot(p, World[1]), dot(p, World[2]), 1);
    return float4(dot(w, ViewProjection[0]), dot(w, ViewProjection[1]), dot(w, ViewProjection[2]), dot(w, ViewProjection[3]));
}
)";

// draws a moving mesh with its world rows from this frame, moved to absolute positions using this frame's camera offset
constexpr const char *MOVING_CASTER_SHADER = R"(
cbuffer Caster : register(b0) { float4 ViewProjection[4]; uint4 Index; };
cbuffer Camera : register(b3) { float4 CameraOffset; };
ByteAddressBuffer Worlds : register(t13);

float4 main(float3 position : POSITION) : SV_Position
{
    uint start = Index.x * 48;
    float4 row0 = asfloat(Worlds.Load4(start));
    float4 row1 = asfloat(Worlds.Load4(start + 16));
    float4 row2 = asfloat(Worlds.Load4(start + 32));
    float4 p = float4(position, 1);
    float4 w = float4(float3(dot(p, row0), dot(p, row1), dot(p, row2)) - CameraOffset.xyz, 1);
    return float4(dot(w, ViewProjection[0]), dot(w, ViewProjection[1]), dot(w, ViewProjection[2]), dot(w, ViewProjection[3]));
}
)";

// draws a moving mesh into every moving-map face it reaches in one instanced draw: instance n draws into the slice at
// Slices[Index.y + n] (a light's slot times 6 plus the face), with that slice's view-projection rows from FaceRows.
// SV_InstanceID does not include the draw's start instance, so the list's start comes in Index.y.
constexpr const char *INSTANCED_CASTER_SHADER = R"(
cbuffer Caster : register(b0) { uint4 Index; };
cbuffer Camera : register(b3) { float4 CameraOffset; };
ByteAddressBuffer Worlds : register(t13);
StructuredBuffer<float4> FaceRows : register(t14);
Buffer<uint> Slices : register(t15);

struct Output
{
    float4 position : SV_Position;
    uint slice : SV_RenderTargetArrayIndex;
};

Output main(float3 position : POSITION, uint instance : SV_InstanceID)
{
    uint start = Index.x * 48;
    float4 row0 = asfloat(Worlds.Load4(start));
    float4 row1 = asfloat(Worlds.Load4(start + 16));
    float4 row2 = asfloat(Worlds.Load4(start + 32));
    float4 p = float4(position, 1);
    float4 w = float4(float3(dot(p, row0), dot(p, row1), dot(p, row2)) - CameraOffset.xyz, 1);
    Output output;
    output.slice = Slices[Index.y + instance];
    uint row = output.slice * 4;
    output.position = float4(dot(w, FaceRows[row]), dot(w, FaceRows[row + 1]), dot(w, FaceRows[row + 2]),
        dot(w, FaceRows[row + 3]));
    return output;
}
)";

// tests whether each probe lies in front of the depth buffer at its pixel, so that nothing hides it
constexpr const char *PROBE_SHADER = R"(
Texture2D<float> Depth : register(t0);
StructuredBuffer<float4> Probes : register(t1);
RWBuffer<uint> Visible : register(u0);

[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    float4 probe = Probes[id.x];
    Visible[id.x] = probe.z <= Depth.Load(int3(probe.xy, 0)) ? 1 : 0;
}
)";

struct DrawCall
{
    bool Indexed;
    uint32_t Count, Instances, First;
    int32_t BaseVertex;
    uint32_t FirstInstance;
};

struct Geometry
{
    ComPtr<ID3D11Buffer> VertexBuffer, IndexBuffer;
    ComPtr<ID3D11InputLayout> Layout;
    UINT Stride, VertexOffset, IndexOffset;
    DXGI_FORMAT IndexFormat;
    D3D11_PRIMITIVE_TOPOLOGY Topology;
    DrawCall Call;
    uint64_t Key;
    // whether the vertex or index buffer is one the game rewrites, whose contents at a place can change at any time
    bool Dynamic = false;
};

struct Caster
{
    Geometry Mesh;
    float World[12];
    uint64_t LastSeen;
    uint32_t FramesSeen;
    uint64_t LastProbed;
    // probes in a row that found the object's spot in view while the game did not draw it
    uint32_t Misses;
    // the bounds of the object's triangles in world positions, once read back
    XMFLOAT3 Low, High;
    bool BoundsKnown = false;
    // set once the bounds are asked for, read or not, and the frame they were last asked for; a read asked for on
    // another frame, or before the object's buffers were last rewritten, is ignored
    bool BoundsRequested = false;
    uint64_t BoundsAskedFrame = 0;
    // set when the object's buffers were rewritten after its bounds were asked for; they are asked for again once the
    // writes settle, and the old bounds are used until then
    bool BoundsStale = false;
    // when the game last drew the object, by the clock, so its age does not depend on the frame rate
    std::chrono::steady_clock::time_point SeenTime;
};

struct Light
{
    XMFLOAT3 Position;
    // the light's colour over its brightest channel, which flicker leaves alone; it tells apart lights that come close,
    // such as a carried torch passing a lantern
    XMFLOAT3 Hue = {};
    // the light's colour, row c11, and its reach in world units, as last drawn; a reach of 0 is not known yet
    XMFLOAT4 Colour = {};
    float Reach = 0;
    // the power of the light's fade, as its lamp's fade texture was measured; 0 is not known yet
    float FadePower = 0;
    // frames in a row the light was in view at the end of the light buffer's lamps without the game drawing it
    uint32_t MissedInView = 0;
    // the last frame the game drew the light or the camera was within its reach and a margin
    uint64_t LastInRange = 0;
    // the last frame the add-on added the light in place of the game, which keeps its map as if the game had drawn it
    uint64_t LastAdded = 0;
    // where the map was drawn from; shadows are looked up from here, so the map and the lookup agree as the light flickers
    XMFLOAT3 BuiltPosition;
    uint64_t LastSeen;
    uint64_t LastSelected;
    std::unordered_set<uint64_t> CasterKeys;
    // the shadow radius its objects were gathered within, which is also its maps' far plane; 0 before they are gathered
    float Radius = 0;
    // set while the light's objects were gathered within the fallback radius of a lamp whose reach was not known yet;
    // Radius may already be the reach-based one, and the objects are gathered again once the lamp has a slot
    bool FallbackRadius = false;
    uint64_t Version;
    // set when objects were removed from the light, so its map is redrawn on the next frame
    bool RedrawNow;
    // set when the light moved to a slot of another size, so its map is drawn at once rather than waiting its turn
    bool SlotChanged;
    int Slot = -1;
    uint64_t BuiltFrame = 0;
    uint64_t BuiltVersion = 0;
    // a light the add-on draws for a glowing surface, rather than one the game draws
    bool Glow = false;
    // Independent native particle source; kept in spare glow-shadow constants.
    bool Particle = false;
    float ParticleSourceRadius=0,BuiltParticleSourceRadius=0;
    // the glowing object's caster key, left out of the light's casters
    uint64_t EmitterKey = 0;
    // the glow's colour times its cell's area over pi
    XMFLOAT3 GlowColour = {};
    // the glowing surface's normal, or zero when it faces every way
    XMFLOAT3 GlowNormal = {};
    // the glow light's reach, which is also the range its casters are chosen within
    float GlowRange = 0;
    // the area of the glowing cell the light stands for
    float GlowArea = 0;
    // set while the glow light is drawn; a newly picked light waits until its emitter's other new lights have maps
    bool GlowShown = false;
    // set while the glow light is drawn only until its emitter's newly picked lights have maps, so it takes only the
    // slots left over
    bool GlowHeld = false;
};

struct Frame
{
    // ROW_COUNT constant rows per record
    std::vector<XMFLOAT4> Rows;
    std::vector<Geometry> Records;
    // whether each record is from a lamp pass, whose row c13 is its light's position
    std::vector<bool> Lamp;
    uint32_t BaseRecords;
    // the camera's world position while the frame was drawn
    XMFLOAT3 Camera;
    bool CameraKnown;
};

struct CasterConstants
{
    float World[12];
    XMFLOAT4X4 ViewProjection;
};

// what the lamp shaders need to turn a pixel's screen position and depth back into a camera-relative position
struct ViewConstants
{
    XMFLOAT4X4 InverseViewProjection;
    XMFLOAT4 Viewport;
    XMFLOAT4 DepthRange;
};

struct MovingConstants
{
    XMFLOAT4X4 ViewProjection;
    uint32_t Index[4];
};

struct MovingCaster
{
    Geometry Mesh;
    UINT Index;
};

#include "particle_source_shadow.hpp"

struct LightTable
{
    XMFLOAT4 Lights[MAX_LIGHTS];
    // x the maps' near plane, z the shadow strength, w 1 while shadow colours are shown
    XMFLOAT4 Params;
    XMFLOAT4 Softness;
    // x is 1 when the light's moving-object map has anything in it
    XMFLOAT4 Moving[MAX_LIGHTS];
    // x the far plane of the slot's maps, its light's shadow radius
    XMFLOAT4 FarPlanes[MAX_LIGHTS];
};
static_assert(sizeof(LightTable)==32+48*MAX_LIGHTS);
static_assert(sizeof(LightTable)<=D3D11_REQ_CONSTANT_BUFFER_ELEMENT_COUNT*16);

static Frame CurrentFrame;
static uint64_t FrameNumber = 1;
// a vertex or index buffer written from the CPU: the frame it was last written, and how many different frames it has
// been written on; a static index buffer is filled once, so one written again holds the index lists the game refills as
// it goes, such as those it builds for a moving light, and nothing drawn from it is cached. Vertex buffers are left out
// of that: a large static one may be filled over several frames as a zone loads
struct BufferWrite
{
    uint64_t Frame = 0;
    uint32_t Frames = 0;
};
// by buffer handle
static std::unordered_map<uint64_t, BufferWrite> BufferWrites;
// buffers first seen refilled this frame; cached objects drawn with one as their index buffer are dropped
static std::vector<uint64_t> NewlyRefilled;

static bool IsRefilled(ID3D11Buffer *buffer)
{
    auto found = BufferWrites.find(reinterpret_cast<uint64_t>(buffer));
    return found != BufferWrites.end() && found->second.Frames >= 2;
}
static uint64_t NextLightKey = 1;
static std::unordered_map<uint64_t, Caster> Casters;
static std::unordered_map<uint64_t, Light> Lights;
static uint64_t SlotLights[MAX_LIGHTS];
// whether the slot's light is among the lights picked this frame; slots of lights no longer picked keep their maps in
// case the light is picked again, but cast no shadow
static bool SlotActive[MAX_LIGHTS];
static XMFLOAT3 Camera;

// where a mesh was drawn in a frame; Single is false when the frame drew it at more than one position
struct MeshSighting
{
    uint64_t Frame;
    XMFLOAT3 Position;
    float Rotation[9];
    bool Single;
};

// where each mesh was when it was last checked for movement
static std::unordered_map<uint64_t, MeshSighting> Anchors;
// meshes seen moving, such as parts of characters; they are never cached
static std::unordered_set<uint64_t> MovingMeshes;
static std::unordered_set<uint64_t> BaseMeshes,SeeThroughMeshes;
static std::unordered_map<uint64_t,float> MeshSizes;
// Bounded classification ownership: never retain native buffers just for identity.
struct MeshResources{uint64_t vertex,index;};
static std::unordered_map<uint64_t,MeshResources> ClassifiedMeshes;
// the keys of the classified meshes, world meshes and objects with bounds asked for that are drawn from each buffer, by
// buffer handle, so a destroyed or rewritten buffer finds them without a scan; a key can outlive its entry, so each is
// checked against the entry's buffers
static std::unordered_map<uint64_t, std::unordered_set<uint64_t>> BufferMeshes;

static void IndexMesh(uint64_t key, uint64_t vertex, uint64_t index)
{
    if (vertex)
        BufferMeshes[vertex].insert(key);
    if (index && index != vertex)
        BufferMeshes[index].insert(key);
}
// how many sightings in a row each mesh has looked like it moved; a few moves apart from each other are not enough to
// mark it
static std::unordered_map<uint64_t, uint32_t> MoveSightings;
// how many checks in a row each moving mesh has stood still; one that stays still long enough is cached again
static std::unordered_map<uint64_t, uint32_t> StillSightings;
// whether this frame's camera position has been read yet
static bool CameraFound;
// the back buffer's size; the camera is read only from draws into a depth buffer this size
static UINT ScreenWidth, ScreenHeight;
// the camera position read in the previous frame
static XMFLOAT3 PreviousCamera;
static bool PreviousCameraKnown;
// the camera of the last frame that had one, which outlasts loading screens
static XMFLOAT3 LastCamera;
static bool LastCameraKnown;
static uint32_t FramesWithoutCamera;
// the game draws light passes relative to the camera; adding this offset to a world position gives the drawn position
static XMFLOAT3 CurrentOffset;
static bool OffsetKnown;

static ComPtr<ID3D11Texture2D> CubeTextures[SIZE_COUNT];
// indexed by slot, across the arrays of every size
static ComPtr<ID3D11DepthStencilView> FaceViews[MAX_LIGHTS * 6];
static ComPtr<ID3D11ShaderResourceView> CubeViews[SIZE_COUNT];
static ComPtr<ID3D11SamplerState> CompareSampler, PointSampler;
// CameraOffsetBuffer holds CurrentOffset, with w set to 1 once it is known
static ComPtr<ID3D11Buffer> CasterBuffer, LightTableBuffer, CameraOffsetBuffer;
// ViewBuffer holds UploadedView; SlotBuffer holds each slot's index at the start of a 256-byte block, so a slot is
// passed to a shader by binding the buffer at that slot's block, and a last block holding -1 for a light with no map
static ComPtr<ID3D11Buffer> ViewBuffer, SlotBuffer;
static ViewConstants UploadedView;
// the light table as last uploaded to LightTableBuffer
static LightTable UploadedTable;
static ComPtr<ID3D11VertexShader> CasterShader;
static ComPtr<ID3D11RasterizerState> CasterRasterizer;
static ComPtr<ID3D11DepthStencilState> CasterDepth;
static ComPtr<ID3DDeviceContextState> DrawState;
// a copy of each of the game's rasterizer states with the scissor test on, keyed by the game's state
static std::unordered_map<ID3D11RasterizerState *, std::pair<ComPtr<ID3D11RasterizerState>, ComPtr<ID3D11RasterizerState>>>
    ScissorStates;
static ComPtr<ID3D11Texture2D> MovingTexture;
static ComPtr<ID3D11DepthStencilView> MovingFaceViews[SIZE_STARTS[1] * 6];
static ComPtr<ID3D11ShaderResourceView> MovingView, MovingWorldView;
static ComPtr<ID3D11Buffer> MovingWorldBuffer, MovingConstantsBuffer;
static ComPtr<ID3D11VertexShader> MovingCasterShader;
// the instanced moving-map path, all null where the GPU cannot pick the render target slice from the vertex shader,
// which leaves the per-face drawing: a depth view over every moving face, each face's view-projection rows, the faces
// each mesh is drawn into, and the draw's constants
static ComPtr<ID3D11DepthStencilView> MovingArrayView;
static ComPtr<ID3D11Buffer> MovingFaceRowBuffer, MovingSliceBuffer, InstancedConstantsBuffer;
static ComPtr<ID3D11ShaderResourceView> MovingFaceRowView, MovingSliceView;
static ComPtr<ID3D11VertexShader> InstancedCasterShader;
// moving meshes drawn this frame, with their world rows, uploaded to MovingWorldBuffer before drawing
// the light buffer's depth and camera-relative world-to-clip transform for this frame, set when the game finishes it
static ComPtr<ID3D11ShaderResourceView> ProbeDepth;
static XMMATRIX ProbeWorldToClip;
static UINT ProbeWidth, ProbeHeight;
static ComPtr<ID3D11Buffer> ProbeBuffer, VisibleBuffer, VisibleReadback[PROBE_LATENCY];
static ComPtr<ID3D11ShaderResourceView> ProbeView;
static ComPtr<ID3D11UnorderedAccessView> VisibleView;
static ComPtr<ID3D11ComputeShader> ProbeShader;
// the objects probed into each readback buffer, and the frame they were probed
static std::vector<uint64_t> ProbedCasters[PROBE_LATENCY];
static uint64_t ProbedFrames[PROBE_LATENCY];
static std::vector<MovingCaster> MovingCasters;
static std::vector<float> MovingWorlds;
// Private projected-light extension; rendering thread, before these worlds are cleared.
static void (*DrawAdditionalMoving)(ID3D11DeviceContext1 *) = nullptr;
static void (*SceneEpochChanged)() = nullptr;
static std::unordered_set<uint64_t> MovingThisFrame;
// meshes in any recorded draw this frame
static std::unordered_set<uint64_t> DrawnMeshes;
// meshes drawn into the light buffer's base light this frame
// see-through meshes such as window glass: drawn in lamp passes but blended over the scene afterwards instead of drawn
// into the base light. They let light through, so they cast no shadow

// a moving mesh that stays in one place, such as a swinging lantern, kept in the moving-object maps from where it was
// last drawn while it is off screen and the game no longer draws it
struct HeldMesh
{
    Geometry Mesh;
    // the world rows of each copy drawn in the last lamp pass that drew it, with absolute positions
    std::vector<float> Worlds;
    // the centre of its copies when it began staying within IN_PLACE_RADIUS of it
    XMFLOAT3 Anchor = {};
    uint64_t InPlaceSince = 0;
    uint64_t LastDrawn = 0;
};
static std::unordered_map<uint64_t, HeldMesh> HeldMeshes;
static std::atomic<int> ParticleShadowLimit=50;
static int CleanParticleShadowLimit(int value){return std::clamp(value,1,int(emission_policy::ParticleLimit));}
static bool MovingMapUsed[MAX_LIGHTS];
namespace particle_moving { static void Reset();static void Request(bool,uint64_t);static void WriteState(FILE *); }

// a moving mesh whose world rows are the identity at the world origin, so its vertices are already world positions and
// its origin says nothing about where it is, with the buffers it is drawn from; an entry is made when its bounds are
// asked for
struct WorldMesh
{
    uint64_t Vertex, Index;
    XMFLOAT3 Low, High;
    bool BoundsKnown = false;
    // set when its bounds cannot be read: it is not a triangle mesh within glow::MAX_TRIANGLES, or has no triangles
    bool Unreadable = false;
    // the frame its bounds were asked for; a read asked for on another frame, for an entry since dropped or before its
    // buffers were last rewritten, is ignored
    uint64_t AskedFrame = 0;
    // set when its buffers were rewritten after its bounds were asked for; they are asked for again once the writes
    // settle, and the old bounds are used until then
    bool Stale = false;
};
// by mesh key; never holds a mesh whose buffers the game rewrites as it draws, which is Dynamic
static std::unordered_map<uint64_t, WorldMesh> WorldMeshes;
constexpr float WORLD_IDENTITY[12] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0 };

// a slot a lamp draw can take its shadow from, as it stood at the end of the last frame, in slot order
struct ShadowSlot
{
    int Slot;
    XMFLOAT3 Position;
    XMFLOAT3 Hue;
};
static std::vector<ShadowSlot> ShadowSlots;

// a directional light the game projects through a window, such as sunlight through stained glass. Its picture and fade
// follow rows that give an absolute position its picture coordinates u and v and its distance along the beam s, 0 at
// the window and 1 at the end of the fade, with no perspective. Its map holds, over the picture, the distance along the
// beam to the nearest object, drawn from the cached objects in the beam, and redrawn every frame a moving mesh is in it.
struct Window
{
    XMFLOAT4 Rows[3];
    // the frame the game last drew its light, and the frame its map was last drawn, 0 before it is
    uint64_t LastDrawn = 0;
    uint64_t BuiltFrame = 0;
    // the cached objects in the beam, and the frame they were found, again each REFRESH_INTERVAL frames as objects are
    // cached and their bounds read
    std::vector<uint64_t> CasterKeys;
    uint64_t CastersFound = 0;
    // whether the last drawing of the map held a moving mesh, which the next drawing clears away
    bool HadMoving = false;
    ComPtr<ID3D11Texture2D> Texture;
    ComPtr<ID3D11DepthStencilView> DepthView;
    ComPtr<ID3D11ShaderResourceView> View;
    // the rows and map parameters for the shadow pass, as WindowConstants
    ComPtr<ID3D11Buffer> Constants;
};

// the shadow pass's window constants: the rows, then the depth bias in s plus where the map starts in s, a filter radius
// in u and v, and the scale from s to the map's depth
struct WindowConstants
{
    XMFLOAT4 Rows[3];
    XMFLOAT4 Params;
};

static std::vector<Window> Windows;
static int WindowMapsDrawnThisSecond, WindowMapsDrawnLastSecond;
static bool CreateFailed;

static std::atomic<int> LightLimit = 50;
static std::atomic<bool> MovingShadows = true;
static std::atomic<float> Strength = 1.0f;
// paints each light's shadow result in solid colours instead of darkening the light
static std::atomic<bool> ShowColours;
// how soft point light shadows are, in multiples of the real sun's angular size, the same scale as the sun's softness:
// a shadow's edge spreads by this much of the real sun's spread per world unit between its caster and the surface
static std::atomic<float> PointSoftness = sun_softness_settings::Default;
// Old ShadowSoftness was a world-space size, not the new angular scale.
// Keep its filter math until a deliberate edit writes PointShadowSoftness.
static std::atomic<bool> LegacyPointSoftness = false;
constexpr float POINT_SOFTNESS_MAXIMUM = 4.0f;
static float CleanPointSoftness(float value)
{ return std::isfinite(value) ? std::clamp(value,0.0f,POINT_SOFTNESS_MAXIMUM) : sun_softness_settings::Default; }
static int MapsDrawnThisSecond, MapsDrawnLastSecond, CastersAddedThisSecond, CastersAddedLastSecond;
static int ExtraDrawsThisSecond, ExtraDrawsLastSecond, MovingDrawsThisSecond, MovingDrawsLastSecond;
// moving-map faces drawn into, which equals moving draws without instancing
static int MovingFacesThisSecond, MovingFacesLastSecond;
static int LampShaderDrawsThisSecond, LampShaderDrawsLastSecond;
static int StaticCasterDrawsThisSecond, StaticCasterDrawsLastSecond;
// lamp draws whose constants gave no reach
static int NoReachLampsThisSecond, NoReachLampsLastSecond;
static int FramesThisSecond, FramesLastSecond;
static std::chrono::steady_clock::time_point SecondStart;

// CPU time the add-on spends inside the game's Direct3D calls, in nanoseconds, summed over a second; atomic, as
// pipelines can be created on other threads
struct CpuTimer
{
    std::atomic<long long> ThisSecond, LastSecond;
};

// adds the time until the end of its scope to a timer
static std::atomic<bool> CpuProfileEnabled=false;
struct CpuScope
{
    CpuTimer &Timer;
    std::chrono::steady_clock::time_point Start{};
    bool Active=false;

    explicit CpuScope(CpuTimer &timer) : Timer(timer)
    {
        Active=CpuProfileEnabled.load(std::memory_order_relaxed)&&tq_world::Work.Tracking();
        if(Active)Start=std::chrono::steady_clock::now();
    }
    ~CpuScope()
    {
        if(!Active)return;
        Timer.ThisSecond += std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - Start).count();
    }
};

// StaticMapCpu and MovingMapCpu are parts of MapCpu, the lamp and window shadow maps' end of frame: StaticMapCpu the
// cube maps and the objects gathered again for them, MovingMapCpu the held moving meshes, the window maps and the
// moving-object maps
static CpuTimer DrawCpu, BindCpu, LightBufferCpu, MapCpu, StaticMapCpu, MovingMapCpu, GlowCpu, PresentCpu, ShaderSortCpu;
static CpuTimer *const CPU_TIMERS[] = { &DrawCpu, &BindCpu, &LightBufferCpu, &MapCpu, &StaticMapCpu, &MovingMapCpu,
    &GlowCpu, &PresentCpu, &ShaderSortCpu };
static int CpuFramesThisSecond, CpuFramesLastSecond;
static double CpuSecondMilliseconds;
static std::chrono::steady_clock::time_point CpuSecondStart;

// counts a frame, and once a second has passed keeps the second's CPU times; separate from the shadow maps' counters,
// which stop while point light shadows are off
static void EndCpuFrame()
{
    if(!CpuProfileEnabled.load(std::memory_order_relaxed)||!tq_world::Work.Tracking())return;
    ++CpuFramesThisSecond;
    const auto now = std::chrono::steady_clock::now();
    if (now - CpuSecondStart < std::chrono::seconds(1))
        return;

    for (CpuTimer *timer : CPU_TIMERS)
        timer->LastSecond = timer->ThisSecond.exchange(0);
    CpuFramesLastSecond = CpuFramesThisSecond;
    CpuFramesThisSecond = 0;
    CpuSecondMilliseconds = std::chrono::duration<double, std::milli>(now - CpuSecondStart).count();
    CpuSecondStart = now;
}

// a timer's average milliseconds per frame over the last second
static double GetCpuMilliseconds(const CpuTimer &timer)
{
    return timer.LastSecond / 1e6 / std::max(CpuFramesLastSecond, 1);
}

// the GPU time from the first Start to the last Stop in a frame, read back a few frames later so nothing waits on it
struct GpuTimer
{
    static constexpr int FRAMES = 4;
    ComPtr<ID3D11Query> Disjoint[FRAMES], Begin[FRAMES], End[FRAMES];
    bool Pending[FRAMES] = {};
    bool Started = false;
    float Milliseconds = 0;
};
// the GPU time of drawing the lamp shadow maps, static and moving, and the window maps
static GpuTimer MapTimer;
static int TimerFrame;

static void StartTimer(ID3D11DeviceContext *ctx, GpuTimer &timer)
{
    if (timer.Started)
        return;

    if (!timer.Disjoint[TimerFrame])
    {
        ComPtr<ID3D11Device> dev;
        ctx->GetDevice(&dev);
        D3D11_QUERY_DESC disjoint = { D3D11_QUERY_TIMESTAMP_DISJOINT };
        D3D11_QUERY_DESC stamp = { D3D11_QUERY_TIMESTAMP };
        if (FAILED(dev->CreateQuery(&disjoint, &timer.Disjoint[TimerFrame]))
            || FAILED(dev->CreateQuery(&stamp, &timer.Begin[TimerFrame]))
            || FAILED(dev->CreateQuery(&stamp, &timer.End[TimerFrame])))
        {
            timer.Disjoint[TimerFrame].Reset();
            return;
        }
    }
    ctx->Begin(timer.Disjoint[TimerFrame].Get());
    ctx->End(timer.Begin[TimerFrame].Get());
    timer.Started = true;
}

static void StopTimer(ID3D11DeviceContext *ctx, GpuTimer &timer)
{
    if (timer.Started)
        ctx->End(timer.End[TimerFrame].Get());
}

// closes this frame's measurement and reads the oldest one if the GPU has finished it, averaged over about 20 frames
static void FinishTimer(ID3D11DeviceContext *ctx, GpuTimer &timer)
{
    if (timer.Started)
    {
        ctx->End(timer.Disjoint[TimerFrame].Get());
        timer.Pending[TimerFrame] = true;
        timer.Started = false;
    }
    const int oldest = (TimerFrame + 1) % GpuTimer::FRAMES;
    if (!timer.Pending[oldest])
        return;

    D3D11_QUERY_DATA_TIMESTAMP_DISJOINT disjoint;
    UINT64 begin = 0, end = 0;
    if (ctx->GetData(timer.Disjoint[oldest].Get(), &disjoint, sizeof(disjoint), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK
        || ctx->GetData(timer.Begin[oldest].Get(), &begin, sizeof(begin), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK
        || ctx->GetData(timer.End[oldest].Get(), &end, sizeof(end), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK)
        return;

    timer.Pending[oldest] = false;
    if (disjoint.Disjoint || end < begin)
        return;

    const float milliseconds = static_cast<float>(end - begin) * 1000.0f / static_cast<float>(disjoint.Frequency);
    timer.Milliseconds += (milliseconds - timer.Milliseconds) * 0.05f;
}

static uint64_t HashValue(uint64_t hash, uint64_t value)
{
    return (hash ^ value) * 0x100000001b3;
}

static bool CompileShader(const char *source, const char *target, ComPtr<ID3DBlob> &code)
{
    ComPtr<ID3DBlob> errors;
    if (FAILED(D3DCompile(source, strlen(source), "caster", nullptr, nullptr, "main", target,
            D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors)))
    {
        if (errors)
            reshade::log::message(reshade::log::level::error, static_cast<const char *>(errors->GetBufferPointer()));
        return false;
    }
    return true;
}

static bool CreateVertexShader(ID3D11Device *dev, const char *source, ComPtr<ID3D11VertexShader> &out)
{
    ComPtr<ID3DBlob> code;
    return CompileShader(source, "vs_5_0", code)
        && SUCCEEDED(dev->CreateVertexShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &out));
}

static bool CreateProbeObjects(ID3D11Device *dev)
{
    D3D11_BUFFER_DESC probeDesc = { MAX_PROBES * 16, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE, 0,
        D3D11_RESOURCE_MISC_BUFFER_STRUCTURED, 16 };
    D3D11_BUFFER_DESC visibleDesc = { MAX_PROBES * 4, D3D11_USAGE_DEFAULT, D3D11_BIND_UNORDERED_ACCESS };
    D3D11_BUFFER_DESC readbackDesc = { MAX_PROBES * 4, D3D11_USAGE_STAGING, 0, D3D11_CPU_ACCESS_READ };
    D3D11_UNORDERED_ACCESS_VIEW_DESC visibleViewDesc = {};
    visibleViewDesc.Format = DXGI_FORMAT_R32_UINT;
    visibleViewDesc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
    visibleViewDesc.Buffer = { 0, MAX_PROBES, 0 };
    if (FAILED(dev->CreateBuffer(&probeDesc, nullptr, &ProbeBuffer))
        || FAILED(dev->CreateBuffer(&visibleDesc, nullptr, &VisibleBuffer))
        || FAILED(dev->CreateShaderResourceView(ProbeBuffer.Get(), nullptr, &ProbeView))
        || FAILED(dev->CreateUnorderedAccessView(VisibleBuffer.Get(), &visibleViewDesc, &VisibleView)))
        return false;
    for (auto &readback : VisibleReadback)
        if (FAILED(dev->CreateBuffer(&readbackDesc, nullptr, &readback)))
            return false;

    ComPtr<ID3DBlob> code;
    return CompileShader(PROBE_SHADER, "cs_5_0", code)
        && SUCCEEDED(dev->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &ProbeShader));
}

// the index into FACE_SIZES of a slot's map size
static int GetSizeIndex(int slot)
{
    int index = 0;
    while (index + 1 < SIZE_COUNT && slot >= SIZE_STARTS[index + 1])
        ++index;
    return index;
}

static int GetSizeEnd(int index)
{
    return index + 1 < SIZE_COUNT ? SIZE_STARTS[index + 1] : MAX_LIGHTS;
}

// a depth cube map array of count cubes: one view per face for drawing, and a cube array view for sampling
static bool CreateCubeArray(ID3D11Device *dev, UINT size, UINT count, ComPtr<ID3D11Texture2D> &texture,
    ComPtr<ID3D11DepthStencilView> *faceViews, ComPtr<ID3D11ShaderResourceView> &view)
{
    D3D11_TEXTURE2D_DESC desc = { size, size, 1, count * 6, DXGI_FORMAT_R32_TYPELESS, { 1, 0 }, D3D11_USAGE_DEFAULT,
        D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE, 0, D3D11_RESOURCE_MISC_TEXTURECUBE };
    if (FAILED(dev->CreateTexture2D(&desc, nullptr, &texture)))
        return false;

    for (UINT face = 0; face < count * 6; ++face)
    {
        D3D11_DEPTH_STENCIL_VIEW_DESC faceDesc = {};
        faceDesc.Format = DXGI_FORMAT_D32_FLOAT;
        faceDesc.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2DARRAY;
        faceDesc.Texture2DArray = { 0, face, 1 };
        if (FAILED(dev->CreateDepthStencilView(texture.Get(), &faceDesc, &faceViews[face])))
            return false;
    }

    D3D11_SHADER_RESOURCE_VIEW_DESC viewDesc = {};
    viewDesc.Format = DXGI_FORMAT_R32_FLOAT;
    viewDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURECUBEARRAY;
    viewDesc.TextureCubeArray = { 0, 1, 0, count };
    return SUCCEEDED(dev->CreateShaderResourceView(texture.Get(), &viewDesc, &view));
}

// Extra 256 maps cost 384 MiB at D32/256px; lower limits pay no texture cost.
static bool EnsureExtraCubeArray(ID3D11Device *dev)
{
    constexpr int extra = SIZE_COUNT - 1;
    return CubeViews[extra] || CreateCubeArray(dev, FACE_SIZES[extra], MAX_LIGHTS - SIZE_STARTS[extra],
        CubeTextures[extra], &FaceViews[SIZE_STARTS[extra] * 6], CubeViews[extra]);
}

static void ResetInstancedObjects()
{
    MovingArrayView.Reset();
    MovingFaceRowBuffer.Reset();
    MovingSliceBuffer.Reset();
    InstancedConstantsBuffer.Reset();
    MovingFaceRowView.Reset();
    MovingSliceView.Reset();
    InstancedCasterShader.Reset();
}

// makes the instanced moving-map objects; false where the GPU cannot write the render target slice from the vertex
// shader or an object cannot be made, which leaves the per-face drawing
static bool CreateInstancedObjects(ID3D11Device *dev)
{
    D3D11_FEATURE_DATA_D3D11_OPTIONS3 options = {};
    if (FAILED(dev->CheckFeatureSupport(D3D11_FEATURE_D3D11_OPTIONS3, &options, sizeof(options)))
        || !options.VPAndRTArrayIndexFromAnyShaderFeedingRasterizer)
        return false;

    D3D11_DEPTH_STENCIL_VIEW_DESC arrayDesc = {};
    arrayDesc.Format = DXGI_FORMAT_D32_FLOAT;
    arrayDesc.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2DARRAY;
    arrayDesc.Texture2DArray = { 0, 0, MOVING_SLICES };
    if (FAILED(dev->CreateDepthStencilView(MovingTexture.Get(), &arrayDesc, &MovingArrayView)))
        return false;

    D3D11_BUFFER_DESC rowDesc = { MOVING_SLICES * 4 * 16, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE, 0,
        D3D11_RESOURCE_MISC_BUFFER_STRUCTURED, 16 };
    D3D11_BUFFER_DESC sliceDesc = { MAX_MOVING * MOVING_SLICES * 4, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE };
    D3D11_BUFFER_DESC constantsDesc = { 16, D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER };
    if (FAILED(dev->CreateBuffer(&rowDesc, nullptr, &MovingFaceRowBuffer))
        || FAILED(dev->CreateBuffer(&sliceDesc, nullptr, &MovingSliceBuffer))
        || FAILED(dev->CreateBuffer(&constantsDesc, nullptr, &InstancedConstantsBuffer)))
        return false;

    D3D11_SHADER_RESOURCE_VIEW_DESC rowViewDesc = {};
    rowViewDesc.Format = DXGI_FORMAT_UNKNOWN;
    rowViewDesc.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
    rowViewDesc.Buffer = { 0, MOVING_SLICES * 4 };
    D3D11_SHADER_RESOURCE_VIEW_DESC sliceViewDesc = {};
    sliceViewDesc.Format = DXGI_FORMAT_R32_UINT;
    sliceViewDesc.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
    sliceViewDesc.Buffer = { 0, MAX_MOVING * MOVING_SLICES };
    return SUCCEEDED(dev->CreateShaderResourceView(MovingFaceRowBuffer.Get(), &rowViewDesc, &MovingFaceRowView))
        && SUCCEEDED(dev->CreateShaderResourceView(MovingSliceBuffer.Get(), &sliceViewDesc, &MovingSliceView))
        && CreateVertexShader(dev, INSTANCED_CASTER_SHADER, InstancedCasterShader);
}

static bool CreateMovingObjects(ID3D11Device *dev)
{
    if (!CreateCubeArray(dev, MOVING_FACE_SIZE, SIZE_STARTS[1], MovingTexture, MovingFaceViews, MovingView))
        return false;

    D3D11_BUFFER_DESC worldDesc = { MAX_MOVING * 48, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE, 0,
        D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS };
    D3D11_BUFFER_DESC constantsDesc = { sizeof(MovingConstants), D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER };
    if (FAILED(dev->CreateBuffer(&worldDesc, nullptr, &MovingWorldBuffer))
        || FAILED(dev->CreateBuffer(&constantsDesc, nullptr, &MovingConstantsBuffer)))
        return false;

    D3D11_SHADER_RESOURCE_VIEW_DESC worldViewDesc = {};
    worldViewDesc.Format = DXGI_FORMAT_R32_TYPELESS;
    worldViewDesc.ViewDimension = D3D11_SRV_DIMENSION_BUFFEREX;
    worldViewDesc.BufferEx = { 0, MAX_MOVING * 12, D3D11_BUFFEREX_SRV_FLAG_RAW };
    if (FAILED(dev->CreateShaderResourceView(MovingWorldBuffer.Get(), &worldViewDesc, &MovingWorldView)))
        return false;

    if (!CreateInstancedObjects(dev))
        ResetInstancedObjects();
    return CreateVertexShader(dev, MOVING_CASTER_SHADER, MovingCasterShader);
}

static bool CreateObjects(ID3D11Device *dev)
{
    ComPtr<ID3D11Device1> dev1;
    if (FAILED(dev->QueryInterface(IID_PPV_ARGS(&dev1))))
        return false;

    // Preserve the original allocation until a selected light needs the extra tier.
    for (int index = 0; index < SIZE_COUNT - 1; ++index)
        if (!CreateCubeArray(dev, FACE_SIZES[index], GetSizeEnd(index) - SIZE_STARTS[index], CubeTextures[index],
                &FaceViews[SIZE_STARTS[index] * 6], CubeViews[index]))
            return false;

    D3D11_SAMPLER_DESC samplerDesc = {};
    samplerDesc.Filter = D3D11_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT;
    samplerDesc.AddressU = samplerDesc.AddressV = samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    samplerDesc.ComparisonFunc = D3D11_COMPARISON_LESS_EQUAL;
    samplerDesc.MaxLOD = D3D11_FLOAT32_MAX;
    if (FAILED(dev->CreateSamplerState(&samplerDesc, &CompareSampler)))
        return false;

    samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    samplerDesc.ComparisonFunc = D3D11_COMPARISON_NEVER;
    if (FAILED(dev->CreateSamplerState(&samplerDesc, &PointSampler)))
        return false;

    D3D11_BUFFER_DESC casterDesc = { sizeof(CasterConstants), D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER };
    D3D11_BUFFER_DESC tableDesc = { sizeof(LightTable), D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER };
    D3D11_BUFFER_DESC offsetDesc = { 16, D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER };
    const XMFLOAT4 noOffset = { 0, 0, 0, 0 };
    const D3D11_SUBRESOURCE_DATA noOffsetData = { &noOffset };
    D3D11_BUFFER_DESC viewDesc = { sizeof(ViewConstants), D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER };
    std::vector<XMFLOAT4> slots((MAX_LIGHTS + 1) * 16);
    for (int slot = 0; slot < MAX_LIGHTS; ++slot)
        slots[slot * 16] = { static_cast<float>(slot), 0, 0, 0 };
    slots[MAX_LIGHTS * 16] = { -1, 0, 0, 0 };
    D3D11_BUFFER_DESC slotDesc = { static_cast<UINT>(slots.size() * sizeof(XMFLOAT4)), D3D11_USAGE_IMMUTABLE,
        D3D11_BIND_CONSTANT_BUFFER };
    const D3D11_SUBRESOURCE_DATA slotData = { slots.data() };
    UploadedView = {};
    if (FAILED(dev->CreateBuffer(&casterDesc, nullptr, &CasterBuffer))
        || FAILED(dev->CreateBuffer(&tableDesc, nullptr, &LightTableBuffer))
        || FAILED(dev->CreateBuffer(&offsetDesc, &noOffsetData, &CameraOffsetBuffer))
        || FAILED(dev->CreateBuffer(&viewDesc, nullptr, &ViewBuffer))
        || FAILED(dev->CreateBuffer(&slotDesc, &slotData, &SlotBuffer)))
        return false;

    if (!CreateVertexShader(dev, CASTER_SHADER, CasterShader))
        return false;

    D3D11_RASTERIZER_DESC rasterizerDesc = {};
    rasterizerDesc.FillMode = D3D11_FILL_SOLID;
    rasterizerDesc.CullMode = D3D11_CULL_NONE;
    rasterizerDesc.SlopeScaledDepthBias = 1.5f;
    rasterizerDesc.DepthClipEnable = TRUE;
    if (FAILED(dev->CreateRasterizerState(&rasterizerDesc, &CasterRasterizer)))
        return false;

    D3D11_DEPTH_STENCIL_DESC depthDesc = {};
    depthDesc.DepthEnable = TRUE;
    depthDesc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    depthDesc.DepthFunc = D3D11_COMPARISON_LESS;
    if (FAILED(dev->CreateDepthStencilState(&depthDesc, &CasterDepth)))
        return false;

    if (!CreateMovingObjects(dev) || !CreateProbeObjects(dev))
        return false;

    const D3D_FEATURE_LEVEL level = dev->GetFeatureLevel();
    return SUCCEEDED(dev1->CreateDeviceContextState(0, &level, 1, D3D11_SDK_VERSION, __uuidof(ID3D11Device), nullptr,
        &DrawState));
}

static void StartOver();

static void Reset()
{
    particle_source_shadow::Reset();
    particle_moving::Reset();
    CurrentFrame.Rows.clear();
    CurrentFrame.Records.clear();
    CurrentFrame.Lamp.clear();
    CurrentFrame.BaseRecords = 0;
    CurrentFrame.CameraKnown = false;
    StartOver();
    MapTimer = {};
    Lights.clear();
    std::fill(std::begin(SlotLights), std::end(SlotLights), 0);
    std::fill(std::begin(SlotActive), std::end(SlotActive), false);
    CameraOffsetBuffer.Reset();
    ViewBuffer.Reset();
    SlotBuffer.Reset();
    CameraFound = false;
    PreviousCameraKnown = false;
    OffsetKnown = false;
    for (auto &texture : CubeTextures)
        texture.Reset();
    for (auto &view : FaceViews)
        view.Reset();
    for (auto &view : CubeViews)
        view.Reset();
    CompareSampler.Reset();
    PointSampler.Reset();
    CasterBuffer.Reset();
    LightTableBuffer.Reset();
    CasterShader.Reset();
    CasterRasterizer.Reset();
    CasterDepth.Reset();
    DrawState.Reset();
    ScissorStates.clear();
    MovingTexture.Reset();
    for (auto &view : MovingFaceViews)
        view.Reset();
    MovingView.Reset();
    MovingWorldView.Reset();
    MovingWorldBuffer.Reset();
    MovingConstantsBuffer.Reset();
    MovingCasterShader.Reset();
    ResetInstancedObjects();
    ProbeDepth.Reset();
    ProbeBuffer.Reset();
    VisibleBuffer.Reset();
    for (auto &readback : VisibleReadback)
        readback.Reset();
    ProbeView.Reset();
    VisibleView.Reset();
    ProbeShader.Reset();
    for (auto &probed : ProbedCasters)
        probed.clear();
    MovingCasters.clear();
    MovingWorlds.clear();
    MovingThisFrame.clear();
    DrawnMeshes.clear();
    BaseMeshes.clear();
    SeeThroughMeshes.clear();
    Windows.clear();
    std::fill(std::begin(MovingMapUsed), std::end(MovingMapUsed), false);
    ShadowSlots.clear();
    FramesWithoutCamera=0;
    CreateFailed = false;
}

static uint64_t HashMesh(const Geometry &mesh)
{
    uint64_t hash = 0xcbf29ce484222325;
    hash = HashValue(hash, reinterpret_cast<uint64_t>(mesh.VertexBuffer.Get()));
    hash = HashValue(hash, reinterpret_cast<uint64_t>(mesh.IndexBuffer.Get()));
    hash = HashValue(hash, mesh.VertexOffset);
    hash = HashValue(hash, mesh.IndexOffset);
    hash = HashValue(hash, mesh.Call.Count);
    hash = HashValue(hash, mesh.Call.First);
    return HashValue(hash, static_cast<uint32_t>(mesh.Call.BaseVertex));
}

static float FindScale(const float *world)
{
    float scale = 0;
    for (int row = 0; row < 3; ++row)
        scale = std::max(scale, std::sqrt(world[row * 4] * world[row * 4] + world[row * 4 + 1] * world[row * 4 + 1]
            + world[row * 4 + 2] * world[row * 4 + 2]));
    return scale;
}

// an object is its mesh in an eighth-of-a-unit grid cell; its rotation is left out because billboards turn to face the
// camera
static uint64_t HashCaster(uint64_t meshKey, int64_t x, int64_t y, int64_t z)
{
    return HashValue(HashValue(HashValue(meshKey, static_cast<uint64_t>(x)), static_cast<uint64_t>(y)),
        static_cast<uint64_t>(z));
}

static float Distance(const XMFLOAT3 &a, const XMFLOAT3 &b)
{
    const float dx = a.x - b.x;
    const float dy = a.y - b.y;
    const float dz = a.z - b.z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

// finds the key of the cached object for a mesh at this absolute position. The offset error can put a sighting in a
// neighbouring grid cell, so those are searched before a new key is made.
static uint64_t FindCasterKey(uint64_t meshKey, const XMFLOAT3 &position)
{
    const int64_t x = std::llround(position.x * 8.0);
    const int64_t y = std::llround(position.y * 8.0);
    const int64_t z = std::llround(position.z * 8.0);
    const uint64_t key = HashCaster(meshKey, x, y, z);
    if (Casters.contains(key))
        return key;

    for (int64_t dx = -1; dx <= 1; ++dx)
        for (int64_t dy = -1; dy <= 1; ++dy)
            for (int64_t dz = -1; dz <= 1; ++dz)
            {
                auto caster = Casters.find(HashCaster(meshKey, x + dx, y + dy, z + dz));
                if (caster == Casters.end())
                    continue;

                const float *world = caster->second.World;
                if (Distance({ world[3], world[7], world[11] }, position) < CASTER_MATCH_DISTANCE)
                    return caster->first;
            }
    return key;
}

static void UploadOffset(ID3D11DeviceContext *ctx)
{
    const XMFLOAT4 offset = { CurrentOffset.x, CurrentOffset.y, CurrentOffset.z, OffsetKnown ? 1.0f : 0.0f };
    ctx->UpdateSubresource(CameraOffsetBuffer.Get(), 0, nullptr, &offset, 0, 0);
}

// reads the camera's world position from the frame's first draw into a screen-sized depth buffer that holds it. Most of
// the game's vertex constants keep it in row c16, with row c19 set to 10 100 1000 10000. Draws into smaller targets
// can hold the previous frame's camera, or the mirrored camera of water reflections.
static void ReadCamera(ID3D11DeviceContext *ctx)
{
    constexpr UINT CAMERA_ROW = 16;
    constexpr UINT MARKER_ROW = 19;
    ComPtr<ID3D11DeviceContext1> ctx1;
    if (CameraFound || !CameraOffsetBuffer || !ScreenWidth || FAILED(ctx->QueryInterface(IID_PPV_ARGS(&ctx1))))
        return;

    ComPtr<ID3D11DepthStencilView> depthView;
    ctx->OMGetRenderTargets(0, nullptr, &depthView);
    if (!depthView)
        return;
    ComPtr<ID3D11Resource> depthResource;
    depthView->GetResource(&depthResource);
    ComPtr<ID3D11Texture2D> depthTexture;
    if (FAILED(depthResource.As(&depthTexture)))
        return;
    D3D11_TEXTURE2D_DESC depthDesc;
    depthTexture->GetDesc(&depthDesc);
    if (depthDesc.Width != ScreenWidth || depthDesc.Height != ScreenHeight)
        return;

    ComPtr<ID3D11Buffer> constants;
    UINT firstConstant = 0, constantCount = 0;
    ctx1->VSGetConstantBuffers1(0, 1, &constants, &firstConstant, &constantCount);
    if (!constants)
        return;

    D3D11_BUFFER_DESC desc;
    constants->GetDesc(&desc);
    const UINT start = firstConstant * 16;
    if (desc.Usage != D3D11_USAGE_DYNAMIC || (constantCount && constantCount <= MARKER_ROW)
        || start + (MARKER_ROW + 1) * 16 > desc.ByteWidth)
        return;

    D3D11_MAPPED_SUBRESOURCE mapped;
    if (FAILED(ctx->Map(constants.Get(), 0, D3D11_MAP_WRITE_NO_OVERWRITE, 0, &mapped)))
        return;
    const auto *rows = reinterpret_cast<const XMFLOAT4 *>(static_cast<const uint8_t *>(mapped.pData) + start);
    const XMFLOAT4 camera = rows[CAMERA_ROW];
    const XMFLOAT4 marker = rows[MARKER_ROW];
    ctx->Unmap(constants.Get(), 0);
    if (marker.x != 10 || marker.y != 100 || marker.z != 1000 || marker.w != 10000 || std::abs(camera.w - 0.0001f) > 1e-6f)
        return;

    Camera = { camera.x, camera.y, camera.z };
    CurrentOffset = { -camera.x, -camera.y, -camera.z };
    OffsetKnown = true;
    CameraFound = true;
    CurrentFrame.Camera = Camera;
    CurrentFrame.CameraKnown = true;
    UploadOffset(ctx);
}

// reads a draw's vertex constant rows c[3..13]. The game writes them from the CPU, so they are read from the buffer's
// memory; a GPU copy per draw cost about a quarter of the GPU's time.
static bool ReadDrawRows(ID3D11DeviceContext *ctx, XMFLOAT4 (&rows)[ROW_COUNT])
{
    ComPtr<ID3D11DeviceContext1> ctx1;
    if (FAILED(ctx->QueryInterface(IID_PPV_ARGS(&ctx1))))
        return false;

    ComPtr<ID3D11Buffer> constants;
    UINT firstConstant = 0, constantCount = 0;
    ctx1->VSGetConstantBuffers1(0, 1, &constants, &firstConstant, &constantCount);
    if (!constants)
        return false;

    D3D11_BUFFER_DESC desc;
    constants->GetDesc(&desc);
    const UINT start = (firstConstant + FIRST_ROW) * 16;
    if (desc.Usage != D3D11_USAGE_DYNAMIC || start + RECORD_SIZE > desc.ByteWidth)
        return false;

    D3D11_MAPPED_SUBRESOURCE mapped;
    if (FAILED(ctx->Map(constants.Get(), 0, D3D11_MAP_WRITE_NO_OVERWRITE, 0, &mapped)))
        return false;
    std::memcpy(rows, static_cast<const uint8_t *>(mapped.pData) + start, RECORD_SIZE);
    ctx->Unmap(constants.Get(), 0);
    return true;
}

// a game draw being handled, with the state bound for it read from the context the first time a handler asks, so the
// draw's handlers read each part once. It lives only while its draw is handled: whatever binds other state for the draw
// puts it back before the next handler, and whatever binds other vertex constants calls ForgetRows
struct BoundDraw
{
    ID3D11DeviceContext *Context;
    DrawCall Call;
    // whether lightbuffer::OnAnyDraw has followed this draw
    bool LightBufferNoted = false;

    // the first viewport, with how many are bound up to one
    const D3D11_VIEWPORT &GetViewport(UINT &count)
    {
        if (!ViewportRead)
        {
            ViewportCount = 1;
            Context->RSGetViewports(&ViewportCount, &Viewport);
            ViewportRead = true;
        }
        count = ViewportCount;
        return Viewport;
    }

    // the blend state's description, null with none bound, and the sample mask
    const D3D11_BLEND_DESC *GetBlend(UINT *sampleMask = nullptr)
    {
        if (!BlendRead)
        {
            ComPtr<ID3D11BlendState> blend;
            Context->OMGetBlendState(&blend, nullptr, &SampleMask);
            if (blend)
                blend->GetDesc(&BlendDesc);
            HasBlend = blend != nullptr;
            BlendRead = true;
        }
        if (sampleMask)
            *sampleMask = SampleMask;
        return HasBlend ? &BlendDesc : nullptr;
    }

    // the depth-stencil state's description, null with none bound
    const D3D11_DEPTH_STENCIL_DESC *GetDepth()
    {
        if (!DepthRead)
        {
            ComPtr<ID3D11DepthStencilState> depth;
            Context->OMGetDepthStencilState(&depth, nullptr);
            if (depth)
                depth->GetDesc(&DepthDesc);
            HasDepth = depth != nullptr;
            DepthRead = true;
        }
        return HasDepth ? &DepthDesc : nullptr;
    }

    // the vertex and index buffers drawn from, keyed by HashMesh
    const Geometry &GetMesh()
    {
        if (!MeshRead)
        {
            Context->IAGetVertexBuffers(0, 1, &Mesh.VertexBuffer, &Mesh.Stride, &Mesh.VertexOffset);
            Context->IAGetIndexBuffer(&Mesh.IndexBuffer, &Mesh.IndexFormat, &Mesh.IndexOffset);
            Mesh.Call = Call;
            Mesh.Key = HashMesh(Mesh);
            MeshRead = true;
        }
        return Mesh;
    }

    D3D11_PRIMITIVE_TOPOLOGY GetTopology()
    {
        if (!TopologyRead)
        {
            Context->IAGetPrimitiveTopology(&Topology);
            TopologyRead = true;
        }
        return Topology;
    }

    // reads the draw's vertex constant rows into Rows once; false when they cannot be read
    bool ReadRows()
    {
        if (!RowsTried)
        {
            RowsValid = ReadDrawRows(Context, Rows);
            RowsTried = true;
        }
        return RowsValid;
    }

    void ForgetRows()
    {
        RowsTried = false;
    }

    XMFLOAT4 Rows[ROW_COUNT] = {};
    D3D11_VIEWPORT Viewport = {};
    UINT ViewportCount = 0, SampleMask = 0;
    D3D11_BLEND_DESC BlendDesc = {};
    D3D11_DEPTH_STENCIL_DESC DepthDesc = {};
    Geometry Mesh = {};
    D3D11_PRIMITIVE_TOPOLOGY Topology = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
    bool ViewportRead = false, BlendRead = false, HasBlend = false, DepthRead = false, HasDepth = false, MeshRead = false;
    bool TopologyRead = false, RowsTried = false, RowsValid = false;
};

// how many times further than the game's own reach a lamp's light carries
constexpr float LIGHT_REACH = 1.0f;
// the constant windows already stretched, with the frame they were stretched on, so a window drawn twice is stretched once
static std::unordered_map<uint64_t, uint64_t> StretchedWindows;

// widens a lamp draw's light. The game fades a lamp's light by projecting fade textures onto the draw's object, through
// rows c20 to c35 that put the light at texture coordinate 0.5; each such row is scaled about the light so the fade
// reaches LIGHT_REACH times as far. Only objects the game draws for the light receive it. The colour pass of a two-pass
// lamp is not stretched here; its replacement shader fades over LIGHT_REACH times its coordinates' reach instead.
static void StretchLight(ID3D11DeviceContext *ctx)
{
    constexpr UINT FIRST_PROJECTION_ROW = 20;
    constexpr UINT LAST_ROW = 36;
    if constexpr (LIGHT_REACH == 1.0f)
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
    if (desc.Usage != D3D11_USAGE_DYNAMIC || (firstConstant + LAST_ROW) * 16 > desc.ByteWidth)
        return;

    // ponytail: a window reused after the game wraps its buffer within a frame is left unstretched for that frame
    uint64_t &stretched = StretchedWindows[reinterpret_cast<uint64_t>(constants.Get()) + firstConstant * 16ull];
    if (stretched == FrameNumber)
        return;
    stretched = FrameNumber;

    D3D11_MAPPED_SUBRESOURCE mapped;
    if (FAILED(ctx->Map(constants.Get(), 0, D3D11_MAP_WRITE_NO_OVERWRITE, 0, &mapped)))
        return;

    auto *rows = reinterpret_cast<XMFLOAT4 *>(static_cast<uint8_t *>(mapped.pData) + firstConstant * 16);
    // the light in the object's own space, through the inverse of its world rows c7 to c9
    const XMMATRIX world(rows[7].x, rows[8].x, rows[9].x, 0, rows[7].y, rows[8].y, rows[9].y, 0, rows[7].z, rows[8].z,
        rows[9].z, 0, rows[7].w, rows[8].w, rows[9].w, 1);
    XMVECTOR determinant;
    const XMMATRIX inverse = XMMatrixInverse(&determinant, world);
    if (std::abs(XMVectorGetX(determinant)) > 1e-12f)
    {
        XMFLOAT3 light;
        XMStoreFloat3(&light, XMVector3TransformCoord(XMVectorSet(rows[13].x, rows[13].y, rows[13].z, 1), inverse));
        for (UINT row = FIRST_PROJECTION_ROW; row < LAST_ROW; ++row)
        {
            XMFLOAT4 &projection = rows[row];
            if (projection.x == 0 && projection.y == 0 && projection.z == 0)
                continue;

            const float centre = projection.x * light.x + projection.y * light.y + projection.z * light.z + projection.w;
            if (std::abs(centre - 0.5f) > 0.01f)
                continue;

            projection = { projection.x / LIGHT_REACH, projection.y / LIGHT_REACH, projection.z / LIGHT_REACH,
                0.5f + (projection.w - 0.5f) / LIGHT_REACH };
        }
    }
    ctx->Unmap(constants.Get(), 0);
}

// a lamp draw's light colour, row c11, over its brightest channel
static XMFLOAT3 FindHue(const XMFLOAT4 &colour)
{
    const float brightest = std::max({ colour.x, colour.y, colour.z, 1e-6f });
    return { colour.x / brightest, colour.y / brightest, colour.z / brightest };
}

// whether two hues are of the same light; a flickering light's hue drifts a few hundredths
static bool IsSameHue(const XMFLOAT3 &a, const XMFLOAT3 &b)
{
    constexpr float HUE_TOLERANCE = 0.1f;
    return std::abs(a.x - b.x) < HUE_TOLERANCE && std::abs(a.y - b.y) < HUE_TOLERANCE && std::abs(a.z - b.z) < HUE_TOLERANCE;
}

// whether two lamp lights this far apart with these hues are one light: within SAME_SPOT_DISTANCE whatever their hues,
// beyond it only when their hues match
static bool IsSameLamp(float distance, const XMFLOAT3 &a, const XMFLOAT3 &b)
{
    return distance < SAME_SPOT_DISTANCE || IsSameHue(a, b);
}

// finds the slot of the nearest light within LIGHT_MATCH_DISTANCE of a lamp drawn at this camera-relative position whose
// map has anything in it, of the lamp's hue unless it is within SAME_SPOT_DISTANCE, or -1
static int FindShadowSlot(const XMFLOAT4 &relative, const XMFLOAT4 &colour)
{
    if (!OffsetKnown)
        return -1;

    const XMFLOAT3 hue = FindHue(colour);
    const XMFLOAT3 position = { relative.x - CurrentOffset.x, relative.y - CurrentOffset.y, relative.z - CurrentOffset.z };
    int nearest = -1;
    float nearestDistance = LIGHT_MATCH_DISTANCE;
    for (const ShadowSlot &candidate : ShadowSlots)
    {
        const float distance = Distance(candidate.Position, position);
        if (!(distance < nearestDistance) || !IsSameLamp(distance, candidate.Hue, hue))
            continue;

        // a see-through mesh noted during the frame can take a light's last caster
        if (!MovingMapUsed[candidate.Slot])
        {
            auto light = Lights.find(SlotLights[candidate.Slot]);
            if (light == Lights.end() || light->second.CasterKeys.empty())
                continue;
        }
        nearest = candidate.Slot;
        nearestDistance = distance;
    }
    return nearest;
}

// lists the slots FindShadowSlot can pick; run once the frame's maps are drawn
static void ListShadowSlots()
{
    ShadowSlots.clear();
    for (int slot = 0; slot < MAX_LIGHTS; ++slot)
    {
        auto light = Lights.find(SlotLights[slot]);
        if (!SlotActive[slot] || light == Lights.end() || light->second.Glow || !light->second.BuiltFrame
            || (light->second.CasterKeys.empty() && !MovingMapUsed[slot]))
            continue;

        ShadowSlots.push_back({ slot, light->second.BuiltPosition, light->second.Hue });
    }
}

// finds a light draw's camera-relative world-to-clip transform from its object-to-clip rows and world rows
static bool FindWorldToClip(const XMFLOAT4 (&rows)[ROW_COUNT], XMMATRIX &worldToClip)
{
    const XMMATRIX objectToClip = XMLoadFloat4x4(reinterpret_cast<const XMFLOAT4X4 *>(&rows[0]));
    const XMMATRIX world(XMLoadFloat4(&rows[4]), XMLoadFloat4(&rows[5]), XMLoadFloat4(&rows[6]), g_XMIdentityR3);
    XMVECTOR determinant;
    const XMMATRIX inverseWorld = XMMatrixInverse(&determinant, world);
    if (std::abs(XMVectorGetX(determinant)) < 1e-12f)
        return false;

    worldToClip = XMMatrixMultiply(objectToClip, inverseWorld);
    return true;
}

// uploads the inverse of a light draw's world-to-clip transform and its viewport when they differ from the last upload.
// Each draw rebuilds the transform from its own rows, so values within rounding error count as the same.
static void UpdateView(ID3D11DeviceContext *ctx, const XMFLOAT4 (&rows)[ROW_COUNT], const D3D11_VIEWPORT &viewport)
{
    constexpr float TOLERANCE = 1e-5f;

    XMMATRIX worldToClip;
    if (!FindWorldToClip(rows, worldToClip))
        return;

    ViewConstants view;
    XMStoreFloat4x4(&view.InverseViewProjection, XMMatrixInverse(nullptr, worldToClip));
    view.Viewport = { viewport.TopLeftX, viewport.TopLeftY, viewport.Width, viewport.Height };
    view.DepthRange = { viewport.MinDepth, viewport.MaxDepth, 0, 0 };
    const auto *values = reinterpret_cast<const float *>(&view);
    const auto *uploaded = reinterpret_cast<const float *>(&UploadedView);
    bool same = true;
    for (size_t n = 0; n < sizeof(view) / sizeof(float) && same; ++n)
        same = std::abs(values[n] - uploaded[n]) <= TOLERANCE * (1 + std::abs(uploaded[n]));
    if (same)
        return;

    UploadedView = view;
    ctx->UpdateSubresource(ViewBuffer.Get(), 0, nullptr, &view, 0, 0);
}

// finds the screen rectangle that pixels within the shadow radius of a light draw's light, whose map is in this slot, can
// cover. Returns false when that radius is off screen; leaves full set when it reaches behind the camera, where no
// rectangle bounds it.
static bool FindShadowRect(const XMFLOAT4 (&rows)[ROW_COUNT], int slot, const D3D11_VIEWPORT &viewport, D3D11_RECT &rect,
    bool &full)
{
    full = true;
    XMMATRIX worldToClip;
    if (!FindWorldToClip(rows, worldToClip))
        return true;

    worldToClip = XMMatrixTranspose(worldToClip);
    const float range = UploadedTable.FarPlanes[slot].x;
    float minX = 1e30f, minY = 1e30f, maxX = -1e30f, maxY = -1e30f;
    for (int corner = 0; corner < 8; ++corner)
    {
        const XMVECTOR point = XMVectorSet(rows[10].x + ((corner & 1) ? range : -range),
            rows[10].y + ((corner & 2) ? range : -range), rows[10].z + ((corner & 4) ? range : -range), 1);
        XMFLOAT4 clip;
        XMStoreFloat4(&clip, XMVector4Transform(point, worldToClip));
        if (clip.w <= NEAR_PLANE)
            return true;

        minX = std::min(minX, clip.x / clip.w);
        maxX = std::max(maxX, clip.x / clip.w);
        minY = std::min(minY, clip.y / clip.w);
        maxY = std::max(maxY, clip.y / clip.w);
    }

    full = false;
    if (maxX < -1 || minX > 1 || maxY < -1 || minY > 1)
        return false;

    // a couple of pixels of margin covers the game's sub-pixel offset of its geometry
    const float left = viewport.TopLeftX + (std::max(minX, -1.0f) * 0.5f + 0.5f) * viewport.Width - 2;
    const float right = viewport.TopLeftX + (std::min(maxX, 1.0f) * 0.5f + 0.5f) * viewport.Width + 2;
    const float top = viewport.TopLeftY + (0.5f - std::min(maxY, 1.0f) * 0.5f) * viewport.Height - 2;
    const float bottom = viewport.TopLeftY + (0.5f - std::max(minY, -1.0f) * 0.5f) * viewport.Height + 2;
    rect = { static_cast<LONG>(std::floor(left)), static_cast<LONG>(std::floor(top)), static_cast<LONG>(std::ceil(right)),
        static_cast<LONG>(std::ceil(bottom)) };
    return true;
}

static ID3D11RasterizerState *GetScissorState(ID3D11DeviceContext *ctx, ID3D11RasterizerState *state)
{
    auto cached = ScissorStates.find(state);
    if (cached != ScissorStates.end())
        return cached->second.second.Get();

    CD3D11_RASTERIZER_DESC desc(D3D11_DEFAULT);
    if (state)
        state->GetDesc(&desc);
    desc.ScissorEnable = TRUE;
    ComPtr<ID3D11Device> dev;
    ctx->GetDevice(&dev);
    ComPtr<ID3D11RasterizerState> scissored;
    if (FAILED(dev->CreateRasterizerState(&desc, &scissored)))
        return nullptr;

    ScissorStates[state] = { state, scissored };
    return scissored.Get();
}

// a position's picture coordinates and distance along the beam of a window
static XMFLOAT3 ProjectToWindow(const Window &window, const XMFLOAT3 &position)
{
    const XMVECTOR point = XMVectorSet(position.x, position.y, position.z, 1);
    return { XMVectorGetX(XMVector4Dot(XMLoadFloat4(&window.Rows[0]), point)),
        XMVectorGetX(XMVector4Dot(XMLoadFloat4(&window.Rows[1]), point)),
        XMVectorGetX(XMVector4Dot(XMLoadFloat4(&window.Rows[2]), point)) };
}

// a window's distance along the beam per world unit
static float GetBeamScale(const Window &window)
{
    return XMVectorGetX(XMVector3Length(XMLoadFloat4(&window.Rows[2])));
}

// whether a mesh's origin is within MESH_RADIUS of a window's beam
static bool IsNearBeam(const Window &window, const XMFLOAT3 &position)
{
    const float margin = MESH_RADIUS * GetBeamScale(window);
    const float reach = MESH_RADIUS * std::max(XMVectorGetX(XMVector3Length(XMLoadFloat4(&window.Rows[0]))),
        XMVectorGetX(XMVector3Length(XMLoadFloat4(&window.Rows[1]))));
    const XMFLOAT3 projected = ProjectToWindow(window, position);
    return projected.x > -reach && projected.x < 1 + reach && projected.y > -reach && projected.y < 1 + reach
        && projected.z > -margin && projected.z < WINDOW_DEPTH_RANGE + margin;
}

// whether a mesh's origin is near the beam of a window the game drew lately
static bool IsNearWindows(const XMFLOAT3 &position)
{
    return std::any_of(Windows.begin(), Windows.end(), [&](const Window &window)
        { return window.LastDrawn + RECENT_FRAMES >= FrameNumber && IsNearBeam(window, position); });
}

// Records only moving geometry, without treating a directional light as a point position.
static void RememberMoving(const Geometry &geometry, const XMFLOAT4 (&rows)[ROW_COUNT])
{
    if (MovingShadows && (geometry.Dynamic || MovingMeshes.contains(geometry.Key)) && MovingCasters.size() < MAX_MOVING
        && MovingThisFrame.insert(HashCaster(geometry.Key, std::llround(rows[4].w * 8.0), std::llround(rows[5].w * 8.0),
            std::llround(rows[6].w * 8.0))).second)
    {
        const float *world = &rows[4].x;
        MovingWorlds.insert(MovingWorlds.end(), world, world + 12);
        MovingCasters.push_back({ geometry, static_cast<UINT>(MovingCasters.size()) });
    }
}
static void RecordDirectionalMoving(ID3D11DeviceContext *ctx, const DrawCall &call, const XMFLOAT4 (&rows)[ROW_COUNT])
{
    if (!MovingShadows || call.Instances != 1 || MovingCasters.size() >= MAX_MOVING) return;
    Geometry geometry;
    ctx->IAGetVertexBuffers(0, 1, &geometry.VertexBuffer, &geometry.Stride, &geometry.VertexOffset);
    ctx->IAGetIndexBuffer(&geometry.IndexBuffer, &geometry.IndexFormat, &geometry.IndexOffset);
    ctx->IAGetInputLayout(&geometry.Layout); ctx->IAGetPrimitiveTopology(&geometry.Topology);
    geometry.Call=call; geometry.Key=HashMesh(geometry);
    D3D11_BUFFER_DESC desc{}; if(geometry.VertexBuffer)geometry.VertexBuffer->GetDesc(&desc);
    geometry.Dynamic=desc.Usage==D3D11_USAGE_DYNAMIC || IsRefilled(geometry.IndexBuffer.Get());
    DrawnMeshes.insert(geometry.Key); RememberMoving(geometry,rows);
}

// records a draw of a mesh with its constant rows, from a lamp pass or from the light buffer's base light
static void Record(BoundDraw &bound, const XMFLOAT4 (&rows)[ROW_COUNT], bool lamp)
{
    Frame &frame = CurrentFrame;
    if (frame.Records.size() >= MAX_RECORDS || bound.Call.Instances != 1)
        return;

    frame.Rows.insert(frame.Rows.end(), rows, rows + ROW_COUNT);
    frame.Lamp.push_back(lamp);
    if (!lamp)
        ++frame.BaseRecords;

    Geometry geometry = bound.GetMesh();
    bound.Context->IAGetInputLayout(&geometry.Layout);
    geometry.Topology = bound.GetTopology();
    D3D11_BUFFER_DESC vertexDesc = {};
    if (geometry.VertexBuffer)
        geometry.VertexBuffer->GetDesc(&vertexDesc);
    geometry.Dynamic = vertexDesc.Usage == D3D11_USAGE_DYNAMIC || IsRefilled(geometry.IndexBuffer.Get());
    DrawnMeshes.insert(geometry.Key);
    // a frame capture notes how each recorded mesh is treated, after the draw's line in its log
    if (capture::State == capture::Phase::Capturing && capture::Log)
    {
        const XMFLOAT3 position = { rows[4].w - CurrentOffset.x, rows[5].w - CurrentOffset.y, rows[6].w - CurrentOffset.z };
        auto anchor = Anchors.find(geometry.Key);
        auto moves = MoveSightings.find(geometry.Key);
        fprintf(capture::Log, "  shadow mesh %016llx at %.3f %.3f %.3f: %s, %u moves, anchor %lld frames ago %.3f away\n",
            static_cast<unsigned long long>(geometry.Key), position.x, position.y, position.z,
            geometry.Dynamic ? "rewritten buffer" : MovingMeshes.contains(geometry.Key) ? "moving" : "cached", moves == MoveSightings.end() ? 0u : moves->second,
            anchor == Anchors.end() ? -1ll : static_cast<long long>(FrameNumber - anchor->second.Frame),
            anchor == Anchors.end() ? 0.0f : Distance(anchor->second.Position, position));
    }
    if (lamp || IsNearWindows({ rows[4].w - CurrentOffset.x, rows[5].w - CurrentOffset.y, rows[6].w - CurrentOffset.z })) RememberMoving(geometry, rows);
    frame.Records.push_back(std::move(geometry));
}

// the distance from a point to the nearest point of a box
static float FindBoxDistance(const XMFLOAT3 &low, const XMFLOAT3 &high, const XMFLOAT3 &point)
{
    const XMFLOAT3 nearest = { std::clamp(point.x, low.x, high.x), std::clamp(point.y, low.y, high.y),
        std::clamp(point.z, low.z, high.z) };
    return Distance(nearest, point);
}

// the distance from a point to an object's bounds, or to its origin while its bounds are not known
static float FindCasterDistance(const Caster &caster, const XMFLOAT3 &point)
{
    if (!caster.BoundsKnown)
        return Distance({ caster.World[3], caster.World[7], caster.World[11] }, point);

    return FindBoxDistance(caster.Low, caster.High, point);
}

// whether world rows are the identity at the world origin once offset is taken from their position, which means the
// mesh's vertices are already world positions
static bool IsInWorldPositions(const float *world, const XMFLOAT3 &offset)
{
    // the offset comes from the camera as read, which may differ from the game's in the last bits
    constexpr float ORIGIN_TOLERANCE = 0.01f;
    for (int i : { 0, 1, 2, 4, 5, 6, 8, 9, 10 })
        if (world[i] != WORLD_IDENTITY[i])
            return false;
    return std::abs(world[3] - offset.x) < ORIGIN_TOLERANCE && std::abs(world[7] - offset.y) < ORIGIN_TOLERANCE
        && std::abs(world[11] - offset.z) < ORIGIN_TOLERANCE;
}

// whether every copy of a held mesh is in world positions, so its anchor says nothing about where it is
static bool IsHeldInWorldPositions(const HeldMesh &held)
{
    if (held.Worlds.empty())
        return false;
    for (size_t start = 0; start + 12 <= held.Worlds.size(); start += 12)
        if (!IsInWorldPositions(&held.Worlds[start], {}))
            return false;
    return true;
}

// how far a light's shadows can reach: a lamp's light ends at the rim of its fade, and a glow light's at its range. Any
// object that shadows a lit surface lies between it and the light, so this bounds the light's objects.
static float FindShadowRadius(const Light &light)
{
    const float radius = light.Glow ? light.GlowRange : light.Reach > 0 ? light.Reach * FADE_EXTENT : FALLBACK_LAMP_RADIUS;
    // the far plane stays past the near plane
    return std::max(radius, 2 * NEAR_PLANE);
}

// gives a light every confirmed object within its shadow radius other than its emitter, and notes that radius
static void GatherCasters(Light &light)
{
    light.Radius = FindShadowRadius(light);
    light.FallbackRadius = !light.Glow && light.Reach <= 0;
    light.CasterKeys.clear();
    for (const auto &[casterKey, caster] : Casters)
        if (casterKey != light.EmitterKey && caster.FramesSeen >= CONFIRM_FRAMES
            && FindCasterDistance(caster, light.Position) < light.Radius)
            light.CasterKeys.insert(casterKey);
}

// adds a newly confirmed object to every light whose shadow radius reaches it, other than a glow light's emitter
static void AddToNearbyLights(uint64_t casterKey, const Caster &caster)
{
    for (auto &[key, light] : Lights)
        if (casterKey != light.EmitterKey && FindCasterDistance(caster, light.Position) < light.Radius
            && light.CasterKeys.insert(casterKey).second)
            ++light.Version;
}

// finds the lamp nearest this position within this distance, of this hue unless it is within SAME_SPOT_DISTANCE, or 0;
// glow lights are never found
static uint64_t FindNearestLight(const XMFLOAT3 &position, const XMFLOAT3 &hue, float within)
{
    uint64_t nearest = 0;
    float nearestDistance = within;
    for (auto &[key, light] : Lights)
    {
        if (light.Glow)
            continue;

        const float distance = Distance(light.Position, position);
        if (distance < nearestDistance && IsSameLamp(distance, light.Hue, hue))
        {
            nearest = key;
            nearestDistance = distance;
        }
    }
    return nearest;
}

// finds the lamp at this position, or adds one holding every confirmed object within its shadow radius; glow lights are
// added by the glow code and never found here. A new lamp's reach is not known yet, so it starts with the fallback
// radius and gathers its objects again once its reach is read.
static uint64_t FindLight(const XMFLOAT3 &position, const XMFLOAT3 &hue)
{
    if (const uint64_t nearest = FindNearestLight(position, hue, LIGHT_MERGE_DISTANCE))
        return nearest;

    const uint64_t key = NextLightKey++;
    Light &light = Lights[key];
    light.Position = position;
    light.Hue = hue;
    GatherCasters(light);
    return key;
}

static void StartOver()
{
    Windows.clear();
    if(SceneEpochChanged)SceneEpochChanged();
    Casters.clear();
    BaseMeshes.clear();SeeThroughMeshes.clear();MeshSizes.clear();ClassifiedMeshes.clear();BufferMeshes.clear();
    Anchors.clear();
    MovingMeshes.clear();
    MoveSightings.clear();
    StillSightings.clear();
    WorldMeshes.clear();
    HeldMeshes.clear();
    for (auto &[key, light] : Lights)
        light.CasterKeys.clear();
    std::erase_if(Lights, [](const auto &entry) { return entry.second.Slot < 0; });
}

// notes a CPU write of a buffer the game can draw from; only vertex and index buffers are looked up in BufferWrites, so
// the constant buffers written for nearly every draw are left out
static void NoteBufferWrite(uint64_t buffer)
{
    D3D11_BUFFER_DESC desc;
    reinterpret_cast<ID3D11Buffer *>(buffer)->GetDesc(&desc);
    if ((desc.BindFlags & (D3D11_BIND_VERTEX_BUFFER | D3D11_BIND_INDEX_BUFFER)) == 0)
        return;

    BufferWrite &write = BufferWrites[buffer];
    if (write.Frame == FrameNumber)
        return;
    write.Frame = FrameNumber;
    if (++write.Frames == 2)
        NewlyRefilled.push_back(buffer);

    // bounds asked for before the buffer's first write in a frame go stale, and reads in flight are dropped. A mesh
    // drawn with it as a dynamic vertex buffer or a refilled index buffer is Dynamic and keeps no bounds. Keys whose
    // entries are gone are dropped
    auto meshes = BufferMeshes.find(buffer);
    if (meshes == BufferMeshes.end())
        return;
    const auto keepsBounds = [buffer, &desc, &write](uint64_t vertex, uint64_t index) {
        return !(vertex == buffer && desc.Usage == D3D11_USAGE_DYNAMIC) && !(index == buffer && write.Frames >= 2);
    };
    std::unordered_set<uint64_t> &keys = meshes->second;
    for (auto key = keys.begin(); key != keys.end();)
    {
        bool used = false;
        if (auto it = WorldMeshes.find(*key); it != WorldMeshes.end()
            && (it->second.Vertex == buffer || it->second.Index == buffer))
        {
            used = true;
            if (keepsBounds(it->second.Vertex, it->second.Index))
            {
                it->second.Stale = true;
                it->second.AskedFrame = 0;
            }
        }
        if (auto it = Casters.find(*key); it != Casters.end())
        {
            const uint64_t vertex = reinterpret_cast<uint64_t>(it->second.Mesh.VertexBuffer.Get());
            const uint64_t index = reinterpret_cast<uint64_t>(it->second.Mesh.IndexBuffer.Get());
            if (vertex == buffer || index == buffer)
            {
                used = true;
                if (keepsBounds(vertex, index))
                {
                    it->second.BoundsStale = true;
                    it->second.BoundsAskedFrame = 0;
                }
            }
        }
        if (auto it = ClassifiedMeshes.find(*key); it != ClassifiedMeshes.end()
            && (it->second.vertex == buffer || it->second.index == buffer))
            used = true;
        key = used ? std::next(key) : keys.erase(key);
    }
    if (keys.empty())
        BufferMeshes.erase(meshes);
}

// whether a mesh's vertex or index buffer was written from the CPU within this many frames
static bool IsWrittenWithin(const Geometry &mesh, uint64_t frames)
{
    for (const ID3D11Buffer *buffer : { mesh.VertexBuffer.Get(), mesh.IndexBuffer.Get() })
        if (auto write = BufferWrites.find(reinterpret_cast<uint64_t>(buffer));
            buffer && write != BufferWrites.end() && write->second.Frame + frames >= FrameNumber)
            return true;
    return false;
}

static void OnMapBuffer(device *, resource buffer, uint64_t, uint64_t, map_access access, void **)
{
    if (!tq_world::Work.Point())return;
    const shadowmaps::CpuScope scope(shadowmaps::BindCpu);
    if (access == map_access::write_only || access == map_access::write_discard)
        NoteBufferWrite(buffer.handle);
}

static bool OnUpdateBuffer(device *, const void *, resource buffer, uint64_t, uint64_t)
{
    if(!tq_world::Work.Point())return false;
    const shadowmaps::CpuScope scope(shadowmaps::BindCpu);
    NoteBufferWrite(buffer.handle);
    return false;
}

// forgets a destroyed buffer, so a new one at the same address starts unwritten, with the meshes drawn from it
static void OnDestroyResource(device *, resource handle)
{
    BufferWrites.erase(handle.handle);
    auto meshes = BufferMeshes.find(handle.handle);
    if (meshes == BufferMeshes.end())
        return;

    for (uint64_t key : meshes->second)
    {
        if (auto it = ClassifiedMeshes.find(key); it != ClassifiedMeshes.end()
            && (it->second.vertex == handle.handle || it->second.index == handle.handle))
        {BaseMeshes.erase(key);SeeThroughMeshes.erase(key);MeshSizes.erase(key);ClassifiedMeshes.erase(it);}
        if (auto it = WorldMeshes.find(key); it != WorldMeshes.end()
            && (it->second.Vertex == handle.handle || it->second.Index == handle.handle))
            WorldMeshes.erase(it);
    }
    BufferMeshes.erase(meshes);
}

// marks a mesh as moving when its vertices have been rewritten from the CPU lately, which is how the game animates
// characters, or when, several times, it is drawn at one position and has moved a short distance or turned since it
// was last checked. A mesh is checked against where it was up to ANCHOR_FRAMES ago, so a slow walker, which moves less
// than MIN_MOVE in a frame, still adds up to a move. Copies of a static mesh drawn in different places are usually
// further apart than a move, and do not keep showing up at a slowly changing position. Billboards turn to face the
// camera, so they count as moving too.
static void FindMovingMeshes(const Frame &frame, const XMFLOAT4 *records, const XMFLOAT3 &offset)
{
    // the game does not pose every character every frame
    constexpr uint64_t ANIMATION_FRAMES = 30;
    constexpr uint64_t ANCHOR_FRAMES = 30;
    // smaller moves and turns are rounding
    constexpr float MIN_MOVE = 0.03f;
    constexpr float MIN_TURN = 0.01f;
    constexpr float MAX_MOVE_PER_FRAME = 0.5f;
    constexpr uint32_t MOVE_SIGHTINGS = 3;
    // checks a moving mesh must stand still for to be cached again, about 600 frames
    constexpr uint32_t STILL_SIGHTINGS = 20;

    std::unordered_map<uint64_t, MeshSighting> sightings;
    std::unordered_set<uint64_t> animated;
    for (size_t i = 0; i < frame.Records.size(); ++i)
    {
        if (frame.Records[i].Dynamic)
            continue;

        auto written = BufferWrites.find(reinterpret_cast<uint64_t>(frame.Records[i].VertexBuffer.Get()));
        if (written != BufferWrites.end() && written->second.Frame + ANIMATION_FRAMES >= FrameNumber)
            animated.insert(frame.Records[i].Key);

        const float *world = &records[i * ROW_COUNT + 4].x;
        MeshSighting sighting = { FrameNumber, { world[3] - offset.x, world[7] - offset.y, world[11] - offset.z },
            { world[0], world[1], world[2], world[4], world[5], world[6], world[8], world[9], world[10] }, true };
        auto [entry, added] = sightings.try_emplace(frame.Records[i].Key, sighting);
        if (!added && Distance(entry->second.Position, sighting.Position) > 0.01f)
            entry->second.Single = false;
    }

    bool found = false;
    for (const auto &[meshKey, sighting] : sightings)
    {
        if (animated.contains(meshKey))
        {
            Anchors[meshKey] = sighting;
            MoveSightings.erase(meshKey);
            StillSightings.erase(meshKey);
            if (MovingMeshes.insert(meshKey).second)
                found = true;
            continue;
        }

        auto anchor = Anchors.find(meshKey);
        if (anchor == Anchors.end() || !anchor->second.Single || !sighting.Single)
        {
            Anchors[meshKey] = sighting;
            continue;
        }

        // not seen for a while: start again from here
        const uint64_t frames = FrameNumber - anchor->second.Frame;
        if (frames > ANCHOR_FRAMES)
        {
            anchor->second = sighting;
            continue;
        }

        const float moved = Distance(anchor->second.Position, sighting.Position);
        float turned = 0;
        for (int n = 0; n < 9; ++n)
            turned = std::max(turned, std::abs(anchor->second.Rotation[n] - sighting.Rotation[n]));

        // too far to have walked there: another copy of the mesh
        if (moved >= MAX_MOVE_PER_FRAME * frames)
        {
            MoveSightings.erase(meshKey);
            anchor->second = sighting;
            continue;
        }

        if (moved > MIN_MOVE || turned > MIN_TURN)
        {
            anchor->second = sighting;
            StillSightings.erase(meshKey);
            if (++MoveSightings[meshKey] >= MOVE_SIGHTINGS && MovingMeshes.insert(meshKey).second)
                found = true;
            continue;
        }

        // not moved yet; a slow walker may still move far enough before the anchor is due
        if (frames < ANCHOR_FRAMES)
            continue;

        anchor->second = sighting;
        MoveSightings.erase(meshKey);
        if (MovingMeshes.contains(meshKey) && ++StillSightings[meshKey] >= STILL_SIGHTINGS)
        {
            MovingMeshes.erase(meshKey);
            StillSightings.erase(meshKey);
        }
    }

    std::erase_if(Anchors, [](const auto &entry) { return entry.second.Frame + 2 * ANCHOR_FRAMES < FrameNumber; });

    if (!found)
        return;

    std::erase_if(Casters, [](const auto &entry) { return MovingMeshes.contains(entry.second.Mesh.Key); });
    for (auto &[key, light] : Lights)
        if (std::erase_if(light.CasterKeys, [](uint64_t casterKey) { return !Casters.contains(casterKey); }))
        {
            ++light.Version;
            light.RedrawNow = true;
        }
}

static void WriteVector(FILE *file, const XMFLOAT3 &v)
{
    fprintf(file, "%.3f %.3f %.3f", v.x, v.y, v.z);
}

// a buffer's usage and how many frames it was written on, for the state dump
static void WriteBuffer(FILE *file, const char *name, ID3D11Buffer *buffer)
{
    D3D11_BUFFER_DESC desc = {};
    if (buffer)
        buffer->GetDesc(&desc);
    auto written = BufferWrites.find(reinterpret_cast<uint64_t>(buffer));
    fprintf(file, " %s usage %d written on %u frames", name, static_cast<int>(desc.Usage),
        written == BufferWrites.end() ? 0u : written->second.Frames);
}

static void WriteGeometry(FILE *file, const Geometry &mesh)
{
    fprintf(file, "mesh %016llx vb %p ib %p layout %p stride %u vb offset %u ib offset %u index format %d topology %d "
        "%s count %u instances %u first %u base %d first instance %u%s", static_cast<unsigned long long>(mesh.Key),
        static_cast<void *>(mesh.VertexBuffer.Get()), static_cast<void *>(mesh.IndexBuffer.Get()),
        static_cast<void *>(mesh.Layout.Get()), mesh.Stride, mesh.VertexOffset, mesh.IndexOffset,
        static_cast<int>(mesh.IndexFormat), static_cast<int>(mesh.Topology), mesh.Call.Indexed ? "indexed" : "plain",
        mesh.Call.Count, mesh.Call.Instances, mesh.Call.First, mesh.Call.BaseVertex, mesh.Call.FirstInstance,
        mesh.Dynamic ? " rewritten" : "");
    WriteBuffer(file, "vb", mesh.VertexBuffer.Get());
    WriteBuffer(file, "ib", mesh.IndexBuffer.Get());
}

static void WriteRows(FILE *file, const float *values, int count)
{
    for (int n = 0; n < count; ++n)
        fprintf(file, n ? " %g" : "%g", values[n]);
}

// a frame capture writes everything the shadow code holds to state.txt: every setting and counter, this frame's
// recorded draws with their constant rows, every cached object, every light and slot, every mesh's movement record,
// every buffer write, this frame's moving objects and the pending probes
static void WriteState()
{
    FILE *file = nullptr;
    if (_wfopen_s(&file, (capture::Folder / L"state.txt").c_str(), L"w") != 0)
        return;

    const auto ago = [](uint64_t frame) { return static_cast<long long>(FrameNumber - frame); };
    fprintf(file, "frame %llu, screen %ux%u, camera found %d, camera ", static_cast<unsigned long long>(FrameNumber),
        ScreenWidth, ScreenHeight, CameraFound ? 1 : 0);
    WriteVector(file, Camera);
    fprintf(file, ", previous camera ");
    WriteVector(file, PreviousCamera);
    fprintf(file, " (%d), last camera ", PreviousCameraKnown ? 1 : 0);
    WriteVector(file, LastCamera);
    fprintf(file, " (%d), offset ", LastCameraKnown ? 1 : 0);
    WriteVector(file, CurrentOffset);
    fprintf(file, " (%d)\n", OffsetKnown ? 1 : 0);
    fprintf(file, "frame camera ");
    WriteVector(file, CurrentFrame.Camera);
    fprintf(file, " (%d), next light key %llu, create failed %d\n", CurrentFrame.CameraKnown ? 1 : 0,
        static_cast<unsigned long long>(NextLightKey), CreateFailed ? 1 : 0);
    fprintf(file, "last second: %d frames, %d maps drawn, %d objects added, %d extra draws, %d moving draws (%d faces), "
        "%d lamp shader draws, %d static caster draws, %d lamp draws with no reach; moving maps instanced %d\n",
        FramesLastSecond, MapsDrawnLastSecond, CastersAddedLastSecond, ExtraDrawsLastSecond, MovingDrawsLastSecond,
        MovingFacesLastSecond, LampShaderDrawsLastSecond, StaticCasterDrawsLastSecond, NoReachLampsLastSecond,
        InstancedCasterShader ? 1 : 0);
    fprintf(file, "cpu ms per frame over the last second (profiling %d): game draws %.3f, pipeline binds, buffer "
        "writes and samplers %.3f, light buffer passes %.3f, lamp and window shadow maps %.3f (static maps %.3f, "
        "moving and window maps %.3f), emissive lights %.3f, end of frame %.3f; shader sorting %.3f ms in the second\n",
        CpuProfileEnabled.load() ? 1 : 0,
        GetCpuMilliseconds(DrawCpu), GetCpuMilliseconds(BindCpu), GetCpuMilliseconds(LightBufferCpu),
        GetCpuMilliseconds(MapCpu), GetCpuMilliseconds(StaticMapCpu), GetCpuMilliseconds(MovingMapCpu),
        GetCpuMilliseconds(GlowCpu), GetCpuMilliseconds(PresentCpu), ShaderSortCpu.LastSecond / 1e6);
    fprintf(file, "this second so far: %d frames, %d maps drawn, %d objects added, %d extra draws, %d moving draws, %d "
        "lamp shader draws, %d static caster draws, %d lamp draws with no reach\n", FramesThisSecond,
        MapsDrawnThisSecond, CastersAddedThisSecond, ExtraDrawsThisSecond, MovingDrawsThisSecond,
        LampShaderDrawsThisSecond, StaticCasterDrawsThisSecond, NoReachLampsThisSecond);
    fprintf(file, "settings: strength %.2f, softness %.3f, light limit %d, moving shadows %d, show colours %d\n",
        Strength.load(), PointSoftness.load(), LightLimit.load(), MovingShadows.load() ? 1 : 0,
        ShowColours.load() ? 1 : 0);

    struct MeshCount
    {
        uint32_t Objects = 0, SeenOnce = 0;
    };
    std::unordered_map<uint64_t, MeshCount> counts;
    for (const auto &[key, caster] : Casters)
    {
        MeshCount &count = counts[caster.Mesh.Key];
        ++count.Objects;
        count.SeenOnce += caster.FramesSeen <= 1;
    }
    std::vector<std::pair<uint64_t, MeshCount>> sorted(counts.begin(), counts.end());
    std::sort(sorted.begin(), sorted.end(), [](const auto &a, const auto &b) { return a.second.Objects > b.second.Objects; });
    fprintf(file, "\n--- objects by mesh: %zu objects of %zu meshes\n", Casters.size(), counts.size());
    for (const auto &[meshKey, count] : sorted)
        fprintf(file, "mesh %016llx: %u objects, %u seen once, %s\n", static_cast<unsigned long long>(meshKey),
            count.Objects, count.SeenOnce, MovingMeshes.contains(meshKey) ? "moving" : "not moving");

    fprintf(file, "\n--- this frame's records: %zu, %u from base draws; index, lamp, geometry, then the %u constant rows\n",
        CurrentFrame.Records.size(), CurrentFrame.BaseRecords, ROW_COUNT);
    for (size_t i = 0; i < CurrentFrame.Records.size(); ++i)
    {
        fprintf(file, "%zu %s ", i, i < CurrentFrame.Lamp.size() && CurrentFrame.Lamp[i] ? "lamp" : "base");
        WriteGeometry(file, CurrentFrame.Records[i]);
        fprintf(file, "\n  rows ");
        if ((i + 1) * ROW_COUNT <= CurrentFrame.Rows.size())
            WriteRows(file, &CurrentFrame.Rows[i * ROW_COUNT].x, ROW_COUNT * 4);
        fprintf(file, "\n");
    }

    fprintf(file, "\n--- objects: key, geometry, world rows, frames seen, last seen frames ago, last probed frames ago, "
        "misses, bounds\n");
    for (const auto &[key, caster] : Casters)
    {
        fprintf(file, "%016llx ", static_cast<unsigned long long>(key));
        WriteGeometry(file, caster.Mesh);
        fprintf(file, " world ");
        WriteRows(file, caster.World, 12);
        fprintf(file, " seen %u, %lld ago, probed %lld ago, misses %u, bounds ", caster.FramesSeen, ago(caster.LastSeen),
            ago(caster.LastProbed), caster.Misses);
        if (caster.BoundsKnown)
        {
            WriteVector(file, caster.Low);
            fprintf(file, " to ");
            WriteVector(file, caster.High);
        }
        else
            fprintf(file, caster.BoundsRequested ? "requested" : "unknown");
        fprintf(file, "\n");
    }

    fprintf(file,"\nparticle source core depth shader: available %d, failed %d, maximum radius %.3f\n",
        particle_source_shadow::Single&&particle_source_shadow::Layered&&particle_source_shadow::Constants?1:0,
        particle_source_shadow::Failed?1:0,particle_source_shadow::MaximumRadius);
    fprintf(file, "\n--- lights: %zu\n", Lights.size());
    for (const auto &[key, light] : Lights)
    {
        fprintf(file, "light %llu at ", static_cast<unsigned long long>(key));
        WriteVector(file, light.Position);
        fprintf(file, ", built at ");
        WriteVector(file, light.BuiltPosition);
        fprintf(file, ", seen %lld ago, selected %lld ago, built %lld ago, version %llu, built version %llu, slot %d, "
            "redraw now %d, slot changed %d, %zu objects, radius %.2f", ago(light.LastSeen), ago(light.LastSelected),
            ago(light.BuiltFrame), static_cast<unsigned long long>(light.Version),
            static_cast<unsigned long long>(light.BuiltVersion), light.Slot, light.RedrawNow ? 1 : 0,
            light.SlotChanged ? 1 : 0, light.CasterKeys.size(), light.Radius);
        if (light.Glow)
        {
            fprintf(file, ", glow emitter %016llx, range %.2f, area %.2f, shown %d, held %d, colour ",
                static_cast<unsigned long long>(light.EmitterKey), light.GlowRange, light.GlowArea,
                light.GlowShown ? 1 : 0, light.GlowHeld ? 1 : 0);
            WriteVector(file, light.GlowColour);
            fprintf(file, ", normal ");
            WriteVector(file, light.GlowNormal);
            if(light.Particle)fprintf(file,", particle source core current %.4f built %.4f",
                light.ParticleSourceRadius,light.BuiltParticleSourceRadius);
        }
        fprintf(file, "\n  objects");
        for (uint64_t casterKey : light.CasterKeys)
            fprintf(file, " %016llx", static_cast<unsigned long long>(casterKey));
        fprintf(file, "\n");
    }

    fprintf(file, "\n--- slots: slot, light key, active, moving map used\n");
    for (int slot = 0; slot < MAX_LIGHTS; ++slot)
        if (SlotLights[slot] || SlotActive[slot] || MovingMapUsed[slot])
            fprintf(file, "%d %llu %d %d\n", slot, static_cast<unsigned long long>(SlotLights[slot]),
                SlotActive[slot] ? 1 : 0, MovingMapUsed[slot] ? 1 : 0);

    fprintf(file, "\n--- moving meshes: %zu; mesh, still sightings\n", MovingMeshes.size());
    for (uint64_t meshKey : MovingMeshes)
    {
        auto still = StillSightings.find(meshKey);
        fprintf(file, "%016llx %u\n", static_cast<unsigned long long>(meshKey),
            still == StillSightings.end() ? 0u : still->second);
    }
    fprintf(file, "\n--- move sightings: %zu\n", MoveSightings.size());
    for (const auto &[meshKey, moves] : MoveSightings)
        fprintf(file, "%016llx %u\n", static_cast<unsigned long long>(meshKey), moves);
    fprintf(file, "\n--- still sightings: %zu\n", StillSightings.size());
    for (const auto &[meshKey, still] : StillSightings)
        fprintf(file, "%016llx %u\n", static_cast<unsigned long long>(meshKey), still);
    fprintf(file, "\n--- anchors: %zu; mesh, frames ago, position, drawn at one position, rotation\n", Anchors.size());
    for (const auto &[meshKey, anchor] : Anchors)
    {
        fprintf(file, "%016llx %lld ", static_cast<unsigned long long>(meshKey), ago(anchor.Frame));
        WriteVector(file, anchor.Position);
        fprintf(file, " %d ", anchor.Single ? 1 : 0);
        WriteRows(file, anchor.Rotation, 9);
        fprintf(file, "\n");
    }

    fprintf(file, "\n--- buffers written from the CPU: %zu; buffer handle, last written frames ago, frames written on\n",
        BufferWrites.size());
    for (const auto &[buffer, write] : BufferWrites)
        fprintf(file, "%016llx %lld %u\n", static_cast<unsigned long long>(buffer), ago(write.Frame), write.Frames);

    fprintf(file, "\n--- moving objects drawn this frame: %zu, %zu distinct\n", MovingCasters.size(), MovingThisFrame.size());
    for (const MovingCaster &moving : MovingCasters)
    {
        fprintf(file, "%u ", moving.Index);
        WriteGeometry(file, moving.Mesh);
        fprintf(file, " world ");
        if ((moving.Index + 1) * 12 <= MovingWorlds.size())
            WriteRows(file, &MovingWorlds[moving.Index * 12], 12);
        fprintf(file, "\n");
    }

    fprintf(file, "\n--- held moving meshes: %zu; mesh, copies, anchor, in place frames, last drawn frames ago\n",
        HeldMeshes.size());
    for (const auto &[meshKey, held] : HeldMeshes)
    {
        fprintf(file, "%016llx %zu ", static_cast<unsigned long long>(meshKey), held.Worlds.size() / 12);
        WriteVector(file, held.Anchor);
        fprintf(file, " %lld %lld\n", static_cast<long long>(held.LastDrawn - held.InPlaceSince), ago(held.LastDrawn));
    }

    fprintf(file, "\n--- probes in flight\n");
    for (UINT slot = 0; slot < PROBE_LATENCY; ++slot)
    {
        fprintf(file, "slot %u, probed %lld ago:", slot, ago(ProbedFrames[slot]));
        for (uint64_t casterKey : ProbedCasters[slot])
            fprintf(file, " %016llx", static_cast<unsigned long long>(casterKey));
        fprintf(file, "\n");
    }
    particle_moving::WriteState(file);
    fclose(file);
}

static void ReadFrame(Frame &frame)
{
    if (capture::State == capture::Phase::Capturing && capture::Log)
        WriteState();
    // objects never seen on enough frames to cast, and not seen for a while, were passing draws such as a moving
    // light's index lists; they are dropped every so often so they cannot pile up
    constexpr uint64_t UNCONFIRMED_FRAMES = 60;
    if (FrameNumber % UNCONFIRMED_FRAMES == 0)
        std::erase_if(Casters, [](const auto &entry) {
            return entry.second.FramesSeen < CONFIRM_FRAMES && entry.second.LastSeen + UNCONFIRMED_FRAMES < FrameNumber;
        });
    if (!NewlyRefilled.empty())
    {
        const std::unordered_set<uint64_t> refilled(NewlyRefilled.begin(), NewlyRefilled.end());
        NewlyRefilled.clear();
        const size_t cached = Casters.size();
        std::erase_if(Casters, [&refilled](const auto &entry) {
            return refilled.contains(reinterpret_cast<uint64_t>(entry.second.Mesh.IndexBuffer.Get()));
        });
        if (Casters.size() != cached)
            for (auto &[key, light] : Lights)
                if (std::erase_if(light.CasterKeys, [](uint64_t casterKey) { return !Casters.contains(casterKey); }))
                {
                    ++light.Version;
                    light.RedrawNow = true;
                }
    }
    const bool cameraKnown = frame.CameraKnown;
    frame.CameraKnown = false;
    if (!cameraKnown)
    {
        frame.Rows.clear();
        frame.Records.clear();
        frame.Lamp.clear();
        frame.BaseRecords = 0;
    }
    if (frame.Records.empty())
        return;

    const XMFLOAT4 *records = frame.Rows.data();
    const XMFLOAT3 offset = { -frame.Camera.x, -frame.Camera.y, -frame.Camera.z };
    FindMovingMeshes(frame, records, offset);
    const auto now = std::chrono::steady_clock::now();

    for (size_t i = 0; i < frame.Records.size(); ++i)
    {
        const XMFLOAT4 *rows = &records[i * ROW_COUNT];
        const float *world = &rows[4].x;
        // every lamp draw shows its light, whether or not its object is cached; a carried torch lights mostly the index
        // lists the game refills for it, which are never cached
        uint64_t lightKey = 0;
        if (frame.Lamp[i])
        {
            const XMFLOAT3 lightPosition = { rows[10].x - offset.x, rows[10].y - offset.y, rows[10].z - offset.z };
            const XMFLOAT3 hue = FindHue(rows[11 - FIRST_ROW]);
            lightKey = FindLight(lightPosition, hue);
            Light &light = Lights[lightKey];
            light.Position = lightPosition;
            light.Hue = hue;
            light.LastSeen = FrameNumber;
        }
        if (!std::isfinite(world[0]) || (world[0] == 0 && world[1] == 0 && world[2] == 0)
            || frame.Records[i].Dynamic || MovingMeshes.contains(frame.Records[i].Key) || SeeThroughMeshes.contains(frame.Records[i].Key))
            continue;

        if(auto size=MeshSizes.find(frame.Records[i].Key);size!=MeshSizes.end()
            &&size->second*FindScale(world)<MIN_CASTER_SIZE)continue;
        const XMFLOAT3 position = { world[3] - offset.x, world[7] - offset.y, world[11] - offset.z };
        const uint64_t casterKey = FindCasterKey(frame.Records[i].Key, position);
        auto [entry, added] = Casters.try_emplace(casterKey);
        Caster &caster = entry->second;
        if (added)
        {
            caster.Mesh = frame.Records[i];
            ++CastersAddedThisSecond;
        }
        std::copy(world, world + 12, caster.World);
        caster.World[3] = position.x;
        caster.World[7] = position.y;
        caster.World[11] = position.z;
        if (caster.LastSeen != FrameNumber && ++caster.FramesSeen == CONFIRM_FRAMES)
            AddToNearbyLights(casterKey, caster);
        caster.LastSeen = FrameNumber;
        caster.SeenTime = now;
        if (!lightKey)
            continue;

        Light &light = Lights[lightKey];
        if (caster.FramesSeen >= CONFIRM_FRAMES && light.CasterKeys.insert(casterKey).second)
            ++light.Version;
    }
    frame.Rows.clear();
    frame.Records.clear();
    frame.Lamp.clear();
    frame.BaseRecords = 0;
}

// drops an object from the cache and from every light holding it
static void RemoveCaster(uint64_t casterKey)
{
    Casters.erase(casterKey);
    for (auto &[key, light] : Lights)
        if (light.CasterKeys.erase(casterKey))
        {
            ++light.Version;
            light.RedrawNow = true;
        }
}

// remembers the buffers of a mesh being classified; false when ClassifiedMeshes is full
static bool KeepMeshResources(const Geometry &mesh)
{
    const MeshResources resources{reinterpret_cast<uint64_t>(mesh.VertexBuffer.Get()),reinterpret_cast<uint64_t>(mesh.IndexBuffer.Get())};
    auto found=ClassifiedMeshes.find(mesh.Key);
    if(found==ClassifiedMeshes.end())
    {
        if(ClassifiedMeshes.size()>=MAX_RECORDS)return false;
        ClassifiedMeshes.emplace(mesh.Key,resources);
    }
    else if(found->second.vertex==resources.vertex&&found->second.index==resources.index)return true;
    else found->second=resources;
    IndexMesh(mesh.Key,resources.vertex,resources.index);
    return true;
}

static void NoteBaseMesh(const Geometry &mesh)
{
    const uint64_t meshKey=mesh.Key;
    if(!KeepMeshResources(mesh))return;
    BaseMeshes.insert(meshKey);
    SeeThroughMeshes.erase(meshKey);
}

static void NoteBlendedMesh(const Geometry &mesh)
{
    const uint64_t meshKey=mesh.Key;
    if(!KeepMeshResources(mesh))return;
    if (BaseMeshes.contains(meshKey) || !SeeThroughMeshes.insert(meshKey).second)
        return;

    std::vector<uint64_t> cached;
    for (const auto &[casterKey, caster] : Casters)
        if (caster.Mesh.Key == meshKey)
            cached.push_back(casterKey);
    for (uint64_t casterKey : cached)
        RemoveCaster(casterKey);
}

// reads the probes sent PROBE_LATENCY frames ago into this slot. An object whose spot was in view on MISSES_TO_REMOVE
// probes in a row while the game did not draw it is gone; one whose spot was hidden may only be culled by the game.
static void ReadProbes(ID3D11DeviceContext *ctx, UINT slot)
{
    constexpr uint32_t MISSES_TO_REMOVE = 3;
    std::vector<uint64_t> &probed = ProbedCasters[slot];
    if (probed.empty())
        return;

    D3D11_MAPPED_SUBRESOURCE mapped;
    if (FAILED(ctx->Map(VisibleReadback[slot].Get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped)))
    {
        probed.clear();
        return;
    }

    const auto *visible = static_cast<const uint32_t *>(mapped.pData);
    for (size_t n = 0; n < probed.size(); ++n)
    {
        auto caster = Casters.find(probed[n]);
        if (caster == Casters.end())
            continue;

        if (caster->second.LastSeen >= ProbedFrames[slot] || !visible[n])
        {
            caster->second.Misses = 0;
            continue;
        }
        if (++caster->second.Misses >= MISSES_TO_REMOVE)
            RemoveCaster(probed[n]);
    }
    ctx->Unmap(VisibleReadback[slot].Get(), 0);
    probed.clear();
}

// probes the spots of cached objects within PROBE_DISTANCE of the camera that the game did not draw this frame against
// the light buffer's depth
static void SendProbes(ID3D11DeviceContext1 *ctx, UINT slot)
{
    constexpr uint64_t PROBE_INTERVAL = 10;
    // a probe is moved this far toward the camera, so the ground an object stands on does not hide its origin
    constexpr float PROBE_BIAS = 0.15f;
    // probes this close to the screen edge are left out, where the game's culling and ours may disagree
    constexpr float EDGE = 0.98f;

    const XMMATRIX worldToClip = XMMatrixTranspose(ProbeWorldToClip);
    std::vector<XMFLOAT4> probes;
    for (auto &[key, caster] : Casters)
    {
        if (probes.size() >= MAX_PROBES)
            break;
        if (caster.LastSeen == FrameNumber || caster.FramesSeen < CONFIRM_FRAMES
            || caster.LastProbed + PROBE_INTERVAL > FrameNumber)
            continue;

        const XMVECTOR relative = XMVectorSet(caster.World[3] - Camera.x, caster.World[7] - Camera.y,
            caster.World[11] - Camera.z, 0);
        const float distance = XMVectorGetX(XMVector3Length(relative));
        if (distance > PROBE_DISTANCE || distance < 2 * PROBE_BIAS)
            continue;

        XMFLOAT4 clip;
        XMStoreFloat4(&clip, XMVector4Transform(XMVectorSetW(XMVectorScale(relative, 1 - PROBE_BIAS / distance), 1),
            worldToClip));
        if (clip.w <= NEAR_PLANE || std::abs(clip.x) > EDGE * clip.w || std::abs(clip.y) > EDGE * clip.w)
            continue;

        caster.LastProbed = FrameNumber;
        probes.push_back({ (clip.x / clip.w * 0.5f + 0.5f) * ProbeWidth, (0.5f - clip.y / clip.w * 0.5f) * ProbeHeight,
            clip.z / clip.w, 0 });
        ProbedCasters[slot].push_back(key);
    }
    if (probes.empty())
        return;

    // unused probes sit behind any depth
    probes.resize(MAX_PROBES, { 0, 0, 2, 0 });
    ProbedFrames[slot] = FrameNumber;
    ctx->UpdateSubresource(ProbeBuffer.Get(), 0, nullptr, probes.data(), 0, 0);

    ComPtr<ID3DDeviceContextState> previous;
    ctx->SwapDeviceContextState(DrawState.Get(), &previous);
    ID3D11ShaderResourceView *views[] = { ProbeDepth.Get(), ProbeView.Get() };
    ID3D11UnorderedAccessView *visible = VisibleView.Get();
    ctx->CSSetShader(ProbeShader.Get(), nullptr, 0);
    ctx->CSSetShaderResources(0, 2, views);
    ctx->CSSetUnorderedAccessViews(0, 1, &visible, nullptr);
    ctx->Dispatch(MAX_PROBES / 64, 1, 1);
    ID3D11ShaderResourceView *noViews[2] = {};
    ID3D11UnorderedAccessView *noVisible = nullptr;
    ctx->CSSetShaderResources(0, 2, noViews);
    ctx->CSSetUnorderedAccessViews(0, 1, &noVisible, nullptr);
    ctx->SwapDeviceContextState(previous.Get(), nullptr);
    ctx->CopyResource(VisibleReadback[slot].Get(), VisibleBuffer.Get());
}

// drops objects not seen for a while once the camera is far from them, and those not seen for CASTER_AGE_LIMIT that no
// light holds; the game does not draw objects out of view, so nearby ones and those a light shadows with are kept
static void Evict()
{
    if (CameraFound)
    {
        // a jump this far, or a loading screen, is a zone change or a teleport, and the place left behind may share
        // coordinates with the new one
        const bool jumped = LastCameraKnown
            && (Distance(Camera, LastCamera) > KEEP_DISTANCE || FramesWithoutCamera >= LOADING_FRAMES);
        FramesWithoutCamera = 0;
        LastCamera = Camera;
        LastCameraKnown = true;
        if (jumped)
        {
            StartOver();
            return;
        }
    }
    else
        ++FramesWithoutCamera;

    std::vector<uint64_t> tiny;
    for(const auto &[key,caster]:Casters)if(caster.BoundsKnown&&Distance(caster.Low,caster.High)<MIN_CASTER_SIZE)tiny.push_back(key);
    for(uint64_t key:tiny)RemoveCaster(key);
    // zone meshes all have the world origin as theirs, so the distance is to an object's bounds once they are known
    std::erase_if(Casters, [](const auto &entry) {
        const Caster &caster = entry.second;
        const XMFLOAT3 nearest = caster.BoundsKnown
            ? XMFLOAT3 { std::clamp(Camera.x, caster.Low.x, caster.High.x), std::clamp(Camera.y, caster.Low.y, caster.High.y),
                  std::clamp(Camera.z, caster.Low.z, caster.High.z) }
            : XMFLOAT3 { caster.World[3], caster.World[7], caster.World[11] };
        if (caster.BoundsKnown && Distance(caster.Low, caster.High) < MIN_CASTER_SIZE)
            return true;

        return caster.LastSeen + CASTER_LIFETIME < FrameNumber && Distance(nearest, Camera) > KEEP_DISTANCE;
    });
    if (FrameNumber % AGE_CHECK_FRAMES == 0)
    {
        const auto now = std::chrono::steady_clock::now();
        std::vector<uint64_t> old;
        for (const auto &[key, caster] : Casters)
            if (now - caster.SeenTime > CASTER_AGE_LIMIT)
                old.push_back(key);
        if (!old.empty())
        {
            // a glow emitter is held by its own lights, which leave it out of their objects
            std::unordered_set<uint64_t> held;
            for (const auto &[key, light] : Lights)
            {
                held.insert(light.EmitterKey);
                held.insert(light.CasterKeys.begin(), light.CasterKeys.end());
            }
            for (const Window &window : Windows)
                held.insert(window.CasterKeys.begin(), window.CasterKeys.end());
            for (uint64_t key : old)
                if (!held.contains(key))
                    Casters.erase(key);
        }
    }
    std::erase_if(Lights, [](const auto &entry) {
        return entry.second.Slot < 0 && entry.second.LastSeen + LIGHT_LIFETIME < FrameNumber
            && entry.second.LastInRange + REMEMBERED_LAMP_FRAMES < FrameNumber;
    });
}

// folds lamp lights within SAME_SPOT_DISTANCE of each other, such as those a lamp left while its colour drifted, into
// the one whose slot is active, else the one with a slot, else the one seen last. It keeps the larger reach, and gathers
// its objects again through the usual shadow radius change; the other light's slot is freed.
// ponytail: compares every pair of lamp lights each frame, half a million distances at a thousand lamps; a spatial grid
// if lamps ever number that many
static void FoldStackedLights()
{
    // map nodes stay put when other entries are erased
    std::vector<std::pair<uint64_t, Light *>> lamps;
    for (auto &[key, light] : Lights)
        if (!light.Glow)
            lamps.emplace_back(key, &light);
    const auto rank = [](const Light &light) {
        return std::tuple(light.Slot >= 0 && SlotActive[light.Slot], light.Slot >= 0, light.LastSeen);
    };
    for (size_t a = 0; a < lamps.size(); ++a)
        for (size_t b = a + 1; b < lamps.size() && lamps[a].second; ++b)
        {
            if (!lamps[b].second)
                continue;

            Light &first = *lamps[a].second;
            Light &second = *lamps[b].second;
            if (Distance(first.Position, second.Position) >= SAME_SPOT_DISTANCE)
                continue;

            const bool keepFirst = rank(first) >= rank(second);
            Light &survivor = keepFirst ? first : second;
            const Light &ghost = keepFirst ? second : first;
            if (ghost.Reach > survivor.Reach)
            {
                survivor.Reach = ghost.Reach;
                survivor.Colour = ghost.Colour;
            }
            if (survivor.FadePower <= 0)
                survivor.FadePower = ghost.FadePower;
            survivor.LastSeen = std::max(survivor.LastSeen, ghost.LastSeen);
            survivor.LastInRange = std::max(survivor.LastInRange, ghost.LastInRange);
            survivor.LastAdded = std::max(survivor.LastAdded, ghost.LastAdded);
            survivor.MissedInView = std::min(survivor.MissedInView, ghost.MissedInView);
            if (ghost.Slot >= 0)
            {
                SlotLights[ghost.Slot] = 0;
                MovingMapUsed[ghost.Slot] = false;
            }
            auto &[ghostKey, ghostLight] = keepFirst ? lamps[b] : lamps[a];
            Lights.erase(ghostKey);
            ghostLight = nullptr;
        }
}

// gives the nearest lights a cube map slot. A slot keeps its map until a nearer light needs it, and then goes to the
// light that was selected longest ago.
static std::vector<uint64_t> AssignSlots()
{
    // glow lights rank as if this many times further away, so lamps keep the larger maps
    constexpr float GLOW_RANK_FACTOR = 4;

    // held glow lights sort after every other light, nearest first among themselves
    std::vector<std::tuple<bool, float, uint64_t>> nearby;
    for (const auto &[key, light] : Lights)
    {
        // a glow light competes only on the frame it is picked, so glow lights never take more slots than they are given
        if (light.Glow ? light.LastSeen != FrameNumber
                       : light.LastSeen + RECENT_FRAMES < FrameNumber && light.LastAdded != FrameNumber)
            continue;

        const float dx = light.Position.x - Camera.x;
        const float dy = light.Position.y - Camera.y;
        const float dz = light.Position.z - Camera.z;
        const float factor = light.Glow ? GLOW_RANK_FACTOR * GLOW_RANK_FACTOR : 1;
        nearby.emplace_back(light.GlowHeld, (dx * dx + dy * dy + dz * dz) * factor, key);
    }
    std::sort(nearby.begin(), nearby.end());
    nearby.resize(std::min<size_t>(nearby.size(), std::clamp(LightLimit.load(), 1, MAX_LIGHTS)));

    // a light moves up a size only when it is this much nearer than the light it takes the slot from, so two lights
    // at about the same distance do not swap sizes every frame
    constexpr float SWAP_MARGIN = 1.2f;

    std::vector<uint64_t> selected;
    // lights that lost their slot this frame while in use
    std::vector<uint64_t> displaced;
    for (size_t rank = 0; rank < nearby.size(); ++rank)
    {
        const auto &[held, distance, key] = nearby[rank];
        Light &light = Lights[key];
        light.LastSelected = FrameNumber;
        selected.push_back(key);
        const int wanted = GetSizeIndex(static_cast<int>(rank));
        const int current = light.Slot >= 0 ? GetSizeIndex(light.Slot) : SIZE_COUNT;
        if (current <= wanted)
            continue;

        // the least recently picked slot, then the one whose light is furthest from the camera, of the wanted size or of
        // the next smaller size that is still bigger than the light's current one
        int slot = -1;
        for (int index = wanted; index < current && slot < 0; ++index)
        {
            uint64_t oldest = UINT64_MAX;
            float furthest = -1;
            for (int candidate = SIZE_STARTS[index]; candidate < GetSizeEnd(index); ++candidate)
            {
                auto owner = Lights.find(SlotLights[candidate]);
                const uint64_t lastSelected = owner == Lights.end() ? 0 : owner->second.LastSelected;
                // a held glow light counts as the furthest, so its slot goes first
                const float ownerDistance = owner == Lights.end() ? 0
                    : owner->second.GlowHeld ? FLT_MAX
                    : Distance(owner->second.Position, Camera) * (owner->second.Glow ? GLOW_RANK_FACTOR : 1);
                if (lastSelected == FrameNumber || lastSelected > oldest
                    || (lastSelected == oldest && ownerDistance <= furthest))
                    continue;

                if (light.Slot >= 0 && lastSelected + 1 == FrameNumber
                    && distance * SWAP_MARGIN * SWAP_MARGIN > ownerDistance * ownerDistance)
                    continue;

                slot = candidate;
                oldest = lastSelected;
                furthest = ownerDistance;
            }
        }
        if (slot < 0)
            continue;

        if (auto owner = Lights.find(SlotLights[slot]); owner != Lights.end())
        {
            owner->second.Slot = -1;
            if (owner->second.LastSelected + 1 == FrameNumber)
                displaced.push_back(owner->first);
        }
        if (light.Slot >= 0)
        {
            SlotLights[light.Slot] = 0;
            light.SlotChanged = true;
        }
        if (std::find(displaced.begin(), displaced.end(), key) != displaced.end())
            light.SlotChanged = true;
        SlotLights[slot] = key;
        light.Slot = slot;
        light.BuiltFrame = 0;
    }
    for (uint64_t key : displaced)
        if (Light &light = Lights[key]; light.Slot < 0)
            light.SlotChanged = false;

    for (int slot = 0; slot < MAX_LIGHTS; ++slot)
    {
        auto owner = Lights.find(SlotLights[slot]);
        SlotActive[slot] = SlotLights[slot] && owner != Lights.end() && owner->second.LastSelected == FrameNumber;
    }
    return selected;
}

static void DrawCaster(ID3D11DeviceContext *ctx, const Geometry &mesh, UINT instances = 1)
{
    ID3D11Buffer *vertexBuffer = mesh.VertexBuffer.Get();
    ctx->IASetInputLayout(mesh.Layout.Get());
    ctx->IASetVertexBuffers(0, 1, &vertexBuffer, &mesh.Stride, &mesh.VertexOffset);
    ctx->IASetIndexBuffer(mesh.IndexBuffer.Get(), mesh.IndexFormat, mesh.IndexOffset);
    ctx->IASetPrimitiveTopology(mesh.Topology);
    if (instances > 1)
    {
        if (mesh.Call.Indexed)
            ctx->DrawIndexedInstanced(mesh.Call.Count, instances, mesh.Call.First, mesh.Call.BaseVertex, 0);
        else
            ctx->DrawInstanced(mesh.Call.Count, instances, mesh.Call.First, 0);
    }
    else if (mesh.Call.Indexed)
        ctx->DrawIndexed(mesh.Call.Count, mesh.Call.First, mesh.Call.BaseVertex);
    else
        ctx->Draw(mesh.Call.Count, mesh.Call.First);
}

// whether an object is a lamp's own model: small, with the light inside its bounds or just outside them
static bool HoldsLight(const Caster &caster, const XMFLOAT3 &position)
{
    // the largest object counted as a lamp's model, across its bounds' diagonal
    constexpr float LAMP_MODEL_SIZE = 8;
    // how far outside its model's bounds a lamp's light may sit
    constexpr float LAMP_MODEL_MARGIN = 0.5f;
    if (!caster.BoundsKnown || Distance(caster.Low, caster.High) > LAMP_MODEL_SIZE)
        return false;

    return position.x >= caster.Low.x - LAMP_MODEL_MARGIN && position.x <= caster.High.x + LAMP_MODEL_MARGIN
        && position.y >= caster.Low.y - LAMP_MODEL_MARGIN && position.y <= caster.High.y + LAMP_MODEL_MARGIN
        && position.z >= caster.Low.z - LAMP_MODEL_MARGIN && position.z <= caster.High.z + LAMP_MODEL_MARGIN;
}

static lamp_holder_policy::Bounds HolderBounds(const Caster &caster)
{
    if(!caster.BoundsKnown)return {};
    return {{caster.Low.x,caster.Low.y,caster.Low.z},{caster.High.x,caster.High.y,caster.High.z},caster.BoundsKnown};
}

static lamp_holder_policy::Identity HolderIdentity(const Caster &caster)
{
    lamp_holder_policy::Identity id{};
    const Geometry &mesh=caster.Mesh;
    id.vertex=reinterpret_cast<uintptr_t>(mesh.VertexBuffer.Get());id.index=reinterpret_cast<uintptr_t>(mesh.IndexBuffer.Get());
    id.stride=mesh.Stride;id.vertexOffset=mesh.VertexOffset;id.indexOffset=mesh.IndexOffset;
    id.indexFormat=static_cast<uint32_t>(mesh.IndexFormat);id.topology=static_cast<uint32_t>(mesh.Topology);
    id.dynamic=mesh.Dynamic;id.indexed=mesh.Call.Indexed;
    std::copy(std::begin(caster.World),std::end(caster.World),id.world.begin());
    return id;
}

// A native fixture can span several submeshes; one may narrowly exceed the old
// holder diagonal despite sharing an already recognized holder's exact model.
// Build this bounded exception set once per native map, never for emissive lights.
static std::unordered_set<uint64_t> FindSiblingHolders(const Light &light)
{
    namespace policy=lamp_holder_policy;
    std::unordered_set<uint64_t> siblings;
    if(light.Glow)return siblings;
    struct Anchor { policy::Identity identity;policy::Bounds bounds; };
    std::array<Anchor,policy::MaxAnchors> anchors{};unsigned count=0;
    const policy::Point position{light.Position.x,light.Position.y,light.Position.z};
    for(uint64_t key:light.CasterKeys)
    {
        auto found=Casters.find(key);if(found==Casters.end())continue;
        const Caster &caster=found->second;const auto bounds=HolderBounds(caster);
        if(!HoldsLight(caster,light.Position)||!policy::Anchor(bounds,position))continue;
        const auto identity=HolderIdentity(caster);
        if(!policy::SameIdentity(identity,identity))continue;
        if(count==policy::MaxAnchors)return {};
        anchors[count++]={identity,bounds};
    }
    if(!count)return siblings;
    for(uint64_t key:light.CasterKeys)
    {
        auto found=Casters.find(key);if(found==Casters.end())continue;
        const Caster &caster=found->second;const auto bounds=HolderBounds(caster);
        if(HoldsLight(caster,light.Position)||!policy::Contains(bounds,position)||!policy::Local(bounds))continue;
        const auto identity=HolderIdentity(caster);
        for(unsigned n=0;n<count;++n)
            if(policy::Sibling(anchors[n].identity,anchors[n].bounds,identity,bounds,position))
            {
                if(siblings.size()==policy::MaxSiblings)return {};
                siblings.insert(key);break;
            }
    }
    return siblings;
}

// whether a sphere about a position relative to a light can reach into a cube face, which sees a 90 degree square cone
// around its axis
static bool ReachesFace(int face, const XMFLOAT3 &relative, float radius)
{
    const float axes[3] = { relative.x, relative.y, relative.z };
    const int axis = face / 2;
    const float along = (face % 2 ? -1.0f : 1.0f) * axes[axis];
    const float across = std::max(std::abs(axes[(axis + 1) % 3]), std::abs(axes[(axis + 2) % 3]));
    return !(along + radius * 1.4143f < across);
}

static void DrawCubeMap(ID3D11DeviceContext1 *ctx, Light &light)
{
    const CpuScope scope(StaticMapCpu);
    const auto siblingHolders=FindSiblingHolders(light);
    ComPtr<ID3DDeviceContextState> previous;
    ctx->SwapDeviceContextState(DrawState.Get(), &previous);

    ctx->RSSetState(CasterRasterizer.Get());
    ctx->OMSetDepthStencilState(CasterDepth.Get(), 0);
    ctx->VSSetShader(CasterShader.Get(), nullptr, 0);
    ctx->PSSetShader(nullptr, nullptr, 0);
    ID3D11Buffer *casterBuffer = CasterBuffer.Get();
    ctx->VSSetConstantBuffers(0, 1, &casterBuffer);

    light.BuiltPosition = light.Position;
    light.BuiltParticleSourceRadius=light.Particle?particle_source_shadow::Clean(light.ParticleSourceRadius):0.f;
    const XMVECTOR eye = XMLoadFloat3(&light.BuiltPosition);
    const XMMATRIX projection = XMMatrixPerspectiveFovLH(XM_PIDIV2, 1.0f, NEAR_PLANE, light.Radius);
    const float size = static_cast<float>(FACE_SIZES[GetSizeIndex(light.Slot)]);
    const D3D11_VIEWPORT viewport = { 0, 0, size, size, 0, 1 };
    ctx->RSSetViewports(1, &viewport);
    particle_source_shadow::BindSingle(ctx,light,UINT(size));
    // a frame capture notes the objects close to a lamp and whether each is taken for the lamp's own model
    if (!light.Glow && capture::State == capture::Phase::Capturing && capture::Log)
    {
        fprintf(capture::Log, "--- lamp map at %.2f %.2f %.2f\n", light.Position.x, light.Position.y, light.Position.z);
        for (uint64_t key : light.CasterKeys)
        {
            auto caster = Casters.find(key);
            if (caster == Casters.end())
                continue;

            const Caster &object = caster->second;
            const XMFLOAT3 origin = { object.World[3], object.World[7], object.World[11] };
            if (!object.BoundsKnown)
            {
                if (Distance(origin, light.Position) < 8)
                    fprintf(capture::Log, "  object %016llx, origin %.2f away, bounds not known\n",
                        static_cast<unsigned long long>(key), Distance(origin, light.Position));
                continue;
            }
            const XMFLOAT3 nearest = { std::clamp(light.Position.x, object.Low.x, object.High.x),
                std::clamp(light.Position.y, object.Low.y, object.High.y), std::clamp(light.Position.z, object.Low.z, object.High.z) };
            if (Distance(nearest, light.Position) < 3)
                fprintf(capture::Log, "  object %016llx, bounds %.2f %.2f %.2f to %.2f %.2f %.2f, %.2f across, %.2f away: %s\n",
                    static_cast<unsigned long long>(key), object.Low.x, object.Low.y, object.Low.z, object.High.x, object.High.y,
                    object.High.z, Distance(object.Low, object.High), Distance(nearest, light.Position),
                    HoldsLight(object, light.Position) ? "left out" : siblingHolders.contains(key) ? "left out (holder sibling)" : "kept");
        }
    }

    // each object to draw, with the centre of its bounds relative to the light and how far it reaches from there. An
    // object whose bounds are not known yet may be any size, so it goes into every face.
    std::vector<std::tuple<const Caster *, XMFLOAT3, float, std::uint8_t>> drawn;
    drawn.reserve(light.CasterKeys.size());
    for (uint64_t key : light.CasterKeys)
    {
        // a lamp's own model would shadow its whole light, which the game means to shine through it
        auto caster = Casters.find(key);
        if (caster == Casters.end() || (!light.Glow && (HoldsLight(caster->second, light.Position)||siblingHolders.contains(key))))
            continue;

        const Caster &object = caster->second;
        if (!object.BoundsKnown)
        {
            drawn.emplace_back(&object, XMFLOAT3 {}, FLT_MAX, shadow_face_culling::AllFaces);
            continue;
        }
        const XMFLOAT3 relative = { (object.Low.x + object.High.x) / 2 - light.BuiltPosition.x,
            (object.Low.y + object.High.y) / 2 - light.BuiltPosition.y, (object.Low.z + object.High.z) / 2 - light.BuiltPosition.z };
        const auto faceMask = shadow_face_culling::ConservativeMask(
            { object.Low.x, object.Low.y, object.Low.z }, { object.High.x, object.High.y, object.High.z },
            { light.BuiltPosition.x, light.BuiltPosition.y, light.BuiltPosition.z });
        drawn.emplace_back(&object, relative, Distance(object.Low, object.High) / 2, faceMask);
    }

    CasterConstants constants;
    for (int face = 0; face < 6; ++face)
    {
        ID3D11DepthStencilView *view = FaceViews[light.Slot * 6 + face].Get();
        ctx->ClearDepthStencilView(view, D3D11_CLEAR_DEPTH, 1.0f, 0);
        ctx->OMSetRenderTargets(0, nullptr, view);
        const XMMATRIX viewMatrix = XMMatrixLookToLH(eye, FACE_DIRECTIONS[face], FACE_UPS[face]);
        XMStoreFloat4x4(&constants.ViewProjection, XMMatrixTranspose(XMMatrixMultiply(viewMatrix, projection)));
        for (const auto &[object, relative, extent, faceMask] : drawn)
        {
            if (!(faceMask & (1u << face)) || !ReachesFace(face, relative, extent))
                continue;

            std::copy(object->World, object->World + 12, constants.World);
            ctx->UpdateSubresource(CasterBuffer.Get(), 0, nullptr, &constants, 0, 0);
            DrawCaster(ctx, object->Mesh);
            ++StaticCasterDrawsThisSecond;
        }
    }

    particle_source_shadow::Clear(ctx);
    ctx->SwapDeviceContextState(previous.Get(), nullptr);
    std::erase_if(light.CasterKeys, [](uint64_t key) { return !Casters.contains(key); });
    light.BuiltFrame = FrameNumber;
    light.BuiltVersion = light.Version;
    light.RedrawNow = false;
    light.SlotChanged = false;
    ++MapsDrawnThisSecond;
}

// whether a position is inside the camera's view, by the light buffer's camera
static bool IsOnScreen(const XMFLOAT3 &position)
{
    if (!ProbeDepth)
        return false;

    XMFLOAT4 clip;
    XMStoreFloat4(&clip, XMVector4Transform(XMVectorSet(position.x - Camera.x, position.y - Camera.y,
        position.z - Camera.z, 1), XMMatrixTranspose(ProbeWorldToClip)));
    return clip.w > NEAR_PLANE && std::abs(clip.x) <= clip.w && std::abs(clip.y) <= clip.w;
}

// whether a box may be inside the camera's view: the camera is inside it, or its corners are not all beyond the same
// side of the view, IsOnScreen's left, right, top, bottom or near plane
static bool IsBoxOnScreen(const XMFLOAT3 &low, const XMFLOAT3 &high)
{
    if (!ProbeDepth)
        return false;
    if (FindBoxDistance(low, high, Camera) == 0)
        return true;

    const XMMATRIX worldToClip = XMMatrixTranspose(ProbeWorldToClip);
    // one bit for each side every corner so far is beyond
    int outside = 0x1f;
    for (int corner = 0; corner < 8; ++corner)
    {
        XMFLOAT4 clip;
        XMStoreFloat4(&clip, XMVector4Transform(XMVectorSet((corner & 1 ? high.x : low.x) - Camera.x,
            (corner & 2 ? high.y : low.y) - Camera.y, (corner & 4 ? high.z : low.z) - Camera.z, 1), worldToClip));
        outside &= (clip.x < -clip.w ? 1 : 0) | (clip.x > clip.w ? 2 : 0) | (clip.y < -clip.w ? 4 : 0)
            | (clip.y > clip.w ? 8 : 0) | (clip.w <= NEAR_PLANE ? 16 : 0);
    }
    return outside == 0;
}

// notes where this frame's moving meshes were drawn, and adds those that stay in one place but were not drawn because
// they are off screen, from where they were last drawn. One that is on screen and not drawn is gone, and one that
// moves about, such as a character, is not kept, so neither leaves a shadow behind.
static void HoldMovingMeshes()
{
    // a mesh whose copies stay within this distance of where they were for IN_PLACE_FRAMES is kept
    constexpr float IN_PLACE_RADIUS = 1.0f;
    constexpr uint64_t IN_PLACE_FRAMES = 60;

    // a mesh whose buffers the game now rewrites is no longer held
    std::unordered_map<uint64_t, std::vector<const MovingCaster *>> drawn;
    for (const MovingCaster &caster : MovingCasters)
        if (caster.Mesh.Dynamic)
            HeldMeshes.erase(caster.Mesh.Key);
        else
            drawn[caster.Mesh.Key].push_back(&caster);
    for (const auto &[meshKey, copies] : drawn)
    {
        HeldMesh &held = HeldMeshes[meshKey];
        std::vector<float> worlds;
        XMFLOAT3 centre = {};
        for (const MovingCaster *caster : copies)
        {
            const float *world = &MovingWorlds[caster->Index * 12];
            const size_t start = worlds.size();
            worlds.insert(worlds.end(), world, world + 12);
            worlds[start + 3] -= CurrentOffset.x;
            worlds[start + 7] -= CurrentOffset.y;
            worlds[start + 11] -= CurrentOffset.z;
            centre = { centre.x + worlds[start + 3] / copies.size(), centre.y + worlds[start + 7] / copies.size(),
                centre.z + worlds[start + 11] / copies.size() };
        }
        if (held.Worlds.size() != worlds.size() || Distance(held.Anchor, centre) > IN_PLACE_RADIUS)
        {
            held.Anchor = centre;
            held.InPlaceSince = FrameNumber;
        }
        held.Mesh = copies.front()->Mesh;
        held.Worlds = std::move(worlds);
        held.LastDrawn = FrameNumber;
    }

    for (auto held = HeldMeshes.begin(); held != HeldMeshes.end();)
    {
        HeldMesh &mesh = held->second;
        // drawn this frame, or drawn but lit by no lamp, which keeps it for when it is
        if (mesh.LastDrawn == FrameNumber || DrawnMeshes.contains(held->first))
        {
            ++held;
            continue;
        }
        // a mesh in world positions is judged by its bounds, and kept while they are being read; one whose bounds cannot
        // be read is judged by its origin
        bool onScreen = IsOnScreen(mesh.Anchor);
        float distance = Distance(mesh.Anchor, Camera);
        if (IsHeldInWorldPositions(mesh))
            if (auto bounds = WorldMeshes.find(held->first); bounds == WorldMeshes.end() || !bounds->second.Unreadable)
            {
                const bool known = bounds != WorldMeshes.end() && bounds->second.BoundsKnown;
                // one whose bounds cannot be asked for while the game keeps writing its buffers is not held, like a
                // mesh whose buffers the game rewrites
                if (!known && IsWrittenWithin(mesh.Mesh, READ_LATENCY))
                {
                    held = HeldMeshes.erase(held);
                    continue;
                }
                onScreen = known && IsBoxOnScreen(bounds->second.Low, bounds->second.High);
                distance = known ? FindBoxDistance(bounds->second.Low, bounds->second.High, Camera) : 0;
            }
        if (mesh.InPlaceSince + IN_PLACE_FRAMES > mesh.LastDrawn || onScreen || distance > HELD_DISTANCE)
        {
            held = HeldMeshes.erase(held);
            continue;
        }

        for (size_t start = 0; start + 12 <= mesh.Worlds.size() && MovingCasters.size() < MAX_MOVING; start += 12)
        {
            const UINT index = static_cast<UINT>(MovingCasters.size());
            MovingWorlds.insert(MovingWorlds.end(), mesh.Worlds.begin() + start, mesh.Worlds.begin() + start + 12);
            MovingWorlds[index * 12 + 3] += CurrentOffset.x;
            MovingWorlds[index * 12 + 7] += CurrentOffset.y;
            MovingWorlds[index * 12 + 11] += CurrentOffset.z;
            MovingCasters.push_back({ mesh.Mesh, index });
        }
        ++held;
    }
}

// whether two windows' rows give the same picture and beam
static bool IsSameWindow(const XMFLOAT4 (&a)[3], const XMFLOAT4 (&b)[3])
{
    for (int row = 0; row < 3; ++row)
    {
        const XMVECTOR gradient = XMLoadFloat4(&a[row]);
        const float length = XMVectorGetX(XMVector3Length(gradient));
        if (XMVectorGetX(XMVector3Length(gradient - XMLoadFloat4(&b[row]))) > 0.01f * length
            || std::abs(a[row].w - b[row].w) > WINDOW_MATCH_DISTANCE * length)
            return false;
    }
    return true;
}

// finds the window with these rows, added when new, and notes that the game drew its light this frame; nullptr when
// MAX_WINDOWS are in use. The pointer lasts until the end of the frame.
static Window *NoteWindow(const XMFLOAT4 (&rows)[3])
{
    for (Window &window : Windows)
        if (IsSameWindow(window.Rows, rows))
        {
            window.LastDrawn = FrameNumber;
            return &window;
        }
    if (Windows.size() == MAX_WINDOWS)
        return nullptr;

    Window &window = Windows.emplace_back();
    std::copy(rows, rows + 3, window.Rows);
    window.LastDrawn = FrameNumber;
    return &window;
}

static bool CreateWindowMap(ID3D11Device *dev, Window &window)
{
    const D3D11_TEXTURE2D_DESC desc = { WINDOW_MAP_SIZE, WINDOW_MAP_SIZE, 1, 1, DXGI_FORMAT_R32_TYPELESS, { 1, 0 },
        D3D11_USAGE_DEFAULT, D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE };
    D3D11_DEPTH_STENCIL_VIEW_DESC depthDesc = {};
    depthDesc.Format = DXGI_FORMAT_D32_FLOAT;
    depthDesc.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
    D3D11_SHADER_RESOURCE_VIEW_DESC viewDesc = {};
    viewDesc.Format = DXGI_FORMAT_R32_FLOAT;
    viewDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    viewDesc.Texture2D.MipLevels = 1;
    WindowConstants constants;
    std::copy(window.Rows, window.Rows + 3, constants.Rows);
    constants.Params = { (WINDOW_BIAS + WINDOW_MARGIN) * GetBeamScale(window), WINDOW_FILTER_TEXELS / WINDOW_MAP_SIZE,
        1 / WINDOW_DEPTH_RANGE, 0 };
    const D3D11_BUFFER_DESC constantsDesc = { sizeof(WindowConstants), D3D11_USAGE_IMMUTABLE, D3D11_BIND_CONSTANT_BUFFER };
    const D3D11_SUBRESOURCE_DATA constantsData = { &constants };
    if (FAILED(dev->CreateTexture2D(&desc, nullptr, &window.Texture))
        || FAILED(dev->CreateDepthStencilView(window.Texture.Get(), &depthDesc, &window.DepthView))
        || FAILED(dev->CreateShaderResourceView(window.Texture.Get(), &viewDesc, &window.View))
        || FAILED(dev->CreateBuffer(&constantsDesc, &constantsData, &window.Constants)))
    {
        window.Texture.Reset();
        return false;
    }
    return true;
}

// the cached objects whose bounds reach into a window's map, other than those that hold the whole picture where the map
// starts, such as the wall around the window; one whose bounds are not read yet is left out until they are
static std::vector<uint64_t> FindWindowCasters(const Window &window)
{
    const float start = WINDOW_MARGIN * GetBeamScale(window);
    std::vector<uint64_t> keys;
    for (const auto &[key, caster] : Casters)
    {
        if (!caster.BoundsKnown)
            continue;

        XMFLOAT3 low = { FLT_MAX, FLT_MAX, FLT_MAX }, high = { -FLT_MAX, -FLT_MAX, -FLT_MAX };
        for (int corner = 0; corner < 8; ++corner)
        {
            const XMFLOAT3 projected = ProjectToWindow(window, { corner & 1 ? caster.High.x : caster.Low.x,
                corner & 2 ? caster.High.y : caster.Low.y, corner & 4 ? caster.High.z : caster.Low.z });
            low = { std::min(low.x, projected.x), std::min(low.y, projected.y), std::min(low.z, projected.z) };
            high = { std::max(high.x, projected.x), std::max(high.y, projected.y), std::max(high.z, projected.z) };
        }
        const bool inMap = high.x > 0 && low.x < 1 && high.y > 0 && low.y < 1 && high.z > start
            && low.z < start + WINDOW_DEPTH_RANGE;
        const bool holdsPicture = low.x <= 0 && high.x >= 1 && low.y <= 0 && high.y >= 1 && low.z < start;
        if (inMap && !holdsPicture)
            keys.push_back(key);
    }
    return keys;
}

// drops the windows the game has not drawn for WINDOW_LIFETIME frames, and draws the maps of those drawn lately that
// are new, due a refresh, or have a moving mesh in their beam this frame or had one last time
static void DrawWindowMaps(ID3D11DeviceContext1 *ctx)
{
    std::erase_if(Windows, [](const Window &window) { return window.LastDrawn + WINDOW_LIFETIME < FrameNumber; });
    ComPtr<ID3DDeviceContextState> previous;
    bool swapped = false;
    for (Window &window : Windows)
    {
        if (window.LastDrawn + RECENT_FRAMES < FrameNumber)
            continue;

        std::vector<const MovingCaster *> moving;
        for (const MovingCaster &caster : MovingCasters)
        {
            const float *world = &MovingWorlds[caster.Index * 12];
            if (MovingShadows && !SeeThroughMeshes.contains(caster.Mesh.Key)
                && IsNearBeam(window, { world[3] - CurrentOffset.x, world[7] - CurrentOffset.y, world[11] - CurrentOffset.z }))
                moving.push_back(&caster);
        }
        const bool refresh = !window.CastersFound || window.CastersFound + REFRESH_INTERVAL < FrameNumber;
        if (!refresh && moving.empty() && !window.HadMoving)
            continue;

        ComPtr<ID3D11Device> dev;
        ctx->GetDevice(&dev);
        if (!window.Texture && !CreateWindowMap(dev.Get(), window))
            continue;
        if (refresh)
        {
            window.CasterKeys = FindWindowCasters(window);
            window.CastersFound = FrameNumber;
        }

        if (!swapped)
        {
            ctx->SwapDeviceContextState(DrawState.Get(), &previous);
            swapped = true;
            const D3D11_VIEWPORT viewport = { 0, 0, WINDOW_MAP_SIZE, WINDOW_MAP_SIZE, 0, 1 };
            ctx->RSSetViewports(1, &viewport);
            ctx->RSSetState(CasterRasterizer.Get());
            ctx->OMSetDepthStencilState(CasterDepth.Get(), 0);
            ctx->PSSetShader(nullptr, nullptr, 0);
            ID3D11Buffer *offset = CameraOffsetBuffer.Get();
            ctx->VSSetConstantBuffers(3, 1, &offset);
            ID3D11ShaderResourceView *worlds = MovingWorldView.Get();
            ctx->VSSetShaderResources(13, 1, &worlds);
        }
        ctx->ClearDepthStencilView(window.DepthView.Get(), D3D11_CLEAR_DEPTH, 1.0f, 0);
        ctx->OMSetRenderTargets(0, nullptr, window.DepthView.Get());

        // u and v across the map, top to bottom in v, and s from WINDOW_MARGIN in front of the window over
        // WINDOW_DEPTH_RANGE as its depth, which clips away anything nearer the window
        const XMFLOAT4 *rows = window.Rows;
        const float start = WINDOW_MARGIN * GetBeamScale(window);
        const XMFLOAT4X4 viewProjection(2 * rows[0].x, 2 * rows[0].y, 2 * rows[0].z, 2 * rows[0].w - 1, -2 * rows[1].x,
            -2 * rows[1].y, -2 * rows[1].z, 1 - 2 * rows[1].w, rows[2].x / WINDOW_DEPTH_RANGE,
            rows[2].y / WINDOW_DEPTH_RANGE, rows[2].z / WINDOW_DEPTH_RANGE, (rows[2].w - start) / WINDOW_DEPTH_RANGE, 0, 0,
            0, 1);
        CasterConstants constants;
        constants.ViewProjection = viewProjection;
        ctx->VSSetShader(CasterShader.Get(), nullptr, 0);
        ID3D11Buffer *casterBuffer = CasterBuffer.Get();
        ctx->VSSetConstantBuffers(0, 1, &casterBuffer);
        for (uint64_t key : window.CasterKeys)
        {
            auto caster = Casters.find(key);
            if (caster == Casters.end())
                continue;

            std::copy(caster->second.World, caster->second.World + 12, constants.World);
            ctx->UpdateSubresource(CasterBuffer.Get(), 0, nullptr, &constants, 0, 0);
            DrawCaster(ctx, caster->second.Mesh);
        }

        MovingConstants movingConstants = {};
        movingConstants.ViewProjection = viewProjection;
        ctx->VSSetShader(MovingCasterShader.Get(), nullptr, 0);
        ID3D11Buffer *movingBuffer = MovingConstantsBuffer.Get();
        ctx->VSSetConstantBuffers(0, 1, &movingBuffer);
        for (const MovingCaster *caster : moving)
        {
            movingConstants.Index[0] = caster->Index;
            ctx->UpdateSubresource(MovingConstantsBuffer.Get(), 0, nullptr, &movingConstants, 0, 0);
            DrawCaster(ctx, caster->Mesh);
        }
        if (capture::State == capture::Phase::Capturing && capture::Log)
        {
            fprintf(capture::Log, "--- window map: %zu objects, %zu moving meshes; rows %g %g %g %g / %g %g %g %g / %g %g %g %g\n",
                window.CasterKeys.size(), moving.size(), rows[0].x, rows[0].y, rows[0].z, rows[0].w, rows[1].x, rows[1].y,
                rows[1].z, rows[1].w, rows[2].x, rows[2].y, rows[2].z, rows[2].w);
            for (uint64_t key : window.CasterKeys)
                fprintf(capture::Log, "  object %016llx\n", static_cast<unsigned long long>(key));
            capture::AddSnapshot(ctx, window.Texture.Get(), "window-map");
        }
        window.HadMoving = !moving.empty();
        window.BuiltFrame = FrameNumber;
        ++WindowMapsDrawnThisSecond;
    }
    if (!swapped)
        return;

    // the map stays bound as depth in the drawing state otherwise, which would unbind it from the shadow pass
    ctx->OMSetRenderTargets(0, nullptr, nullptr);
    ctx->SwapDeviceContextState(previous.Get(), nullptr);
}

// redraws each light's moving-object map from the moving meshes drawn this frame near that light, and the window maps
// draws each moving mesh once, into every moving-map face set in its entry of faces, by its index; the faces are
// cleared already
static void DrawInstancedMoving(ID3D11DeviceContext1 *ctx, const std::vector<std::bitset<MOVING_SLICES>> &faces,
    const std::vector<XMFLOAT4X4> &faceRows)
{
    std::vector<UINT> slices;
    for (const MovingCaster &caster : MovingCasters)
        for (UINT slice = 0; slice < MOVING_SLICES; ++slice)
            if (faces[caster.Index][slice])
                slices.push_back(slice);
    if (slices.empty())
        return;

    const D3D11_BOX box = { 0, 0, 0, static_cast<UINT>(slices.size() * sizeof(UINT)), 1, 1 };
    ctx->UpdateSubresource(MovingSliceBuffer.Get(), 0, &box, slices.data(), 0, 0);
    ctx->UpdateSubresource(MovingFaceRowBuffer.Get(), 0, nullptr, faceRows.data(), 0, 0);

    ComPtr<ID3DDeviceContextState> previous;
    ctx->SwapDeviceContextState(DrawState.Get(), &previous);
    const D3D11_VIEWPORT viewport = { 0, 0, MOVING_FACE_SIZE, MOVING_FACE_SIZE, 0, 1 };
    ctx->RSSetViewports(1, &viewport);
    ctx->RSSetState(CasterRasterizer.Get());
    ctx->OMSetDepthStencilState(CasterDepth.Get(), 0);
    ctx->VSSetShader(InstancedCasterShader.Get(), nullptr, 0);
    ctx->PSSetShader(nullptr, nullptr, 0);
    particle_source_shadow::ParametersArray sourceParameters{};
    for(int slot=0;slot<SIZE_STARTS[1];++slot)
        if(const auto light=Lights.find(SlotLights[slot]);SlotActive[slot]&&light!=Lights.end())
            sourceParameters[slot]=particle_source_shadow::Parameters(light->second,MOVING_FACE_SIZE);
    particle_source_shadow::BindLayered(ctx,sourceParameters);
    ID3D11Buffer *constants = InstancedConstantsBuffer.Get();
    ctx->VSSetConstantBuffers(0, 1, &constants);
    ID3D11Buffer *offset = CameraOffsetBuffer.Get();
    ctx->VSSetConstantBuffers(3, 1, &offset);
    ID3D11ShaderResourceView *views[] = { MovingWorldView.Get(), MovingFaceRowView.Get(), MovingSliceView.Get() };
    ctx->VSSetShaderResources(13, 3, views);
    ctx->OMSetRenderTargets(0, nullptr, MovingArrayView.Get());

    UINT start = 0;
    for (const MovingCaster &caster : MovingCasters)
    {
        const UINT count = static_cast<UINT>(faces[caster.Index].count());
        if (!count)
            continue;

        const uint32_t index[4] = { caster.Index, start, 0, 0 };
        ctx->UpdateSubresource(InstancedConstantsBuffer.Get(), 0, nullptr, index, 0, 0);
        DrawCaster(ctx, caster.Mesh, count);
        start += count;
        ++MovingDrawsThisSecond;
        MovingFacesThisSecond += count;
    }

    // the maps stay bound as depth in the drawing state otherwise, which would unbind them from the shadow pass
    ctx->OMSetRenderTargets(0, nullptr, nullptr);
    particle_source_shadow::Clear(ctx);
    ctx->SwapDeviceContextState(previous.Get(), nullptr);
}

#include "particle_moving_shadows.hpp"

static void DrawMovingMaps(ID3D11DeviceContext1 *ctx)
{
    const CpuScope scope(MovingMapCpu);
    // the world rows are relative to this frame's camera, so they need this frame's camera position
    if (!CameraFound)
        MovingCasters.clear();
    else if (MovingShadows)
        HoldMovingMeshes();
    if (!MovingCasters.empty())
    {
        const D3D11_BOX box = { 0, 0, 0, static_cast<UINT>(MovingWorlds.size() * sizeof(float)), 1, 1 };
        ctx->UpdateSubresource(MovingWorldBuffer.Get(), 0, &box, MovingWorlds.data(), 0, 0);
    }
    if (!Windows.empty())
        DrawWindowMaps(ctx);

    // for each moving mesh in world positions, by index, its entry, or nullptr while it has none; a mesh whose bounds
    // are not read goes in every map. Empty for a mesh found by its origin
    std::vector<std::optional<const WorldMesh *>> worldMeshes(MovingCasters.size());
    for (const MovingCaster &caster : MovingCasters)
        if (!caster.Mesh.Dynamic && IsInWorldPositions(&MovingWorlds[caster.Index * 12], CurrentOffset))
        {
            auto found = WorldMeshes.find(caster.Mesh.Key);
            worldMeshes[caster.Index] = found == WorldMeshes.end() ? nullptr : &found->second;
        }

    // with instancing, each moving mesh's faces to draw into and each face's view-projection rows, drawn after the loop
    const bool instanced = InstancedCasterShader != nullptr;
    std::vector<std::bitset<MOVING_SLICES>> faces(instanced ? MovingCasters.size() : 0);
    std::vector<XMFLOAT4X4> faceRows(instanced ? MOVING_SLICES : 0);
    ComPtr<ID3DDeviceContextState> previous;
    bool swapped = false;
    for (int slot = 0; slot < SIZE_STARTS[1]; ++slot)
    {
        auto light = Lights.find(SlotLights[slot]);
        // each nearby mesh with the centre of its bounds, or its origin, relative to the light, and how far it reaches
        // from there
        std::vector<std::tuple<const MovingCaster *, XMFLOAT3, float>> nearby;
        if (SlotActive[slot] && light != Lights.end())
            for (const MovingCaster &caster : MovingCasters)
            {
                if(SeeThroughMeshes.contains(caster.Mesh.Key))continue;
                const XMFLOAT3 &lightPosition = light->second.BuiltPosition;
                if (const std::optional<const WorldMesh *> &entry = worldMeshes[caster.Index])
                {
                    const WorldMesh *worldMesh = *entry;
                    if (!worldMesh || !worldMesh->BoundsKnown)
                        nearby.emplace_back(&caster, XMFLOAT3 {}, FLT_MAX);
                    else if (FindBoxDistance(worldMesh->Low, worldMesh->High, lightPosition) < light->second.Radius)
                        nearby.emplace_back(&caster, XMFLOAT3 { (worldMesh->Low.x + worldMesh->High.x) / 2 - lightPosition.x,
                            (worldMesh->Low.y + worldMesh->High.y) / 2 - lightPosition.y,
                            (worldMesh->Low.z + worldMesh->High.z) / 2 - lightPosition.z },
                            Distance(worldMesh->Low, worldMesh->High) / 2);
                    continue;
                }
                const float *world = &MovingWorlds[caster.Index * 12];
                const XMFLOAT3 relative = { world[3] - CurrentOffset.x - lightPosition.x,
                    world[7] - CurrentOffset.y - lightPosition.y, world[11] - CurrentOffset.z - lightPosition.z };
                if (Distance(relative, { 0, 0, 0 }) < light->second.Radius + MESH_RADIUS)
                    nearby.emplace_back(&caster, relative, MESH_RADIUS);
            }
        if (nearby.empty())
        {
            if (MovingMapUsed[slot])
                for (int face = 0; face < 6; ++face)
                    ctx->ClearDepthStencilView(MovingFaceViews[slot * 6 + face].Get(), D3D11_CLEAR_DEPTH, 1.0f, 0);
            MovingMapUsed[slot] = false;
            continue;
        }

        if (instanced)
        {
            const XMVECTOR eye = XMLoadFloat3(&light->second.BuiltPosition);
            const XMMATRIX projection = XMMatrixPerspectiveFovLH(XM_PIDIV2, 1.0f, NEAR_PLANE, light->second.Radius);
            for (int face = 0; face < 6; ++face)
            {
                const UINT slice = static_cast<UINT>(slot * 6 + face);
                ctx->ClearDepthStencilView(MovingFaceViews[slice].Get(), D3D11_CLEAR_DEPTH, 1.0f, 0);
                const XMMATRIX viewMatrix = XMMatrixLookToLH(eye, FACE_DIRECTIONS[face], FACE_UPS[face]);
                XMStoreFloat4x4(&faceRows[slice], XMMatrixTranspose(XMMatrixMultiply(viewMatrix, projection)));
                for (const auto &[caster, relative, extent] : nearby)
                    if (ReachesFace(face, relative, extent))
                        faces[caster->Index].set(slice);
            }
            MovingMapUsed[slot] = true;
            continue;
        }

        if (!swapped)
        {
            ctx->SwapDeviceContextState(DrawState.Get(), &previous);
            swapped = true;
            const D3D11_VIEWPORT viewport = { 0, 0, MOVING_FACE_SIZE, MOVING_FACE_SIZE, 0, 1 };
            ctx->RSSetViewports(1, &viewport);
            ctx->RSSetState(CasterRasterizer.Get());
            ctx->OMSetDepthStencilState(CasterDepth.Get(), 0);
            ctx->VSSetShader(MovingCasterShader.Get(), nullptr, 0);
            ctx->PSSetShader(nullptr, nullptr, 0);
            ID3D11Buffer *constants[] = { MovingConstantsBuffer.Get() };
            ctx->VSSetConstantBuffers(0, 1, constants);
            ID3D11Buffer *offset = CameraOffsetBuffer.Get();
            ctx->VSSetConstantBuffers(3, 1, &offset);
            ID3D11ShaderResourceView *worlds = MovingWorldView.Get();
            ctx->VSSetShaderResources(13, 1, &worlds);
        }

        particle_source_shadow::BindSingle(ctx,light->second,MOVING_FACE_SIZE);
        MovingConstants constants = {};
        const XMVECTOR eye = XMLoadFloat3(&light->second.BuiltPosition);
        const XMMATRIX projection = XMMatrixPerspectiveFovLH(XM_PIDIV2, 1.0f, NEAR_PLANE, light->second.Radius);
        for (int face = 0; face < 6; ++face)
        {
            ID3D11DepthStencilView *view = MovingFaceViews[slot * 6 + face].Get();
            ctx->ClearDepthStencilView(view, D3D11_CLEAR_DEPTH, 1.0f, 0);
            ctx->OMSetRenderTargets(0, nullptr, view);
            const XMMATRIX viewMatrix = XMMatrixLookToLH(eye, FACE_DIRECTIONS[face], FACE_UPS[face]);
            XMStoreFloat4x4(&constants.ViewProjection, XMMatrixTranspose(XMMatrixMultiply(viewMatrix, projection)));
            for (const auto &[caster, relative, extent] : nearby)
            {
                if (!ReachesFace(face, relative, extent))
                    continue;

                constants.Index[0] = caster->Index;
                ctx->UpdateSubresource(MovingConstantsBuffer.Get(), 0, nullptr, &constants, 0, 0);
                DrawCaster(ctx, caster->Mesh);
                ++MovingDrawsThisSecond;
                ++MovingFacesThisSecond;
            }
        }
        MovingMapUsed[slot] = true;
    }

    if (swapped)
    {
        particle_source_shadow::Clear(ctx);
        ctx->SwapDeviceContextState(previous.Get(), nullptr);
    }
    if (instanced)
        DrawInstancedMoving(ctx, faces, faceRows);
    particle_moving::Draw(ctx,worldMeshes);
    if (DrawAdditionalMoving) DrawAdditionalMoving(ctx);
    MovingCasters.clear();
    MovingWorlds.clear();
    MovingThisFrame.clear();
    DrawnMeshes.clear();
    BaseMeshes.clear();
}

static void UploadLightTable(ID3D11DeviceContext *ctx)
{
    LightTable &table = UploadedTable;
    table = {};
    for (int slot = 0; slot < MAX_LIGHTS; ++slot)
    {
        table.Lights[slot] = { 0, 0, 0, -1 };
        auto it = Lights.find(SlotLights[slot]);
        if (!SlotActive[slot] || it == Lights.end() || !it->second.BuiltFrame)
            continue;

        const XMFLOAT3 &position = it->second.BuiltPosition;
        table.Lights[slot] = { position.x, position.y, position.z, static_cast<float>(slot) };
        table.FarPlanes[slot] = { it->second.Radius, 0, 0, 0 };
    }
    table.Params = { NEAR_PLANE, 0, Strength.load(), ShowColours.load() ? 1.0f : 0.0f };
    const bool legacy=LegacyPointSoftness.load();
    table.Softness = { PointSoftness.load() * (legacy?1.0f:sun_softness_settings::Spread), legacy?1.0f:0.0f, 0, 0 };
    for (int slot = 0; slot < MAX_LIGHTS; ++slot)
        table.Moving[slot] = { MovingMapUsed[slot] ? 1.0f : 0.0f, 0, 0, 0 };
    ctx->UpdateSubresource(LightTableBuffer.Get(), 0, nullptr, &table, 0, 0);
}

// runs at the end of each frame: reads back the oldest frame's records, then updates and draws the cube maps
static void EndFrame(ID3D11Device *dev, ID3D11DeviceContext *ctx)
{
    const shadowmaps::CpuScope scope(shadowmaps::MapCpu);
    if (CreateFailed)
        return;
    if (!DrawState && !CreateObjects(dev))
    {
        reshade::log::message(reshade::log::level::error, "Could not create the shadow map objects.");
        Reset();
        CreateFailed = true;
        return;
    }

    // a frame whose base light was not fully recorded would make every object in view look removed
    const bool probe = ProbeDepth && CurrentFrame.CameraKnown && CurrentFrame.BaseRecords
        && CurrentFrame.Records.size() < MAX_RECORDS;
    ReadFrame(CurrentFrame);
    Evict();
    const UINT probeSlot = FrameNumber % PROBE_LATENCY;
    ReadProbes(ctx, probeSlot);
    FoldStackedLights();
    const std::vector<uint64_t> selected = AssignSlots();
    if (!CubeViews[SIZE_COUNT - 1] && std::any_of(selected.begin(), selected.end(), [](uint64_t key) {
            return Lights.at(key).Slot >= SIZE_STARTS[SIZE_COUNT - 1];
        }) && !EnsureExtraCubeArray(dev))
    {
        reshade::log::message(reshade::log::level::error, "Could not allocate the requested extra shadow maps.");
        Reset();
        CreateFailed = true;
        return;
    }


    ComPtr<ID3D11DeviceContext1> ctx1;
    if (SUCCEEDED(ctx->QueryInterface(IID_PPV_ARGS(&ctx1))))
    {
        if (probe)
            SendProbes(ctx1.Get(), probeSlot);

        StartTimer(ctx, MapTimer);
        // one new or stale map per frame, plus every map whose light has moved or whose lamp changed size
        bool drewStale = false;
        // the glow light resized longest ago, drawn as the frame's stale map when no other map is stale, so lights a
        // slider keeps resizing take turns and do not hold back new and stale maps; one whose map is older than
        // REFRESH_INTERVAL is stale like any other
        Light *resizedGlow = nullptr;
        for (uint64_t key : selected)
        {
            Light &light = Lights[key];
            if (light.Slot < 0)
                continue;

            // a lamp whose shadow radius changed gathers its objects again and, like a moved light, is redrawn at once.
            // A lamp gathered with the fallback radius does so as soon as its reach is read, however close the two are.
            // A glow light's radius follows the strength settings, which a slider can change every frame, so its map
            // goes stale instead, and it gathers its objects again when it is redrawn.
            const float radius = FindShadowRadius(light);
            const bool radiusChanged = radius > light.Radius * RADIUS_GROWTH || radius < light.Radius * RADIUS_SHRINK;
            bool resized = false;
            if (!light.Glow && ((light.FallbackRadius && light.Reach > 0) || radiusChanged))
            {
                const CpuScope mapScope(StaticMapCpu);
                GatherCasters(light);
                ++light.Version;
                resized = light.BuiltFrame != 0;
            }
            const bool moved = light.BuiltFrame && Distance(light.Position, light.BuiltPosition) > MOVED_LIGHT_DISTANCE;
            const bool stale = !light.BuiltFrame || light.RedrawNow
                || ((light.BuiltVersion < light.Version || (light.Glow && radiusChanged))
                    && light.BuiltFrame + REFRESH_INTERVAL < FrameNumber);
            if (moved || resized || light.SlotChanged || (stale && !drewStale))
            {
                if (light.Glow && radiusChanged)
                {
                    const CpuScope mapScope(StaticMapCpu);
                    GatherCasters(light);
                    ++light.Version;
                }
                DrawCubeMap(ctx1.Get(), light);
                drewStale = drewStale || !(moved || resized);
            }
            else if (light.Glow && radiusChanged && (!resizedGlow || light.BuiltFrame < resizedGlow->BuiltFrame))
                resizedGlow = &light;
        }
        if (resizedGlow && !drewStale)
        {
            {
                const CpuScope mapScope(StaticMapCpu);
                GatherCasters(*resizedGlow);
                ++resizedGlow->Version;
            }
            DrawCubeMap(ctx1.Get(), *resizedGlow);
        }
        DrawMovingMaps(ctx1.Get());
        StopTimer(ctx, MapTimer);
    }
    // a lamp with no map takes its own shadow radius as soon as its reach is read, so objects added to it later are
    // those its light reaches; it keeps FallbackRadius, and gathers all its objects again once it has a slot
    for (auto &[key, light] : Lights)
        if (light.Slot < 0 && light.FallbackRadius && light.Reach > 0)
            light.Radius = FindShadowRadius(light);

    const auto now = std::chrono::steady_clock::now();
    if (now - SecondStart >= std::chrono::seconds(1))
    {
        MapsDrawnLastSecond = MapsDrawnThisSecond;
        MapsDrawnThisSecond = 0;
        CastersAddedLastSecond = CastersAddedThisSecond;
        CastersAddedThisSecond = 0;
        ExtraDrawsLastSecond = ExtraDrawsThisSecond;
        ExtraDrawsThisSecond = 0;
        MovingDrawsLastSecond = MovingDrawsThisSecond;
        MovingDrawsThisSecond = 0;
        MovingFacesLastSecond = MovingFacesThisSecond;
        MovingFacesThisSecond = 0;
        WindowMapsDrawnLastSecond = WindowMapsDrawnThisSecond;
        WindowMapsDrawnThisSecond = 0;
        LampShaderDrawsLastSecond = LampShaderDrawsThisSecond;
        LampShaderDrawsThisSecond = 0;
        StaticCasterDrawsLastSecond = StaticCasterDrawsThisSecond;
        StaticCasterDrawsThisSecond = 0;
        NoReachLampsLastSecond = NoReachLampsThisSecond;
        NoReachLampsThisSecond = 0;
        FramesLastSecond = FramesThisSecond;
        FramesThisSecond = 0;
        SecondStart = now;
    }

    ListShadowSlots();
    UploadLightTable(ctx);
    // a light draw can come before the frame's first draw that holds the camera; it uses a prediction from the last two
    // frames
    if (CameraFound && PreviousCameraKnown)
    {
        CurrentOffset = { PreviousCamera.x - 2 * Camera.x, PreviousCamera.y - 2 * Camera.y,
            PreviousCamera.z - 2 * Camera.z };
        UploadOffset(ctx);
    }
    PreviousCamera = Camera;
    PreviousCameraKnown = CameraFound;
    BaseMeshes.clear();
    ProbeDepth.Reset();
    CameraFound = false;
    ++FramesThisSecond;
    ++FrameNumber;
}

static void RebuildAll()
{
    for (auto &[key, light] : Lights)
        light.BuiltFrame = 0;
    ShadowSlots.clear();
}
}
