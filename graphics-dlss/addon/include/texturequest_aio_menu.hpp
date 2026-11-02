#pragma once
// One owned same-process GPU bridge. The game HWND/input remain untouched.
// Producer copies a completed real presentation source; main D3D11 runtime
// samples it BEFORE ReShade FX/GUI. No second GUI and no guessed pixel mask.
static D3D12_RESOURCE_BARRIER TQMenuTransition(ID3D12Resource*r,D3D12_RESOURCE_STATES before,D3D12_RESOURCE_STATES after){D3D12_RESOURCE_BARRIER b{};b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;b.Transition={r,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,before,after};return b;}
struct TQMenuBridge {
 std::mutex mutex;
 Microsoft::WRL::ComPtr<ID3D12Resource> texture12;
 Microsoft::WRL::ComPtr<ID3D11Texture2D> texture11;
 Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv;
 Microsoft::WRL::ComPtr<ID3D12Fence> fence12;
 Microsoft::WRL::ComPtr<ID3D11Fence> fence11;
 Microsoft::WRL::ComPtr<ID3DDeviceContextState> state;
 Microsoft::WRL::ComPtr<ID3D11VertexShader> vs;
 Microsoft::WRL::ComPtr<ID3D11PixelShader> ps;
 Microsoft::WRL::ComPtr<ID3D11SamplerState> sampler;
 UINT64 timeline=0,published=0,consumer=0;
 UINT width=0,height=0;DXGI_FORMAT format=DXGI_FORMAT_UNKNOWN;
 bool broken=false;
} g_tq_menu;
static bool TQMenuIdle(){std::lock_guard<std::mutex>l(g_tq_menu.mutex);return !g_tq_menu.broken&&(!g_tq_menu.fence12||g_tq_menu.fence12->GetCompletedValue()>=g_tq_menu.timeline);}
static void TQMenuReset(){std::lock_guard<std::mutex>l(g_tq_menu.mutex);g_tq_menu.state.Reset();g_tq_menu.vs.Reset();g_tq_menu.ps.Reset();g_tq_menu.sampler.Reset();g_tq_menu.srv.Reset();g_tq_menu.texture11.Reset();g_tq_menu.texture12.Reset();g_tq_menu.fence11.Reset();g_tq_menu.fence12.Reset();g_tq_menu.timeline=g_tq_menu.published=g_tq_menu.consumer=0;g_tq_menu.width=g_tq_menu.height=0;}
static bool TQMenuInitialize(const D3D12_RESOURCE_DESC &desc)
{
 auto&m=g_tq_menu;
 if(m.broken)return false;
 if(m.texture12)return m.width==desc.Width&&m.height==desc.Height&&m.format==desc.Format;
 if(!CreateSharedPair11(UINT(desc.Width),desc.Height,desc.Format,m.texture12,m.texture11))return false;
 HRESULT hr=g_neural_device->CreateFence(0,D3D12_FENCE_FLAG_SHARED,IID_PPV_ARGS(&m.fence12));
 HANDLE shared=nullptr;if(SUCCEEDED(hr))hr=g_neural_device->CreateSharedHandle(m.fence12.Get(),nullptr,GENERIC_ALL,nullptr,&shared);
 Microsoft::WRL::ComPtr<ID3D11Device5>d5;if(SUCCEEDED(hr))hr=g_legacy_device11.As(&d5);
 if(SUCCEEDED(hr))hr=d5->OpenSharedFence(shared,IID_PPV_ARGS(&m.fence11));if(shared)CloseHandle(shared);
 if(SUCCEEDED(hr))hr=g_legacy_device11->CreateShaderResourceView(m.texture11.Get(),nullptr,&m.srv);
 if(FAILED(hr)){m.broken=true;Log("TextureQuest native menu bridge initialization failed 0x%08X",UINT(hr));return false;}
 m.width=UINT(desc.Width);m.height=desc.Height;m.format=desc.Format;return true;
}
static bool TQMenuPrepareCopy(ID3D12GraphicsCommandList*list,ID3D12Resource*source,std::unique_lock<std::mutex>&lock)
{
 if(!g_reshade_overlay_open.load(std::memory_order_acquire)||!source||!TQEffectiveEnabled())return false;
 lock=std::unique_lock<std::mutex>(g_tq_menu.mutex);
 if(!TQMenuInitialize(source->GetDesc())){lock.unlock();return false;}
 if(g_tq_menu.consumer&&FAILED(g_command_queue->Wait(g_tq_menu.fence12.Get(),g_tq_menu.consumer))){g_tq_menu.broken=true;lock.unlock();return false;}
 D3D12_RESOURCE_BARRIER before[]={TQMenuTransition(source,D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_COPY_SOURCE),TQMenuTransition(g_tq_menu.texture12.Get(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_COPY_DEST)};
 list->ResourceBarrier(2,before);list->CopyResource(g_tq_menu.texture12.Get(),source);
 D3D12_RESOURCE_BARRIER after[]={TQMenuTransition(source,D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_COMMON),TQMenuTransition(g_tq_menu.texture12.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_COMMON)};
 list->ResourceBarrier(2,after);return true; // Keep custody until submitted/signal.
}
static void TQMenuPublish(std::unique_lock<std::mutex>&lock)
{
 if(!lock.owns_lock())return;
 const UINT64 n=++g_tq_menu.timeline;
 if(SUCCEEDED(g_command_queue->Signal(g_tq_menu.fence12.Get(),n)))g_tq_menu.published=n;
 else {g_tq_menu.broken=true;Log("TextureQuest native menu bridge producer fence failed");}
 lock.unlock();
}
static bool TQMenuShaders()
{
 auto&m=g_tq_menu;if(m.state&&m.vs&&m.ps&&m.sampler)return true;
 Microsoft::WRL::ComPtr<ID3D11Device1>d1;HRESULT hr=g_legacy_device11.As(&d1);
 const D3D_FEATURE_LEVEL levels[]={D3D_FEATURE_LEVEL_11_0};D3D_FEATURE_LEVEL chosen{};
 if(SUCCEEDED(hr))hr=d1->CreateDeviceContextState(0,levels,1,D3D11_SDK_VERSION,__uuidof(ID3D11Device),&chosen,&m.state);
 static const char code[]="Texture2D<float4> Src:register(t0);SamplerState Samp:register(s0);struct O{float4 p:SV_Position;float2 uv:TEXCOORD0;};O VS(uint id:SV_VertexID){O o;float2 p=float2((id<<1)&2,id&2);o.uv=p;o.p=float4(p*float2(2,-2)+float2(-1,1),0,1);return o;}float4 PS(O i):SV_Target{return Src.SampleLevel(Samp,i.uv,0);}";
 Microsoft::WRL::ComPtr<ID3DBlob>vs,ps,error;
 if(SUCCEEDED(hr))hr=D3DCompile(code,sizeof(code)-1,"TQ-native-menu",nullptr,nullptr,"VS","vs_5_0",0,0,&vs,&error);
 if(SUCCEEDED(hr))hr=D3DCompile(code,sizeof(code)-1,"TQ-native-menu",nullptr,nullptr,"PS","ps_5_0",0,0,&ps,&error);
 if(SUCCEEDED(hr))hr=g_legacy_device11->CreateVertexShader(vs->GetBufferPointer(),vs->GetBufferSize(),nullptr,&m.vs);
 if(SUCCEEDED(hr))hr=g_legacy_device11->CreatePixelShader(ps->GetBufferPointer(),ps->GetBufferSize(),nullptr,&m.ps);
 D3D11_SAMPLER_DESC sample{};sample.Filter=D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT;sample.AddressU=sample.AddressV=sample.AddressW=D3D11_TEXTURE_ADDRESS_CLAMP;sample.MaxLOD=D3D11_FLOAT32_MAX;
 if(SUCCEEDED(hr))hr=g_legacy_device11->CreateSamplerState(&sample,&m.sampler);
 if(FAILED(hr)){Log("TextureQuest native menu shaders/state unavailable 0x%08X",UINT(hr));return false;}return true;
}
static void TQOnBeginEffects(reshade::api::effect_runtime*runtime,reshade::api::command_list*,reshade::api::resource_view target,reshade::api::resource_view)
{
 TextureQuestAioActivity activity(g_tq_gate);
 if(!activity||!TQEffectiveEnabled()||runtime!=g_runtime||!target.handle||!g_reshade_overlay_open.load(std::memory_order_acquire)||g_present_api!=reshade::api::device_api::d3d11)return;
 std::lock_guard<std::mutex>lock(g_tq_menu.mutex);auto&m=g_tq_menu;
 if(m.broken||!m.published||!m.fence11||!TQMenuShaders())return;
 Microsoft::WRL::ComPtr<ID3D11DeviceContext1>context1;if(FAILED(g_legacy_context11.As(&context1)))return;
 if(FAILED(g_legacy_context4->Wait(m.fence11.Get(),m.published))){m.broken=true;return;}
 auto*rtv=reinterpret_cast<ID3D11RenderTargetView*>(target.handle);Microsoft::WRL::ComPtr<ID3D11Resource>resource;rtv->GetResource(&resource);
 Microsoft::WRL::ComPtr<ID3D11Texture2D>color;if(FAILED(resource.As(&color)))return;D3D11_TEXTURE2D_DESC d{};color->GetDesc(&d);
 Microsoft::WRL::ComPtr<ID3DDeviceContextState>old;context1->SwapDeviceContextState(m.state.Get(),&old);
 ID3D11ShaderResourceView*srv=m.srv.Get();ID3D11SamplerState*sampler=m.sampler.Get();
 D3D11_VIEWPORT viewport{0,0,float(d.Width),float(d.Height),0,1};
 g_legacy_context11->RSSetViewports(1,&viewport);g_legacy_context11->OMSetRenderTargets(1,&rtv,nullptr);
 g_legacy_context11->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);g_legacy_context11->VSSetShader(m.vs.Get(),nullptr,0);g_legacy_context11->PSSetShader(m.ps.Get(),nullptr,0);g_legacy_context11->PSSetShaderResources(0,1,&srv);g_legacy_context11->PSSetSamplers(0,1,&sampler);g_legacy_context11->Draw(3,0);
 context1->SwapDeviceContextState(old.Get(),nullptr); // Full native D3D11 state restoration.
 const UINT64 n=++m.timeline;
 if(SUCCEEDED(g_legacy_context4->Signal(m.fence11.Get(),n)))m.consumer=n;else m.broken=true;
 g_legacy_context11->Flush();
}
