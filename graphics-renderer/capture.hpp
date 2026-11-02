// Frame capture: writes one frame's draws, bound state and the constants each draw reads to a text file, plus the layout
// of every shader those draws used, to work out how the game renders and lights a scene. The draws that change the depth
// at the screen centre are listed separately, to find the draws of whatever is in the middle of the screen.
#pragma once
#include <imgui.h>
#include <reshade.hpp>
#include <d3d11_1.h>
#include <wrl/client.h>
#include <d3d11shader.h>
#include <d3dcompiler.h>
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "dxguid.lib")

using namespace reshade::api;

namespace capture
{
constexpr uint32_t MAX_RENDER_TARGETS = 8;
constexpr uint32_t MAX_CONSTANT_BUFFERS = 14;
constexpr uint32_t MAX_SHADER_RESOURCES = 16;
// constant rows written per bound constant buffer; the game's per-draw constants fit in the first 64 rows of b0 and in
// the 11 rows of b1
constexpr UINT MAX_DUMPED_ROWS[2] = { 64, 11 };

enum class Phase { Idle, Requested, Capturing };

struct Shader
{
    const char *Stage;
    uint64_t Hash;
    std::vector<uint8_t> Code;
};

static std::mutex Lock;
static std::unordered_map<uint64_t, Shader> Shaders;
static std::unordered_map<uint64_t, std::string> States;

// state bound on the immediate context
static uint64_t VertexShader, PixelShader, BlendState, DepthState;
static resource_view RenderTargets[MAX_RENDER_TARGETS];
static uint32_t RenderTargetCount;
static resource_view DepthTarget;
static resource VertexBuffer, IndexBuffer;
static uint64_t VertexOffset, IndexOffset;
static uint32_t VertexStride;
static buffer_range VertexConstants[MAX_CONSTANT_BUFFERS], PixelConstants[MAX_CONSTANT_BUFFERS];
static resource_view PixelResources[MAX_SHADER_RESOURCES];

static void ClearBindings()
{
    VertexShader=PixelShader=BlendState=DepthState=0;
    RenderTargetCount=0;DepthTarget={};VertexBuffer={};IndexBuffer={};
    VertexOffset=IndexOffset=0;VertexStride=0;
    std::fill(std::begin(RenderTargets),std::end(RenderTargets),resource_view{});
    std::fill(std::begin(VertexConstants),std::end(VertexConstants),buffer_range{});
    std::fill(std::begin(PixelConstants),std::end(PixelConstants),buffer_range{});
    std::fill(std::begin(PixelResources),std::end(PixelResources),resource_view{});
}

// One boundary snapshot on re-enable, so a still-bound native shader is not
// missed just because it was created/bound while scene tracking was idle.
// COM ownership stays with the context; only compared handle values are saved.
static void RefreshBindings(ID3D11DeviceContext *ctx)
{
    ClearBindings();
    Microsoft::WRL::ComPtr<ID3D11VertexShader>vs;
    Microsoft::WRL::ComPtr<ID3D11PixelShader>ps;
    Microsoft::WRL::ComPtr<ID3D11BlendState>blend;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilState>depth;
    ctx->VSGetShader(&vs,nullptr,nullptr);ctx->PSGetShader(&ps,nullptr,nullptr);
    ctx->OMGetBlendState(&blend,nullptr,nullptr);ctx->OMGetDepthStencilState(&depth,nullptr);
    VertexShader=reinterpret_cast<uintptr_t>(vs.Get());PixelShader=reinterpret_cast<uintptr_t>(ps.Get());
    BlendState=reinterpret_cast<uintptr_t>(blend.Get());DepthState=reinterpret_cast<uintptr_t>(depth.Get());
    Microsoft::WRL::ComPtr<ID3D11Buffer>vb,ib;UINT stride=0,vertexOffset=0,indexOffset=0;DXGI_FORMAT format{};
    ctx->IAGetVertexBuffers(0,1,&vb,&stride,&vertexOffset);ctx->IAGetIndexBuffer(&ib,&format,&indexOffset);
    VertexBuffer={reinterpret_cast<uintptr_t>(vb.Get())};VertexStride=stride;VertexOffset=vertexOffset;
    IndexBuffer={reinterpret_cast<uintptr_t>(ib.Get())};IndexOffset=indexOffset;
    ID3D11RenderTargetView*targets[MAX_RENDER_TARGETS]={};Microsoft::WRL::ComPtr<ID3D11DepthStencilView>dsv;
    ctx->OMGetRenderTargets(MAX_RENDER_TARGETS,targets,&dsv);
    DepthTarget={reinterpret_cast<uintptr_t>(dsv.Get())};
    for(UINT i=0;i<MAX_RENDER_TARGETS;++i)if(targets[i]){
        RenderTargetCount=i+1;RenderTargets[i]={reinterpret_cast<uintptr_t>(targets[i])};targets[i]->Release();
    }
}

static std::atomic<Phase> State = Phase::Idle;
static std::filesystem::path Folder;
static FILE *Log;
static uint32_t DrawIndex;
static std::unordered_set<uint64_t> UsedShaders;
static std::unordered_map<uint64_t, int> ResourceIds;
static std::string Status;

// copies of render targets taken mid-frame, written out as images when the capture ends
struct Snapshot
{
    std::string Name;
    resource Staging;
    uint32_t Width, Height;
};

static std::vector<Snapshot> Snapshots;
// the target of the last draw if it added its colour (blend one, one), to be saved once that draw is done
static uint64_t AddedTarget;
static uint32_t AddedDraw;

// the depth at the screen centre is copied before each draw into one texel of ProbeStaging
constexpr UINT MAX_PROBES = 8192;

struct Probe
{
    uint32_t Draw;
    ID3D11Resource *Depth;
};

static std::vector<Probe> Probes;
static Microsoft::WRL::ComPtr<ID3D11Texture2D> ProbeCopy, ProbeStaging;
static std::unordered_set<uint64_t> LitTargets;
static std::unordered_set<uint64_t> SavedLampTextures;
static uint64_t PendingTarget;

static uint64_t HashCode(const void *code, size_t size)
{
    uint64_t hash = 0xcbf29ce484222325;
    for (size_t i = 0; i < size; ++i)
        hash = (hash ^ static_cast<const uint8_t *>(code)[i]) * 0x100000001b3;
    return hash;
}

static std::string DescribeBlend(const blend_desc &desc)
{
    if (!desc.blend_enable[0])
        return "opaque";

    char text[64];
    sprintf_s(text, "src*%d op%d dst*%d", static_cast<int>(desc.source_color_blend_factor[0]),
        static_cast<int>(desc.color_blend_op[0]), static_cast<int>(desc.dest_color_blend_factor[0]));
    return text;
}

static std::string DescribeDepth(const depth_stencil_desc &desc)
{
    char text[64];
    sprintf_s(text, "depth %s func%d write %d stencil %d", desc.depth_enable ? "on" : "off",
        static_cast<int>(desc.depth_func), desc.depth_write_mask ? 1 : 0, desc.stencil_enable ? 1 : 0);
    return text;
}

static void OnInitPipeline(device *, pipeline_layout, uint32_t count, const pipeline_subobject *subobjects, pipeline handle)
{
    std::lock_guard guard(Lock);
    for (uint32_t i = 0; i < count; ++i)
    {
        const pipeline_subobject &sub = subobjects[i];
        if (!sub.count || !sub.data)
            continue;

        if (sub.type == pipeline_subobject_type::vertex_shader || sub.type == pipeline_subobject_type::pixel_shader)
        {
            const auto &desc = *static_cast<const shader_desc *>(sub.data);
            const auto *code = static_cast<const uint8_t *>(desc.code);
            Shaders[handle.handle] = { sub.type == pipeline_subobject_type::vertex_shader ? "vs" : "ps",
                HashCode(code, desc.code_size), std::vector<uint8_t>(code, code + desc.code_size) };
        }
        else if (sub.type == pipeline_subobject_type::blend_state)
            States[handle.handle] = DescribeBlend(*static_cast<const blend_desc *>(sub.data));
        else if (sub.type == pipeline_subobject_type::depth_stencil_state)
            States[handle.handle] = DescribeDepth(*static_cast<const depth_stencil_desc *>(sub.data));
    }
}

static void OnDestroyPipeline(device *, pipeline handle)
{
    std::lock_guard guard(Lock);
    Shaders.erase(handle.handle);
    States.erase(handle.handle);
}

static int GetResourceId(uint64_t handle)
{
    return ResourceIds.try_emplace(handle, static_cast<int>(ResourceIds.size()) + 1).first->second;
}

// writes the rows of a constant buffer slot as the draw sees them, read from the buffer's memory like the shadow code
// reads light constants; non-dynamic buffers cannot be mapped and are only named
static void WriteBoundRows(ID3D11DeviceContext *ctx, const char *label, UINT slot, ID3D11Buffer *buffer, UINT first,
    UINT count)
{
    if (!buffer)
        return;

    D3D11_BUFFER_DESC desc;
    buffer->GetDesc(&desc);
    const UINT start = first * 16;
    const UINT rows = std::min({ count ? count : desc.ByteWidth / 16, MAX_DUMPED_ROWS[slot],
        start < desc.ByteWidth ? (desc.ByteWidth - start) / 16 : 0u });
    fprintf(Log, "  %s b%u r%d at c%u:\n", label, slot,
        GetResourceId(reinterpret_cast<uint64_t>(buffer)), first);
    D3D11_MAPPED_SUBRESOURCE mapped;
    if (desc.Usage != D3D11_USAGE_DYNAMIC || FAILED(ctx->Map(buffer, 0, D3D11_MAP_WRITE_NO_OVERWRITE, 0, &mapped)))
        return;

    const auto *values = reinterpret_cast<const float *>(static_cast<const uint8_t *>(mapped.pData) + start);
    for (UINT row = 0; row < rows; ++row)
        fprintf(Log, "    c%u: %g %g %g %g\n", row, values[row * 4], values[row * 4 + 1], values[row * 4 + 2],
            values[row * 4 + 3]);
    ctx->Unmap(buffer, 0);
}

// writes the constants b0 and b1 hold for the vertex and pixel shader, once when both stages share a range
static void WriteDrawConstants(command_list *cmd)
{
    using Microsoft::WRL::ComPtr;
    auto *ctx = reinterpret_cast<ID3D11DeviceContext1 *>(cmd->get_native());
    for (UINT slot = 0; slot < 2; ++slot)
    {
        ComPtr<ID3D11Buffer> vertexBuffer, pixelBuffer;
        UINT vertexFirst = 0, vertexCount = 0, pixelFirst = 0, pixelCount = 0;
        ctx->VSGetConstantBuffers1(slot, 1, &vertexBuffer, &vertexFirst, &vertexCount);
        ctx->PSGetConstantBuffers1(slot, 1, &pixelBuffer, &pixelFirst, &pixelCount);
        const bool shared = vertexBuffer == pixelBuffer && vertexFirst == pixelFirst;
        WriteBoundRows(ctx, shared ? "vs+ps" : "vs", slot, vertexBuffer.Get(), vertexFirst, vertexCount);
        if (!shared)
            WriteBoundRows(ctx, "ps", slot, pixelBuffer.Get(), pixelFirst, pixelCount);
    }
}

static DXGI_FORMAT GetTypelessDepthFormat(DXGI_FORMAT format)
{
    switch (format)
    {
    case DXGI_FORMAT_D24_UNORM_S8_UINT:
    case DXGI_FORMAT_R24G8_TYPELESS:
        return DXGI_FORMAT_R24G8_TYPELESS;
    case DXGI_FORMAT_D32_FLOAT:
    case DXGI_FORMAT_R32_TYPELESS:
        return DXGI_FORMAT_R32_TYPELESS;
    case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
    case DXGI_FORMAT_R32G8X24_TYPELESS:
        return DXGI_FORMAT_R32G8X24_TYPELESS;
    case DXGI_FORMAT_D16_UNORM:
    case DXGI_FORMAT_R16_TYPELESS:
        return DXGI_FORMAT_R16_TYPELESS;
    default:
        return DXGI_FORMAT_UNKNOWN;
    }
}

// copies the depth at the centre of the bound screen-sized depth target, before the next draw; a draw that changes it
// drew there. A depth target can only be copied whole, so it goes through ProbeCopy.
static void ProbeCentre(ID3D11DeviceContext *ctx, uint32_t draw)
{
    using Microsoft::WRL::ComPtr;
    ComPtr<ID3D11DepthStencilView> view;
    ctx->OMGetRenderTargets(0, nullptr, &view);
    if (!view || Probes.size() >= MAX_PROBES)
        return;

    ComPtr<ID3D11Resource> resource;
    view->GetResource(&resource);
    ComPtr<ID3D11Texture2D> texture;
    if (FAILED(resource.As(&texture)))
        return;

    D3D11_TEXTURE2D_DESC desc;
    texture->GetDesc(&desc);
    const DXGI_FORMAT format = GetTypelessDepthFormat(desc.Format);
    if (format == DXGI_FORMAT_UNKNOWN || desc.SampleDesc.Count != 1 || desc.ArraySize != 1 || desc.Width < 1280)
        return;

    ID3D11Device *dev = nullptr;
    ctx->GetDevice(&dev);
    D3D11_TEXTURE2D_DESC stagingDesc = { MAX_PROBES, 1, 1, 1, format, { 1, 0 }, D3D11_USAGE_STAGING, 0,
        D3D11_CPU_ACCESS_READ };
    if (!ProbeStaging && FAILED(dev->CreateTexture2D(&stagingDesc, nullptr, &ProbeStaging)))
        ProbeStaging.Reset();
    D3D11_TEXTURE2D_DESC copyDesc = {};
    if (ProbeCopy)
        ProbeCopy->GetDesc(&copyDesc);
    if (copyDesc.Width != desc.Width || copyDesc.Height != desc.Height || copyDesc.Format != format
        || copyDesc.MipLevels != desc.MipLevels)
    {
        copyDesc = desc;
        copyDesc.Format = format;
        copyDesc.Usage = D3D11_USAGE_DEFAULT;
        copyDesc.BindFlags = 0;
        copyDesc.CPUAccessFlags = 0;
        copyDesc.MiscFlags = 0;
        ProbeCopy.Reset();
        if (FAILED(dev->CreateTexture2D(&copyDesc, nullptr, &ProbeCopy)))
            ProbeCopy.Reset();
    }
    dev->Release();
    if (!ProbeStaging || !ProbeCopy)
        return;
    ProbeStaging->GetDesc(&stagingDesc);
    if (stagingDesc.Format != format)
        return;

    ctx->CopyResource(ProbeCopy.Get(), texture.Get());
    const D3D11_BOX box = { desc.Width / 2, desc.Height / 2, 0, desc.Width / 2 + 1, desc.Height / 2 + 1, 1 };
    ctx->CopySubresourceRegion(ProbeStaging.Get(), 0, static_cast<UINT>(Probes.size()), 0, 0, ProbeCopy.Get(), 0, &box);
    Probes.push_back({ draw, texture.Get() });
}

// writes the draws after which the depth at the screen centre had changed
static void WriteCentreDraws(ID3D11DeviceContext *ctx)
{
    D3D11_MAPPED_SUBRESOURCE mapped;
    if (Probes.empty() || !ProbeStaging || FAILED(ctx->Map(ProbeStaging.Get(), 0, D3D11_MAP_READ, 0, &mapped)))
        return;

    D3D11_TEXTURE2D_DESC desc;
    ProbeStaging->GetDesc(&desc);
    const size_t size = desc.Format == DXGI_FORMAT_R32G8X24_TYPELESS ? 8 : desc.Format == DXGI_FORMAT_R16_TYPELESS ? 2 : 4;
    const auto *texels = static_cast<const uint8_t *>(mapped.pData);
    FILE *file = nullptr;
    if (_wfopen_s(&file, (Folder / L"centre.txt").c_str(), L"w") == 0)
    {
        fprintf(file, "draws that changed the depth at the screen centre (depth target, raw value before and after)\n");
        for (size_t i = 0; i + 1 < Probes.size(); ++i)
        {
            if (Probes[i].Depth != Probes[i + 1].Depth
                || std::memcmp(texels + i * size, texels + (i + 1) * size, size) == 0)
                continue;

            uint32_t before = 0, after = 0;
            std::memcpy(&before, texels + i * size, std::min<size_t>(size, 4));
            std::memcpy(&after, texels + (i + 1) * size, std::min<size_t>(size, 4));
            fprintf(file, "#%u r%d %08x -> %08x\n", Probes[i].Draw,
                GetResourceId(reinterpret_cast<uint64_t>(Probes[i].Depth)), before, after);
        }
        fclose(file);
    }
    ctx->Unmap(ProbeStaging.Get(), 0);
}

static void WriteBufferFile(const char *name, const void *data, UINT size)
{
    FILE *file = nullptr;
    if (_wfopen_s(&file, (Folder / L"buffers" / (std::string(name) + ".bin")).c_str(), L"wb") == 0)
    {
        fwrite(data, 1, size, file);
        fclose(file);
    }
}

// saves a buffer the CPU rewrites every frame, as the draw sees it, to <name>.bin; other buffers cannot be read back here
static void SaveDynamicBuffer(ID3D11DeviceContext *ctx, ID3D11Buffer *buffer, const char *name)
{
    // larger buffers are skipped; ground cover and particles fit well within this
    constexpr UINT MAX_SAVED_BYTES = 4 << 20;
    if (!buffer)
        return;

    D3D11_BUFFER_DESC desc;
    buffer->GetDesc(&desc);
    D3D11_MAPPED_SUBRESOURCE mapped;
    if (desc.Usage != D3D11_USAGE_DYNAMIC || desc.ByteWidth > MAX_SAVED_BYTES
        || FAILED(ctx->Map(buffer, 0, D3D11_MAP_WRITE_NO_OVERWRITE, 0, &mapped)))
        return;

    WriteBufferFile(name, mapped.pData, desc.ByteWidth);
    ctx->Unmap(buffer, 0);
}

// saves a buffer the GPU holds, through a copy the CPU can read, to <name>.bin; waits for the GPU, so only for captures
static void SaveGpuBuffer(ID3D11DeviceContext *ctx, ID3D11Buffer *buffer, const char *name)
{
    constexpr UINT MAX_SAVED_BYTES = 64 << 20;
    using Microsoft::WRL::ComPtr;
    if (!buffer)
        return;

    D3D11_BUFFER_DESC desc;
    buffer->GetDesc(&desc);
    if (desc.Usage == D3D11_USAGE_DYNAMIC)
    {
        SaveDynamicBuffer(ctx, buffer, name);
        return;
    }
    if (desc.ByteWidth > MAX_SAVED_BYTES)
        return;

    ComPtr<ID3D11Device> dev;
    ctx->GetDevice(&dev);
    D3D11_BUFFER_DESC stagingDesc = { desc.ByteWidth, D3D11_USAGE_STAGING, 0, D3D11_CPU_ACCESS_READ };
    ComPtr<ID3D11Buffer> staging;
    if (FAILED(dev->CreateBuffer(&stagingDesc, nullptr, &staging)))
        return;

    ctx->CopyResource(staging.Get(), buffer);
    D3D11_MAPPED_SUBRESOURCE mapped;
    if (FAILED(ctx->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped)))
        return;

