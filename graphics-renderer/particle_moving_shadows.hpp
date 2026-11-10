#pragma once
// Included within shadowmaps, after DrawInstancedMoving. Separate from native
// sixteen 512px maps: every selected particle light can shadow retained characters.
namespace particle_moving
{
constexpr UINT Cubes=32,Banks=4,FaceSize=256,Slices=Cubes*6,Capacity=Cubes*Banks;
static_assert(Capacity==emission_policy::ParticleLimit&&Slices<=D3D11_REQ_TEXTURE2D_ARRAY_AXIS_DIMENSION);
struct Bank
{
    ComPtr<ID3D11Texture2D> texture;
    std::array<ComPtr<ID3D11DepthStencilView>,Slices> faces;
    ComPtr<ID3D11ShaderResourceView> view,rowsView,slicesView;
    ComPtr<ID3D11DepthStencilView> arrayView;
    ComPtr<ID3D11Buffer> rows,slices;
    bool failed=false;
};
struct Mapping
{
    uint64_t key=0,epoch=0,frame=UINT64_MAX;
    XMFLOAT3 position{};float radius=0,sourceRadius=0;bool valid=false;
};
static std::array<Bank,Banks> Pool;
static std::array<Mapping,Capacity> Maps;
static bool Requested=false;
static uint64_t SourceEpoch=0;
static unsigned Eligible=0,Ready=0,Failures=0,MeshesDrawn=0,FacesDrawn=0;
static void Reset()
{
    Pool={};Maps={};Requested=false;SourceEpoch=0;
    Eligible=Ready=Failures=MeshesDrawn=FacesDrawn=0;
}
static void Request(bool active,uint64_t epoch)
{
    if(!active){if(Requested||Ready)Reset();return;}
    if(SourceEpoch!=epoch){Reset();SourceEpoch=epoch;}
    Requested=true;
}
static bool EnsureBank(ID3D11Device *dev,unsigned index)
{
    auto &bank=Pool[index];if(bank.view)return true;if(bank.failed)return false;
    Bank candidate;
    if(!CreateCubeArray(dev,FaceSize,Cubes,candidate.texture,candidate.faces.data(),candidate.view))
    {bank.failed=true;++Failures;return false;}
    if(InstancedCasterShader)
    {
        D3D11_DEPTH_STENCIL_VIEW_DESC dd{};dd.Format=DXGI_FORMAT_D32_FLOAT;dd.ViewDimension=D3D11_DSV_DIMENSION_TEXTURE2DARRAY;
        dd.Texture2DArray={0,0,Slices};
        D3D11_BUFFER_DESC rb{Slices*64,D3D11_USAGE_DEFAULT,D3D11_BIND_SHADER_RESOURCE,0,D3D11_RESOURCE_MISC_BUFFER_STRUCTURED,16};
        D3D11_BUFFER_DESC sb{MAX_MOVING*Slices*4,D3D11_USAGE_DEFAULT,D3D11_BIND_SHADER_RESOURCE};
        D3D11_SHADER_RESOURCE_VIEW_DESC rv{};rv.Format=DXGI_FORMAT_UNKNOWN;rv.ViewDimension=D3D11_SRV_DIMENSION_BUFFER;rv.Buffer={0,Slices*4};
        D3D11_SHADER_RESOURCE_VIEW_DESC sv{};sv.Format=DXGI_FORMAT_R32_UINT;sv.ViewDimension=D3D11_SRV_DIMENSION_BUFFER;sv.Buffer={0,MAX_MOVING*Slices};
        if(FAILED(dev->CreateDepthStencilView(candidate.texture.Get(),&dd,&candidate.arrayView))
            ||FAILED(dev->CreateBuffer(&rb,nullptr,&candidate.rows))||FAILED(dev->CreateBuffer(&sb,nullptr,&candidate.slices))
            ||FAILED(dev->CreateShaderResourceView(candidate.rows.Get(),&rv,&candidate.rowsView))
            ||FAILED(dev->CreateShaderResourceView(candidate.slices.Get(),&sv,&candidate.slicesView)))
        {
            // Valid per-face maps remain available on instancing resource failure.
            candidate.arrayView.Reset();candidate.rows.Reset();candidate.slices.Reset();candidate.rowsView.Reset();candidate.slicesView.Reset();
        }
    }
    bank=std::move(candidate);return true;
}
static int Binding(int slot,const Light &light)
{
    if(!Requested||!MovingShadows||!light.Particle||slot<0||slot>=MAX_LIGHTS||!SlotActive[slot])return -1;
    for(unsigned p=0;p<Capacity;++p)
    {
        const auto &m=Maps[p];
        if(m.valid&&m.key==SlotLights[slot]&&m.epoch==SourceEpoch&&m.frame+1==FrameNumber
            &&m.position.x==light.BuiltPosition.x&&m.position.y==light.BuiltPosition.y&&m.position.z==light.BuiltPosition.z
            &&m.radius==light.Radius&&m.sourceRadius==light.BuiltParticleSourceRadius&&Pool[p/Cubes].view)return static_cast<int>(p);
    }
    return -1;
}
// Describes the actual completed map before its next-frame overwrite. A missing
// map is not invented: the caller then follows the same native-map fallback as
// glow::DrawLights. Only this optional snapshot path accepts the current frame.
static bool Snapshot(int slot,const Light &light,uint64_t frame,ID3D11Texture2D *&texture,UINT &cube)
{
    texture=nullptr;cube=0;
    if(!Requested||!MovingShadows||!light.Particle||slot<0||slot>=MAX_LIGHTS||!SlotActive[slot])return false;
    for(unsigned p=0;p<Capacity;++p)
    {
        const auto &m=Maps[p];
        if(m.valid&&m.key==SlotLights[slot]&&m.epoch==SourceEpoch&&m.frame==frame
            &&m.position.x==light.BuiltPosition.x&&m.position.y==light.BuiltPosition.y&&m.position.z==light.BuiltPosition.z
            &&m.radius==light.Radius&&m.sourceRadius==light.BuiltParticleSourceRadius&&Pool[p/Cubes].view&&Pool[p/Cubes].texture)
        {texture=Pool[p/Cubes].texture.Get();cube=p%Cubes;return true;}
    }
    return false;
}
static void Draw(ID3D11DeviceContext1 *ctx,const std::vector<std::optional<const WorldMesh *>> &worldMeshes)
{
    Eligible=Ready=MeshesDrawn=FacesDrawn=0;
    if(!Requested||!MovingShadows||!CameraFound||MovingCasters.empty())
    {for(auto &bank:Pool)bank={};Maps={};return;}
    struct Work {uint64_t key;Light *light;int physical=-1;double priority=0;std::vector<std::tuple<const MovingCaster *,XMFLOAT3,float>> nearby;};
    std::vector<Work> work;
    for(int slot=0;slot<MAX_LIGHTS;++slot)
    {
        auto it=Lights.find(SlotLights[slot]);
        if(!SlotActive[slot]||it==Lights.end())continue;auto &light=it->second;
        if(!light.Particle||!light.GlowShown||!light.BuiltFrame||light.LastSeen!=FrameNumber)continue;
        Work item{it->first,&light,-1,0,{}};
        for(const auto &caster:MovingCasters)
        {
            if(caster.Index>=worldMeshes.size()||caster.Index>=MAX_MOVING||size_t(caster.Index)*12+12>MovingWorlds.size()||SeeThroughMeshes.contains(caster.Mesh.Key))continue;
            if(const auto &entry=worldMeshes[caster.Index])
            {
                const auto *mesh=*entry;
                if(!mesh||!mesh->BoundsKnown)item.nearby.emplace_back(&caster,XMFLOAT3{},FLT_MAX);
                else if(FindBoxDistance(mesh->Low,mesh->High,light.BuiltPosition)<light.Radius)
                    item.nearby.emplace_back(&caster,XMFLOAT3{(mesh->Low.x+mesh->High.x)/2-light.BuiltPosition.x,
                        (mesh->Low.y+mesh->High.y)/2-light.BuiltPosition.y,(mesh->Low.z+mesh->High.z)/2-light.BuiltPosition.z},Distance(mesh->Low,mesh->High)/2);
            }
            else
            {
                const float *rows=&MovingWorlds[caster.Index*12];
                const XMFLOAT3 relative{rows[3]-CurrentOffset.x-light.BuiltPosition.x,rows[7]-CurrentOffset.y-light.BuiltPosition.y,rows[11]-CurrentOffset.z-light.BuiltPosition.z};
                if(Distance(relative,{})<light.Radius+MESH_RADIUS)item.nearby.emplace_back(&caster,relative,MESH_RADIUS);
            }
        }
        if(!item.nearby.empty())
        {
            double nearest=DBL_MAX;for(const auto &[caster,relative,extent]:item.nearby)
            { (void)caster;nearest=std::min(nearest,double(std::max(0.0f,Distance(relative,{})-extent))); }
            item.priority=std::max({double(light.GlowColour.x),double(light.GlowColour.y),double(light.GlowColour.z)})/std::max(4.0,nearest*nearest);
            for(const auto&m:Maps)if(m.valid&&m.key==item.key&&m.epoch==SourceEpoch){item.priority*=1.15;break;}
            work.push_back(std::move(item));
        }
    }
    std::sort(work.begin(),work.end(),[](const Work&a,const Work&b){return a.priority!=b.priority?a.priority>b.priority:a.key<b.key;});
    Eligible=static_cast<unsigned>(work.size());const auto limit=static_cast<unsigned>(CleanParticleShadowLimit(ParticleShadowLimit.load()));
    if(work.size()>limit)work.resize(limit);
    const auto old=Maps;Maps={};std::array<bool,Capacity> used{};
    for(auto &w:work)for(unsigned p=0;p<Capacity;++p)if(old[p].key==w.key&&old[p].epoch==SourceEpoch)
    {w.physical=static_cast<int>(p);used[p]=true;break;}
    for(auto &w:work)if(w.physical<0)for(unsigned p=0;p<Capacity;++p)if(!used[p]){used[p]=true;w.physical=static_cast<int>(p);break;}
    ComPtr<ID3D11Device>dev;ctx->GetDevice(&dev);
    for(unsigned bi=0;bi<Banks;++bi)
    {
        const bool wanted=std::any_of(work.begin(),work.end(),[bi](const Work&w){return unsigned(w.physical)/Cubes==bi;});
        if(!wanted){Pool[bi]={};continue;}if(!EnsureBank(dev.Get(),bi))continue;auto &bank=Pool[bi];
        const bool instanced=InstancedCasterShader&&bank.arrayView;
        particle_source_shadow::ParametersArray sourceParameters{};
        std::vector<std::bitset<Slices>> faces(instanced?MovingCasters.size():0);std::vector<XMFLOAT4X4> rows(instanced?Slices:0);
        ComPtr<ID3DDeviceContextState> previous;ctx->SwapDeviceContextState(DrawState.Get(),&previous);
        ID3D11ShaderResourceView *empty[4]{};ctx->PSSetShaderResources(9,4,empty);
        const D3D11_VIEWPORT vp{0,0,FaceSize,FaceSize,0,1};ctx->RSSetViewports(1,&vp);ctx->RSSetState(CasterRasterizer.Get());
        ctx->OMSetDepthStencilState(CasterDepth.Get(),0);ctx->PSSetShader(nullptr,nullptr,0);
        ID3D11Buffer *offset=CameraOffsetBuffer.Get();ctx->VSSetConstantBuffers(3,1,&offset);
        ID3D11ShaderResourceView *worlds=MovingWorldView.Get();ctx->VSSetShaderResources(13,1,&worlds);
        if(!instanced){ctx->VSSetShader(MovingCasterShader.Get(),nullptr,0);ID3D11Buffer*c=MovingConstantsBuffer.Get();ctx->VSSetConstantBuffers(0,1,&c);}
        for(auto &w:work)if(unsigned(w.physical)/Cubes==bi)
        {
            const unsigned cube=unsigned(w.physical)%Cubes;const auto &light=*w.light;const XMVECTOR eye=XMLoadFloat3(&light.BuiltPosition);
            sourceParameters[cube]=particle_source_shadow::Parameters(light,FaceSize);
            if(!instanced)particle_source_shadow::BindSingle(ctx,light,FaceSize);
            const XMMATRIX projection=XMMatrixPerspectiveFovLH(XM_PIDIV2,1,NEAR_PLANE,light.Radius);
            for(unsigned face=0;face<6;++face)
            {
                const unsigned slice=cube*6+face;auto *target=bank.faces[slice].Get();ctx->ClearDepthStencilView(target,D3D11_CLEAR_DEPTH,1,0);
                const XMMATRIX view=XMMatrixLookToLH(eye,FACE_DIRECTIONS[face],FACE_UPS[face]);XMFLOAT4X4 matrix;XMStoreFloat4x4(&matrix,XMMatrixTranspose(XMMatrixMultiply(view,projection)));
                if(instanced)rows[slice]=matrix;else ctx->OMSetRenderTargets(0,nullptr,target);
                for(const auto &[caster,relative,extent]:w.nearby)if(ReachesFace(face,relative,extent))
                {
                    if(instanced)faces[caster->Index].set(slice);
                    else {MovingConstants c{};c.ViewProjection=matrix;c.Index[0]=caster->Index;ctx->UpdateSubresource(MovingConstantsBuffer.Get(),0,nullptr,&c,0,0);DrawCaster(ctx,caster->Mesh);++MeshesDrawn;++FacesDrawn;}
                }
            }
            Maps[unsigned(w.physical)]={w.key,SourceEpoch,FrameNumber,light.BuiltPosition,light.Radius,light.BuiltParticleSourceRadius,true};++Ready;
        }
        if(instanced)
        {
            std::vector<UINT>slices;for(const auto &caster:MovingCasters){if(caster.Index>=faces.size())continue;for(UINT slice=0;slice<Slices;++slice)if(faces[caster.Index][slice])slices.push_back(slice);}
            if(!slices.empty())
            {
                particle_source_shadow::BindLayered(ctx,sourceParameters);
                const D3D11_BOX box{0,0,0,UINT(slices.size()*sizeof(UINT)),1,1};ctx->UpdateSubresource(bank.slices.Get(),0,&box,slices.data(),0,0);ctx->UpdateSubresource(bank.rows.Get(),0,nullptr,rows.data(),0,0);
                ctx->VSSetShader(InstancedCasterShader.Get(),nullptr,0);ID3D11Buffer*c=InstancedConstantsBuffer.Get();ctx->VSSetConstantBuffers(0,1,&c);
                ID3D11ShaderResourceView*views[]{MovingWorldView.Get(),bank.rowsView.Get(),bank.slicesView.Get()};ctx->VSSetShaderResources(13,3,views);ctx->OMSetRenderTargets(0,nullptr,bank.arrayView.Get());
                UINT start=0;for(const auto &caster:MovingCasters){if(caster.Index>=faces.size())continue;const UINT count=UINT(faces[caster.Index].count());if(!count)continue;const UINT index[]{caster.Index,start,0,0};ctx->UpdateSubresource(InstancedConstantsBuffer.Get(),0,nullptr,index,0,0);DrawCaster(ctx,caster.Mesh,count);start+=count;++MeshesDrawn;FacesDrawn+=count;}
            }
        }
        ctx->OMSetRenderTargets(0,nullptr,nullptr);ID3D11ShaderResourceView *none[3]{};ctx->VSSetShaderResources(13,3,none);
        particle_source_shadow::Clear(ctx);ctx->SwapDeviceContextState(previous.Get(),nullptr);
    }
}
static void WriteState(FILE *file)
{
    if(!file)return;unsigned allocated=0;for(const auto&bank:Pool)allocated+=bank.view?1:0;
    fprintf(file,"particle moving maps: requested %d epoch %llu eligible %u ready %u banks %u failures %u mesh draws %u face instances %u, face size %u native moving face size %u limit %d\n",Requested?1:0,static_cast<unsigned long long>(SourceEpoch),Eligible,Ready,allocated,Failures,MeshesDrawn,FacesDrawn,FaceSize,MOVING_FACE_SIZE,CleanParticleShadowLimit(ParticleShadowLimit.load()));
}
} // namespace particle_moving
