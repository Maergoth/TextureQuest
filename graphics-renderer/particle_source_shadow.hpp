#pragma once
// Included inside shadowmaps. Discard only fragments inside a measured, bounded
// particle source core; every caster and all geometry outside that core remain.
namespace particle_source_shadow
{
constexpr unsigned Cubes=32;
constexpr float MaximumRadius=.35f;
using ParametersArray=std::array<XMFLOAT4,Cubes>;
constexpr const char *Shader=R"(
cbuffer SourceCore : register(b0) { float4 Core[32]; };
void ExcludeCore(float4 p,float4 source)
{
    if(source.x<=0 || !all(isfinite(p)) || p.w<=0) return;
    // On the actual D3D11 production cube path, pixel SV_Position.w is the
    // unchanged projection's clip W=view Z, proved by the biased-plane fixture.
    // Unlike raster depth p.z, it is unaffected by slope-scaled depth bias.
    float2 across=p.xy*(2*source.y)-1;
    float radiusSquared=(1+dot(across,across))*p.w*p.w;
    if(isfinite(radiusSquared)) clip(radiusSquared-source.x*source.x);
}
void single(float4 p:SV_Position) { ExcludeCore(p,Core[0]); }
void layered(float4 p:SV_Position,uint slice:SV_RenderTargetArrayIndex)
{ if(slice/6<32) ExcludeCore(p,Core[slice/6]); }
)";
static ComPtr<ID3D11PixelShader> Single,Layered;
static ComPtr<ID3D11Buffer> Constants;
static bool Failed=false;
static void Reset(){Single.Reset();Layered.Reset();Constants.Reset();Failed=false;}
static float Clean(float radius)
{return std::isfinite(radius)?std::clamp(radius,0.f,MaximumRadius):0.f;}
static XMFLOAT4 Parameters(const Light&light,UINT faceSize)
{
    const float core=light.Particle?Clean(light.BuiltParticleSourceRadius):0.f;
    if(!faceSize||!std::isfinite(light.Radius)||light.Radius<=NEAR_PLANE)return {};
    return {core,1.f/float(faceSize),NEAR_PLANE,light.Radius};
}
static bool Ensure(ID3D11Device*dev)
{
    if(Single&&Layered&&Constants)return true;if(Failed)return false;
    ComPtr<ID3D11PixelShader> single,layered;ComPtr<ID3D11Buffer> constants;
    const auto compile=[&](const char*entry,ComPtr<ID3D11PixelShader>&output){
        ComPtr<ID3DBlob> code,errors;
        return SUCCEEDED(D3DCompile(Shader,strlen(Shader),"particle-source-shadow",nullptr,nullptr,
            entry,"ps_5_0",D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&code,&errors))
            &&SUCCEEDED(dev->CreatePixelShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,&output));};
    D3D11_BUFFER_DESC desc{sizeof(ParametersArray),D3D11_USAGE_DEFAULT,D3D11_BIND_CONSTANT_BUFFER};
    if(!compile("single",single)||!compile("layered",layered)||FAILED(dev->CreateBuffer(&desc,nullptr,&constants)))
    {Failed=true;return false;}
    Single=std::move(single);Layered=std::move(layered);Constants=std::move(constants);return true;
}
static void Clear(ID3D11DeviceContext*ctx)
{ctx->PSSetShader(nullptr,nullptr,0);ID3D11Buffer*none=nullptr;ctx->PSSetConstantBuffers(0,1,&none);}
static bool Bind(ID3D11DeviceContext*ctx,const ParametersArray&parameters,bool layered)
{
    const bool any=std::any_of(parameters.begin(),parameters.end(),[](const XMFLOAT4&p){return p.x>0;});
    if(!any){Clear(ctx);return true;}
    ComPtr<ID3D11Device>dev;ctx->GetDevice(&dev);
    // Resource failure retains the original opaque shadow behavior.
    if(!Ensure(dev.Get())){Clear(ctx);return false;}
    ctx->UpdateSubresource(Constants.Get(),0,nullptr,parameters.data(),0,0);
    ID3D11Buffer*buffer=Constants.Get();ctx->PSSetConstantBuffers(0,1,&buffer);
    ctx->PSSetShader(layered?Layered.Get():Single.Get(),nullptr,0);return true;
}
static bool BindSingle(ID3D11DeviceContext*ctx,const Light&light,UINT faceSize)
{ParametersArray parameters{};parameters[0]=Parameters(light,faceSize);return Bind(ctx,parameters,false);}
static bool BindLayered(ID3D11DeviceContext*ctx,const ParametersArray&parameters)
{return Bind(ctx,parameters,true);}
}