    WriteBufferFile(name, mapped.pData, desc.ByteWidth);
    ctx->Unmap(staging.Get(), 0);
}

// whether the bound blend state adds the draw's colour onto the target (blend one, one)
static bool IsAddingColour(ID3D11DeviceContext *ctx)
{
    Microsoft::WRL::ComPtr<ID3D11BlendState> blend;
    ctx->OMGetBlendState(&blend, nullptr, nullptr);
    if (!blend)
        return false;

    D3D11_BLEND_DESC desc;
    blend->GetDesc(&desc);
    const D3D11_RENDER_TARGET_BLEND_DESC &target = desc.RenderTarget[0];
    return target.BlendEnable && target.SrcBlend == D3D11_BLEND_ONE && target.DestBlend == D3D11_BLEND_ONE;
}

// for a draw that adds its colour onto the target, which is how glowing surfaces are drawn, saves its slot 0 vertex
// buffer and index buffer
static void SaveGlowBuffers(ID3D11DeviceContext *ctx, uint32_t draw)
{
    using Microsoft::WRL::ComPtr;
    if (!IsAddingColour(ctx))
        return;

    ComPtr<ID3D11Buffer> vertices, indices;
    UINT stride = 0, offset = 0, indexOffset = 0;
    DXGI_FORMAT indexFormat = DXGI_FORMAT_UNKNOWN;
    ctx->IAGetVertexBuffers(0, 1, &vertices, &stride, &offset);
    ctx->IAGetIndexBuffer(&indices, &indexFormat, &indexOffset);
    char name[32];
    sprintf_s(name, "vb_%u", draw);
    SaveGpuBuffer(ctx, vertices.Get(), name);
    sprintf_s(name, "ib_%u", draw);
    SaveGpuBuffer(ctx, indices.Get(), name);
}

