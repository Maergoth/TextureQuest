#pragma once
#include <atomic>
#include <cstdint>

namespace tq_world {
// Effective next-frame dependencies. Sampler-only choices are deliberately absent.
enum WorldFeature : std::uint32_t { PointWork=1, AOWork=2, SunWork=4, CaptureWork=8 };
constexpr std::uint32_t EffectWork=PointWork|AOWork|SunWork;
struct WorldSelection { bool point=false, ao=false, sun=false; };
struct SunSelectionInput {
    bool explicitSun=false, sun=true;
    bool savedPoint=false, point=true, savedAO=false, ao=true;
    bool savedSoftness=false; float softness=0.1f;
    bool savedDirection=false, savedIndoor=false;
};
constexpr bool InitialSunSelection(SunSelectionInput input)noexcept {
    if(input.explicitSun)return input.sun;
    // An old explicit all-off layout must not acquire new hidden sun work.
    const bool oldOff=input.savedPoint&&!input.point&&input.savedAO&&!input.ao
        &&input.savedSoftness&&input.softness<=0&&!input.savedDirection&&!input.savedIndoor;
    return !oldOff;
}
constexpr std::uint32_t WorldMask(WorldSelection selected,bool master,bool capture) noexcept {
    return (master?((selected.point?PointWork:0u)|(selected.ao?AOWork:0u)|(selected.sun?SunWork:0u)):0u)
        |(capture?CaptureWork:0u);
}
struct WorldTransition {
    std::uint32_t before=0,after=0;
    constexpr bool TrackingChanged()const noexcept{return (before!=0)!=(after!=0);}
    constexpr bool ResetScene()const noexcept{return ((before^after)&EffectWork)!=0||TrackingChanged();}
    constexpr bool ResetPoint()const noexcept{return ((before^after)&PointWork)!=0;}
};
class WorldWork {
    std::atomic<std::uint32_t> mask_{0};
public:
    std::uint32_t Mask()const noexcept{return mask_.load(std::memory_order_acquire);}
    bool Tracking()const noexcept{return Mask()!=0;}
    bool Point()const noexcept{return (Mask()&PointWork)!=0;}
    bool AO()const noexcept{return (Mask()&AOWork)!=0;}
    bool Sun()const noexcept{return (Mask()&SunWork)!=0;}
    WorldTransition Commit(std::uint32_t mask)noexcept{
        return {mask_.exchange(mask,std::memory_order_acq_rel),mask};
    }
    void Clear()noexcept{mask_.store(0,std::memory_order_release);}
};
inline WorldWork Work;
}
