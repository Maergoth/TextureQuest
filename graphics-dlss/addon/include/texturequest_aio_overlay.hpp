#pragma once
// Same-process GPU-only native GUI layer. ReShade renders its actual draw lists
// once into transparent RGBA. Native HWND, input, clip rects and GUI geometry do
// not change. D3D12 samples this layer LAST on every real/generated presentation.
// Included after CreateSharedPair11 and before presenter/control callbacks.
#include "texturequest_overlay_ring.hpp"
struct TQOverlaySlot {
    Microsoft::WRL::ComPtr<ID3D12Resource> texture12;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> texture11;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> target11;
    Microsoft::WRL::ComPtr<ID3D12Fence> produced12,consumed12;
    Microsoft::WRL::ComPtr<ID3D11Fence> produced11,consumed11;
};
struct TQOverlayBridge {
    std::mutex mutex;
    std::unique_lock<std::mutex> producer;
    std::array<TQOverlaySlot,tq_overlay_ring::State::Count> slots;
    tq_overlay_ring::State ring;
    unsigned recordingSlot=tq_overlay_ring::State::None;
    UINT64 skippedPublications=0;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> root;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> pipeline;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> heap;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> previous[8];
    Microsoft::WRL::ComPtr<ID3D11DepthStencilView> previous_depth;
    UINT width=0,height=0,stride=0;
    DXGI_FORMAT output_format=DXGI_FORMAT_UNKNOWN;
    bool active=false, redirected=false, broken=false;
} g_tq_overlay;