// for an alpha-tested draw (b1 row c0.x above 0.5), saves its slot 0 vertex buffer and index buffer when the CPU
// rewrites them every frame
static void SaveAlphaTestedBuffers(ID3D11DeviceContext *ctx, uint32_t draw)
{
    using Microsoft::WRL::ComPtr;
    ComPtr<ID3D11Buffer> constants;
    ctx->PSGetConstantBuffers(1, 1, &constants);
    if (!constants)
        return;

    ComPtr<ID3D11DeviceContext1> ctx1;
    UINT first = 0, count = 0;
    if (SUCCEEDED(ctx->QueryInterface(IID_PPV_ARGS(&ctx1))))
    {
        constants.Reset();
        ctx1->PSGetConstantBuffers1(1, 1, &constants, &first, &count);
    }
    D3D11_BUFFER_DESC desc;
    constants->GetDesc(&desc);
    D3D11_MAPPED_SUBRESOURCE mapped;
    if (desc.Usage != D3D11_USAGE_DYNAMIC || first * 16 + 16 > desc.ByteWidth
        || FAILED(ctx->Map(constants.Get(), 0, D3D11_MAP_WRITE_NO_OVERWRITE, 0, &mapped)))
        return;
    float alphaTest = 0;
    std::memcpy(&alphaTest, static_cast<const uint8_t *>(mapped.pData) + first * 16, sizeof(float));
    ctx->Unmap(constants.Get(), 0);
    if (alphaTest <= 0.5f)
        return;

    ComPtr<ID3D11Buffer> vertices, indices;
    UINT stride = 0, offset = 0, indexOffset = 0;
    DXGI_FORMAT indexFormat = DXGI_FORMAT_UNKNOWN;
    ctx->IAGetVertexBuffers(0, 1, &vertices, &stride, &offset);
    ctx->IAGetIndexBuffer(&indices, &indexFormat, &indexOffset);
    char name[32];
    sprintf_s(name, "vb_%u", draw);
    SaveDynamicBuffer(ctx, vertices.Get(), name);
    sprintf_s(name, "ib_%u", draw);
    SaveDynamicBuffer(ctx, indices.Get(), name);
}

