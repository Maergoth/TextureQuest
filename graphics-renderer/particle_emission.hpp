#pragma once
#include "glow.hpp"
#include "particle_emission_policy.hpp"
#include "particle_field_policy.hpp"
#include "particle_coalescing_policy.hpp"
#include "particle_source_policy.hpp"
#include "particle_retention_policy.hpp"

// Observational native material replay: complete independent native draws form
// a bounded partial world-light field. Native rendering is never replaced.
namespace particle_emission
{
using Microsoft::WRL::ComPtr;using namespace DirectX;
namespace policy=particle_emission_policy;namespace field=particle_field_policy;
struct DrawRecord { unsigned first=0,count=0;uint64_t vertex=0,pixel=0;ComPtr<ID3D11Query> query;bool valid=false; };
struct Packet
{
    ComPtr<ID3D11Buffer> output,read,reduced,reducedRead;
    ComPtr<ID3D11UnorderedAccessView> reducedView;
    ComPtr<ID3D11Query> complete;
    std::array<DrawRecord,field::Draws> records;
    field::Ledger ledger;
    ComPtr<ID3D11Resource> scene,target;
    XMFLOAT3 camera{},offset{};XMFLOAT4X4 cameraMatrix{};
    std::chrono::steady_clock::time_point time{};
    bool collecting=false,busy=false,writes=false;
};
static Packet Frame;
static ComPtr<ID3DDeviceContextState> State;
static ComPtr<ID3D11GeometryShader> Shape;
static ComPtr<ID3D11ComputeShader> Reduce,CloneShared;
static ComPtr<ID3D11Buffer> Constants,ReduceConstants,SharedParameters,SharedRaw,SharedConstants;
static ComPtr<ID3D11UnorderedAccessView> SharedRawView;
static ComPtr<ID3D11Texture2D> Atlas;
static ComPtr<ID3D11RenderTargetView> AtlasTarget;
static ComPtr<ID3D11ShaderResourceView> AtlasView;
static ComPtr<ID3D11RasterizerState> Raster;
static ComPtr<ID3D11DepthStencilState> NoDepth;
static bool Failed=false,Probing=false;
static uint64_t Epoch=1,ObservedFrame=UINT64_MAX,ExcludedSourceFrame=UINT64_MAX;
static std::vector<field::Cell> Cells;
struct Selected {field::Key cell;uint64_t light=0;};
static std::array<Selected,field::Lit> SelectedCells;
static unsigned SelectedCount=0;
static std::atomic<float> Strength=1.0f,Radius=1.0f;
static std::chrono::steady_clock::time_point FieldTime{};
static ComPtr<ID3D11Resource> FieldScene;
static bool HasField=false;
static particle_retention_policy::Cache Retained;
static particle_source_policy::Groups Sources;
static double Seconds(std::chrono::steady_clock::time_point time){return std::chrono::duration<double>(time.time_since_epoch()).count();}
static double SecondsNow(){return Seconds(std::chrono::steady_clock::now());}
static std::vector<field::Cell> VisibleCells(double now=SecondsNow())
{return Retained.Merge(Cells,HasField&&!Cells.empty()?field::Visibility(now-Seconds(FieldTime)):0,Epoch,now);}
static unsigned Seen=0,Measured=0,Active=0,Pending=0,RejectedBounds=0,RejectedState=0,RejectedIO=0;
static unsigned CompleteFrames=0,IncompleteFrames=0,OverflowFrames=0,SkippedFrames=0;
struct DomainStats
{
    uint64_t frame=0,epoch=0;unsigned draws=0,triangles=0,included=0,excludedDistance=0,excludedExtent=0,invalid=0;
    unsigned firstExcluded=UINT32_MAX,firstInvalid=UINT32_MAX;bool inspected=false,published=false;
    unsigned attempted=0,capacityExcluded=0,stateExcluded=0,invalidDraws=0,acceptedDraws=0;
};
static DomainStats LastDomain;
static std::mutex ShaderLock;
static std::unordered_map<uint64_t,uint64_t> VertexProfiles,PixelProfiles;
static void RememberShader(uint64_t hash,uint64_t handle,bool vertex)
{
    if(vertex?policy::VertexProfile(hash):policy::PixelProfile(hash))
    {std::lock_guard guard(ShaderLock);(vertex?VertexProfiles:PixelProfiles)[handle]=hash;}
}
static void ForgetShader(uint64_t handle)
{std::lock_guard guard(ShaderLock);VertexProfiles.erase(handle);PixelProfiles.erase(handle);}
static bool Profiles(uint64_t vs,uint64_t ps,uint64_t &vhash,uint64_t &phash)
{
    std::lock_guard guard(ShaderLock);const auto v=VertexProfiles.find(vs),p=PixelProfiles.find(ps);
    if(v==VertexProfiles.end()||p==PixelProfiles.end()||!policy::Profile(v->second,p->second))return false;
    vhash=v->second;phash=p->second;return true;
}
static bool IsProfile(uint64_t vs,uint64_t ps){uint64_t v=0,p=0;return Profiles(vs,ps,v,p);}
struct ShapeConstants {XMFLOAT4X4 inverse;XMFLOAT4 offset;UINT base=0,padding[3]{};};
constexpr const char *Shader=R"(
cbuffer Inverse:register(b0){float4 Rows[4];float4 Offset;uint Base;uint3 pad;};
cbuffer Shared:register(b1){float4 s[11];};
struct Native {float4 position:SV_Position;float4 color0:COLOR0;float4 color1:COLOR1;
 float4 uv[9]:TEXCOORD0;float4 fog:FOG;float4 size:PSIZE;};
struct Out {float4 position:SV_Position;float4 color0:COLOR0;float4 color1:COLOR1;
 float4 uv[9]:TEXCOORD0;float4 fog:FOG;float4 size:PSIZE;float4 worldPosition:WORLD;};
[maxvertexcount(3)] void main(triangle Native vertices[3],uint id:SV_PrimitiveID,inout TriangleStream<Out> stream)
{
 bool safe=s[1].x<.5 && s[7].x<=.5 && (s[9].w<=.5 || all(abs(s[9].xyz)<1e-6));
 [unroll]for(uint j=0;j<11;++j)safe=safe&&all(isfinite(s[j]));
 uint tile=Base+id;float2 origin=float2(tile%128,tile/128)*8;
 [unroll]for(uint i=0;i<3;++i)
 {
  float4 clip=vertices[i].position;
  if(s[10].x>.5)clip.xy+=float2(-s[10].z,s[10].w)*clip.w;
  float4 p=float4(dot(Rows[0],clip),dot(Rows[1],clip),dot(Rows[2],clip),dot(Rows[3],clip));
  Out o;o.color0=vertices[i].color0;o.color1=vertices[i].color1;
  [unroll]for(uint k=0;k<9;++k)o.uv[k]=vertices[i].uv[k];
  o.fog=vertices[i].fog;o.size=vertices[i].size;
  bool valid=safe&&all(isfinite(p))&&abs(p.w)>1e-8;
  o.worldPosition=float4(valid?p.xyz/p.w-Offset.xyz:0,valid?1:0);
  float2 pixel=origin+(i==1?float2(7.5,0):(i==2?float2(0,7.5):0));
  o.position=float4(pixel.x/512-1,1-pixel.y/1024,.5,1);
  stream.Append(o);
 }
 stream.RestartStrip();
}
)";
// Integer loads/stores preserve every bit of the actually visible native b1
// window. Only row9.w is replaced, and only on our private PS binding.
constexpr const char *SharedShader=R"(
cbuffer Parameters:register(b0){uint SharedRows;uint3 padding;};
cbuffer NativeShared:register(b1){uint4 Words[4096];};
RWByteAddressBuffer Private:register(u0);
[numthreads(16,1,1)]void main(uint3 id:SV_DispatchThreadID)
{uint i=id.x;if(i>=4096)return;uint4 value=i<SharedRows?Words[i]:0;if(i==9)value.w=0;Private.Store4(i*16,value);}
)";
constexpr const char *ReduceShader=R"(
cbuffer Parameters:register(b0){uint Triangles;uint3 padding;};
Texture2D<float4> Image:register(t0);
struct Result{float4 mean;float4 moment;};RWStructuredBuffer<Result> Results:register(u0);
groupshared float4 P[64];groupshared float4 M[64];
[numthreads(8,8,1)]void main(uint3 local:SV_GroupThreadID,uint3 group:SV_GroupID)
{
 uint index=local.y*8+local.x;float4 value=0,moment=0;
 if(group.x<Triangles&&local.x+local.y<=6)
 {
  uint2 pixel=uint2(group.x%128,group.x/128)*8+local.xy;float4 raw=Image.Load(int3(pixel,0));
  bool valid=all(isfinite(raw))&&all(raw.xyz>=0)&&raw.w>=0&&raw.w<=1;
  float3 energy=valid?raw.rgb*raw.a:0;float l=dot(energy,float3(.2126,.7152,.0722));
  value=float4(energy,valid?0:1);moment=float4(l*(local.x+.5)/7.5,l*(local.y+.5)/7.5,l,0);
 }
 P[index]=value;M[index]=moment;GroupMemoryBarrierWithGroupSync();
 [unroll]for(uint step=32;step>0;step/=2){if(index<step){P[index]+=P[index+step];M[index]+=M[index+step];}GroupMemoryBarrierWithGroupSync();}
 if(index==0&&group.x<Triangles){Result r;r.mean=float4(P[0].xyz/28,P[0].w);r.moment=M[0];Results[group.x]=r;}
}
)";
static void RemoveLights()
{for(auto &s:SelectedCells){if(s.light)shadowmaps::Lights.erase(s.light);s={};}SelectedCount=Active=0;}
static void InvalidateScene()
{
    RemoveLights();Cells.clear();Retained.Clear();Sources.Clear();HasField=false;FieldScene.Reset();++Epoch;
    shadowmaps::particle_moving::Request(false,Epoch);
    // Do not reuse or release the in-flight packet here. Its event fence and
    // epoch are retired nonblocking in ReadFrame, even across master-off.
    if(Frame.collecting)Frame.ledger.Reject();
}
static void Hide(){InvalidateScene();Seen=Measured=0;}
static void Reset(bool keepProfiles=false)
{
    InvalidateScene();Frame={};State.Reset();Shape.Reset();Reduce.Reset();CloneShared.Reset();Constants.Reset();ReduceConstants.Reset();
    SharedParameters.Reset();SharedRaw.Reset();SharedConstants.Reset();SharedRawView.Reset();Atlas.Reset();AtlasTarget.Reset();AtlasView.Reset();Raster.Reset();NoDepth.Reset();
    Failed=false;ObservedFrame=ExcludedSourceFrame=UINT64_MAX;Pending=RejectedBounds=RejectedState=RejectedIO=0;
    LastDomain={};
    if(!keepProfiles){std::lock_guard guard(ShaderLock);VertexProfiles.clear();PixelProfiles.clear();}
}
static bool Compile(ID3D11Device *dev)
{
    if(Failed)return false;if(State)return true;Failed=true;
    ComPtr<ID3D11Device1> d;const auto level=dev->GetFeatureLevel();
    if(FAILED(dev->QueryInterface(IID_PPV_ARGS(&d)))||FAILED(d->CreateDeviceContextState(0,&level,1,D3D11_SDK_VERSION,__uuidof(ID3D11Device),nullptr,&State)))return false;
    ComPtr<ID3DBlob> code,errors;
    if(FAILED(D3DCompile(Shader,strlen(Shader),nullptr,nullptr,nullptr,"main","gs_5_0",D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&code,&errors)))return false;
    const D3D11_SO_DECLARATION_ENTRY declaration{0,"WORLD",0,0,4,0};const UINT stride=16;
    if(FAILED(dev->CreateGeometryShaderWithStreamOutput(code->GetBufferPointer(),code->GetBufferSize(),&declaration,1,&stride,1,0,nullptr,&Shape)))return false;
    code.Reset();errors.Reset();if(FAILED(D3DCompile(SharedShader,strlen(SharedShader),nullptr,nullptr,nullptr,"main","cs_5_0",D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&code,&errors))
        ||FAILED(dev->CreateComputeShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,&CloneShared)))return false;
    code.Reset();errors.Reset();if(FAILED(D3DCompile(ReduceShader,strlen(ReduceShader),nullptr,nullptr,nullptr,"main","cs_5_0",D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&code,&errors))
        ||FAILED(dev->CreateComputeShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,&Reduce)))return false;
    D3D11_BUFFER_DESC b{sizeof(ShapeConstants),D3D11_USAGE_DEFAULT,D3D11_BIND_CONSTANT_BUFFER};
    if(FAILED(dev->CreateBuffer(&b,nullptr,&Constants)))return false;
    b.ByteWidth=16;if(FAILED(dev->CreateBuffer(&b,nullptr,&ReduceConstants))||FAILED(dev->CreateBuffer(&b,nullptr,&SharedParameters)))return false;
    b.ByteWidth=65536;if(FAILED(dev->CreateBuffer(&b,nullptr,&SharedConstants)))return false;
    b.BindFlags=D3D11_BIND_UNORDERED_ACCESS;b.MiscFlags=D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
    if(FAILED(dev->CreateBuffer(&b,nullptr,&SharedRaw)))return false;
    D3D11_UNORDERED_ACCESS_VIEW_DESC u{};u.Format=DXGI_FORMAT_R32_TYPELESS;u.ViewDimension=D3D11_UAV_DIMENSION_BUFFER;u.Buffer.NumElements=65536/4;u.Buffer.Flags=D3D11_BUFFER_UAV_FLAG_RAW;
    if(FAILED(dev->CreateUnorderedAccessView(SharedRaw.Get(),&u,&SharedRawView)))return false;
    b={field::Triangles*3*16,D3D11_USAGE_DEFAULT,D3D11_BIND_STREAM_OUTPUT};
    if(FAILED(dev->CreateBuffer(&b,nullptr,&Frame.output)))return false;
    b.Usage=D3D11_USAGE_STAGING;b.BindFlags=0;b.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
    if(FAILED(dev->CreateBuffer(&b,nullptr,&Frame.read)))return false;
    b={field::Triangles*sizeof(field::Reduction),D3D11_USAGE_DEFAULT,D3D11_BIND_UNORDERED_ACCESS,0,D3D11_RESOURCE_MISC_BUFFER_STRUCTURED,sizeof(field::Reduction)};
    if(FAILED(dev->CreateBuffer(&b,nullptr,&Frame.reduced)))return false;
    u={};u.ViewDimension=D3D11_UAV_DIMENSION_BUFFER;u.Buffer.NumElements=field::Triangles;
    if(FAILED(dev->CreateUnorderedAccessView(Frame.reduced.Get(),&u,&Frame.reducedView)))return false;
    b.Usage=D3D11_USAGE_STAGING;b.BindFlags=0;b.CPUAccessFlags=D3D11_CPU_ACCESS_READ;b.MiscFlags=0;b.StructureByteStride=0;
    if(FAILED(dev->CreateBuffer(&b,nullptr,&Frame.reducedRead)))return false;
    D3D11_TEXTURE2D_DESC image{field::AtlasWidth,field::AtlasHeight,1,1,DXGI_FORMAT_R32G32B32A32_FLOAT,{1,0},D3D11_USAGE_DEFAULT,D3D11_BIND_RENDER_TARGET|D3D11_BIND_SHADER_RESOURCE};
    if(FAILED(dev->CreateTexture2D(&image,nullptr,&Atlas))||FAILED(dev->CreateRenderTargetView(Atlas.Get(),nullptr,&AtlasTarget))||FAILED(dev->CreateShaderResourceView(Atlas.Get(),nullptr,&AtlasView)))return false;
    D3D11_RASTERIZER_DESC raster{};raster.FillMode=D3D11_FILL_SOLID;raster.CullMode=D3D11_CULL_NONE;raster.DepthClipEnable=TRUE;
    if(FAILED(dev->CreateRasterizerState(&raster,&Raster)))return false;
    D3D11_DEPTH_STENCIL_DESC depth{};depth.DepthWriteMask=D3D11_DEPTH_WRITE_MASK_ZERO;depth.DepthFunc=D3D11_COMPARISON_ALWAYS;
    if(FAILED(dev->CreateDepthStencilState(&depth,&NoDepth)))return false;
    D3D11_QUERY_DESC q{D3D11_QUERY_EVENT,0};if(FAILED(dev->CreateQuery(&q,&Frame.complete)))return false;
    q.Query=D3D11_QUERY_SO_STATISTICS;
    for(auto &record:Frame.records)if(FAILED(dev->CreateQuery(&q,&record.query)))return false;
    Failed=false;return true;
}
// Cache every native reference locally, swap to an owned state, then clear the
// cache and restore. No native render target/depth or buffer is modified.
static bool Capture(ID3D11DeviceContext1 *ctx,const shadowmaps::DrawCall &call,const ShapeConstants &data,DrawRecord &record)
{
    if(Probing||ctx->GetType()!=D3D11_DEVICE_CONTEXT_IMMEDIATE)return false;
    ComPtr<ID3D11GeometryShader> gs;ComPtr<ID3D11HullShader> hs;ComPtr<ID3D11DomainShader> ds;
    ctx->GSGetShader(&gs,nullptr,nullptr);ctx->HSGetShader(&hs,nullptr,nullptr);ctx->DSGetShader(&ds,nullptr,nullptr);if(gs||hs||ds)return false;
    ComPtr<ID3D11VertexShader> vs;ComPtr<ID3D11PixelShader> ps;UINT classes=0;ctx->VSGetShader(&vs,nullptr,&classes);if(classes)return false;
    ctx->PSGetShader(&ps,nullptr,&classes);if(classes||!vs||!ps)return false;
    ID3D11Buffer *vbs[32]{};UINT strides[32]{},offsets[32]{};ctx->IAGetVertexBuffers(0,32,vbs,strides,offsets);
    ComPtr<ID3D11InputLayout> layout;ComPtr<ID3D11Buffer> ib;DXGI_FORMAT format{};UINT ibOffset=0;
    ctx->IAGetInputLayout(&layout);ctx->IAGetIndexBuffer(&ib,&format,&ibOffset);
    ID3D11Buffer *vb[2]{},*pb[2]{};UINT vf[2]{},vc[2]{},pf[2]{},pc[2]{};
    ctx->VSGetConstantBuffers1(0,2,vb,vf,vc);ctx->PSGetConstantBuffers1(0,2,pb,pf,pc);
    const auto release=[&]{for(auto *b:vbs)if(b)b->Release();for(auto *b:vb)if(b)b->Release();for(auto *b:pb)if(b)b->Release();};
    if(!vb[0]||!vb[1]||!pb[0]||vb[1]!=pb[1]||vf[1]!=pf[1]||vc[1]!=pc[1]){release();return false;}
    D3D11_BUFFER_DESC shared{};pb[1]->GetDesc(&shared);
    const uint64_t first=uint64_t(pf[1])*16;
    if(first>shared.ByteWidth||shared.ByteWidth-first<11*16||pc[1]&&pc[1]<11){release();return false;}
    const UINT sharedRows=std::min({4096u,UINT((shared.ByteWidth-first)/16),pc[1]?pc[1]:4096u});
    ComPtr<ID3D11ShaderResourceView> texture,cube;ComPtr<ID3D11SamplerState> sampler;
    ctx->PSGetShaderResources(0,1,&texture);ctx->PSGetShaderResources(36,1,&cube);ctx->PSGetSamplers(0,1,&sampler);
    if(!texture||!sampler){release();return false;}
    D3D11_SHADER_RESOURCE_VIEW_DESC td{};texture->GetDesc(&td);ComPtr<ID3D11Resource> resource;texture->GetResource(&resource);
    if(td.ViewDimension!=D3D11_SRV_DIMENSION_TEXTURE2D||resource.Get()==lightbuffer::LightBuffer.Get()
        ||resource.Get()==lightbuffer::CurrentTarget||resource.Get()==lightbuffer::CurrentDepth){release();return false;}
    struct ProbeScope{ProbeScope(){Probing=true;}~ProbeScope(){Probing=false;}} probing;
    ComPtr<ID3DDeviceContextState> previous;ctx->SwapDeviceContextState(State.Get(),&previous);
    const UINT parameters[4]{sharedRows,0,0,0};ctx->UpdateSubresource(SharedParameters.Get(),0,nullptr,parameters,0,0);
    ID3D11Buffer *parameterBuffer=SharedParameters.Get();ctx->CSSetConstantBuffers(0,1,&parameterBuffer);
    if(pc[1])ctx->CSSetConstantBuffers1(1,1,&pb[1],&pf[1],&pc[1]);else ctx->CSSetConstantBuffers(1,1,&pb[1]);
    ID3D11UnorderedAccessView *raw=SharedRawView.Get();ctx->CSSetUnorderedAccessViews(0,1,&raw,nullptr);ctx->CSSetShader(CloneShared.Get(),nullptr,0);ctx->Dispatch(256,1,1);
    ID3D11UnorderedAccessView *noUav=nullptr;ctx->CSSetUnorderedAccessViews(0,1,&noUav,nullptr);ctx->CopyResource(SharedConstants.Get(),SharedRaw.Get());
    ctx->CSSetShader(nullptr,nullptr,0);ID3D11Buffer *empty[32]{};ctx->CSSetConstantBuffers(0,2,empty);
    ctx->UpdateSubresource(Constants.Get(),0,nullptr,&data,0,0);
    ctx->IASetInputLayout(layout.Get());ctx->IASetVertexBuffers(0,32,vbs,strides,offsets);ctx->IASetIndexBuffer(ib.Get(),format,ibOffset);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);ctx->VSSetShader(vs.Get(),nullptr,0);
    for(UINT i=0;i<2;++i){if(vc[i])ctx->VSSetConstantBuffers1(i,1,&vb[i],&vf[i],&vc[i]);else ctx->VSSetConstantBuffers(i,1,&vb[i]);}
    ID3D11Buffer *privateConstants=Constants.Get();ctx->GSSetConstantBuffers(0,1,&privateConstants);
    if(vc[1])ctx->GSSetConstantBuffers1(1,1,&vb[1],&vf[1],&vc[1]);else ctx->GSSetConstantBuffers(1,1,&vb[1]);
    ctx->GSSetShader(Shape.Get(),nullptr,0);ctx->PSSetShader(ps.Get(),nullptr,0);
    if(pc[0])ctx->PSSetConstantBuffers1(0,1,&pb[0],&pf[0],&pc[0]);else ctx->PSSetConstantBuffers(0,1,&pb[0]);
    ID3D11Buffer *privateShared=SharedConstants.Get();ctx->PSSetConstantBuffers(1,1,&privateShared);
    ID3D11ShaderResourceView *t=texture.Get(),*c=cube.Get();ID3D11SamplerState *s=sampler.Get();
    ctx->PSSetShaderResources(0,1,&t);ctx->PSSetShaderResources(36,1,&c);ctx->PSSetSamplers(0,1,&s);
    if(data.base==0){const FLOAT zero[4]{};ctx->ClearRenderTargetView(AtlasTarget.Get(),zero);}
    ID3D11RenderTargetView *target=AtlasTarget.Get();ctx->OMSetRenderTargets(1,&target,nullptr);ctx->OMSetDepthStencilState(NoDepth.Get(),0);
    ctx->OMSetBlendState(nullptr,nullptr,0xffffffff);ctx->RSSetState(Raster.Get());
    D3D11_VIEWPORT viewport{0,0,float(field::AtlasWidth),float(field::AtlasHeight),0,1};ctx->RSSetViewports(1,&viewport);
    ID3D11Buffer *output=Frame.output.Get();UINT start=data.base*3*16;ctx->SOSetTargets(1,&output,&start);ctx->Begin(record.query.Get());
    if(call.Indexed)ctx->DrawIndexed(call.Count,call.First,call.BaseVertex);else ctx->Draw(call.Count,call.First);
    Frame.writes=true;ctx->End(record.query.Get());ID3D11Buffer *none=nullptr;UINT zero=0;ctx->SOSetTargets(1,&none,&zero);
    // Remove all borrowed native state from the owned cache before restoration.
    UINT zeros[32]{};ID3D11ShaderResourceView *noView=nullptr;ID3D11SamplerState *noSampler=nullptr;
    ctx->IASetVertexBuffers(0,32,empty,zeros,zeros);ctx->IASetIndexBuffer(nullptr,DXGI_FORMAT_UNKNOWN,0);ctx->IASetInputLayout(nullptr);
    ctx->VSSetShader(nullptr,nullptr,0);ctx->PSSetShader(nullptr,nullptr,0);ctx->GSSetShader(nullptr,nullptr,0);
    ctx->VSSetConstantBuffers(0,2,empty);ctx->PSSetConstantBuffers(0,2,empty);ctx->GSSetConstantBuffers(0,2,empty);
    ctx->PSSetShaderResources(0,1,&noView);ctx->PSSetShaderResources(36,1,&noView);ctx->PSSetSamplers(0,1,&noSampler);
    ctx->OMSetRenderTargets(0,nullptr,nullptr);ctx->RSSetState(nullptr);ctx->OMSetDepthStencilState(nullptr,0);
    ctx->SwapDeviceContextState(previous.Get(),nullptr);release();return true;
}
static bool BeginFrame(ID3D11DeviceContext *native)
{
    const uint64_t now=shadowmaps::FrameNumber;
    if(ObservedFrame==now)return Frame.collecting;
    ObservedFrame=now;
    if(Frame.busy){++SkippedFrames;return false;}
    if(!emission_policy::Samples.Take(now,emission_policy::Producer::Particle))return false;
    Frame.ledger.Begin(now,Epoch);Frame.collecting=true;Frame.writes=false;Frame.time=std::chrono::steady_clock::now();Frame.scene=lightbuffer::LightBuffer;
    Frame.target=lightbuffer::CurrentTarget;Frame.camera=shadowmaps::Camera;Frame.offset=shadowmaps::CurrentOffset;
    XMStoreFloat4x4(&Frame.cameraMatrix,lightbuffer::WorldToClip);
    (void)native;return true;
}
static bool AdditiveSource(const D3D11_BLEND_DESC &b,const D3D11_DEPTH_STENCIL_DESC &d,UINT mask)
{
    const auto &a=b.RenderTarget[0];
    return !b.AlphaToCoverageEnable&&!b.IndependentBlendEnable&&a.BlendEnable&&a.SrcBlend==D3D11_BLEND_SRC_ALPHA
        &&a.DestBlend==D3D11_BLEND_ONE&&a.BlendOp==D3D11_BLEND_OP_ADD&&(a.RenderTargetWriteMask&7)==7
        &&d.DepthEnable&&d.DepthWriteMask==D3D11_DEPTH_WRITE_MASK_ZERO&&mask==0xffffffffu;
}
static void Observe(shadowmaps::BoundDraw &bound,uint64_t vs,uint64_t ps)
{
    if(!glow::ParticlesActive()||Probing)return;uint64_t vh=0,ph=0;if(!Profiles(vs,ps,vh,ph))return;
    auto *native=bound.Context;const shadowmaps::DrawCall &call=bound.Call;
    // These same native shaders also draw ordinary alpha-over fire layers.
    // Those layers are outside this explicitly additive emission domain; they
    // must neither poison a complete additive packet nor be counted as energy.
    UINT mask=0;const D3D11_BLEND_DESC *blend=bound.GetBlend(&mask);const D3D11_DEPTH_STENCIL_DESC *depth=bound.GetDepth();
    if(!blend||!depth)return;
    if(!AdditiveSource(*blend,*depth,mask))return;
    if(capture::State==capture::Phase::Capturing&&call.Instances==1&&call.Count<=field::Triangles*3)
        capture::SaveGlowBuffers(native,capture::DrawIndex,true);
    const auto reject=[] {ExcludedSourceFrame=shadowmaps::FrameNumber;if(Frame.collecting&&ObservedFrame==shadowmaps::FrameNumber)Frame.ledger.Exclude();++RejectedState;};
    if(!lightbuffer::Processed||!shadowmaps::CameraFound||!lightbuffer::WorldToClipKnown
        ||lightbuffer::CurrentTarget==lightbuffer::LightBuffer.Get()){reject();return;}
    D3D11_TEXTURE2D_DESC target{};if(!lightbuffer::IsScreenSized(lightbuffer::CurrentTarget,target)){reject();return;}
    if(bound.GetTopology()!=D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST){reject();return;}
    UINT views=0;const D3D11_VIEWPORT &viewport=bound.GetViewport(views);
    if(views!=1||viewport.TopLeftX||viewport.TopLeftY||viewport.Width!=target.Width||viewport.Height!=target.Height){reject();return;}
    XMMATRIX camera;
    if(!bound.ReadRows()){reject();return;}
    const auto &rows=bound.Rows;
    for(const auto &row:rows)if(!std::isfinite(row.x)||!std::isfinite(row.y)||!std::isfinite(row.z)||!std::isfinite(row.w)){reject();return;}
    if(!shadowmaps::FindWorldToClip(rows,camera)||!lightbuffer::IsSameCamera(camera,lightbuffer::WorldToClip)){reject();return;}
    // An unrelated first pass must not anchor the packet to the wrong target.
    emission_policy::Samples.NoteParticle(shadowmaps::FrameNumber);if(!BeginFrame(native))return;Frame.ledger.Attempt();
    if(Frame.scene.Get()!=lightbuffer::LightBuffer.Get()||Frame.target.Get()!=lightbuffer::CurrentTarget
        ||!lightbuffer::IsSameCamera(camera,XMLoadFloat4x4(&Frame.cameraMatrix))
        ||shadowmaps::CurrentOffset.x!=Frame.offset.x||shadowmaps::CurrentOffset.y!=Frame.offset.y||shadowmaps::CurrentOffset.z!=Frame.offset.z){reject();return;}
    ComPtr<ID3D11DeviceContext1> ctx;ComPtr<ID3D11Device> dev;native->GetDevice(&dev);
    if(FAILED(native->QueryInterface(IID_PPV_ARGS(&ctx)))||!Compile(dev.Get())){Frame.ledger.Reject();++RejectedIO;return;}
    const unsigned base=Frame.ledger.triangles;
    if(!Frame.ledger.Add(call.Count,call.Instances)){++OverflowFrames;return;}
    ShapeConstants data{};XMStoreFloat4x4(&data.inverse,XMMatrixInverse(nullptr,camera));data.offset={Frame.offset.x,Frame.offset.y,Frame.offset.z,0};data.base=base;
    auto &record=Frame.records[Frame.ledger.draws-1];record.first=base;record.count=call.Count/3;record.vertex=vh;record.pixel=ph;record.valid=true;
    if(!Capture(ctx.Get(),call,data,record)){record.valid=false;Frame.ledger.Undo(call.Count);++RejectedState;}
}
static void Seal(ID3D11DeviceContext *native)
{
    if(!Frame.collecting)return;Frame.collecting=false;
    if(!Frame.ledger.complete||!Frame.ledger.triangles||!State)
    {
        if(!Frame.ledger.complete)++IncompleteFrames;
        else if(!Frame.ledger.attempted){Cells.clear();HasField=true;FieldTime=std::chrono::steady_clock::now();FieldScene=Frame.scene;++CompleteFrames;}
        // No admitted source is not an empty scene: exclusions retain only the
        // existing short retirement window, never refresh stale source energy.
        // Even an incomplete frame may contain queued SO/atlas commands. Fence
        // those commands before allowing the single packet storage to be reused.
        if(Frame.writes&&Frame.complete){native->End(Frame.complete.Get());Frame.busy=true;}
        else {Frame.scene.Reset();Frame.target.Reset();}
        return;
    }
    ComPtr<ID3D11DeviceContext1> ctx;if(FAILED(native->QueryInterface(IID_PPV_ARGS(&ctx))))
    {Frame.ledger.Reject();++RejectedIO;if(Frame.writes&&Frame.complete){native->End(Frame.complete.Get());Frame.busy=true;}return;}
    ComPtr<ID3DDeviceContextState> previous;ctx->SwapDeviceContextState(State.Get(),&previous);
    const UINT parameters[4]{Frame.ledger.triangles,0,0,0};ctx->UpdateSubresource(ReduceConstants.Get(),0,nullptr,parameters,0,0);
    ID3D11Buffer *cb=ReduceConstants.Get();ID3D11ShaderResourceView *image=AtlasView.Get();ID3D11UnorderedAccessView *output=Frame.reducedView.Get();
    ctx->CSSetConstantBuffers(0,1,&cb);ctx->CSSetShaderResources(0,1,&image);ctx->CSSetUnorderedAccessViews(0,1,&output,nullptr);ctx->CSSetShader(Reduce.Get(),nullptr,0);
    ctx->Dispatch(Frame.ledger.triangles,1,1);
    ID3D11UnorderedAccessView *noUav=nullptr;ID3D11ShaderResourceView *noView=nullptr;ID3D11Buffer *noBuffer=nullptr;
    ctx->CSSetUnorderedAccessViews(0,1,&noUav,nullptr);ctx->CSSetShaderResources(0,1,&noView);ctx->CSSetConstantBuffers(0,1,&noBuffer);ctx->CSSetShader(nullptr,nullptr,0);
    ctx->SwapDeviceContextState(previous.Get(),nullptr);
    native->CopyResource(Frame.read.Get(),Frame.output.Get());native->CopyResource(Frame.reducedRead.Get(),Frame.reduced.Get());native->End(Frame.complete.Get());Frame.busy=true;
}
static void ReadFrame(ID3D11DeviceContext *ctx)
{
    if (!glow::ParticlesActive() && (HasField || SelectedCount || Frame.collecting)) Hide();
    // No exact selected draw in this frame is a complete empty observation,
    // only when no earlier frame owns the one packet and this is our service
    // turn. A frame whose draws were skipped while pending is never empty.
    if(glow::ParticlesActive()&&!Frame.busy&&ObservedFrame!=shadowmaps::FrameNumber&&ExcludedSourceFrame!=shadowmaps::FrameNumber
        &&emission_policy::Samples.Recent(shadowmaps::FrameNumber)
        &&shadowmaps::CameraFound&&lightbuffer::WorldToClipKnown&&lightbuffer::LightBuffer)
        BeginFrame(ctx);
    Seal(ctx);
    if(!glow::ParticlesActive()){if(HasField||SelectedCount)Hide();}
    if(Frame.busy)
    {
        BOOL complete=FALSE;const auto result=ctx->GetData(Frame.complete.Get(),&complete,sizeof(complete),D3D11_ASYNC_GETDATA_DONOTFLUSH);
        if(result==S_OK&&complete)
        {
            DomainStats domain;domain.frame=Frame.ledger.frame;domain.epoch=Frame.ledger.epoch;
            domain.draws=Frame.ledger.draws;domain.triangles=Frame.ledger.triangles;
            domain.attempted=Frame.ledger.attempted;domain.capacityExcluded=Frame.ledger.capacityExcluded;domain.stateExcluded=Frame.ledger.stateExcluded;
            bool valid=Frame.ledger.CanPublish(Epoch,std::chrono::duration<double>(std::chrono::steady_clock::now()-Frame.time).count())
                &&glow::ParticlesActive()&&Frame.scene.Get()==lightbuffer::LightBuffer.Get();
            if(valid)for(unsigned i=0;i<Frame.ledger.draws;++i)
            {auto &record=Frame.records[i];D3D11_QUERY_DATA_SO_STATISTICS stats{};const auto state=ctx->GetData(record.query.Get(),&stats,sizeof(stats),D3D11_ASYNC_GETDATA_DONOTFLUSH);
                if(state==S_FALSE){Pending=1;return;}
                if(state!=S_OK){valid=false;break;}
                record.valid=stats.NumPrimitivesWritten==record.count&&stats.PrimitivesStorageNeeded==record.count;
                if(!record.valid)++domain.invalidDraws;}
            D3D11_MAPPED_SUBRESOURCE positions{},material{};
            bool positionMapped=false,materialMapped=false;
            if(valid){const auto mapped=ctx->Map(Frame.read.Get(),0,D3D11_MAP_READ,D3D11_MAP_FLAG_DO_NOT_WAIT,&positions);
                if(mapped==DXGI_ERROR_WAS_STILL_DRAWING){Pending=1;return;}positionMapped=SUCCEEDED(mapped);valid=positionMapped;}
            if(valid){const auto mapped=ctx->Map(Frame.reducedRead.Get(),0,D3D11_MAP_READ,D3D11_MAP_FLAG_DO_NOT_WAIT,&material);
                if(mapped==DXGI_ERROR_WAS_STILL_DRAWING){ctx->Unmap(Frame.read.Get(),0);Pending=1;return;}materialMapped=SUCCEEDED(mapped);valid=materialMapped;}
            std::vector<particle_source_policy::Sample> candidate;if(valid)candidate.reserve(field::Triangles);
            if(valid)
            {
                domain.inspected=true;
                const auto *world=static_cast<const XMFLOAT4*>(positions.pData);const auto *raw=static_cast<const field::Reduction*>(material.pData);
                for(unsigned i=0;i<Frame.ledger.draws;++i)
                {
                    const auto &record=Frame.records[i];if(!record.valid)continue;
                    const size_t start=candidate.size();bool sourceValid=true;
                    for(unsigned t=record.first;t<record.first+record.count;++t)
                    {
                        field::Power energy;XMFLOAT3 location;field::Exclusion reason;
                        const auto kind=field::ClassifyTriangle(world+t*3,raw[t],Frame.camera,energy,location,&reason,field::LightRadius(Radius.load(),field::MaximumWorldRadius));
                        if(kind==field::TriangleClass::Invalid)
                        {++domain.invalid;if(domain.firstInvalid==UINT32_MAX)domain.firstInvalid=t;sourceValid=false;break;}
                        if(kind==field::TriangleClass::Excluded)
                        {
                            if(reason==field::Exclusion::Distance)++domain.excludedDistance;else ++domain.excludedExtent;
                            if(domain.firstExcluded==UINT32_MAX)domain.firstExcluded=t;
                            continue;
                        }
                        if(std::max({energy.x,energy.y,energy.z})>1e20){sourceValid=false;break;}
                        ++domain.included;
                        // One exact area-integrated sample per accepted native
                        // triangle. Keep dark geometry observed too: a real
                        // zero-power animation must replace cached brightness.
                        if(candidate.size()>=particle_source_policy::MaximumSamples){sourceValid=false;break;}
                        candidate.push_back({location,energy});
                    }
                    if(!sourceValid){candidate.resize(start);++domain.invalidDraws;}
                    else ++domain.acceptedDraws;
                }
            }
            if(materialMapped)ctx->Unmap(Frame.reducedRead.Get(),0);if(positionMapped)ctx->Unmap(Frame.read.Get(),0);
            if(valid&&!domain.acceptedDraws)valid=false;
            const auto publication=std::chrono::steady_clock::now();
            particle_source_policy::Prepared prepared;
            if(valid)valid=Sources.Prepare(std::move(candidate),Retained.Keys(Epoch,Seconds(publication)),Epoch,
                Seconds(Frame.time),Seconds(publication),prepared);
            auto nextRetained=Retained;
            if(valid)valid=nextRetained.Observe(prepared.cells,Epoch,Seconds(Frame.time),Seconds(publication),Frame.camera,prepared.superseded);
            if(valid)valid=field::Publish(Cells,std::move(prepared.cells),Frame.ledger,Epoch,
                std::chrono::duration<double>(std::chrono::steady_clock::now()-Frame.time).count());
            if(valid){Sources.Commit(std::move(prepared));Retained=std::move(nextRetained);
                HasField=true;FieldTime=publication;FieldScene=Frame.scene;++CompleteFrames;}
            else {++IncompleteFrames;++RejectedBounds;}
            domain.published=valid;LastDomain=domain;
            Frame.busy=false;Frame.scene.Reset();Frame.target.Reset();
        }
        else if(FAILED(result)){++RejectedIO;Frame.ledger.Reject();} // never reuse storage on an unknown completion
    }
    Pending=Frame.busy?1u:0u;Seen=unsigned(Frame.ledger.draws);Measured=unsigned(Cells.size());
    if(HasField&&FieldScene.Get()!=lightbuffer::LightBuffer.Get())InvalidateScene();
}
static float Visibility()
{const double now=SecondsNow();return float(std::max(Retained.Strongest(Epoch,now),HasField&&!Cells.empty()?double(field::Visibility(now-Seconds(FieldTime))):0));}
static unsigned ReadyCount(){return glow::ParticlesActive()&&field::CleanStrength(Strength.load())>0
    ?std::min(unsigned(VisibleCells().size()),emission_policy::ClampParticleLimit(glow::ParticleLightLimit.load())):0;}
