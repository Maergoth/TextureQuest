#pragma once
#include "projected_shadow_policy.hpp"
#include "projected_shadow_admission.hpp"

// Private extension for the captured directional/projected world-light pass.
// Its native projection, textures, normal mapping, colour and fade are never replaced.
namespace projected_shadows
{
using Microsoft::WRL::ComPtr;
using namespace DirectX;
namespace policy = projected_shadow_policy;
constexpr UINT Slot = 13, ConstantsSlot = 2;
static policy::Slots Requests;
static ComPtr<ID3D11Texture2D> Texture;
static ComPtr<ID3D11DepthStencilView> Targets[policy::Count];
static ComPtr<ID3D11ShaderResourceView> View;
static ComPtr<ID3D11SamplerState> Sampler;
static ComPtr<ID3D11VertexShader> Vertex;
static ComPtr<ID3D11PixelShader> Pixel;
struct Constants { XMFLOAT4X4 Rows; XMFLOAT4 Camera, Params, Direction; };
static ComPtr<ID3D11Buffer> Buffer;
static XMFLOAT4X4 BuiltRows[policy::Count];
static float BuiltExtent[policy::Count];
static bool Failed;
static unsigned DrawsThisFrame, DrawsLastFrame, MapsLastFrame;

constexpr const char *Shader = R"(
cbuffer Game : register(b0) { float4 c[1024]; };
cbuffer Shared : register(b1) { float4 s[11]; };
cbuffer Projection : register(b2) { float4 Rows[4]; float4 Camera; float4 Params; float4 Direction; };
Texture2DArray<float> Maps : register(t13);
SamplerComparisonState Compare : register(s13);
struct Interpolants { float4 position : SV_Position; float3 world : WORLD_POSITION; };
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
    o.world = float3(dot(p,c[7]),dot(p,c[8]),dot(p,c[9])) - Camera.xyz;
    return o;
}
float4 ps_main(Interpolants i) : SV_Target
{
    float3 normal = cross(ddx(i.world), ddy(i.world));
    float n2 = dot(normal, normal);
    if (n2 > 1e-20) normal *= rsqrt(n2); else normal = 0;
    if (dot(normal,Direction.xyz) < 0) normal = -normal;
    float4 p = float4(i.world + normal * Params.w, 1);
    float3 q = float3(dot(p,Rows[0]),dot(p,Rows[1]),dot(p,Rows[2]));
    float2 uv = q.xy * float2(.5,-.5) + .5;
    // This is only an occluder map. Outside it the original light is untouched.
    if (!all(isfinite(q)) || any(uv < Params.y) || any(uv > 1-Params.y) || q.z <= 0 || q.z >= 1)
        return float4(0,0,0,1);
    float lit = 0;
    [unroll] for (int y=-1;y<=1;++y)
        [unroll] for (int x=-1;x<=1;++x)
            lit += Maps.SampleCmpLevelZero(Compare,float3(uv+float2(x,y)*Params.y,Params.z),q.z-Params.y*.25);
    return float4(0,0,0,1-saturate(Params.x)*(1-lit/9));
}
)";

