#pragma once
#include "texturequest_aio_api.h"
#include <atomic>
#include <cstdint>
#include <algorithm>
namespace tq_aio_v2 {
inline bool Valid(const TextureQuestAIOControlsV2 &c) {
 return c.Abi==2&&c.Bytes==32&&c.EnabledMask<=7&&c.SrMode>=1&&c.SrMode<=5&&
 (c.FrameMultiplier>=2&&c.FrameMultiplier<=4)&&
 c.NrModel>=1&&c.NrModel<=3&&c.NrPasses>=1&&c.NrPasses<=3&&c.Reserved==0;
}
inline uint32_t Pack(const TextureQuestAIOControlsV2&c){return c.EnabledMask|(c.SrMode<<3)|(c.FrameMultiplier<<6)|(c.NrModel<<9)|(c.NrPasses<<11);}
inline TextureQuestAIOControlsV2 Unpack(uint64_t t){const auto p=uint32_t(t);return {2,32,p&7,(p>>3)&7,(p>>6)&7,(p>>9)&3,(p>>11)&3,0};}
class Mailbox {
 std::atomic<uint64_t> requested_{0},applied_{0};std::atomic<bool>loaded_{false};
public:
 void Initialize(const TextureQuestAIOControlsV2 &c){const uint64_t t=(uint64_t(1)<<32)|Pack(c);applied_.store(t);requested_.store(t);loaded_.store(true,std::memory_order_release);}
 bool Request(const TextureQuestAIOControlsV2&c,bool&changed){
  changed=false;if(!Valid(c)||!loaded_.load(std::memory_order_acquire))return false;
  auto old=requested_.load(std::memory_order_acquire);const auto p=Pack(c);
  while(uint32_t(old)!=p){const auto n=(((old>>32)+1)<<32)|p;if(requested_.compare_exchange_weak(old,n,std::memory_order_acq_rel)){changed=true;break;}}
  return true;
 }
 uint64_t Requested()const{return requested_.load(std::memory_order_acquire);}
 void Applied(uint64_t t){applied_.store(t,std::memory_order_release);}
 TextureQuestAIOControlStatusV2 Status()const{const auto r=requested_.load(std::memory_order_acquire),a=applied_.load(std::memory_order_acquire);return{2,96,uint32_t((loaded_.load(std::memory_order_acquire)?1:0)|(r!=a?2:0)),0,r>>32,a>>32,Unpack(r),Unpack(a)};}
};
struct Extent {uint32_t width,height;};
inline Extent ProcessingExtent(Extent capture,uint32_t srMode,bool enabled=true){
 if(!enabled||srMode<=1)return capture;
 const uint32_t numerator=srMode==2?2:srMode==3?58:srMode==4?1:1;
 const uint32_t denominator=srMode==2?3:srMode==3?100:srMode==4?2:3;
 auto scale=[&](uint32_t v){return std::max(32u,std::min(v,((v*numerator/denominator)/2)*2));};
 return{scale(capture.width),scale(capture.height)};
}
struct Geometry {uint32_t logicalWidth,logicalHeight,physicalWidth,physicalHeight;float scaleX,scaleY;};
inline bool GeometryFromBounds(uint32_t lw,uint32_t lh,int64_t left,int64_t top,int64_t right,int64_t bottom,Geometry&g){
 const auto w=right-left,h=bottom-top;if(!lw||!lh||lw>16384||lh>16384||w<=0||h<=0||w>16384||h>16384)return false;
 g={lw,lh,uint32_t(w),uint32_t(h),float(lw)/float(w),float(lh)/float(h)};return true;
}
}