static void UpdateLight(shadowmaps::Light &light,const field::Cell &cell,float gain,float radius)
{
    const auto position=field::LightPosition(cell);
    const float sourceCore=field::CleanSourceCoreRadius(cell.emittedSourceCoreRadius);
    const bool moved=!light.BuiltFrame||shadowmaps::Distance(light.BuiltPosition,position)>shadowmaps::MOVED_LIGHT_DISTANCE
        ||std::abs(light.GlowRange-radius)>1e-4f;
    const bool coreChanged=std::abs(light.BuiltParticleSourceRadius-sourceCore)>.015f;
    light.Glow=true;light.Particle=true;light.GlowShown=true;light.GlowHeld=false;light.EmitterKey=0;light.Position=position;
    light.GlowNormal={};light.GlowArea=1;light.GlowRange=radius;
    light.ParticleSourceRadius=sourceCore;
    const double strength=field::StrengthReference*gain/XM_PI;
    light.GlowColour={float(cell.power.x*strength),float(cell.power.y*strength),float(cell.power.z*strength)};light.LastSeen=shadowmaps::FrameNumber;
    if(moved)
    {
        // Use the same complete confirmed-radius query as native/object lights.
        // An unordered prefix can omit a nearby wall even when the light is selected.
        shadowmaps::GatherCasters(light);
    }
    if(moved||coreChanged){++light.Version;light.RedrawNow=true;}
}
static void EndFrame(ID3D11DeviceContext *,unsigned allowance=field::Lit,bool allocationReady=true,double now=SecondsNow())
{
    shadowmaps::particle_moving::Request(glow::ParticlesActive()&&shadowmaps::MovingShadows,Epoch);
    if(!glow::ParticlesActive()){if(HasField||SelectedCount)Hide();return;}
    auto visible=VisibleCells(now);Sources.Filter(visible,Epoch,now);const float gain=field::CleanStrength(Strength.load());
    const float radius=field::LightRadius(Radius.load(),field::MaximumWorldRadius);
    if(!allocationReady)
    {
        // A skipped camera/allocation service is not a proven zero budget. Keep
        // only committed identities, refreshing their actual retained energy;
        // no new light or expired observation can be admitted by this path.
        const auto old=SelectedCells;SelectedCells={};SelectedCount=Active=0;
        const unsigned limit=gain>0?emission_policy::ClampParticleLimit(glow::ParticleLightLimit.load()):0;
        for(const auto &selected:old)if(selected.light)
        {
            const auto cell=std::lower_bound(visible.begin(),visible.end(),selected.cell,
                [](const field::Cell &value,const field::Key &key){return value.key<key;});
            auto light=shadowmaps::Lights.find(selected.light);
            if(SelectedCount<limit&&cell!=visible.end()&&cell->key==selected.cell&&light!=shadowmaps::Lights.end())
            {UpdateLight(light->second,*cell,gain,radius);SelectedCells[SelectedCount++]=selected;++Active;}
            else shadowmaps::Lights.erase(selected.light);
        }
        return;
    }
    std::array<field::Key,field::Lit> prior{};
    for(unsigned i=0;i<SelectedCount;++i)prior[i]=SelectedCells[i].cell;
    const unsigned wanted=gain>0?std::min({allowance,field::Lit,
        emission_policy::ClampParticleLimit(glow::ParticleLightLimit.load())}):0;
    const auto picks=field::Pick(visible,prior,SelectedCount,shadowmaps::Camera,wanted);auto old=SelectedCells;SelectedCells={};const unsigned oldCount=SelectedCount;SelectedCount=Active=0;
    for(unsigned i=0;i<wanted&&picks[i]!=SIZE_MAX;++i)
    {
        const auto &cell=visible[picks[i]];Selected selected{cell.key,0};
        for(unsigned j=0;j<oldCount;++j)if(old[j].cell==cell.key){selected.light=old[j].light;old[j].light=0;break;}
        if(!selected.light||!shadowmaps::Lights.contains(selected.light))selected.light=shadowmaps::NextLightKey++;
        UpdateLight(shadowmaps::Lights[selected.light],cell,gain,radius);
        SelectedCells[SelectedCount++]=selected;++Active;
    }
    for(auto &s:old)if(s.light)shadowmaps::Lights.erase(s.light);
}
// Explicit capture diagnostic only. No per-draw I/O or new runtime polling.
static void WriteState(FILE *file)
{
    if(!file)return;
    fprintf(file,"particle observed cache: entries %zu, grace 2.0s, fade 1.0s, hard expiry 3.0s\n",Retained.Size());
    fprintf(file,"particle stable sources: tracked %zu, owner bins %zu, acquire 4.0, retain 6.0, ownership hysteresis 0.75, pose tau 0.20s; inferred identities, native power unfiltered\n",Sources.Size(),Sources.OwnerCount());
    const auto core=Sources.SummarizeCore();
    fprintf(file,"particle source cores: tracked %zu, positive centroid samples %zu, max RMS %.6f, max target %.6f, max emitted %.6f, cap 0.35, scale 0.50, tau 0.20s; between-lit-centroid spread only\n",
        core.groups,core.positiveSamples,core.maximumRms,double(core.maximumTarget),double(core.maximumEmitted));
    fprintf(file,"particle controls: strength %.3f, radius scale %.3f, world radius %.3f\n",field::CleanStrength(Strength.load()),field::CleanRadius(Radius.load()),field::LightRadius(Radius.load(),field::MaximumWorldRadius));
    fprintf(file,"particle field epoch %llu, field %d, cells %zu, selected %u, visibility %.3f; packet frame %llu epoch %llu draws %u triangles %u complete %d collecting %d busy %d\n",
        static_cast<unsigned long long>(Epoch),HasField?1:0,Cells.size(),SelectedCount,Visibility(),
        static_cast<unsigned long long>(Frame.ledger.frame),static_cast<unsigned long long>(Frame.ledger.epoch),
        Frame.ledger.draws,Frame.ledger.triangles,Frame.ledger.complete?1:0,Frame.collecting?1:0,Frame.busy?1:0);
    fprintf(file,"particle source domain: current attempted %u admitted %u capacity excluded %u state excluded %u; last attempted %u admitted %u capacity excluded %u state excluded %u accepted draws %u invalid draws %u\n",
        Frame.ledger.attempted,Frame.ledger.draws,Frame.ledger.capacityExcluded,Frame.ledger.stateExcluded,
        LastDomain.attempted,LastDomain.draws,LastDomain.capacityExcluded,LastDomain.stateExcluded,LastDomain.acceptedDraws,LastDomain.invalidDraws);
    fprintf(file,"particle last read frame %llu epoch %llu draws %u triangles %u inspected %d included %u excluded far %u large %u invalid %u first excluded %u first invalid %u published %d; totals complete %u incomplete %u overflow %u skipped %u rejected state %u bounds %u IO %u\n",
        static_cast<unsigned long long>(LastDomain.frame),static_cast<unsigned long long>(LastDomain.epoch),
        LastDomain.draws,LastDomain.triangles,LastDomain.inspected?1:0,LastDomain.included,LastDomain.excludedDistance,LastDomain.excludedExtent,LastDomain.invalid,
        LastDomain.firstExcluded,LastDomain.firstInvalid,LastDomain.published?1:0,CompleteFrames,IncompleteFrames,OverflowFrames,SkippedFrames,
        RejectedState,RejectedBounds,RejectedIO);
}
} // namespace particle_emission