static std::string DescribeView(device *dev, resource_view view)
{
    if (!view.handle)
        return "-";

    const resource res = dev->get_resource_from_view(view);
    const resource_desc desc = dev->get_resource_desc(res);
    char text[64];
    if (desc.type == resource_type::buffer)
        sprintf_s(text, "r%d buffer %llu", GetResourceId(res.handle), desc.buffer.size);
    else
        sprintf_s(text, "r%d %ux%u f%u", GetResourceId(res.handle), desc.texture.width, desc.texture.height,
            static_cast<uint32_t>(desc.texture.format));
    return text;
}

static std::string DescribeShader(uint64_t handle)
{
    if (!handle)
        return "-";

    std::lock_guard guard(Lock);
    auto it = Shaders.find(handle);
    if (it == Shaders.end())
        return "?";

    UsedShaders.insert(handle);
    char text[20];
    sprintf_s(text, "%016llx", it->second.Hash);
    return text;
}

static std::string DescribeState(uint64_t handle)
{
    if (!handle)
        return "default";

    std::lock_guard guard(Lock);
    auto it = States.find(handle);
    return it == States.end() ? "?" : it->second;
}

static void WriteConstants(const char *label, const buffer_range *buffers)
{
    fprintf(Log, "  %s:", label);
    for (uint32_t i = 0; i < MAX_CONSTANT_BUFFERS; ++i)
        if (buffers[i].buffer.handle)
            fprintf(Log, " %u=r%d", i, GetResourceId(buffers[i].buffer.handle));
    fprintf(Log, "\n");
}

