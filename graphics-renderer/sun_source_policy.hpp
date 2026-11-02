#pragma once
#include <cmath>
#include <cstdint>

// Conservative native-shadow subset, not a general physical-sun/indoor classifier.
namespace sun_source_policy
{
struct Evidence
{
    uint64_t generation=0,firstSeen=0,writeFrame=0,writeEpoch=0;
    uint32_t writes=0;
    bool sameResource=false,exactBaseView=false;
};
inline bool NativeWrite(bool colourBound,bool colourWrite,bool depthBound,bool depthWrite)
{return (colourBound&&colourWrite)||(depthBound&&depthWrite);}
inline bool SameCamera(float x,float y,float z,float a,float b,float c)
{return std::isfinite(x)&&std::isfinite(y)&&std::isfinite(z)&&std::isfinite(a)&&std::isfinite(b)&&std::isfinite(c)
    &&std::abs(x-a)<=.01f&&std::abs(y-b)<=.01f&&std::abs(z-c)<=.01f;}
inline bool Proven(const Evidence &source,uint64_t frame,uint64_t epoch)
{
    return source.sameResource&&source.exactBaseView
        &&source.generation&&source.firstSeen<frame&&source.writeFrame==frame&&source.writeEpoch==epoch&&source.writes;
}
inline bool Admit(const Evidence &source,uint64_t frame,uint64_t epoch,float nativeStrength)
{return Proven(source,frame,epoch)&&std::isfinite(nativeStrength)&&nativeStrength>0;}
}
