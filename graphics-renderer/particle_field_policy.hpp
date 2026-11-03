#pragma once
#include <DirectXMath.h>
#include "emission_policy.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <compare>
#include <utility>
#include <tuple>
#include <vector>

// Geometry/material integration, independent of frame rate, screen coverage,
// native buffer addresses and draw batching. No engine emitter IDs are inferred.
namespace particle_field_policy
{
using DirectX::XMFLOAT3;using DirectX::XMFLOAT4;
constexpr unsigned Triangles=32768,Draws=512,Tile=8,AtlasWidth=1024,AtlasHeight=2048,Samples=28,Lit=emission_policy::ParticleLimit;
constexpr float CellWidth=2;
// Particle gain is independent of surface emission. Existing settings migrate
// their previous shared gain once; both new multipliers otherwise start at 1.
constexpr float StrengthReference=1.0f;
constexpr float MaximumStrength=128.0f,MaximumRadiusScale=16.0f,MaximumWorldRadius=128.0f;
inline float CleanStrength(float value){return std::isfinite(value)?std::clamp(value,0.0f,MaximumStrength):1.0f;}
inline float CleanRadius(float value){return std::isfinite(value)?std::clamp(value,.25f,MaximumRadiusScale):1.0f;}
inline float LightRadius(float scale,float shadowRange)
{
    const float limit=std::isfinite(shadowRange)?std::max(2.0f,std::min(MaximumWorldRadius,shadowRange)):24.0f;
    return std::clamp(CellWidth*4*CleanRadius(scale),2.0f,limit);
}
constexpr double CaptureSeconds=.5,HoldSeconds=.20,ExpireSeconds=.70;
struct Key { int x=0,y=0,z=0; auto operator<=>(const Key&)const=default; };
struct Power { double x=0,y=0,z=0; };
struct Cell { Key key;Power power;double mx=0,my=0,mz=0,weight=0; };
struct Reduction { XMFLOAT4 mean;XMFLOAT4 moment; };
static_assert(sizeof(Reduction)==32);
inline bool Finite(const XMFLOAT3 &p){return std::isfinite(p.x)&&std::isfinite(p.y)&&std::isfinite(p.z);}
inline double Luma(const Power &p){return .2126*p.x+.7152*p.y+.0722*p.z;}
inline bool Good(const Power &p){return std::isfinite(p.x)&&std::isfinite(p.y)&&std::isfinite(p.z)&&p.x>=0&&p.y>=0&&p.z>=0;}
inline Power Scale(const Power &p,double w){return {p.x*w,p.y*w,p.z*w};}
inline void Add(Power &a,const Power &b){a.x+=b.x;a.y+=b.y;a.z+=b.z;}
enum class TriangleClass { Invalid, Excluded, Included };
enum class Exclusion { None, Extent, Distance };
inline TriangleClass ClassifyTriangle(const XMFLOAT4 *p,const Reduction &r,const XMFLOAT3 &camera,
    Power &power,XMFLOAT3 &position,Exclusion *reason=nullptr,float receiverRadius=0)
{
    if(reason)*reason=Exclusion::None;
    if(!p||!Finite(camera))return TriangleClass::Invalid;
    for(unsigned i=0;i<3;++i)if(!Finite({p[i].x,p[i].y,p[i].z})||p[i].w!=1)return TriangleClass::Invalid;
    const double ax=double(p[1].x)-p[0].x,ay=double(p[1].y)-p[0].y,az=double(p[1].z)-p[0].z;
    const double bx=double(p[2].x)-p[0].x,by=double(p[2].y)-p[0].y,bz=double(p[2].z)-p[0].z;
    const double cx=ay*bz-az*by,cy=az*bx-ax*bz,cz=ax*by-ay*bx;
    const double area=.5*std::sqrt(cx*cx+cy*cy+cz*cz);
    const Power mean{r.mean.x,r.mean.y,r.mean.z};
    if(!std::isfinite(area)||!Good(mean)||r.mean.w!=0||!std::isfinite(r.moment.x)||!std::isfinite(r.moment.y)
        ||!std::isfinite(r.moment.z)||r.moment.x<0||r.moment.y<0||r.moment.z<0
        ||r.moment.x+r.moment.y>r.moment.z*1.00001f)return TriangleClass::Invalid;
    // Native animated material power is retained. No coverage denominator,
    // per-batch normalization or brightness history is applied.
    power=Scale(mean,area);
    double u=1.0/3,v=1.0/3;
    if(r.moment.z>1e-12){u=r.moment.x/r.moment.z;v=r.moment.y/r.moment.z;}
    position={float(p[0].x+u*ax+v*bx),float(p[0].y+u*ay+v*by),float(p[0].z+u*az+v*bz)};
    if(!Finite(position)||!Good(power))return TriangleClass::Invalid;
    // Finite proven geometry outside this local-light domain contributes zero;
    // it does not invalidate unrelated near sources in the same native frame.
    // All malformed geometry/material/provenance checks above remain fatal.
    for(unsigned i=0;i<3;++i)
    {
        const double x=double(p[i].x)-position.x,y=double(p[i].y)-position.y,z=double(p[i].z)-position.z;
        if(x*x+y*y+z*z>64)
        {power={};if(reason)*reason=Exclusion::Extent;return TriangleClass::Excluded;}
    }
    const double dx=double(position.x)-camera.x,dy=double(position.y)-camera.y,dz=double(position.z)-camera.z;
    // A captured source outside the receiver domain can still light it when
    // its radius overlaps. The bounded reach does not invent culled sources.
    const double distanceLimit=96+(std::isfinite(receiverRadius)?std::clamp(receiverRadius,0.0f,MaximumWorldRadius):0.0f);
    if(dx*dx+dy*dy+dz*dz>distanceLimit*distanceLimit)
    {power={};if(reason)*reason=Exclusion::Distance;return TriangleClass::Excluded;}
    return TriangleClass::Included;
}
// Retain the original strict single-triangle predicate for existing callers.
inline bool Triangle(const XMFLOAT4 *p,const Reduction &r,const XMFLOAT3 &camera,Power &power,XMFLOAT3 &position)
{return ClassifyTriangle(p,r,camera,power,position)==TriangleClass::Included;}
inline bool Splat(std::vector<Cell> &parts,const XMFLOAT3 &p,const Power &power)
{
    if(!Finite(p)||!Good(power)||parts.size()>Triangles*8-8)return false;
    if(Luma(power)<=0)return true;
    int base[3];double fraction[3];const float values[3]{p.x,p.y,p.z};
    for(unsigned a=0;a<3;++a)
    {
        const double coordinate=double(values[a])/CellWidth;
        if(!std::isfinite(coordinate)||coordinate<-10000000||coordinate>10000000)return false;
        base[a]=int(std::floor(coordinate));fraction[a]=coordinate-base[a];
    }
    for(unsigned z=0;z<2;++z)for(unsigned y=0;y<2;++y)for(unsigned x=0;x<2;++x)
    {
        const double w=(x?fraction[0]:1-fraction[0])*(y?fraction[1]:1-fraction[1])*(z?fraction[2]:1-fraction[2]);
        if(w<=0)continue;
        Cell c;c.key={base[0]+int(x),base[1]+int(y),base[2]+int(z)};c.power=Scale(power,w);c.weight=Luma(c.power);
        c.mx=c.weight*p.x;c.my=c.weight*p.y;c.mz=c.weight*p.z;parts.push_back(c);
    }
    return true;
}
inline void Collapse(std::vector<Cell> &parts)
{
    std::sort(parts.begin(),parts.end(),[](const Cell&a,const Cell&b){
        if(a.key!=b.key)return a.key<b.key;
        return std::tie(a.power.x,a.power.y,a.power.z,a.mx,a.my,a.mz)
            <std::tie(b.power.x,b.power.y,b.power.z,b.mx,b.my,b.mz);});
    size_t out=0;
    for(const auto &p:parts)
    {
        if(out&&parts[out-1].key==p.key)
        {auto &c=parts[out-1];Add(c.power,p.power);c.mx+=p.mx;c.my+=p.my;c.mz+=p.mz;c.weight+=p.weight;}
        else parts[out++]=p;
    }
    parts.resize(out);
}
inline XMFLOAT3 Position(const Cell &c)
{return c.weight>0?XMFLOAT3{float(c.mx/c.weight),float(c.my/c.weight),float(c.mz/c.weight)}:XMFLOAT3{};}
inline std::array<size_t,Lit> Pick(const std::vector<Cell>&cells,const std::array<Key,Lit>&old,unsigned oldCount,
    const XMFLOAT3&receiver={},unsigned limit=emission_policy::DefaultParticleLimit)
{
    std::array<size_t,Lit> chosen;chosen.fill(SIZE_MAX);
    if(!Finite(receiver))return chosen;
    for(unsigned n=0;n<std::min(limit,Lit);++n)
    {
        double best=0;
        for(size_t i=0;i<cells.size();++i)
        {
            if(std::find(chosen.begin(),chosen.begin()+n,i)!=chosen.begin()+n)continue;
            const auto &c=cells[i];const auto p=Position(c);
            const double dx=double(p.x)-receiver.x,dy=double(p.y)-receiver.y,dz=double(p.z)-receiver.z;
            // Approximate expected receiver contribution using world distance,
            // never screen coverage/view angle. This affects allocation only;
            // original cell energy and animated material values stay intact.
            double score=Luma(c.power)/std::max(double(CellWidth*CellWidth),dx*dx+dy*dy+dz*dz);
            for(unsigned k=0;k<std::min(oldCount,Lit);++k)if(c.key==old[k]){score*=1.15;break;}
            if(score>best){best=score;chosen[n]=i;}
        }
    }
    return chosen;
}
inline float Visibility(double age)
{
    if(!std::isfinite(age)||age<0||age>=ExpireSeconds)return 0;
    return float(std::clamp((ExpireSeconds-age)/(ExpireSeconds-HoldSeconds),0.0,1.0));
}
struct Ledger
{
    uint64_t frame=0,epoch=0;unsigned draws=0,triangles=0;bool complete=true;
    // Capacity and unsupported individual sources do not invalidate other draws.
    // These counters describe a bounded partial domain, never complete coverage.
    unsigned attempted=0,capacityExcluded=0,stateExcluded=0;
    void Begin(uint64_t f,uint64_t e){*this={};frame=f;epoch=e;}
    void Attempt(){++attempted;}
    bool Add(unsigned vertices,unsigned instances)
    {
        if(!complete)return false;
        if(!vertices||vertices%3||instances!=1){++stateExcluded;return false;}
        if(draws>=Draws||vertices/3>Triangles-triangles){++capacityExcluded;return false;}
        ++draws;triangles+=vertices/3;return true;
    }
    void Exclude(){++stateExcluded;}
    void Undo(unsigned vertices){if(draws&&vertices/3<=triangles){--draws;triangles-=vertices/3;}Exclude();}
    // Only packet-wide provenance/completion failures use Reject.
    void Reject(){complete=false;}
    bool CanPublish(uint64_t e,double age)const
    {return complete&&epoch==e&&std::isfinite(age)&&age>=0&&age<=CaptureSeconds;}
};
inline bool Publish(std::vector<Cell>&old,std::vector<Cell>&&candidate,const Ledger &packet,uint64_t epoch,double age)
{
    if(!packet.CanPublish(epoch,age))return false;
    // Swap the entire generation. No cell from an older source frame is added
    // to the new frame's energy, even when the same cell key occurs in both.
    old=std::move(candidate);return true;
}
} // namespace particle_field_policy