static D3D12_RESOURCE_BARRIER TQOverlayTransition(ID3D12Resource*r,D3D12_RESOURCE_STATES a,D3D12_RESOURCE_STATES b) {
    D3D12_RESOURCE_BARRIER v{};v.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    v.Transition={r,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,a,b};return v;
}
static bool TQOverlayCompletions(UINT&produced,UINT&consumed) {
    auto&m=g_tq_overlay;produced=consumed=0;
    for(unsigned i=0;i<tq_overlay_ring::State::Count;++i) {
        const auto&s=m.slots[i];const auto&v=m.ring.Get(i);
        const UINT64 p=s.produced12?s.produced12->GetCompletedValue():0;
        const UINT64 c=s.consumed12?s.consumed12->GetCompletedValue():0;
        if(p==UINT64_MAX||c==UINT64_MAX){m.broken=true;return false;}
        if(p>=v.Produced)produced|=1u<<i;
        if(c>=v.Consumed)consumed|=1u<<i;
    }
    return true;
}
static bool TQOverlayIdle() {
    std::lock_guard<std::mutex> lock(g_tq_overlay.mutex);auto&m=g_tq_overlay;
    UINT p=0,c=0;
    return !m.active&&!m.broken&&TQOverlayCompletions(p,c)&&
        (p&c)==tq_overlay_ring::State::All;
}
static void TQOverlayReset() {
    std::lock_guard<std::mutex> lock(g_tq_overlay.mutex);auto&m=g_tq_overlay;
    // Caller has drained every producer/consumer fence and gate owner first.
    for(auto&s:m.slots)s={};m.ring.Reset();m.recordingSlot=tq_overlay_ring::State::None;
    m.pipeline.Reset();m.root.Reset();m.heap.Reset();
    for(auto&v:m.previous)v.Reset();m.previous_depth.Reset();
    m.width=m.height=m.stride=0;m.skippedPublications=0;
    m.output_format=DXGI_FORMAT_UNKNOWN;m.redirected=false;
}
static HRESULT TQOverlayFencePair(Microsoft::WRL::ComPtr<ID3D12Fence>&f12,Microsoft::WRL::ComPtr<ID3D11Fence>&f11) {
    HRESULT hr=g_neural_device->CreateFence(0,D3D12_FENCE_FLAG_SHARED,IID_PPV_ARGS(&f12));
    HANDLE shared=nullptr;
    if(SUCCEEDED(hr))hr=g_neural_device->CreateSharedHandle(f12.Get(),nullptr,GENERIC_ALL,nullptr,&shared);
    Microsoft::WRL::ComPtr<ID3D11Device5>d5;
    if(SUCCEEDED(hr))hr=g_legacy_device11.As(&d5);
    if(SUCCEEDED(hr))hr=d5->OpenSharedFence(shared,IID_PPV_ARGS(&f11));
    if(shared)CloseHandle(shared);return hr;
}
static bool TQOverlayInitialize(UINT width,UINT height) {
    auto&m=g_tq_overlay;
    if(m.broken||!width||!height||!g_legacy_context4)return false;
    if(m.slots[0].texture12) {
        if(m.width==width&&m.height==height)return true;
        g_tq_gate.Invalidate();return false;
    }
    HRESULT hr=S_OK;
    for(auto&s:m.slots) {
        if(!CreateSharedPair11(width,height,DXGI_FORMAT_R8G8B8A8_UNORM,s.texture12,s.texture11,true)){hr=E_FAIL;break;}
        hr=g_legacy_device11->CreateRenderTargetView(s.texture11.Get(),nullptr,&s.target11);
        if(SUCCEEDED(hr))hr=TQOverlayFencePair(s.produced12,s.produced11);
        if(SUCCEEDED(hr))hr=TQOverlayFencePair(s.consumed12,s.consumed11);
        if(FAILED(hr))break;
    }
    if(FAILED(hr)) {
        for(auto&s:m.slots)s={};
        Log("TextureQuest native GUI ring unavailable: 0x%08X",UINT(hr));return false;
    }
    m.width=width;m.height=height;return true;
}
static void TQOverlayRedirect(const ImDrawList*,const ImDrawCmd*) {
    auto&m=g_tq_overlay;
    if(!m.active||!m.producer.owns_lock()||!TQEffectiveEnabled())return;
    ID3D11RenderTargetView*previous[8]={};ID3D11DepthStencilView*depth=nullptr;
    g_legacy_context11->OMGetRenderTargets(8,previous,&depth);
    for(UINT i=0;i<8;++i)m.previous[i].Attach(previous[i]);m.previous_depth.Attach(depth);
    ID3D11RenderTargetView*target=m.slots[m.ring.Writer()].target11.Get();
    g_legacy_context11->OMSetRenderTargets(1,&target,nullptr);m.redirected=true;
}
static bool TQOverlayPipeline(DXGI_FORMAT);
static bool TQOverlayPrepare(UINT width,UINT height) {
    auto&m=g_tq_overlay;
    // Gate custody spans the actual native GUI render, not just this callback.
    if(m.active||!g_tq_gate.Enter())return false;
    m.producer=std::unique_lock<std::mutex>(m.mutex);
    DXGI_SWAP_CHAIN_DESC output{};
    if(!TQEffectiveEnabled()||!g_proxy_swapchain||FAILED(g_proxy_swapchain->GetDesc(&output))||
        !TQOverlayInitialize(width,height)||!TQOverlayPipeline(output.BufferDesc.Format)) {
        m.producer.unlock();g_tq_gate.Leave();return false;
    }
    UINT p=0,c=0;
    if(!TQOverlayCompletions(p,c)){m.producer.unlock();g_tq_gate.Leave();return false;}
    const unsigned slot=m.ring.ReserveProducer(p,c);
    if(slot==tq_overlay_ring::State::None) {
        // Reuse a completed native GUI image; never queue a D11 wait behind
        // the neural/presenter path or stop Home input/scene processing.
        if(++m.skippedPublications<=4)Log("TextureQuest GUI ring busy; retaining completed GUI publication without game queue wait");
        m.producer.unlock();g_tq_gate.Leave();return true;
    }
    const float clear[4]={0,0,0,0};g_legacy_context11->ClearRenderTargetView(m.slots[slot].target11.Get(),clear);
    m.active=true;m.redirected=false;
    // ReShade's pinned renderer calls UserCallback before drawing windows and
    // does not rebind its RTV in the command loop. No stack userdata is retained.
    ImGui::GetBackgroundDrawList(nullptr)->AddCallback(TQOverlayRedirect,nullptr);
    return true;
}
static void TQOverlayFinish() {
    auto&m=g_tq_overlay;if(!m.active)return;
    // Normal provider already restored FULL application state before this
    // event. Only repair our target if it remains bound (never overwrite a
    // successfully restored application RTV/DSV with the GUI entry state).
    if(m.redirected) {
        Microsoft::WRL::ComPtr<ID3D11RenderTargetView>current;
        g_legacy_context11->OMGetRenderTargets(1,&current,nullptr);
        if(current.Get()==m.slots[m.ring.Writer()].target11.Get()) {
            ID3D11RenderTargetView*previous[8]={};
            for(UINT i=0;i<8;++i)previous[i]=m.previous[i].Get();
            g_legacy_context11->OMSetRenderTargets(8,previous,m.previous_depth.Get());
        }
    }
    for(auto &v:m.previous)v.Reset();m.previous_depth.Reset();
    const UINT64 signal=m.ring.NextProducer();
    if(signal&&SUCCEEDED(g_legacy_context4->Signal(m.slots[m.ring.Writer()].produced11.Get(),signal))) {
        if(!m.ring.PublishProducer(signal))m.broken=true;
    } else {m.broken=true;m.ring.CancelProducer();Log("TextureQuest native GUI producer fence failed");}
    g_legacy_context11->Flush();m.active=false;m.redirected=false;
    m.producer.unlock();g_tq_gate.Leave();
}
static bool TQOverlayPipeline(DXGI_FORMAT format) {
    auto&m=g_tq_overlay;if(m.pipeline)return m.output_format==format;
    D3D12_DESCRIPTOR_RANGE range{};range.RangeType=D3D12_DESCRIPTOR_RANGE_TYPE_SRV;range.NumDescriptors=1;
    D3D12_ROOT_PARAMETER parameter{};parameter.ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameter.DescriptorTable.NumDescriptorRanges=1;parameter.DescriptorTable.pDescriptorRanges=&range;
    parameter.ShaderVisibility=D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_STATIC_SAMPLER_DESC sampler{};sampler.Filter=D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU=sampler.AddressV=sampler.AddressW=D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.ShaderVisibility=D3D12_SHADER_VISIBILITY_PIXEL;sampler.MaxLOD=D3D12_FLOAT32_MAX;
    D3D12_ROOT_SIGNATURE_DESC rd{};rd.NumParameters=1;rd.pParameters=&parameter;rd.NumStaticSamplers=1;rd.pStaticSamplers=&sampler;
    rd.Flags=D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    Microsoft::WRL::ComPtr<ID3DBlob>signature,error,vs,ps;
    HRESULT hr=D3D12SerializeRootSignature(&rd,D3D_ROOT_SIGNATURE_VERSION_1,&signature,&error);
    if(SUCCEEDED(hr))hr=g_neural_device->CreateRootSignature(0,signature->GetBufferPointer(),signature->GetBufferSize(),IID_PPV_ARGS(&m.root));
    static const char code[]="Texture2D<float4> Gui:register(t0);SamplerState Samp:register(s0);struct O{float4 p:SV_Position;float2 uv:TEXCOORD0;};O VS(uint id:SV_VertexID){O o;float2 p=float2((id<<1)&2,id&2);o.uv=p;o.p=float4(p*float2(2,-2)+float2(-1,1),0,1);return o;}float4 PS(O i):SV_Target{return Gui.SampleLevel(Samp,i.uv,0);}";
    if(SUCCEEDED(hr))hr=D3DCompile(code,sizeof(code)-1,"TQ-native-GUI",nullptr,nullptr,"VS","vs_5_0",0,0,&vs,&error);
    if(SUCCEEDED(hr))hr=D3DCompile(code,sizeof(code)-1,"TQ-native-GUI",nullptr,nullptr,"PS","ps_5_0",0,0,&ps,&error);
    D3D12_GRAPHICS_PIPELINE_STATE_DESC p{};p.pRootSignature=m.root.Get();
    if(vs)p.VS={vs->GetBufferPointer(),vs->GetBufferSize()};if(ps)p.PS={ps->GetBufferPointer(),ps->GetBufferSize()};
    auto&b=p.BlendState.RenderTarget[0];b.BlendEnable=TRUE;
    // Native ReShade source-alpha RGB / one-alpha rendering into clear RGBA
    // produces premultiplied color. Composite once with ONE/INV_SRC_ALPHA.
    b.SrcBlend=D3D12_BLEND_ONE;b.DestBlend=D3D12_BLEND_INV_SRC_ALPHA;b.BlendOp=D3D12_BLEND_OP_ADD;
    b.SrcBlendAlpha=D3D12_BLEND_ONE;b.DestBlendAlpha=D3D12_BLEND_INV_SRC_ALPHA;b.BlendOpAlpha=D3D12_BLEND_OP_ADD;
    b.RenderTargetWriteMask=D3D12_COLOR_WRITE_ENABLE_ALL;
    p.SampleMask=UINT_MAX;p.RasterizerState.FillMode=D3D12_FILL_MODE_SOLID;p.RasterizerState.CullMode=D3D12_CULL_MODE_NONE;
    p.DepthStencilState.DepthEnable=FALSE;p.DepthStencilState.StencilEnable=FALSE;
    p.PrimitiveTopologyType=D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;p.NumRenderTargets=1;p.RTVFormats[0]=format;p.SampleDesc.Count=1;
    if(SUCCEEDED(hr))hr=g_neural_device->CreateGraphicsPipelineState(&p,IID_PPV_ARGS(&m.pipeline));
    D3D12_DESCRIPTOR_HEAP_DESC hd{};hd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.NumDescriptors=kProxyCommandSlotCount;hd.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if(SUCCEEDED(hr))hr=g_neural_device->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&m.heap));
    if(FAILED(hr)){m.pipeline.Reset();m.root.Reset();m.heap.Reset();Log("TextureQuest native GUI compositor unavailable: 0x%08X",UINT(hr));return false;}
    m.stride=g_neural_device->GetDescriptorHandleIncrementSize(hd.Type);m.output_format=format;return true;
}
static bool TQOverlayDraw(ID3D12GraphicsCommandList*list,UINT slot,DXGI_FORMAT format,std::unique_lock<std::mutex>&lock) {
    lock=std::unique_lock<std::mutex>(g_tq_overlay.mutex);auto&m=g_tq_overlay;
    if(m.broken||!m.slots[0].texture12||slot>=kProxyCommandSlotCount||!TQOverlayPipeline(format)) {lock.unlock();return false;}
    UINT p=0,c=0;
    if(!TQOverlayCompletions(p,c)){lock.unlock();return false;}
    const unsigned selected=m.ring.Latest(p);
    if(selected==tq_overlay_ring::State::None){lock.unlock();return false;}
    auto&read=m.slots[selected];const auto&state=m.ring.Get(selected);
    // Only wait for an already completed producer value: explicit visibility,
    // never a future GUI publication that could stall the compositor queue.
    if(FAILED(g_command_queue->Wait(read.produced12.Get(),state.Produced))) {m.broken=true;lock.unlock();return false;}
    m.recordingSlot=selected;
    auto cpu=m.heap->GetCPUDescriptorHandleForHeapStart();cpu.ptr+=SIZE_T(slot)*m.stride;
    D3D12_SHADER_RESOURCE_VIEW_DESC s{};s.Format=DXGI_FORMAT_R8G8B8A8_UNORM;s.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;
    s.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;s.Texture2D.MipLevels=1;
    g_neural_device->CreateShaderResourceView(read.texture12.Get(),&s,cpu);
    auto barrier=TQOverlayTransition(read.texture12.Get(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);list->ResourceBarrier(1,&barrier);
    ID3D12DescriptorHeap*heaps[]={m.heap.Get()};list->SetDescriptorHeaps(1,heaps);
    list->SetGraphicsRootSignature(m.root.Get());list->SetPipelineState(m.pipeline.Get());
    auto gpu=m.heap->GetGPUDescriptorHandleForHeapStart();gpu.ptr+=UINT64(slot)*m.stride;
    list->SetGraphicsRootDescriptorTable(0,gpu);list->DrawInstanced(3,1,0,0);
    barrier=TQOverlayTransition(read.texture12.Get(),D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_COMMON);list->ResourceBarrier(1,&barrier);
    return true; // Keep CPU custody until the sampling list is submitted.
}
static void TQOverlayConsumed(std::unique_lock<std::mutex>&lock) {
    if(!lock.owns_lock())return;auto&m=g_tq_overlay;const unsigned slot=m.recordingSlot;
    const UINT64 signal=m.ring.NextConsumer(slot);
    if(signal&&SUCCEEDED(g_command_queue->Signal(m.slots[slot].consumed12.Get(),signal))) {
        if(!m.ring.SubmitConsumer(slot,signal))m.broken=true;
    } else {m.broken=true;Log("TextureQuest native GUI consumer fence failed");}
    m.recordingSlot=tq_overlay_ring::State::None;lock.unlock();
}