static void SnapshotAddedColour(command_list *cmd, ID3D11DeviceContext *ctx, uint32_t draw);

static void WriteDraw(command_list *cmd, const char *kind, uint32_t count, uint32_t instances, uint32_t first,
    int32_t baseVertex)
{
    if (State != Phase::Capturing)
        return;

    device *dev = cmd->get_device();
    auto *ctx = reinterpret_cast<ID3D11DeviceContext *>(cmd->get_native());
    ProbeCentre(ctx, DrawIndex + 1);
    D3D11_PRIMITIVE_TOPOLOGY topology = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
    ctx->IAGetPrimitiveTopology(&topology);
    fprintf(Log, "#%u %s count %u instances %u first %u base %d topology %d vs %s ps %s | blend %s | %s\n", ++DrawIndex,
        kind, count, instances, first, baseVertex, topology, DescribeShader(VertexShader).c_str(),
        DescribeShader(PixelShader).c_str(), DescribeState(BlendState).c_str(), DescribeState(DepthState).c_str());
    D3D11_VIEWPORT viewport = {};
    UINT viewportCount = 1;
    ctx->RSGetViewports(&viewportCount, &viewport);
    fprintf(Log, "  viewport %g x %g at %g %g\n", viewport.Width, viewport.Height, viewport.TopLeftX, viewport.TopLeftY);
    SaveAlphaTestedBuffers(ctx, DrawIndex);
    SaveGlowBuffers(ctx, DrawIndex);
    SnapshotAddedColour(cmd, ctx, DrawIndex);
    WriteConstants("vs cb", VertexConstants);
    WriteConstants("ps cb", PixelConstants);
    WriteDrawConstants(cmd);
    if (VertexBuffer.handle)
    {
        const bool dynamic = dev->get_resource_desc(VertexBuffer).heap == memory_heap::upload;
        fprintf(Log, "  vb r%d offset %llu stride %u%s", GetResourceId(VertexBuffer.handle), VertexOffset, VertexStride,
            dynamic ? " dynamic" : "");
        if (IndexBuffer.handle)
            fprintf(Log, " ib r%d offset %llu", GetResourceId(IndexBuffer.handle), IndexOffset);
        fprintf(Log, "\n");
    }
    fprintf(Log, "  ps srv:");
    for (uint32_t i = 0; i < MAX_SHADER_RESOURCES; ++i)
        if (PixelResources[i].handle)
            fprintf(Log, " %u=%s", i, DescribeView(dev, PixelResources[i]).c_str());
    fprintf(Log, "\n");
}

static void AddSnapshot(command_list *cmd, resource target, const char *stage);

// saves the colour target after each draw that added its colour with depth write on, and before the first of a run of
// them, so each draw's share can be told apart
static void SnapshotAddedColour(command_list *cmd, ID3D11DeviceContext *ctx, uint32_t draw)
{
    char stage[32];
    const bool follows = AddedTarget != 0;
    if (AddedTarget)
    {
        sprintf_s(stage, "added-after-%u", AddedDraw);
        AddSnapshot(cmd, { AddedTarget }, stage);
        AddedTarget = 0;
    }
    if (!draw || !RenderTargets[0].handle || !IsAddingColour(ctx))
        return;

    Microsoft::WRL::ComPtr<ID3D11DepthStencilState> depth;
    ctx->OMGetDepthStencilState(&depth, nullptr);
    D3D11_DEPTH_STENCIL_DESC depthDesc = {};
    if (depth)
        depth->GetDesc(&depthDesc);
    if (!depthDesc.DepthEnable || depthDesc.DepthWriteMask != D3D11_DEPTH_WRITE_MASK_ALL)
        return;

    const resource target = cmd->get_device()->get_resource_from_view(RenderTargets[0]);
    if (!follows)
    {
        sprintf_s(stage, "added-before-%u", draw);
        AddSnapshot(cmd, target, stage);
    }
    AddedTarget = target.handle;
    AddedDraw = draw;
}

static void AddSnapshot(command_list *cmd, resource target, const char *stage)
{
    device *dev = cmd->get_device();
    resource_desc desc = dev->get_resource_desc(target);
    // snapshots are written as 32-bit bitmaps
    if (desc.type != resource_type::texture_2d || desc.texture.samples != 1
        || format_row_pitch(desc.texture.format, 1) != 4)
        return;

    desc.texture.levels = 1;
    desc.heap = memory_heap::readback;
    desc.usage = resource_usage::copy_dest;
    desc.flags = resource_flags::none;
    resource staging;
    if (!dev->create_resource(desc, nullptr, resource_usage::copy_dest, &staging))
        return;

    cmd->copy_texture_region(target, 0, nullptr, staging, 0, nullptr);
    char name[64];
    sprintf_s(name, "r%d-%s", GetResourceId(target.handle), stage);
    Snapshots.push_back({ name, staging, desc.texture.width, desc.texture.height });
    fprintf(Log, "--- snapshot %s\n", name);
}

