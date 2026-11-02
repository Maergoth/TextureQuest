#pragma once
namespace offscreen_lamp_policy
{
// A screen pass without a mask is safe only if no moving replay needs exclusion.
constexpr bool CanRelight(bool movingReplay,bool currentMark)
{return !movingReplay||currentMark;}
}
