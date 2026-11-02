#pragma once
#include "texturequest_world_master.hpp"
#include "world_work_policy.hpp"
#include <type_traits>
namespace tq_world {
inline bool Accept(reshade::api::device*dev) {
    return dev&&dev->get_api()==reshade::api::device_api::d3d11&&Managed.Device(dev->get_native());
}
template<class T>bool Accept(T*object){return object&&Accept(object->get_device());}
template<auto Fn>struct Guard;
template<class R,class First,class...Rest,R(*Fn)(First,Rest...)>
struct Guard<Fn> {
    static R Call(First first,Rest...rest){
        if(!Accept(first)){if constexpr(!std::is_void_v<R>)return R{};else return;}
        return Fn(first,rest...);
    }
};
// Hot scene callbacks must skip even device/native ownership queries when idle.
// Lifecycle and shader classification continue through the ordinary Guard.
template<auto Fn,bool PointOnly=false>struct WorkGuard;
template<bool PointOnly,class R,class First,class...Rest,R(*Fn)(First,Rest...)>
struct WorkGuard<Fn,PointOnly> {
    static R Call(First first,Rest...rest){
        if(!(PointOnly?Work.Point():Work.Tracking())){
            if constexpr(!std::is_void_v<R>)return R{};else return;
        }
        return Guard<Fn>::Call(first,rest...);
    }
};
}
