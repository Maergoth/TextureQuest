#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>

namespace emission_policy
{
// Storage ceilings are distinct from the saved per-user defaults.
constexpr unsigned GlowLimit=512,ParticleLimit=128,DefaultGlowLimit=64,DefaultParticleLimit=64;
inline unsigned ClampGlowLimit(int value){return static_cast<unsigned>(std::clamp(value,1,int(GlowLimit)));}
inline unsigned ClampParticleLimit(int value){return static_cast<unsigned>(std::clamp(value,1,int(ParticleLimit)));}
struct Budget { unsigned total=0,particles=0,surfaces=0; };
inline Budget Split(int limit,unsigned native,unsigned particles,int emissiveLimit=DefaultGlowLimit,
    int particleLimit=DefaultParticleLimit)
{
    const unsigned all=static_cast<unsigned>(std::clamp(limit,1,int(GlowLimit)));
    const unsigned total=std::min(native>=all?0u:all-native,ClampGlowLimit(emissiveLimit));
    const unsigned reserved=std::min({particles,ClampParticleLimit(particleLimit),total});
    return {total,reserved,total-reserved};
}
inline unsigned Held(unsigned surfaces,unsigned picked)
{return picked>=surfaces?0:surfaces-picked;}
enum class Producer { Surface,Particle };
struct SampleService
{
    uint64_t frame=UINT64_MAX,particleSeen=UINT64_MAX;
    bool used=false;
    void Clear(){*this={};}
    void NoteParticle(uint64_t now){particleSeen=now;}
    bool Recent(uint64_t now)const
    {return particleSeen!=UINT64_MAX&&now>=particleSeen&&now-particleSeen<=3;}
    bool Take(uint64_t now,Producer producer)
    {
        if(frame!=now){frame=now;used=false;}
        if(used)return false;
        const bool recent=Recent(now),particleTurn=recent&&now%3!=0;
        if(producer==Producer::Surface?particleTurn:!particleTurn)return false;
        used=true;return true;
    }
};
inline SampleService Samples;
inline bool Measurement(uint64_t ticket,bool painted,uint64_t savedTicket,bool savedPainted)
{return ticket&&ticket==savedTicket&&painted==savedPainted;}
inline bool ValidPixel(const float *color,float depth)
{
    return std::isfinite(depth)&&depth>=0&&depth<1&&std::isfinite(color[3])&&color[3]<=.5f
        &&std::isfinite(color[0])&&std::isfinite(color[1])&&std::isfinite(color[2])
        &&color[0]>=0&&color[1]>=0&&color[2]>=0;
}
inline bool Unproject(const float *m,float clipX,float clipY,float depth,const float *offset,float *point)
{
    const float w=m[12]*clipX+m[13]*clipY+m[14]*depth+m[15];
    if(!std::isfinite(w)||std::abs(w)<1e-12f)return false;
    for(unsigned row=0;row<3;++row)
    {
        point[row]=(m[row*4]*clipX+m[row*4+1]*clipY+m[row*4+2]*depth+m[row*4+3])/w-offset[row];
        if(!std::isfinite(point[row]))return false;
    }
    return true;
}
inline bool Cell(float point,float width,int &cell)
{
    if(!std::isfinite(point)||!std::isfinite(width)||width<=0)return false;
    const double value=std::floor(static_cast<double>(point)/width);
    if(value<std::numeric_limits<int>::min()||value>std::numeric_limits<int>::max())return false;
    cell=static_cast<int>(value);return true;
}
} // namespace emission_policy
