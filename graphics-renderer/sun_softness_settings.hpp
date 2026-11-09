#pragma once
#include <algorithm>
#include <cmath>
namespace sun_softness_settings
{
constexpr float Spread=0.00465f;
constexpr float LegacyFactor=(1.0f/3.0f)/Spread;
constexpr float Default=1.6f;
constexpr float Maximum=2.0f*LegacyFactor;
inline float Clean(float v,float fallback=Default){return std::isfinite(v)&&v>=0?std::min(v,Maximum):fallback;}
struct Values{float Active=0,Preference=Default;bool Saved=false;};
// Old keys remain immutable historical values. A native-unit value always wins,
// even without our version marker (an upstream install may have written it).
template<class Get,class Set> Values Load(Get get,Set set)
{
    Values v;float legacy=0,native=0,preference=0;
    const bool old=get("SunShadowSize",legacy);
    const bool modern=get("SunSoftness",native);
    v.Saved=old||modern;
    v.Active=modern?Clean(native):(old?Clean(legacy*LegacyFactor):0);
    const bool nativePreference=get("TextureQuestSunSoftnessPreferenceRealSun",preference);
    if(nativePreference)v.Preference=Clean(preference);
    else if(get("TextureQuestSunSoftnessPreference",preference))v.Preference=Clean(preference*LegacyFactor);
    else v.Preference=v.Active>0?v.Active:Default;
    if(v.Preference<=0)v.Preference=Default;
    int version=0;get("TextureQuestSunSoftnessUnits",version);
    // Partial preference saves are repaired independently; the marker alone is
    // never evidence that a missing native-unit value was already published.
    if(!modern)set("SunSoftness",v.Active);
    if(!nativePreference)set("TextureQuestSunSoftnessPreferenceRealSun",v.Preference);
    if(version<1)set("TextureQuestSunSoftnessUnits",1);
    return v;
}
template<class Set> void Store(float active,float preference,Set set)
{
    set("SunSoftness",Clean(active));
    set("TextureQuestSunSoftnessPreferenceRealSun",Clean(preference));
    set("TextureQuestSunSoftnessUnits",1);
}
}