// saves a texture through the native context, for a snapshot between the game's draws, such as after the add-on's own;
// the staging copy goes to Snapshots, which releases it
static void AddSnapshot(ID3D11DeviceContext *ctx, ID3D11Texture2D *target, const char *stage)
{
    D3D11_TEXTURE2D_DESC desc;
    target->GetDesc(&desc);
    // snapshots are written as 32-bit bitmaps; format numbers are DXGI's
    if (desc.SampleDesc.Count != 1 || format_row_pitch(static_cast<format>(desc.Format), 1) != 4)
        return;

    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    desc.MiscFlags = 0;
    Microsoft::WRL::ComPtr<ID3D11Device> dev;
    ctx->GetDevice(&dev);
    ID3D11Texture2D *staging = nullptr;
    if (FAILED(dev->CreateTexture2D(&desc, nullptr, &staging)))
        return;

    ctx->CopySubresourceRegion(staging, 0, 0, 0, 0, target, 0, nullptr);
    char name[64];
    sprintf_s(name, "r%d-%s", GetResourceId(reinterpret_cast<uint64_t>(target)), stage);
    Snapshots.push_back({ name, { reinterpret_cast<uint64_t>(staging) }, desc.Width, desc.Height });
    fprintf(Log, "--- snapshot %s\n", name);
}

// saves the bound render target the first time point lights draw into it, before they add their light
static void SnapshotBeforeLights(command_list *cmd)
{
    if (State != Phase::Capturing || !RenderTargets[0].handle)
        return;

    const resource target = cmd->get_device()->get_resource_from_view(RenderTargets[0]);
    if (!LitTargets.insert(target.handle).second)
        return;

    AddSnapshot(cmd, target, "before-lights");
    PendingTarget = target.handle;
}

// saves each texture a lamp draw reads, once per capture, such as the fade textures that shape a lamp's light
static void SnapshotLampTextures(command_list *cmd)
{
    if (State != Phase::Capturing)
        return;

    device *dev = cmd->get_device();
    for (const resource_view &view : PixelResources)
    {
        if (!view.handle)
            continue;

        const resource texture = dev->get_resource_from_view(view);
        if (SavedLampTextures.insert(texture.handle).second)
            AddSnapshot(cmd, texture, "lamp-texture");
    }
}

static void WriteSnapshot(device *dev, const Snapshot &snapshot)
{
    subresource_data data;
    if (!dev->map_texture_region(snapshot.Staging, 0, nullptr, map_access::read_only, &data))
        return;

    FILE *file = nullptr;
    if (_wfopen_s(&file, (Folder / (snapshot.Name + ".bmp")).c_str(), L"wb") == 0)
    {
        const uint32_t imageSize = snapshot.Width * snapshot.Height * 4;
        BITMAPFILEHEADER header = { 0x4D42, sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER) + imageSize, 0, 0,
            sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER) };
        BITMAPINFOHEADER info = { sizeof(BITMAPINFOHEADER), static_cast<LONG>(snapshot.Width),
            -static_cast<LONG>(snapshot.Height), 1, 32, BI_RGB, imageSize };
        fwrite(&header, sizeof(header), 1, file);
        fwrite(&info, sizeof(info), 1, file);
        for (uint32_t y = 0; y < snapshot.Height; ++y)
            fwrite(static_cast<const uint8_t *>(data.data) + static_cast<size_t>(y) * data.row_pitch, 1,
                snapshot.Width * 4, file);
        fclose(file);
    }
    dev->unmap_texture_region(snapshot.Staging, 0);
}

static void OnBindVertexBuffers(command_list *, uint32_t first, uint32_t count, const resource *buffers,
    const uint64_t *offsets, const uint32_t *strides)
{
    if (first != 0 || !count)
        return;

    VertexBuffer = buffers[0];
    VertexOffset = offsets ? offsets[0] : 0;
    VertexStride = strides ? strides[0] : 0;
}

static void OnBindIndexBuffer(command_list *, resource buffer, uint64_t offset, uint32_t)
{
    IndexBuffer = buffer;
    IndexOffset = offset;
}

static void OnBindPipeline(command_list *, pipeline_stage stages, pipeline handle)
{
    if ((stages & pipeline_stage::vertex_shader) != 0)
        VertexShader = handle.handle;
    if ((stages & pipeline_stage::pixel_shader) != 0)
        PixelShader = handle.handle;
    if ((stages & pipeline_stage::output_merger) != 0)
        BlendState = handle.handle;
    if ((stages & pipeline_stage::depth_stencil) != 0)
        DepthState = handle.handle;
}

static void OnBindRenderTargets(command_list *cmd, uint32_t count, const resource_view *rtvs, resource_view dsv)
{
    if (State == Phase::Capturing && PendingTarget
        && (!count || !rtvs[0].handle || cmd->get_device()->get_resource_from_view(rtvs[0]).handle != PendingTarget))
    {
        AddSnapshot(cmd, { PendingTarget }, "after-lights");
        PendingTarget = 0;
    }

    RenderTargetCount = std::min(count, MAX_RENDER_TARGETS);
    for (uint32_t i = 0; i < MAX_RENDER_TARGETS; ++i)
        RenderTargets[i] = i < RenderTargetCount ? rtvs[i] : resource_view {};
    DepthTarget = dsv;

    if (State != Phase::Capturing)
        return;

    device *dev = cmd->get_device();
    fprintf(Log, "--- render targets:");
    for (uint32_t i = 0; i < RenderTargetCount; ++i)
        fprintf(Log, " %u=%s", i, DescribeView(dev, RenderTargets[i]).c_str());
    fprintf(Log, " depth=%s\n", DescribeView(dev, DepthTarget).c_str());
}

