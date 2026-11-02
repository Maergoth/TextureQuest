#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

// A small existing lamp holder can identify another submesh of the same model.
// This never identifies arbitrary nearby geometry as part of a light source.
namespace lamp_holder_policy
{
constexpr unsigned MaxAnchors=16, MaxSiblings=16;
constexpr float AnchorDiagonal=8, AssemblyDiagonal=12, AssemblyAxis=8, Margin=.5f;
using Point=std::array<float,3>;
struct Bounds { Point low{},high{};bool known=false; };
struct Identity
{
    uintptr_t vertex=0,index=0;
    uint32_t stride=0,vertexOffset=0,indexOffset=0,indexFormat=0,topology=0;
    bool dynamic=false,indexed=true;
    std::array<float,12> world{};
};
inline bool Valid(const Bounds &box)
{
    if(!box.known)return false;
    for(unsigned n=0;n<3;++n)
        if(!std::isfinite(box.low[n])||!std::isfinite(box.high[n])||box.low[n]>box.high[n])return false;
    return true;
}
inline float DiagonalSquared(const Bounds &box)
{
    float sum=0;for(unsigned n=0;n<3;++n){const float d=box.high[n]-box.low[n];sum+=d*d;}return sum;
}
inline bool Contains(const Bounds &box,const Point &light)
{
    if(!Valid(box))return false;
    for(unsigned n=0;n<3;++n)
        if(!std::isfinite(light[n])||light[n]<box.low[n]-Margin||light[n]>box.high[n]+Margin)return false;
    return true;
}
inline bool Local(const Bounds &box)
{
    if(!Valid(box)||DiagonalSquared(box)>AssemblyDiagonal*AssemblyDiagonal)return false;
    for(unsigned n=0;n<3;++n)if(box.high[n]-box.low[n]>AssemblyAxis)return false;
    return true;
}
inline bool Anchor(const Bounds &box,const Point &light)
{return Contains(box,light)&&DiagonalSquared(box)<=AnchorDiagonal*AnchorDiagonal;}
inline bool SameIdentity(const Identity &a,const Identity &b)
{
    if(a.dynamic||b.dynamic||!a.indexed||!b.indexed||!a.vertex||!a.index||a.vertex!=b.vertex||a.index!=b.index||a.stride!=b.stride
        ||a.vertexOffset!=b.vertexOffset||a.indexOffset!=b.indexOffset||a.indexFormat!=b.indexFormat||a.topology!=b.topology)return false;
    for(unsigned n=0;n<12;++n)
        if(!std::isfinite(a.world[n])||!std::isfinite(b.world[n])||a.world[n]!=b.world[n])return false;
    return true;
}
inline bool Sibling(const Identity &anchorIdentity,const Bounds &anchor,const Identity &candidateIdentity,
    const Bounds &candidate,const Point &light)
{
    if(!Anchor(anchor,light)||!Contains(candidate,light)||!Local(candidate)||!SameIdentity(anchorIdentity,candidateIdentity))return false;
    Bounds joined{};joined.known=true;
    for(unsigned n=0;n<3;++n)
    {
        if(anchor.high[n]<candidate.low[n]||candidate.high[n]<anchor.low[n])return false;
        joined.low[n]=std::min(anchor.low[n],candidate.low[n]);joined.high[n]=std::max(anchor.high[n],candidate.high[n]);
    }
    return Local(joined);
}
}
