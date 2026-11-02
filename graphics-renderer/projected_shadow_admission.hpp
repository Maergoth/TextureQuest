#pragma once
#include <d3d11.h>
namespace projected_shadows
{
// Replays must be RGB source-alpha/add and read-only with respect to native depth/stencil.
inline bool ReplaySafe(const D3D11_BLEND_DESC &b,const D3D11_DEPTH_STENCIL_DESC &d)
{
    const auto &t=b.RenderTarget[0];
    const auto keep=[](const D3D11_DEPTH_STENCILOP_DESC &f) {
        return f.StencilFailOp==D3D11_STENCIL_OP_KEEP && f.StencilDepthFailOp==D3D11_STENCIL_OP_KEEP && f.StencilPassOp==D3D11_STENCIL_OP_KEEP;
    };
    return !b.AlphaToCoverageEnable && !b.IndependentBlendEnable && t.BlendEnable && t.SrcBlend==D3D11_BLEND_SRC_ALPHA
        && t.DestBlend==D3D11_BLEND_ONE && t.BlendOp==D3D11_BLEND_OP_ADD && (t.RenderTargetWriteMask&7)==7
        && d.DepthEnable && d.DepthWriteMask==D3D11_DEPTH_WRITE_MASK_ZERO
        && (!d.StencilEnable || d.StencilWriteMask==0 || (keep(d.FrontFace)&&keep(d.BackFace)));
}
}
