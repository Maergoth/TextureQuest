// Qualified whole-primitive native material observation. No native targets/resources are written.
#pragma once
#include <d3d11_1.h>
#include <d3dcompiler.h>
#include <DirectXMath.h>
#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <functional>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace object_chart
{
using Microsoft::WRL::ComPtr;
using DirectX::XMFLOAT3;using DirectX::XMFLOAT4;using DirectX::XMFLOAT4X4;
constexpr unsigned MaxTriangles=4096,Tile=8,Width=512,Height=512,Samples=28,Latency=3;
constexpr uint64_t VertexProfile=0xd9379a14dac0f2edull,PixelProfile=0x7a8c2bd9056f65beull;
struct Draw {bool indexed=false;UINT count=0,first=0;INT base=0;};
struct Parameters {XMFLOAT4X4 inverse;XMFLOAT4 offset;};
struct Result
{
    uint64_t owner=0,ticket=0,epoch=0,sequence=0,geometry=0,material=0;
    bool valid=false;unsigned triangles=0;bool superseded=false;
    std::vector<XMFLOAT4> positions;
    std::vector<XMFLOAT3> colours;
};
struct Packet
{
    ComPtr<ID3D11Texture2D> colours;
    ComPtr<ID3D11Buffer> positions;
    ComPtr<ID3D11Query> ready,count;
    Result result;
    std::vector<ComPtr<ID3D11Resource>> borrowed;
    std::vector<std::pair<uint64_t,uint64_t>> revisions;
    bool busy=false;
};
struct Stats {uint64_t queued=0,complete=0,invalid=0,unsupported=0,busy=0,superseded=0;unsigned pending=0;};
inline Stats Counts;
inline std::atomic<bool> Active=false;
inline std::mutex VersionLock;
inline std::unordered_map<uint64_t,uint64_t> Versions;
inline uint64_t NextVersion=1,NextSequence=1,Epoch=0;
inline uint64_t Track(ID3D11Resource *resource)
{
    const auto handle=reinterpret_cast<uint64_t>(resource);std::lock_guard guard(VersionLock);
    return Versions.try_emplace(handle,NextVersion++).first->second;
}
inline void NoteWrite(uint64_t handle)
{
    if(!Active.load(std::memory_order_relaxed))return;
    std::lock_guard guard(VersionLock);auto found=Versions.find(handle);if(found!=Versions.end())found->second=NextVersion++;
}
inline void ForgetResource(uint64_t handle)
{if(!Active.load(std::memory_order_relaxed))return;std::lock_guard guard(VersionLock);Versions.erase(handle);}
inline void Mix(uint64_t &hash,uint64_t value){hash^=value;hash*=1099511628211ull;}
// Native caster hashing mixes whole signed integers and aliases nearby negative grid coordinates. The canonical
// chart uses fixed-width coordinate bytes instead; insertion and child lookup also validate the actual coordinates.
inline uint64_t CellKey(int level,int x,int y,int z)
{
    uint64_t hash=14695981039346656037ull;
    for(int coordinate:{level,x,y,z})
    {
        const uint32_t bits=static_cast<uint32_t>(coordinate);
        for(unsigned byte=0;byte<4;++byte)Mix(hash,(bits>>(byte*8))&255u);
    }
    return hash;
}
inline uint64_t ResourceKey(ID3D11Resource *resource,uint64_t *recorded=nullptr)
{uint64_t key=reinterpret_cast<uint64_t>(resource),revision=Track(resource);if(recorded)*recorded=revision;Mix(key,revision);return key;}
inline bool StaticInput(ID3D11Buffer *buffer)
{
    if(!buffer)return true;D3D11_BUFFER_DESC d{};buffer->GetDesc(&d);
    return (d.Usage==D3D11_USAGE_DEFAULT||d.Usage==D3D11_USAGE_IMMUTABLE)&&!d.CPUAccessFlags
        &&!(d.BindFlags&(D3D11_BIND_STREAM_OUTPUT|D3D11_BIND_UNORDERED_ACCESS))
        &&!(d.MiscFlags&(D3D11_RESOURCE_MISC_SHARED|D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX|D3D11_RESOURCE_MISC_SHARED_NTHANDLE));
}

inline constexpr const char *GeometryShader=R"(
cbuffer Parameters:register(b0){float4 Rows[4];float4 Offset;};
cbuffer NativeShared:register(b1){float4 s[11];};
struct Native{float4 position:SV_Position;float4 color0:COLOR0;float4 color1:COLOR1;
 float4 uv[9]:TEXCOORD0;float4 fog:FOG;float4 size:PSIZE;};
struct Out{float4 position:SV_Position;float4 color0:COLOR0;float4 color1:COLOR1;
 float4 uv[9]:TEXCOORD0;float4 fog:FOG;float4 size:PSIZE;float4 world:WORLD;};
[maxvertexcount(3)]void main(triangle Native v[3],uint id:SV_PrimitiveID,inout TriangleStream<Out> stream)
{
 bool safe=all(s[1].xyz<.5)&&s[7].x<=.5;
 [unroll]for(uint j=0;j<11;++j)safe=safe&&all(isfinite(s[j]));
 float2 origin=float2(id%64,id/64)*8;
 [unroll]for(uint i=0;i<3;++i)
 {
  // This exact VS supplies the unbiased native clip coordinate in TEXCOORD8.
  float4 clip=v[i].uv[8];float4 p=float4(dot(Rows[0],clip),dot(Rows[1],clip),dot(Rows[2],clip),dot(Rows[3],clip));
  bool valid=safe&&all(isfinite(p))&&abs(p.w)>1e-8;
  Out o;o.color0=v[i].color0;o.color1=v[i].color1;o.fog=v[i].fog;o.size=v[i].size;
  [unroll]for(uint k=0;k<9;++k){o.uv[k]=v[i].uv[k];valid=valid&&all(isfinite(v[i].uv[k]));}
  o.world=float4(valid?p.xyz/p.w-Offset.xyz:0,valid?1:0);
  float2 pixel=origin+(i==1?float2(7.5,0):(i==2?float2(0,7.5):0));
  o.position=float4(pixel.x/256-1,1-pixel.y/256,.5,1);stream.Append(o);
 }
 stream.RestartStrip();
}
)";
inline constexpr const char *CloneShader=R"(
cbuffer NativeShared:register(b1){uint4 Words[11];};
RWByteAddressBuffer Private:register(u0);
[numthreads(16,1,1)]void main(uint3 id:SV_DispatchThreadID)
{if(id.x>=11)return;uint4 value=Words[id.x];if(id.x==9)value.w=0;Private.Store4(id.x*16,value);}
)";
inline ComPtr<ID3DDeviceContextState> State;
inline ComPtr<ID3D11GeometryShader> Shape;
inline ComPtr<ID3D11ComputeShader> Clone;
inline ComPtr<ID3D11Buffer> Constants,RawShared,PrivateShared,Output;
inline ComPtr<ID3D11UnorderedAccessView> RawView;
inline ComPtr<ID3D11Texture2D> Atlas;
inline ComPtr<ID3D11RenderTargetView> Target;
inline ComPtr<ID3D11RasterizerState> Raster;
inline ComPtr<ID3D11DepthStencilState> Depth;
inline std::array<Packet,Latency> Packets;
inline bool Failed=false;
inline void Invalidate(uint64_t epoch,bool active)
{
    Epoch=epoch;Active.store(active,std::memory_order_relaxed);
    std::lock_guard guard(VersionLock);Versions.clear();
    // Busy packets remain fenced; the epoch drops their observations without reusing staging resources prematurely.
}
inline void Reset()
{
    Invalidate(0,false);for(auto &packet:Packets)packet={};State.Reset();Shape.Reset();Clone.Reset();Constants.Reset();
    RawShared.Reset();PrivateShared.Reset();RawView.Reset();Output.Reset();Atlas.Reset();Target.Reset();Raster.Reset();Depth.Reset();Failed=false;Counts={};
}
inline bool Create(ID3D11Device *device)
{
    if(Failed)return false;if(State)return true;Failed=true;
    ComPtr<ID3D11Device1> dev;auto level=device->GetFeatureLevel();
    if(FAILED(device->QueryInterface(IID_PPV_ARGS(&dev)))||FAILED(dev->CreateDeviceContextState(0,&level,1,D3D11_SDK_VERSION,
        __uuidof(ID3D11Device),nullptr,&State)))return false;
    ComPtr<ID3DBlob> code,error;
    if(FAILED(D3DCompile(GeometryShader,strlen(GeometryShader),nullptr,nullptr,nullptr,"main","gs_5_0",D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&code,&error)))return false;
    D3D11_SO_DECLARATION_ENTRY declaration{0,"WORLD",0,0,4,0};UINT stride=16;
    if(FAILED(device->CreateGeometryShaderWithStreamOutput(code->GetBufferPointer(),code->GetBufferSize(),&declaration,1,&stride,1,0,nullptr,&Shape)))return false;
    code.Reset();error.Reset();if(FAILED(D3DCompile(CloneShader,strlen(CloneShader),nullptr,nullptr,nullptr,"main","cs_5_0",D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&code,&error))
        ||FAILED(device->CreateComputeShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,&Clone)))return false;
    D3D11_BUFFER_DESC b{sizeof(Parameters),D3D11_USAGE_DEFAULT,D3D11_BIND_CONSTANT_BUFFER};
    if(FAILED(device->CreateBuffer(&b,nullptr,&Constants)))return false;
    b.ByteWidth=11*16;if(FAILED(device->CreateBuffer(&b,nullptr,&PrivateShared)))return false;
    b.BindFlags=D3D11_BIND_UNORDERED_ACCESS;b.MiscFlags=D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
    if(FAILED(device->CreateBuffer(&b,nullptr,&RawShared)))return false;
    D3D11_UNORDERED_ACCESS_VIEW_DESC u{};u.Format=DXGI_FORMAT_R32_TYPELESS;u.ViewDimension=D3D11_UAV_DIMENSION_BUFFER;
    u.Buffer.NumElements=44;u.Buffer.Flags=D3D11_BUFFER_UAV_FLAG_RAW;if(FAILED(device->CreateUnorderedAccessView(RawShared.Get(),&u,&RawView)))return false;
    b={MaxTriangles*3*16,D3D11_USAGE_DEFAULT,D3D11_BIND_STREAM_OUTPUT};if(FAILED(device->CreateBuffer(&b,nullptr,&Output)))return false;
    D3D11_TEXTURE2D_DESC t{Width,Height,1,1,DXGI_FORMAT_R32G32B32A32_FLOAT,{1,0},D3D11_USAGE_DEFAULT,D3D11_BIND_RENDER_TARGET};
    if(FAILED(device->CreateTexture2D(&t,nullptr,&Atlas))||FAILED(device->CreateRenderTargetView(Atlas.Get(),nullptr,&Target)))return false;
    t.Usage=D3D11_USAGE_STAGING;t.BindFlags=0;t.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
    b.Usage=D3D11_USAGE_STAGING;b.BindFlags=0;b.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
    for(auto &packet:Packets)
    {
        D3D11_QUERY_DESC q{D3D11_QUERY_EVENT,0};if(FAILED(device->CreateQuery(&q,&packet.ready)))return false;
        q.Query=D3D11_QUERY_SO_STATISTICS;if(FAILED(device->CreateQuery(&q,&packet.count))
            ||FAILED(device->CreateTexture2D(&t,nullptr,&packet.colours))||FAILED(device->CreateBuffer(&b,nullptr,&packet.positions)))return false;
    }
    D3D11_RASTERIZER_DESC r{};r.FillMode=D3D11_FILL_SOLID;r.CullMode=D3D11_CULL_NONE;r.DepthClipEnable=TRUE;
    D3D11_DEPTH_STENCIL_DESC d{};d.DepthFunc=D3D11_COMPARISON_ALWAYS;
    if(FAILED(device->CreateRasterizerState(&r,&Raster))||FAILED(device->CreateDepthStencilState(&d,&Depth)))return false;
    Failed=false;return true;
}
enum class Admission {Unsupported,Busy,Queued};
inline Admission Capture(ID3D11DeviceContext *native,const Draw &draw,uint64_t owner,uint64_t ticket,uint64_t epoch,
    uint64_t vertexHash,uint64_t pixelHash,const Parameters &parameters)
{
    if(!Active.load(std::memory_order_relaxed)||epoch!=Epoch||vertexHash!=VertexProfile||pixelHash!=PixelProfile
        ||!draw.count||draw.count%3||draw.count/3>MaxTriangles||native->GetType()!=D3D11_DEVICE_CONTEXT_IMMEDIATE)
    {++Counts.unsupported;return Admission::Unsupported;}
    Packet *packet=nullptr;for(auto &entry:Packets)if(!entry.busy){packet=&entry;break;}
    if(!packet){++Counts.busy;return Admission::Busy;}
    ComPtr<ID3D11DeviceContext1> ctx;ComPtr<ID3D11Device> device;native->GetDevice(&device);
    if(FAILED(native->QueryInterface(IID_PPV_ARGS(&ctx)))||!Create(device.Get())){++Counts.unsupported;return Admission::Unsupported;}
    ComPtr<ID3D11GeometryShader> gs;ComPtr<ID3D11HullShader> hs;ComPtr<ID3D11DomainShader> ds;
    ctx->GSGetShader(&gs,nullptr,nullptr);ctx->HSGetShader(&hs,nullptr,nullptr);ctx->DSGetShader(&ds,nullptr,nullptr);
    D3D11_PRIMITIVE_TOPOLOGY topology{};ctx->IAGetPrimitiveTopology(&topology);
    if(topology!=D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST||gs||hs||ds){++Counts.unsupported;return Admission::Unsupported;}
    ComPtr<ID3D11VertexShader> vs;ComPtr<ID3D11PixelShader> ps;UINT classes=0;ctx->VSGetShader(&vs,nullptr,&classes);
    if(classes)return Admission::Unsupported;ctx->PSGetShader(&ps,nullptr,&classes);if(classes||!vs||!ps)return Admission::Unsupported;
    ComPtr<ID3D11InputLayout> layout;ComPtr<ID3D11Buffer> ib;DXGI_FORMAT format{};UINT ibOffset=0;
    ctx->IAGetInputLayout(&layout);ctx->IAGetIndexBuffer(&ib,&format,&ibOffset);if(!StaticInput(ib.Get()))return Admission::Unsupported;
    std::array<ComPtr<ID3D11Buffer>,32> vertices;ID3D11Buffer *rawVertices[32]{};UINT strides[32]{},offsets[32]{};
    ctx->IAGetVertexBuffers(0,32,rawVertices,strides,offsets);for(unsigned i=0;i<32;++i){vertices[i].Attach(rawVertices[i]);if(!StaticInput(rawVertices[i]))return Admission::Unsupported;}
    std::array<ComPtr<ID3D11Buffer>,2> vb,pb;ID3D11Buffer *v[2]{},*p[2]{};UINT vf[2]{},vc[2]{},pf[2]{},pc[2]{};
    ctx->VSGetConstantBuffers1(0,2,v,vf,vc);ctx->PSGetConstantBuffers1(0,2,p,pf,pc);
    for(unsigned i=0;i<2;++i){vb[i].Attach(v[i]);pb[i].Attach(p[i]);}
    if(!v[0]||!p[1]||pc[1]&&pc[1]<11)return Admission::Unsupported;
    D3D11_BUFFER_DESC shared{};p[1]->GetDesc(&shared);if(uint64_t(pf[1])*16+11*16>shared.ByteWidth)return Admission::Unsupported;
    std::array<ComPtr<ID3D11ShaderResourceView>,3> views;std::array<ComPtr<ID3D11SamplerState>,3> samplers;
    std::vector<ComPtr<ID3D11Resource>> borrowed;uint64_t geometry=1469598103934665603ull,material=geometry;
    std::vector<std::pair<uint64_t,uint64_t>> revisions;
    const auto record=[&](ID3D11Resource *resource){uint64_t revision=0;const auto key=ResourceKey(resource,&revision);
        revisions.emplace_back(reinterpret_cast<uint64_t>(resource),revision);return key;};
    Mix(geometry,owner);Mix(geometry,draw.count);Mix(geometry,draw.first);Mix(geometry,uint64_t(draw.base));Mix(geometry,ibOffset);Mix(geometry,format);
    if(ib){Mix(geometry,record(ib.Get()));borrowed.push_back(ib);}
    for(unsigned i=0;i<32;++i)if(vertices[i]){Mix(geometry,record(vertices[i].Get()));Mix(geometry,strides[i]);Mix(geometry,offsets[i]);borrowed.push_back(vertices[i]);}
    for(UINT slot=0;slot<3;++slot)
    {
        ctx->PSGetShaderResources(slot,1,&views[slot]);ctx->PSGetSamplers(slot,1,&samplers[slot]);if(!views[slot]||!samplers[slot])return Admission::Unsupported;
        D3D11_SHADER_RESOURCE_VIEW_DESC sd{};views[slot]->GetDesc(&sd);ComPtr<ID3D11Resource> resource;views[slot]->GetResource(&resource);
        ComPtr<ID3D11Texture2D> texture;if(sd.ViewDimension!=D3D11_SRV_DIMENSION_TEXTURE2D||FAILED(resource.As(&texture)))return Admission::Unsupported;
        D3D11_TEXTURE2D_DESC td{};texture->GetDesc(&td);
        if((td.Usage!=D3D11_USAGE_DEFAULT&&td.Usage!=D3D11_USAGE_IMMUTABLE)||td.CPUAccessFlags||td.SampleDesc.Count!=1
            ||td.BindFlags&(D3D11_BIND_RENDER_TARGET|D3D11_BIND_UNORDERED_ACCESS)
            ||td.MiscFlags&(D3D11_RESOURCE_MISC_SHARED|D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX|D3D11_RESOURCE_MISC_SHARED_NTHANDLE))return Admission::Unsupported;
        Mix(material,record(resource.Get()));Mix(material,reinterpret_cast<uint64_t>(views[slot].Get()));Mix(material,reinterpret_cast<uint64_t>(samplers[slot].Get()));borrowed.push_back(resource);
    }
    struct Restore
    {
        ID3D11DeviceContext1 *ctx;ComPtr<ID3DDeviceContextState> previous;
        ~Restore(){ctx->ClearState();ctx->SwapDeviceContextState(previous.Get(),nullptr);}
    } restore{ctx.Get()};ctx->SwapDeviceContextState(State.Get(),&restore.previous);
    if(pc[1])ctx->CSSetConstantBuffers1(1,1,&p[1],&pf[1],&pc[1]);else ctx->CSSetConstantBuffers(1,1,&p[1]);
    ID3D11UnorderedAccessView *raw=RawView.Get();ctx->CSSetUnorderedAccessViews(0,1,&raw,nullptr);ctx->CSSetShader(Clone.Get(),nullptr,0);ctx->Dispatch(1,1,1);
    ID3D11UnorderedAccessView *noUav=nullptr;ctx->CSSetUnorderedAccessViews(0,1,&noUav,nullptr);ctx->CopyResource(PrivateShared.Get(),RawShared.Get());
    ctx->CSSetShader(nullptr,nullptr,0);ID3D11Buffer *empty[32]{};ctx->CSSetConstantBuffers(0,2,empty);
    ctx->UpdateSubresource(Constants.Get(),0,nullptr,&parameters,0,0);ctx->IASetInputLayout(layout.Get());ctx->IASetVertexBuffers(0,32,rawVertices,strides,offsets);
    ctx->IASetIndexBuffer(ib.Get(),format,ibOffset);ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);ctx->VSSetShader(vs.Get(),nullptr,0);
    for(UINT i=0;i<2;++i){if(vc[i])ctx->VSSetConstantBuffers1(i,1,&v[i],&vf[i],&vc[i]);else ctx->VSSetConstantBuffers(i,1,&v[i]);}
    ID3D11Buffer *constants=Constants.Get();ctx->GSSetConstantBuffers(0,1,&constants);
    if(pc[1])ctx->GSSetConstantBuffers1(1,1,&p[1],&pf[1],&pc[1]);else ctx->GSSetConstantBuffers(1,1,&p[1]);
    ctx->GSSetShader(Shape.Get(),nullptr,0);ctx->PSSetShader(ps.Get(),nullptr,0);ID3D11Buffer *privateShared=PrivateShared.Get();ctx->PSSetConstantBuffers(1,1,&privateShared);
    for(UINT i=0;i<3;++i){ID3D11ShaderResourceView *view=views[i].Get();ID3D11SamplerState *sampler=samplers[i].Get();ctx->PSSetShaderResources(i,1,&view);ctx->PSSetSamplers(i,1,&sampler);}
    const FLOAT zero[4]{};ctx->ClearRenderTargetView(Target.Get(),zero);ID3D11RenderTargetView *target=Target.Get();ctx->OMSetRenderTargets(1,&target,nullptr);
    ctx->OMSetDepthStencilState(Depth.Get(),0);ctx->OMSetBlendState(nullptr,nullptr,0xffffffff);ctx->RSSetState(Raster.Get());
    const D3D11_VIEWPORT viewport{0,0,float(Width),float(Height),0,1};ctx->RSSetViewports(1,&viewport);
    ID3D11Buffer *output=Output.Get();UINT start=0;ctx->SOSetTargets(1,&output,&start);ctx->Begin(packet->count.Get());
    if(draw.indexed)ctx->DrawIndexed(draw.count,draw.first,draw.base);else ctx->Draw(draw.count,draw.first);
    ctx->End(packet->count.Get());ID3D11Buffer *none=nullptr;ctx->SOSetTargets(1,&none,&start);
    ctx->CopyResource(packet->positions.Get(),Output.Get());ctx->CopyResource(packet->colours.Get(),Atlas.Get());ctx->End(packet->ready.Get());
    packet->result={owner,ticket,epoch,NextSequence++,geometry,material,false,draw.count/3};packet->borrowed=std::move(borrowed);packet->revisions=std::move(revisions);packet->busy=true;++Counts.queued;
    return Admission::Queued;
}
inline void Read(ID3D11DeviceContext *ctx,uint64_t epoch,const std::function<void(Result&&)> &consume)
{
    Counts.pending=0;
    for(auto &packet:Packets)if(packet.busy)
    {
        if(ctx->GetData(packet.ready.Get(),nullptr,0,D3D11_ASYNC_GETDATA_DONOTFLUSH)!=S_OK){++Counts.pending;continue;}
        if(packet.result.epoch!=epoch||!Active.load(std::memory_order_relaxed)){packet.busy=false;packet.borrowed.clear();packet.revisions.clear();continue;}
        D3D11_QUERY_DATA_SO_STATISTICS count{};if(ctx->GetData(packet.count.Get(),&count,sizeof(count),D3D11_ASYNC_GETDATA_DONOTFLUSH)!=S_OK){++Counts.pending;continue;}
        D3D11_MAPPED_SUBRESOURCE positions{},colours{};
        if(FAILED(ctx->Map(packet.positions.Get(),0,D3D11_MAP_READ,D3D11_MAP_FLAG_DO_NOT_WAIT,&positions))){++Counts.pending;continue;}
        if(FAILED(ctx->Map(packet.colours.Get(),0,D3D11_MAP_READ,D3D11_MAP_FLAG_DO_NOT_WAIT,&colours))){ctx->Unmap(packet.positions.Get(),0);++Counts.pending;continue;}
        Result result=packet.result;result.valid=count.NumPrimitivesWritten==result.triangles&&count.PrimitivesStorageNeeded==result.triangles;
        {std::lock_guard guard(VersionLock);for(const auto &[handle,revision]:packet.revisions){const auto current=Versions.find(handle);
            if(current==Versions.end()||current->second!=revision){result.superseded=true;break;}}}
        const auto *world=static_cast<const XMFLOAT4*>(positions.pData);result.positions.assign(world,world+result.triangles*3);
        result.colours.reserve(result.triangles*Samples);
        for(unsigned triangle=0;triangle<result.triangles;++triangle)
        {
            for(unsigned i=0;i<3;++i){const auto&p=result.positions[triangle*3+i];result.valid=result.valid&&std::isfinite(p.x)&&std::isfinite(p.y)&&std::isfinite(p.z)&&p.w==1;}
            for(unsigned y=0;y<7;++y)for(unsigned x=0;x+y<=6;++x)
            {
                const auto *row=reinterpret_cast<const XMFLOAT4*>(static_cast<const uint8_t*>(colours.pData)+(triangle/64*8+y)*colours.RowPitch);
                const auto &v=row[triangle%64*8+x];result.valid=result.valid&&std::isfinite(v.x)&&std::isfinite(v.y)&&std::isfinite(v.z)&&v.x>=0&&v.y>=0&&v.z>=0;
                result.colours.push_back({v.x,v.y,v.z});
            }
        }
        ctx->Unmap(packet.colours.Get(),0);ctx->Unmap(packet.positions.Get(),0);packet.busy=false;packet.borrowed.clear();packet.revisions.clear();
        if(result.superseded)++Counts.superseded;
        if(result.valid)++Counts.complete;else ++Counts.invalid;consume(std::move(result));
    }
}
}
