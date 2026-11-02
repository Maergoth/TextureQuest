#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>

// Native near-cascade evidence, then one frame-owned correction snapshot.
// This state never owns a native pointer or changes the game's requested settings.
namespace sun_direction_policy
{
struct Vector {float x=0,y=0,z=0;};
inline bool Finite(Vector v){return std::isfinite(v.x)&&std::isfinite(v.y)&&std::isfinite(v.z);}
inline double Length(Vector v){return std::sqrt(double(v.x)*v.x+double(v.y)*v.y+double(v.z)*v.z);}
inline bool Unit(Vector v,Vector &out)
{
    const double length=Length(v);
    if(!Finite(v)||!std::isfinite(length)||length<1e-10)return false;
    out={float(v.x/length),float(v.y/length),float(v.z/length)};return Finite(out);
}
inline Vector Interpolate(Vector a,Vector b,double t)
{
    t=std::clamp(t,0.0,1.0);
    const double dot=std::clamp(double(a.x)*b.x+double(a.y)*b.y+double(a.z)*b.z,-1.0,1.0);
    if(dot>.9995)
    {Vector result;return Unit({float(a.x+(b.x-a.x)*t),float(a.y+(b.y-a.y)*t),float(a.z+(b.z-a.z)*t)},result)?result:a;}
    Vector tangent;
    if(!Unit({float(b.x-dot*a.x),float(b.y-dot*a.y),float(b.z-dot*a.z)},tangent))
    {
        // Deterministic perpendicular for opposite vectors; no zero at the midpoint.
        const Vector axis=std::abs(a.x)<.8f?Vector{1,0,0}:Vector{0,1,0};
        const double projection=double(axis.x)*a.x+double(axis.y)*a.y+double(axis.z)*a.z;
        if(!Unit({float(axis.x-projection*a.x),float(axis.y-projection*a.y),float(axis.z-projection*a.z)},tangent))return a;
    }
    const double angle=std::acos(dot)*t,c=std::cos(angle),s=std::sin(angle);Vector result;
    return Unit({float(a.x*c+tangent.x*s),float(a.y*c+tangent.y*s),float(a.z*c+tangent.z*s)},result)?result:a;
}

struct State
{
    static constexpr uint64_t SourceFrames=4;
    static constexpr double SourceSeconds=.25,FadeSeconds=.2,DirectionSeconds=.15;
    uint64_t epoch=1,evidenceEpoch=0,evidenceFrame=0,publishedFrame=0;
    double evidenceTime=0,publishTime=0,weight=0,nativeActivity=0;
    Vector observed{},direction{};
    bool evidence=false,active=false,known=false,published=false;
    void Clear(){const uint64_t next=epoch+1;*this={};epoch=next;}
    bool Observe(uint64_t frame,double now,Vector toward,float strength)
    {
        if(!std::isfinite(now)||!std::isfinite(strength)||now<0)return false;
        if(evidence&&(frame<evidenceFrame||now<evidenceTime))return false;
        Vector normalized;
        if(strength>0&&!Unit(toward,normalized))return false;
        evidence=true;active=strength>0;evidenceFrame=frame;evidenceTime=now;evidenceEpoch=epoch;
        // Native c19.x stores one tenth of the shadow strength (native shader multiplies10).
        nativeActivity=std::clamp(double(strength)*10.0,0.0,1.0);
        if(active)observed=normalized;
        return true;
    }
    void Advance(uint64_t frame,double now)
    {
        if(!std::isfinite(now)||now<0)return;
        if(published&&frame<=publishedFrame)return;
        if(published&&now<publishTime){Clear();return;}
        const double dt=published?now-publishTime:0;
        const bool fresh=evidence&&evidenceEpoch==epoch&&frame>=evidenceFrame&&frame-evidenceFrame<=SourceFrames
            &&now>=evidenceTime&&now-evidenceTime<=SourceSeconds;
        const bool target=fresh&&active;
        if(target)
        {
            if(!known){direction=observed;known=true;}
            else direction=Interpolate(direction,observed,std::min(dt/DirectionSeconds,1.0));
        }
        const double step=std::max(dt,0.0)/FadeSeconds;
        const double targetWeight=target?nativeActivity:0;
        weight=std::clamp(weight+std::clamp(targetWeight-weight,-step,step),0.0,1.0);
        if(!known)weight=0;
        published=true;publishedFrame=frame;publishTime=now;
    }
    bool Apply(Vector native,float w,Vector &result)const
    {
        // Caller leaves every native bit untouched when this returns false.
        if(!published||!known||weight<=0||w!=0||!std::isfinite(w))return false;
        Vector unit;if(!Unit(native,unit))return false;
        const double length=Length(native);const Vector blended=Interpolate(unit,direction,weight);
        result={float(blended.x*length),float(blended.y*length),float(blended.z*length)};
        return Finite(result);
    }
};
}
