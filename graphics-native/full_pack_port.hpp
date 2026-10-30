#pragma once
#include "adapter.hpp"
namespace tqab {
struct FullSlot {uint32_t stage=0,slot=0;uint64_t view=0;};
struct FullPackPort:AdapterPort {
 virtual std::vector<FullSlot> AllBound()=0;
 virtual bool DescribeBound(uint32_t stage,uint32_t slot,uint64_t expected,uint64_t resource,ViewDesc&)=0;
 virtual std::shared_ptr<void> Fence()=0;
 virtual int FenceReady(const std::shared_ptr<void>&)=0;
 virtual void ForgetTargets(const std::vector<std::shared_ptr<View>>&,const std::vector<std::shared_ptr<Texture>>&)=0;
};
}