static void Invalidate() { Requests.Invalidate(); }
static void Reset()
{
    Requests = {}; Failed = false; Texture.Reset(); View.Reset(); Sampler.Reset();
    Vertex.Reset(); Pixel.Reset(); Buffer.Reset();
    for (auto &target : Targets) target.Reset();
    DrawsThisFrame = DrawsLastFrame = MapsLastFrame = 0;
}
static bool Create(ID3D11Device *dev)
{
    if (Buffer) return true;
    if (Failed) return false;
    Failed = true;
    ComPtr<ID3DBlob> vs, ps, errors;
    if (FAILED(D3DCompile(Shader, strlen(Shader), nullptr, nullptr, nullptr, "vs_main", "vs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &vs, &errors))
        || FAILED(D3DCompile(Shader, strlen(Shader), nullptr, nullptr, nullptr, "ps_main", "ps_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &ps, &errors)))
        return false;
    D3D11_TEXTURE2D_DESC texture = {policy::Size,policy::Size,1,policy::Count,DXGI_FORMAT_R32_TYPELESS,{1,0},D3D11_USAGE_DEFAULT,
        D3D11_BIND_DEPTH_STENCIL|D3D11_BIND_SHADER_RESOURCE};
    if (FAILED(dev->CreateTexture2D(&texture,nullptr,&Texture))) return false;
    for (UINT i=0;i<policy::Count;++i)
    {
        D3D11_DEPTH_STENCIL_VIEW_DESC target{}; target.Format=DXGI_FORMAT_D32_FLOAT;
        target.ViewDimension=D3D11_DSV_DIMENSION_TEXTURE2DARRAY; target.Texture2DArray={0,i,1};
        if (FAILED(dev->CreateDepthStencilView(Texture.Get(),&target,&Targets[i]))) return false;
    }
    D3D11_SHADER_RESOURCE_VIEW_DESC view{}; view.Format=DXGI_FORMAT_R32_FLOAT;
    view.ViewDimension=D3D11_SRV_DIMENSION_TEXTURE2DARRAY; view.Texture2DArray={0,1,0,policy::Count};
    D3D11_SAMPLER_DESC sampler{}; sampler.Filter=D3D11_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT;
    sampler.AddressU=sampler.AddressV=sampler.AddressW=D3D11_TEXTURE_ADDRESS_BORDER;
    std::fill(std::begin(sampler.BorderColor),std::end(sampler.BorderColor),1.0f);
    sampler.ComparisonFunc=D3D11_COMPARISON_LESS_EQUAL;sampler.MaxLOD=D3D11_FLOAT32_MAX;
    D3D11_BUFFER_DESC buffer{sizeof(Constants),D3D11_USAGE_DEFAULT,D3D11_BIND_CONSTANT_BUFFER};
    if (FAILED(dev->CreateShaderResourceView(Texture.Get(),&view,&View))
        || FAILED(dev->CreateSamplerState(&sampler,&Sampler))
        || FAILED(dev->CreateVertexShader(vs->GetBufferPointer(),vs->GetBufferSize(),nullptr,&Vertex))
        || FAILED(dev->CreatePixelShader(ps->GetBufferPointer(),ps->GetBufferSize(),nullptr,&Pixel))
        || FAILED(dev->CreateBuffer(&buffer,nullptr,&Buffer))) return false;
    Failed=false; return true;
}

// Called on the rendering thread before the existing moving geometry/worlds are cleared.
static void RenderMaps(ID3D11DeviceContext1 *ctx)
{
    DrawsLastFrame=DrawsThisFrame; DrawsThisFrame=0; MapsLastFrame=0;
    if (!shadowmaps::MovingShadows || !shadowmaps::CameraFound || shadowmaps::MovingCasters.empty())
    { Invalidate(); return; }
    bool wanted=false;
    for (const auto &r:Requests.values) wanted=wanted || r.seen==shadowmaps::FrameNumber;
    float extent=0;
    if (!wanted || !policy::Extent(shadowmaps::Range.load(),extent)) { Invalidate();return; }
    ComPtr<ID3D11Device> dev;ctx->GetDevice(&dev);
    if (!Create(dev.Get())) { Invalidate();return; }
    ComPtr<ID3DDeviceContextState> previous;
    ctx->SwapDeviceContextState(shadowmaps::DrawState.Get(),&previous);
    D3D11_VIEWPORT viewport{0,0,static_cast<float>(policy::Size),static_cast<float>(policy::Size),0,1};
    ctx->RSSetViewports(1,&viewport);ctx->RSSetState(shadowmaps::CasterRasterizer.Get());
    ctx->OMSetDepthStencilState(shadowmaps::CasterDepth.Get(),0);
    ctx->VSSetShader(shadowmaps::MovingCasterShader.Get(),nullptr,0);ctx->PSSetShader(nullptr,nullptr,0);
    ID3D11Buffer *constants=shadowmaps::MovingConstantsBuffer.Get(),*camera=shadowmaps::CameraOffsetBuffer.Get();
    ID3D11ShaderResourceView *worlds=shadowmaps::MovingWorldView.Get();
    ctx->VSSetConstantBuffers(0,1,&constants);ctx->VSSetConstantBuffers(3,1,&camera);ctx->VSSetShaderResources(13,1,&worlds);
    for (unsigned i=0;i<policy::Count;++i)
    {
        auto &record=Requests.values[i];record.valid=false;
        if (record.seen!=shadowmaps::FrameNumber) continue;
        const auto d=record.direction;
        shadowmaps::MovingConstants data{};
        if (!policy::MakeRows(d,shadowmaps::Camera.x,shadowmaps::Camera.y,shadowmaps::Camera.z,extent,data.ViewProjection)) continue;
        ctx->ClearDepthStencilView(Targets[i].Get(),D3D11_CLEAR_DEPTH,1,0);
        ctx->OMSetRenderTargets(0,nullptr,Targets[i].Get());
        for (const auto &caster:shadowmaps::MovingCasters)
        {
            data.Index[0]=caster.Index;ctx->UpdateSubresource(constants,0,nullptr,&data,0,0);
            shadowmaps::DrawCaster(ctx,caster.Mesh);
        }
        BuiltRows[i]=data.ViewProjection;BuiltExtent[i]=extent;record.built=shadowmaps::FrameNumber;record.valid=true;++MapsLastFrame;
    }
    // Do not retain borrowed game geometry in the private context-state cache.
    ID3D11Buffer *none=nullptr;ID3D11ShaderResourceView *noView=nullptr;UINT zero=0;
    ctx->IASetVertexBuffers(0,1,&none,&zero,&zero);ctx->IASetIndexBuffer(nullptr,DXGI_FORMAT_UNKNOWN,0);ctx->IASetInputLayout(nullptr);
    ctx->VSSetConstantBuffers(0,1,&none);ctx->VSSetConstantBuffers(3,1,&none);ctx->VSSetShaderResources(13,1,&noView);
    ctx->VSSetShader(nullptr,nullptr,0);ctx->OMSetRenderTargets(0,nullptr,nullptr);
    ctx->SwapDeviceContextState(previous.Get(),nullptr);
}

static bool Apply(reshade::api::command_list *cmd,const shadowmaps::DrawCall &call,const XMFLOAT4 (&rows)[shadowmaps::ROW_COUNT],
    reshade::api::pipeline alphaOnly,reshade::api::pipeline multiply,reshade::api::pipeline colorByAlpha)
{
    if (!shadowmaps::MovingShadows || shadowmaps::CreateFailed || !shadowmaps::CameraFound || call.Instances!=1) return false;
    for (const auto &row:rows)
        if (!std::isfinite(row.x)||!std::isfinite(row.y)||!std::isfinite(row.z)||!std::isfinite(row.w)) return false;
    if (!std::isfinite(shadowmaps::CurrentOffset.x)||!std::isfinite(shadowmaps::CurrentOffset.y)||!std::isfinite(shadowmaps::CurrentOffset.z)) return false;
    if (std::abs(rows[4].w)>1e7f || std::abs(rows[5].w)>1e7f || std::abs(rows[6].w)>1e7f) return false;
    policy::Direction direction;
    const auto &light=rows[13-shadowmaps::FIRST_ROW];
    if (!policy::Normalize(light.x,light.y,light.z,light.w,direction)) return false;
    ComPtr<ID3D11DeviceContext1> ctx;
    if (FAILED(reinterpret_cast<ID3D11DeviceContext *>(cmd->get_native())->QueryInterface(IID_PPV_ARGS(&ctx)))) return false;
    ComPtr<ID3D11BlendState> oldBlend;FLOAT factor[4]{};UINT mask=0;
    ctx->OMGetBlendState(&oldBlend,factor,&mask);
    ComPtr<ID3D11DepthStencilState> depth;ctx->OMGetDepthStencilState(&depth,nullptr);
    if (!oldBlend || !depth) return false;
    D3D11_BLEND_DESC blend{};D3D11_DEPTH_STENCIL_DESC depthDesc{};oldBlend->GetDesc(&blend);depth->GetDesc(&depthDesc);
    if (mask!=0xffffffffu || !ReplaySafe(blend,depthDesc)) return false;
    const int index=Requests.Observe(shadowmaps::FrameNumber,direction);
    shadowmaps::RecordDirectionalMoving(ctx.Get(),call,rows);
    const float strength=shadowmaps::Strength.load();
    if (index<0 || !Requests.Ready(static_cast<unsigned>(index),shadowmaps::FrameNumber) || !Buffer || !std::isfinite(strength)) return false;
    ComPtr<ID3D11ShaderResourceView> oldView;ComPtr<ID3D11SamplerState> oldSampler;
    ComPtr<ID3D11Buffer> oldVSBuffer,oldPSBuffer;UINT vf=0,vc=0,pf=0,pc=0;
    ComPtr<ID3D11VertexShader> oldVS;ComPtr<ID3D11PixelShader> oldPS;
    ctx->VSGetShader(&oldVS,nullptr,nullptr);ctx->PSGetShader(&oldPS,nullptr,nullptr);
    ctx->PSGetShaderResources(Slot,1,&oldView);ctx->PSGetSamplers(Slot,1,&oldSampler);
    ctx->VSGetConstantBuffers1(ConstantsSlot,1,&oldVSBuffer,&vf,&vc);ctx->PSGetConstantBuffers1(ConstantsSlot,1,&oldPSBuffer,&pf,&pc);
    Constants data{BuiltRows[index],{shadowmaps::CurrentOffset.x,shadowmaps::CurrentOffset.y,shadowmaps::CurrentOffset.z,0},
        {std::clamp(strength,0.0f,1.0f),1.0f/policy::Size,static_cast<float>(index),2*BuiltExtent[index]/policy::Size},
        {direction.x,direction.y,direction.z,0}};
    ctx->UpdateSubresource(Buffer.Get(),0,nullptr,&data,0,0);
    const auto issue=[&] { if(call.Indexed)cmd->draw_indexed(call.Count,call.Instances,call.First,call.BaseVertex,call.FirstInstance);
        else cmd->draw(call.Count,call.Instances,call.First,call.FirstInstance); };
    cmd->bind_pipeline(reshade::api::pipeline_stage::output_merger,alphaOnly);issue();
    ID3D11Buffer *buffer=Buffer.Get();ID3D11ShaderResourceView *view=View.Get();ID3D11SamplerState *sampler=Sampler.Get();
    ctx->VSSetConstantBuffers(ConstantsSlot,1,&buffer);ctx->PSSetConstantBuffers(ConstantsSlot,1,&buffer);
    ctx->PSSetShaderResources(Slot,1,&view);ctx->PSSetSamplers(Slot,1,&sampler);ctx->VSSetShader(Vertex.Get(),nullptr,0);ctx->PSSetShader(Pixel.Get(),nullptr,0);
    cmd->bind_pipeline(reshade::api::pipeline_stage::output_merger,multiply);issue();
    ctx->VSSetShader(oldVS.Get(),nullptr,0);ctx->PSSetShader(oldPS.Get(),nullptr,0);
    ID3D11ShaderResourceView *previousView=oldView.Get();ID3D11SamplerState *previousSampler=oldSampler.Get();
    ID3D11Buffer *previousVS=oldVSBuffer.Get(),*previousPS=oldPSBuffer.Get();
    ctx->PSSetShaderResources(Slot,1,&previousView);ctx->PSSetSamplers(Slot,1,&previousSampler);
    if(vc)ctx->VSSetConstantBuffers1(ConstantsSlot,1,&previousVS,&vf,&vc);else ctx->VSSetConstantBuffers(ConstantsSlot,1,&previousVS);
    if(pc)ctx->PSSetConstantBuffers1(ConstantsSlot,1,&previousPS,&pf,&pc);else ctx->PSSetConstantBuffers(ConstantsSlot,1,&previousPS);
    cmd->bind_pipeline(reshade::api::pipeline_stage::output_merger,colorByAlpha);issue();
    ctx->OMSetBlendState(oldBlend.Get(),factor,mask);++DrawsThisFrame;return true;
}
}
