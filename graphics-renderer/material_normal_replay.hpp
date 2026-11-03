#pragma once
// Included inside shadows after the existing kept-draw and shader helpers.
// This cache contains current-frame native references, never texture byte copies.
constexpr size_t MATERIAL_DRAW_LIMIT=512;
struct MaterialDraw { KeptDraw draw;std::vector<std::pair<uint64_t,uint64_t>> buffers;D3D11_RECT scissor{}; };
struct MaterialStats
{
    uint64_t frame=0;unsigned captured=0,drawn=0,unsupported=0,transform=0,camera=0,depth=0,changed=0,capacity=0;
};
static MaterialStats MaterialCounts;
static std::unordered_map<uint64_t,MaterialDraw> MaterialDraws;
static std::unordered_map<uint64_t,uint64_t> MaterialBufferVersions;
static std::mutex MaterialVersionLock;
static ComPtr<ID3D11Texture2D> MaterialTexture;
static ComPtr<ID3D11RenderTargetView> MaterialTarget;
static ComPtr<ID3D11ShaderResourceView> MaterialView;
static ComPtr<ID3D11PixelShader> MaterialPixelShader;
static ComPtr<ID3D11Buffer> MaterialConstants[4],MaterialParameters;
static ComPtr<ID3DDeviceContextState> MaterialState;
static std::unordered_map<ID3D11DepthStencilState*,
    std::pair<ComPtr<ID3D11DepthStencilState>,ComPtr<ID3D11DepthStencilState>>> MaterialDepthStates;