static void OnPushDescriptors(command_list *, shader_stage stages, pipeline_layout, uint32_t, const descriptor_table_update &update)
{
    const bool vertex = (stages & shader_stage::vertex) != 0;
    const bool pixel = (stages & shader_stage::pixel) != 0;
    for (uint32_t i = 0; i < update.count; ++i)
    {
        const uint32_t slot = update.binding + i;
        if (update.type == descriptor_type::constant_buffer && slot < MAX_CONSTANT_BUFFERS)
        {
            const buffer_range range = static_cast<const buffer_range *>(update.descriptors)[i];
            if (vertex)
                VertexConstants[slot] = range;
            if (pixel)
                PixelConstants[slot] = range;
        }
        else if (update.type == descriptor_type::shader_resource_view && pixel && slot < MAX_SHADER_RESOURCES)
            PixelResources[slot] = static_cast<const resource_view *>(update.descriptors)[i];
    }
}

static bool IsVertexBuffer(device *dev, resource res)
{
    return (dev->get_resource_desc(res).usage & resource_usage::vertex_buffer) != 0;
}

static void OnMapBuffer(device *dev, resource res, uint64_t, uint64_t, map_access access, void **)
{
    if (State == Phase::Capturing && IsVertexBuffer(dev, res))
        fprintf(Log, "--- cpu writes vb r%d (%s)\n", GetResourceId(res.handle),
            access == map_access::write_discard ? "discard" : "no overwrite");
}

static bool OnUpdateBuffer(device *dev, const void *, resource dest, uint64_t, uint64_t size)
{
    if (State == Phase::Capturing && IsVertexBuffer(dev, dest))
        fprintf(Log, "--- cpu updates vb r%d (%llu bytes)\n", GetResourceId(dest.handle), size);
    return false;
}

static bool OnDraw(command_list *cmd, uint32_t vertices, uint32_t instances, uint32_t firstVertex, uint32_t)
{
    WriteDraw(cmd, "draw", vertices, instances, firstVertex, 0);
    return false;
}

static bool OnDrawIndexed(command_list *cmd, uint32_t indices, uint32_t instances, uint32_t firstIndex,
    int32_t vertexOffset, uint32_t)
{
    WriteDraw(cmd, "draw_indexed", indices, instances, firstIndex, vertexOffset);
    return false;
}

static bool OnDrawIndirect(command_list *cmd, indirect_command, resource, uint64_t, uint32_t draws, uint32_t)
{
    WriteDraw(cmd, "draw_indirect", draws, 0, 0, 0);
    return false;
}

static bool OnClearRenderTarget(command_list *cmd, resource_view rtv, const float color[4], uint32_t, const rect *)
{
    if (State == Phase::Capturing)
        fprintf(Log, "--- clear %s to %g %g %g %g\n", DescribeView(cmd->get_device(), rtv).c_str(), color[0], color[1],
            color[2], color[3]);
    return false;
}

static bool OnClearDepth(command_list *cmd, resource_view dsv, const float *depth, const uint8_t *, uint32_t, const rect *)
{
    if (State == Phase::Capturing)
        fprintf(Log, "--- clear depth %s to %g\n", DescribeView(cmd->get_device(), dsv).c_str(), depth ? *depth : -1.0f);
    return false;
}

static void WriteShaderLayout(FILE *file, const Shader &shader)
{
    fprintf(file, "%s %016llx (%zu bytes)\n", shader.Stage, shader.Hash, shader.Code.size());
    ID3D11ShaderReflection *reflection = nullptr;
    if (FAILED(D3DReflect(shader.Code.data(), shader.Code.size(), IID_ID3D11ShaderReflection,
            reinterpret_cast<void **>(&reflection))))
    {
        fprintf(file, "  no reflection data\n\n");
        return;
    }

    D3D11_SHADER_DESC desc;
    reflection->GetDesc(&desc);
    for (UINT i = 0; i < desc.InputParameters; ++i)
    {
        D3D11_SIGNATURE_PARAMETER_DESC param;
        reflection->GetInputParameterDesc(i, &param);
        fprintf(file, "  in %s%u\n", param.SemanticName, param.SemanticIndex);
    }
    for (UINT i = 0; i < desc.OutputParameters; ++i)
    {
        D3D11_SIGNATURE_PARAMETER_DESC param;
        reflection->GetOutputParameterDesc(i, &param);
        fprintf(file, "  out %s%u\n", param.SemanticName, param.SemanticIndex);
    }
    for (UINT i = 0; i < desc.BoundResources; ++i)
    {
        D3D11_SHADER_INPUT_BIND_DESC bind;
        reflection->GetResourceBindingDesc(i, &bind);
        fprintf(file, "  bind %s type %d slot %u count %u\n", bind.Name, bind.Type, bind.BindPoint, bind.BindCount);
    }
    for (UINT i = 0; i < desc.ConstantBuffers; ++i)
    {
        ID3D11ShaderReflectionConstantBuffer *buffer = reflection->GetConstantBufferByIndex(i);
        D3D11_SHADER_BUFFER_DESC bufferDesc;
        buffer->GetDesc(&bufferDesc);
        fprintf(file, "  cbuffer %s (%u bytes)\n", bufferDesc.Name, bufferDesc.Size);
        for (UINT j = 0; j < bufferDesc.Variables; ++j)
        {
            ID3D11ShaderReflectionVariable *variable = buffer->GetVariableByIndex(j);
            D3D11_SHADER_VARIABLE_DESC variableDesc;
            variable->GetDesc(&variableDesc);
            D3D11_SHADER_TYPE_DESC typeDesc;
            variable->GetType()->GetDesc(&typeDesc);
            fprintf(file, "    %s %s", typeDesc.Name ? typeDesc.Name : "?", variableDesc.Name);
            if (typeDesc.Elements)
                fprintf(file, "[%u]", typeDesc.Elements);
            fprintf(file, " offset %u size %u\n", variableDesc.StartOffset, variableDesc.Size);
        }
    }
    fprintf(file, "\n");
    reflection->Release();
}

