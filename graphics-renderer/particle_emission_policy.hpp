#pragma once
#include <DirectXMath.h>
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace particle_emission_policy
{
constexpr unsigned Pool=8, Lit=4, Packets=3, Vertices=3072, MeasureSize=64, Levels=7;
constexpr uint64_t SampleInterval=8;
inline bool VertexProfile(uint64_t vs)
{ return vs==0xae351b5cea68255bull || vs==0xea078a606781c9bdull || vs==0x65ceb4dd51400679ull; }
inline bool PixelProfile(uint64_t ps)
{ return ps==0xb519b367da7710d6ull || ps==0x6cd5de4aecacd26cull; }
inline bool Profile(uint64_t vs,uint64_t ps)
{
    if(vs==0xae351b5cea68255bull)return ps==0xb519b367da7710d6ull;
    return (vs==0xea078a606781c9bdull || vs==0x65ceb4dd51400679ull) && ps==0x6cd5de4aecacd26cull;
}
constexpr double CaptureSeconds=.5,HoldSeconds=.15,DisappearSeconds=.25,DisplaySeconds=.75;
inline bool CaptureFresh(double age){return std::isfinite(age)&&age>=0&&age<=CaptureSeconds;}
inline float Visibility(double unseen,double measuredAge,bool measured)
{
    if(!measured||!std::isfinite(unseen)||!std::isfinite(measuredAge)||unseen<0||measuredAge<0
        ||unseen>=DisappearSeconds||measuredAge>=DisplaySeconds)return 0;
    return static_cast<float>(std::clamp((DisappearSeconds-unseen)/(DisappearSeconds-HoldSeconds),0.0,1.0)
        *std::clamp((DisplaySeconds-measuredAge)/.1,0.0,1.0));
}
inline bool Bounds(const DirectX::XMFLOAT4 *p,unsigned count,const DirectX::XMFLOAT3 &camera,
    DirectX::XMFLOAT3 &center,float &radius,float &area)
{
    if(!p || !count || count>Vertices || count%3) return false;
    if(!std::isfinite(camera.x)||!std::isfinite(camera.y)||!std::isfinite(camera.z)) return false;
    double total=0,x=0,y=0,z=0;
    for(unsigned i=0;i<count;++i)
        if(!std::isfinite(p[i].x)||!std::isfinite(p[i].y)||!std::isfinite(p[i].z)||p[i].w!=1) return false;
    for(unsigned i=0;i<count;i+=3)
    {
        const double ax=p[i+1].x-p[i].x,ay=p[i+1].y-p[i].y,az=p[i+1].z-p[i].z;
        const double bx=p[i+2].x-p[i].x,by=p[i+2].y-p[i].y,bz=p[i+2].z-p[i].z;
        const double cx=ay*bz-az*by,cy=az*bx-ax*bz,cz=ax*by-ay*bx;
        const double a=.5*std::sqrt(cx*cx+cy*cy+cz*cz);
        total+=a;x+=a*(p[i].x+p[i+1].x+p[i+2].x)/3;
        y+=a*(p[i].y+p[i+1].y+p[i+2].y)/3;z+=a*(p[i].z+p[i+1].z+p[i+2].z)/3;
    }
    if(!std::isfinite(total)||total<1e-5) return false;
    center={static_cast<float>(x/total),static_cast<float>(y/total),static_cast<float>(z/total)};
    double r2=0;
    for(unsigned i=0;i<count;++i)
    { const double dx=p[i].x-center.x,dy=p[i].y-center.y,dz=p[i].z-center.z;r2=std::max(r2,dx*dx+dy*dy+dz*dz); }
    const double dx=center.x-camera.x,dy=center.y-camera.y,dz=center.z-camera.z;
    if(r2>64 || dx*dx+dy*dy+dz*dz>96*96) return false;
    radius=std::max(.05f,static_cast<float>(std::sqrt(r2)));area=static_cast<float>(std::min(total,32.0));
    return true;
}
inline bool Radiance(const float *pixel,DirectX::XMFLOAT3 &out)
{
    for(unsigned i=0;i<4;++i) if(!std::isfinite(pixel[i])) return false;
    const float coverage=1-pixel[3];
    if(coverage<1.0f/(MeasureSize*MeasureSize) || coverage>1.001f || pixel[0]<0 || pixel[1]<0 || pixel[2]<0) return false;
    out={std::min(4.0f,pixel[0]/coverage),std::min(4.0f,pixel[1]/coverage),std::min(4.0f,pixel[2]/coverage)};
    return std::max({out.x,out.y,out.z})>.001f;
}
}
