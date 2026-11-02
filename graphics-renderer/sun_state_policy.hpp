#pragma once
#include <cmath>
#include <cstdint>

namespace sun_state_policy
{
constexpr bool KnownProjected(uint64_t hash)
{return hash==0x851068275842e7b8ull||hash==0x4346a65a61b62551ull;}
struct BasePass
{
    bool master=false,target=false,sunVertex=false,ambientPixel=false,point=false,projected=false;
    bool fullViewport=false,sameCamera=false,finiteRows=false,singleInstance=false,opaque=false;
};
constexpr bool Admitted(const BasePass &p)
{return p.master&&p.target&&p.sunVertex&&p.ambientPixel&&!p.point&&!p.projected
    &&p.fullViewport&&p.sameCamera&&p.finiteRows&&p.singleInstance&&p.opaque;}
constexpr bool Window(uint32_t first,uint32_t count,uint32_t bytes,uint32_t row,uint32_t rows=1)
{
    const uint64_t begin=uint64_t(first)*16,end=begin+(uint64_t(row)+rows)*16;
    return rows&&end<=bytes&&(!count?first==0:uint64_t(row)+rows<=count);
}
constexpr bool Fresh(uint64_t frame,uint64_t stamp,uint64_t age)
{return stamp&&frame>=stamp&&frame-stamp<=age;}
constexpr uint32_t SliceBytes(uint32_t first,uint32_t count,uint32_t bytes,uint32_t requiredRows)
{
    if(!Window(first,count,bytes,0,requiredRows))return 0;
    const uint64_t available=uint64_t(bytes)-uint64_t(first)*16;
    const uint64_t selected=count?uint64_t(count)*16:available;
    const uint64_t length=selected<available?selected:available;
    return length&&length<=available&&length<=4096?static_cast<uint32_t>(length):0;
}
inline bool Direction(float x,float y,float z)
{return std::isfinite(x)&&std::isfinite(y)&&std::isfinite(z)
    &&double(x)*x+double(y)*y+double(z)*z>1e-20;}
struct CopyShape
{
    uint32_t width=0,height=0,mips=0,layers=0,samples=0,quality=0;
    bool texture2DView=false;uint32_t firstMip=0,viewMips=0;
};
constexpr bool Copyable(const CopyShape &s)
{return s.width&&s.height&&s.mips==1&&s.layers==1&&s.samples==1&&!s.quality
    &&s.texture2DView&&!s.firstMip&&s.viewMips==1;}
}