static void Begin()
{
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    SYSTEMTIME time;
    GetLocalTime(&time);
    wchar_t name[32];
    swprintf_s(name, L"%04u%02u%02u-%02u%02u%02u", time.wYear, time.wMonth, time.wDay, time.wHour, time.wMinute,
        time.wSecond);
    Folder = std::filesystem::path(exe).parent_path() / L"EQ2Graphics-captures" / name;
    std::error_code error;
    std::filesystem::create_directories(Folder / L"shaders", error);
    std::filesystem::create_directories(Folder / L"buffers", error);

    if (_wfopen_s(&Log, (Folder / L"frame.txt").c_str(), L"w") != 0)
    {
        Status = "Could not create the capture file.";
        State = Phase::Idle;
        return;
    }

    DrawIndex = 0;
    UsedShaders.clear();
    ResourceIds.clear();
    LitTargets.clear();
    SavedLampTextures.clear();
    PendingTarget = 0;
    Probes.clear();
    State = Phase::Capturing;
}

static void End(command_queue *queue)
{
    if (PendingTarget)
        AddSnapshot(queue->get_immediate_command_list(), { PendingTarget }, "after-lights");
    PendingTarget = 0;
    SnapshotAddedColour(queue->get_immediate_command_list(),
        reinterpret_cast<ID3D11DeviceContext *>(queue->get_immediate_command_list()->get_native()), 0);

    auto *ctx = reinterpret_cast<ID3D11DeviceContext *>(queue->get_immediate_command_list()->get_native());
    ProbeCentre(ctx, DrawIndex + 1);
    State = Phase::Idle;
    fclose(Log);
    Log = nullptr;

    device *dev = queue->get_device();
    queue->flush_immediate_command_list();
    WriteCentreDraws(ctx);
    Probes.clear();
    ProbeCopy.Reset();
    ProbeStaging.Reset();
    for (const Snapshot &snapshot : Snapshots)
    {
        WriteSnapshot(dev, snapshot);
        dev->destroy_resource(snapshot.Staging);
    }
    Snapshots.clear();

    FILE *layouts = nullptr;
    if (_wfopen_s(&layouts, (Folder / L"shaders.txt").c_str(), L"w") == 0)
    {
        std::lock_guard guard(Lock);
        for (uint64_t handle : UsedShaders)
        {
            auto it = Shaders.find(handle);
            if (it == Shaders.end())
                continue;

            WriteShaderLayout(layouts, it->second);
            char file[40];
            sprintf_s(file, "%s_%016llx.cso", it->second.Stage, it->second.Hash);
            FILE *code = nullptr;
            if (fopen_s(&code, (Folder / L"shaders" / file).string().c_str(), "wb") == 0)
            {
                fwrite(it->second.Code.data(), 1, it->second.Code.size(), code);
                fclose(code);
            }
        }
        fclose(layouts);
    }

    char text[64];
    sprintf_s(text, "Captured %u draws, %zu shaders.", DrawIndex, UsedShaders.size());
    Status = text + std::string(" Saved to ") + Folder.string();
}

static void OnPresent(command_queue *queue, swapchain *, const rect *, const rect *, uint32_t, const rect *)
{
    if (State == Phase::Capturing)
        End(queue);
    else if (State == Phase::Requested)
        Begin();
}

static void Request()
{
    if (State == Phase::Idle)
        State = Phase::Requested;
}

static void DrawSettings()
{
    ImGui::BeginDisabled(State != Phase::Idle);
    if (ImGui::Button("Capture frame"))
        Request();
    ImGui::EndDisabled();
    ImGui::TextWrapped("Draws covering the middle of the screen are listed in centre.txt.");
    if (!Status.empty())
        ImGui::TextWrapped("%s", Status.c_str());
}

static void Register()
{
    reshade::register_event<reshade::addon_event::init_pipeline>(&tq_world::Guard<&OnInitPipeline>::Call);
    reshade::register_event<reshade::addon_event::destroy_pipeline>(&tq_world::Guard<&OnDestroyPipeline>::Call);
    reshade::register_event<reshade::addon_event::bind_pipeline>(&tq_world::WorkGuard<&OnBindPipeline>::Call);
    reshade::register_event<reshade::addon_event::bind_render_targets_and_depth_stencil>(&tq_world::WorkGuard<&OnBindRenderTargets>::Call);
    reshade::register_event<reshade::addon_event::push_descriptors>(&tq_world::WorkGuard<&OnPushDescriptors>::Call);
    reshade::register_event<reshade::addon_event::bind_vertex_buffers>(&tq_world::WorkGuard<&OnBindVertexBuffers>::Call);
    reshade::register_event<reshade::addon_event::bind_index_buffer>(&tq_world::WorkGuard<&OnBindIndexBuffer>::Call);
    reshade::register_event<reshade::addon_event::map_buffer_region>(&tq_world::WorkGuard<&OnMapBuffer>::Call);
    reshade::register_event<reshade::addon_event::update_buffer_region>(&tq_world::WorkGuard<&OnUpdateBuffer>::Call);
    reshade::register_event<reshade::addon_event::draw>(&tq_world::WorkGuard<&OnDraw>::Call);
    reshade::register_event<reshade::addon_event::draw_indexed>(&tq_world::WorkGuard<&OnDrawIndexed>::Call);
    reshade::register_event<reshade::addon_event::draw_or_dispatch_indirect>(&tq_world::WorkGuard<&OnDrawIndirect>::Call);
    reshade::register_event<reshade::addon_event::clear_render_target_view>(&tq_world::WorkGuard<&OnClearRenderTarget>::Call);
    reshade::register_event<reshade::addon_event::clear_depth_stencil_view>(&tq_world::WorkGuard<&OnClearDepth>::Call);
    reshade::register_event<reshade::addon_event::present>(&tq_world::Guard<&OnPresent>::Call);
}
}