static uint64_t MaterialFrame=UINT64_MAX;
static ID3D11Resource *MaterialSource=nullptr;
static DirectX::XMFLOAT4X4 MaterialCamera{};
static DirectX::XMFLOAT3 MaterialOffset{};
static bool MaterialActive()
{return tq_world::Work.Point()&&Enabled&&glow::Enabled&&!shadowmaps::ShowColours;}
static void BeginMaterialFrame()
{
    MaterialDraws.clear();{std::lock_guard guard(MaterialVersionLock);MaterialBufferVersions.clear();}MaterialFrame=UINT64_MAX;MaterialSource=nullptr;
    MaterialCounts={};MaterialCounts.frame=shadowmaps::FrameNumber;
}
static void ResetMaterialNormals()
{
    BeginMaterialFrame();MaterialTexture.Reset();MaterialTarget.Reset();MaterialView.Reset();MaterialPixelShader.Reset();
    for(auto &buffer:MaterialConstants)buffer.Reset();MaterialParameters.Reset();MaterialState.Reset();MaterialDepthStates.clear();
}
static bool MaterialUniform(const std::vector<DirectX::XMFLOAT4>&rows)
{
    if(rows.size()<15)return false;
    double length[3]{};
    for(unsigned j=0;j<3;++j)
    {
        const auto&r=rows[7+j];if(!std::isfinite(r.x)||!std::isfinite(r.y)||!std::isfinite(r.z))return false;
        length[j]=double(r.x)*r.x+double(r.y)*r.y+double(r.z)*r.z;
        if(!std::isfinite(length[j])||length[j]<1e-12)return false;
    }
    const double scale=std::max({length[0],length[1],length[2]});
    for(unsigned j=0;j<3;++j)
    {
        if(std::abs(length[j]-length[0])>scale*1e-4)return false;
        for(unsigned k=0;k<j;++k)
        {const auto&a=rows[7+j];const auto&b=rows[7+k];if(std::abs(double(a.x)*b.x+double(a.y)*b.y+double(a.z)*b.z)>scale*1e-4)return false;}
    }
    return true;
}
static void KeepMaterialDraw(ID3D11DeviceContext *ctx,const shadowmaps::DrawCall&call,bool base,size_t shader)
{
    if(!MaterialActive()||!lightbuffer::LightBuffer||lightbuffer::CurrentTarget!=lightbuffer::LightBuffer.Get()||call.Instances!=1||call.FirstInstance)return;
    MaterialCounts.frame=shadowmaps::FrameNumber;
    {std::lock_guard guard(Lock);if(!RepeatedShaders.contains(capture::VertexShader)){++MaterialCounts.unsupported;return;}}
    ComPtr<ID3D11DeviceContext1> ctx1;if(FAILED(ctx->QueryInterface(IID_PPV_ARGS(&ctx1))))return;
    ComPtr<ID3D11GeometryShader> geometryStage;ComPtr<ID3D11HullShader> hull;ComPtr<ID3D11DomainShader> domain;
    ctx1->GSGetShader(&geometryStage,nullptr,nullptr);ctx1->HSGetShader(&hull,nullptr,nullptr);ctx1->DSGetShader(&domain,nullptr,nullptr);
    ComPtr<ID3D11VertexShader> vertexStage;ComPtr<ID3D11PixelShader> pixelStage;
    UINT vertexClasses=0,pixelClasses=0;ctx1->VSGetShader(&vertexStage,nullptr,&vertexClasses);ctx1->PSGetShader(&pixelStage,nullptr,&pixelClasses);
    if(geometryStage||hull||domain||vertexClasses||pixelClasses){++MaterialCounts.unsupported;return;}
    MaterialDraw candidate;if(!KeepDraw(ctx1.Get(),call,candidate.draw))return;
    auto&draw=candidate.draw;draw.Base=base;draw.LampShader=shader;
    D3D11_RASTERIZER_DESC raster{};if(draw.Rasterizer)draw.Rasterizer->GetDesc(&raster);
    if(raster.ScissorEnable)
    {
        UINT count=1;ctx1->RSGetScissorRects(&count,&candidate.scissor);
        if(count!=1){++MaterialCounts.unsupported;return;}
    }
    else candidate.scissor={0,0,LONG(draw.Viewport.Width),LONG(draw.Viewport.Height)};
    if(draw.Repeated<0||draw.Repeated>=int(std::size(REPEATED_VERTEX_SHADERS))||!draw.VertexShader
        ||draw.Constants[0].size()<(REPEATED_VERTEX_SHADERS[draw.Repeated].MovedTexture?24u:15u)
        ||draw.Constants[1].size()<11||draw.Constants[3].size()<11||(!base&&draw.Constants[2].empty())){++MaterialCounts.unsupported;return;}
    if(!MaterialUniform(draw.Constants[0])){++MaterialCounts.transform;return;}
    for(const auto &binding:draw.Constants)for(const auto&r:binding)
        if(!std::isfinite(r.x)||!std::isfinite(r.y)||!std::isfinite(r.z)||!std::isfinite(r.w))return;
    // A native alpha test can depend on light-facing terms, not just material
    // coverage. Do not infer its holes from coincident depth on another mesh.
    if(draw.Constants[3][0].x>.5f){++MaterialCounts.unsupported;return;}
    ComPtr<ID3D11Resource> materialResource;
    if(!base)
    {
        if(shader>=SINGLE_PASS_SHADERS||!draw.Views[0]||!draw.Samplers[0]||draw.Constants[3][1].x>=.5f)
        {++MaterialCounts.unsupported;return;}
        D3D11_SHADER_RESOURCE_VIEW_DESC d{};draw.Views[0]->GetDesc(&d);
        if(d.ViewDimension!=D3D11_SRV_DIMENSION_TEXTURE2D){++MaterialCounts.unsupported;return;}
        draw.Views[0]->GetResource(&materialResource);ComPtr<ID3D11Texture2D> texture;
        if(FAILED(materialResource.As(&texture))){++MaterialCounts.unsupported;return;}
        D3D11_TEXTURE2D_DESC td{};texture->GetDesc(&td);
        if((td.Usage!=D3D11_USAGE_DEFAULT&&td.Usage!=D3D11_USAGE_IMMUTABLE)||td.CPUAccessFlags||td.SampleDesc.Count!=1
            ||td.BindFlags&(D3D11_BIND_RENDER_TARGET|D3D11_BIND_UNORDERED_ACCESS)
            ||td.MiscFlags&(D3D11_RESOURCE_MISC_SHARED|D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX|D3D11_RESOURCE_MISC_SHARED_NTHANDLE))
        {++MaterialCounts.unsupported;return;}
    }
    DirectX::XMFLOAT4 rows[shadowmaps::ROW_COUNT];
    std::copy_n(draw.Constants[0].begin()+shadowmaps::FIRST_ROW,shadowmaps::ROW_COUNT,rows);
    if(!IsLightBufferDraw(rows)){++MaterialCounts.camera;return;}
    const auto geometry=FindDrawnMesh(ctx,call);draw.Mesh=geometry.Key;
    const uint64_t key=FindCopyKey(geometry.Key,rows);
    if(auto old=MaterialDraws.find(key);old!=MaterialDraws.end()&&(!old->second.draw.Base||base))return;
    // A better material pass may replace its existing base record even at capacity.
    if(!MaterialDraws.contains(key)&&MaterialDraws.size()>=MATERIAL_DRAW_LIMIT){++MaterialCounts.capacity;return;}
    const auto remember=[&](ID3D11Buffer*buffer)
    {
        if(!buffer)return true;
        D3D11_BUFFER_DESC d{};buffer->GetDesc(&d);
        if(d.BindFlags&(D3D11_BIND_STREAM_OUTPUT|D3D11_BIND_UNORDERED_ACCESS))return false;
        const uint64_t handle=reinterpret_cast<uint64_t>(buffer);
        {std::lock_guard guard(MaterialVersionLock);candidate.buffers.push_back({handle,MaterialBufferVersions[handle]});}return true;
    };
    for(const auto&buffer:draw.VertexBuffers)if(!remember(buffer.Get()))return;
    if(!remember(draw.IndexBuffer.Get()))return;
    if(materialResource){const auto handle=reinterpret_cast<uint64_t>(materialResource.Get());std::lock_guard guard(MaterialVersionLock);candidate.buffers.push_back({handle,MaterialBufferVersions[handle]});}
    MaterialDraws[key]=std::move(candidate);++MaterialCounts.captured;
}
static bool CreateMaterialObjects(ID3D11Device*dev)
{
    if(MaterialState)return true;
    if(!CompiledShaders.valid()||CompiledShaders.wait_for(std::chrono::seconds(0))!=std::future_status::ready)return false;
    const auto&compiled=CompiledShaders.get();
    if(compiled.size()<=LAMP_SHADER_COUNT+8)return false;
    const auto vertex=compiled[LAMP_SHADER_COUNT+2];const auto pixel=compiled[LAMP_SHADER_COUNT+8];
    if(!vertex||!pixel)return false;
    if(!AddedVertexShader&&FAILED(dev->CreateVertexShader(vertex->GetBufferPointer(),vertex->GetBufferSize(),nullptr,&AddedVertexShader)))return false;
    if(FAILED(dev->CreatePixelShader(pixel->GetBufferPointer(),pixel->GetBufferSize(),nullptr,&MaterialPixelShader)))return false;
    D3D11_BUFFER_DESC b{MAX_KEPT_CONSTANTS*sizeof(DirectX::XMFLOAT4),D3D11_USAGE_DEFAULT,D3D11_BIND_CONSTANT_BUFFER};
    for(auto&buffer:MaterialConstants)if(FAILED(dev->CreateBuffer(&b,nullptr,&buffer)))return false;
    b.ByteWidth=sizeof(AddedLampConstants);if(FAILED(dev->CreateBuffer(&b,nullptr,&MaterialParameters)))return false;
    ComPtr<ID3D11Device1> device;const auto level=dev->GetFeatureLevel();
    return SUCCEEDED(dev->QueryInterface(IID_PPV_ARGS(&device)))&&SUCCEEDED(device->CreateDeviceContextState(0,&level,1,
        D3D11_SDK_VERSION,__uuidof(ID3D11Device),nullptr,&MaterialState));
}
static ID3D11DepthStencilState *MaterialDepth(ID3D11Device*dev,ID3D11DepthStencilState*native)
{
    if(!native)return nullptr;
    if(auto found=MaterialDepthStates.find(native);found!=MaterialDepthStates.end())return found->second.second.Get();
    if(MaterialDepthStates.size()>=32)return nullptr;
    D3D11_DEPTH_STENCIL_DESC desc{};native->GetDesc(&desc);
    if(!desc.DepthEnable)return nullptr;
    desc.DepthFunc=D3D11_COMPARISON_EQUAL;desc.DepthWriteMask=D3D11_DEPTH_WRITE_MASK_ZERO;desc.StencilWriteMask=0;
    desc.FrontFace.StencilFailOp=desc.FrontFace.StencilDepthFailOp=desc.FrontFace.StencilPassOp=D3D11_STENCIL_OP_KEEP;
    desc.BackFace.StencilFailOp=desc.BackFace.StencilDepthFailOp=desc.BackFace.StencilPassOp=D3D11_STENCIL_OP_KEEP;
    ComPtr<ID3D11DepthStencilState> copy;if(FAILED(dev->CreateDepthStencilState(&desc,&copy)))return nullptr;
    MaterialDepthStates[native]={native,copy};return copy.Get();
}
static void DrawMaterialNormals(ID3D11DeviceContext*ctx)
{
    MaterialFrame=UINT64_MAX;MaterialSource=nullptr;
    auto records=std::move(MaterialDraws);MaterialDraws.clear();
    if(!MaterialActive()||records.empty()||!lightbuffer::WorldToClipKnown||!lightbuffer::LightBufferDepth)return;
    const bool any=std::any_of(shadowmaps::Lights.begin(),shadowmaps::Lights.end(),[](const auto&entry){const auto&l=entry.second;
        return l.Glow&&l.GlowShown&&l.BuiltFrame&&l.Slot>=0;});
    if(!any)return;
    ComPtr<ID3D11Device> dev;ctx->GetDevice(&dev);ComPtr<ID3D11DeviceContext1> ctx1;
    if(FAILED(ctx->QueryInterface(IID_PPV_ARGS(&ctx1)))||!CreateMaterialObjects(dev.Get()))return;
    D3D11_TEXTURE2D_DESC screen{};lightbuffer::LightBuffer->GetDesc(&screen);
    if(screen.SampleDesc.Count!=1||!glow::CreateScreenTexture(dev.Get(),screen.Width,screen.Height,DXGI_FORMAT_R10G10B10A2_UNORM,
        MaterialTexture,MaterialTarget,MaterialView))return;
    ComPtr<ID3DDeviceContextState> previous;ctx1->SwapDeviceContextState(MaterialState.Get(),&previous);
    // A prior consumer's SRV cannot remain in this owned state while the atlas is written.
    ID3D11ShaderResourceView *none[40]{};ctx1->PSSetShaderResources(0,40,none);
    const FLOAT clear[4]{};ctx1->ClearRenderTargetView(MaterialTarget.Get(),clear);
    for(const auto&[key,record]:records)
    {
        (void)key;const auto&draw=record.draw;
        bool unchanged=true;{std::lock_guard guard(MaterialVersionLock);for(const auto&[buffer,version]:record.buffers)
        {auto found=MaterialBufferVersions.find(buffer);if(found==MaterialBufferVersions.end()||found->second!=version){unchanged=false;break;}}}
        if(!unchanged){++MaterialCounts.changed;continue;}
        ComPtr<ID3D11Resource> target,depth;if(draw.Target)draw.Target->GetResource(&target);if(draw.DepthView)draw.DepthView->GetResource(&depth);
        if(target.Get()!=lightbuffer::LightBuffer.Get()||depth.Get()!=lightbuffer::LightBufferDepth.Get()
            ||draw.Viewport.TopLeftX||draw.Viewport.TopLeftY||draw.Viewport.Width!=screen.Width||draw.Viewport.Height!=screen.Height)
        {++MaterialCounts.depth;continue;}
        DirectX::XMFLOAT4 rows[shadowmaps::ROW_COUNT];std::copy_n(draw.Constants[0].begin()+shadowmaps::FIRST_ROW,shadowmaps::ROW_COUNT,rows);
        DirectX::XMMATRIX camera;if(!shadowmaps::FindWorldToClip(rows,camera)||!lightbuffer::IsSameCamera(camera,lightbuffer::WorldToClip))
        {++MaterialCounts.camera;continue;}
        auto*depthState=MaterialDepth(dev.Get(),draw.Depth.Get());if(!depthState){++MaterialCounts.depth;continue;}
        for(unsigned n=0;n<4;++n)
        {
            std::array<DirectX::XMFLOAT4,MAX_KEPT_CONSTANTS> copy{};std::copy(draw.Constants[n].begin(),draw.Constants[n].end(),copy.begin());
            ctx1->UpdateSubresource(MaterialConstants[n].Get(),0,nullptr,copy.data(),0,0);
        }
        const auto&profile=REPEATED_VERTEX_SHADERS[draw.Repeated];AddedLampConstants parameters{};
        parameters.Header={0,0,profile.MovedTexture?1.f:0.f,draw.Base?0.f:1.f};parameters.Range={0,profile.NormalInNormal?1.f:0.f,0,0};
        ctx1->UpdateSubresource(MaterialParameters.Get(),0,nullptr,&parameters,0,0);
        ID3D11Buffer*vc[]{MaterialConstants[0].Get(),MaterialConstants[1].Get()};ID3D11Buffer*pc[]{MaterialConstants[2].Get(),MaterialConstants[3].Get()};
        ctx1->VSSetConstantBuffers(0,2,vc);ctx1->PSSetConstantBuffers(0,2,pc);ID3D11Buffer*params=MaterialParameters.Get();
        ctx1->VSSetConstantBuffers(ADDED_LAMP_SLOT,1,&params);ctx1->PSSetConstantBuffers(ADDED_LAMP_SLOT,1,&params);
        ID3D11Buffer*vertices[KEPT_VERTEX_SLOTS]{};for(unsigned n=0;n<KEPT_VERTEX_SLOTS;++n)vertices[n]=draw.VertexBuffers[n].Get();
        ctx1->IASetVertexBuffers(0,KEPT_VERTEX_SLOTS,vertices,draw.Strides,draw.Offsets);ctx1->IASetInputLayout(draw.Layout.Get());
        ctx1->IASetIndexBuffer(draw.IndexBuffer.Get(),draw.IndexFormat,draw.IndexOffset);ctx1->IASetPrimitiveTopology(draw.Topology);
        ctx1->VSSetShader(AddedVertexShader.Get(),nullptr,0);ctx1->PSSetShader(MaterialPixelShader.Get(),nullptr,0);
        for(size_t n=0;n<std::size(KEPT_VIEW_SLOTS);++n){ID3D11ShaderResourceView*v=draw.Views[n].Get();ctx1->PSSetShaderResources(KEPT_VIEW_SLOTS[n],1,&v);}
        ID3D11SamplerState*samplers[KEPT_SAMPLERS]{};for(unsigned n=0;n<KEPT_SAMPLERS;++n)samplers[n]=draw.Samplers[n].Get();ctx1->PSSetSamplers(0,KEPT_SAMPLERS,samplers);
        ctx1->RSSetState(draw.Rasterizer.Get());ctx1->RSSetViewports(1,&draw.Viewport);ctx1->RSSetScissorRects(1,&record.scissor);
        ctx1->OMSetDepthStencilState(depthState,draw.StencilReference);ctx1->OMSetBlendState(nullptr,nullptr,0xffffffff);
        ID3D11RenderTargetView*output=MaterialTarget.Get();ctx1->OMSetRenderTargets(1,&output,draw.DepthView.Get());
        if(draw.Call.Indexed)ctx1->DrawIndexed(draw.Call.Count,draw.Call.First,draw.Call.BaseVertex);else ctx1->Draw(draw.Call.Count,draw.Call.First);
        ++MaterialCounts.drawn;
    }
    // Clear all borrowed native buffers, views, sampler and depth references before restoration.
    ctx1->ClearState();ctx1->SwapDeviceContextState(previous.Get(),nullptr);
    if(MaterialCounts.drawn)
    {
        MaterialFrame=shadowmaps::FrameNumber;MaterialSource=lightbuffer::LightBuffer.Get();MaterialOffset=shadowmaps::CurrentOffset;
        DirectX::XMStoreFloat4x4(&MaterialCamera,lightbuffer::WorldToClip);
        if(capture::State==capture::Phase::Capturing)capture::AddSnapshot(ctx,MaterialTexture.Get(),"emissive-material-normals-R10G10B10A2");
    }
}
static ID3D11ShaderResourceView*MaterialNormalsView()
{
    return MaterialActive()&&MaterialFrame==shadowmaps::FrameNumber&&MaterialSource==lightbuffer::LightBuffer.Get()
        &&MaterialOffset.x==shadowmaps::CurrentOffset.x&&MaterialOffset.y==shadowmaps::CurrentOffset.y&&MaterialOffset.z==shadowmaps::CurrentOffset.z
        &&lightbuffer::WorldToClipKnown&&lightbuffer::IsSameCamera(DirectX::XMLoadFloat4x4(&MaterialCamera),lightbuffer::WorldToClip)?MaterialView.Get():nullptr;
}
static void MaterialNoteWrite(uint64_t handle)
{if(MaterialActive()){std::lock_guard guard(MaterialVersionLock);auto found=MaterialBufferVersions.find(handle);if(found!=MaterialBufferVersions.end())++found->second;}}
static void OnMaterialMapBuffer(device*dev,resource buffer,uint64_t offset,uint64_t size,map_access access,void**data)
{if(access!=map_access::read_only)MaterialNoteWrite(buffer.handle);shadowmaps::OnMapBuffer(dev,buffer,offset,size,access,data);}
static bool OnMaterialUpdateBuffer(device*dev,const void*data,resource buffer,uint64_t offset,uint64_t size)
{MaterialNoteWrite(buffer.handle);return shadowmaps::OnUpdateBuffer(dev,data,buffer,offset,size);}
static bool OnMaterialCopy(command_list*,resource,resource destination)
{MaterialNoteWrite(destination.handle);return false;}
static bool OnMaterialCopyBuffer(command_list*,resource,uint64_t,resource destination,uint64_t,uint64_t)
{MaterialNoteWrite(destination.handle);return false;}
static void OnMaterialDestroyResource(device*dev,resource handle)
{{std::lock_guard guard(MaterialVersionLock);MaterialBufferVersions.erase(handle.handle);}shadowmaps::OnDestroyResource(dev,handle);}
static bool OnMaterialUpdateTexture(device*,const subresource_data&,resource destination,uint32_t,const subresource_box*)
{MaterialNoteWrite(destination.handle);return false;}
static bool OnMaterialUpdateTextureCommand(command_list*,const subresource_data&,resource destination,uint32_t,const subresource_box*)
{MaterialNoteWrite(destination.handle);return false;}
static bool OnMaterialCopyTexture(command_list*,resource,uint32_t,const subresource_box*,resource destination,uint32_t,const subresource_box*,filter_mode)
{MaterialNoteWrite(destination.handle);return false;}
static bool OnMaterialResolveTexture(command_list*,resource,uint32_t,const subresource_box*,resource destination,uint32_t,uint32_t,uint32_t,uint32_t,format)
{MaterialNoteWrite(destination.handle);return false;}
static void WriteMaterialState(FILE*file)
{
    fprintf(file,"material normal cache: frame %llu, captured %u, replayed %u, unsupported %u, transform %u, camera %u, depth %u, changed resources %u, capacity %u; current atlas %d\n",
        static_cast<unsigned long long>(MaterialCounts.frame),MaterialCounts.captured,MaterialCounts.drawn,MaterialCounts.unsupported,
        MaterialCounts.transform,MaterialCounts.camera,MaterialCounts.depth,MaterialCounts.changed,MaterialCounts.capacity,MaterialNormalsView()?1:0);
}
