#pragma once
#include <windows.h>
#include <cstdint>
namespace tq_depth_v1 {
inline constexpr uint32_t NoManualDepth = 0;
inline constexpr uint32_t ManualDepth = 1;
inline constexpr uint32_t Tracking = 1, Ready = 2, Warming = 4;
inline constexpr uint32_t TechniqueDemand = 1, ManualDemand = 2, UnknownParticipant = 4, Loading = 8;
struct Status {
    uint32_t Size = 32, Version = 1, Flags = 0, Reasons = 0;
    uint64_t DeviceEpoch = 0, RuntimeReadyEpoch = 0;
};
static_assert(sizeof(Status) == 32);
using Register = uint32_t(WINAPI *)(void *module, uint32_t mode);
using Unregister = uint32_t(WINAPI *)(void *module);
using SetRequired = uint32_t(WINAPI *)(void *module, void *runtime, uint32_t required);
using Query = uint32_t(WINAPI *)(void *module, void *runtime, Status *status, uint32_t bytes);
inline constexpr char RegisterExport[] = "TextureQuestRegisterDepthParticipantV1";
inline constexpr char UnregisterExport[] = "TextureQuestUnregisterDepthParticipantV1";
inline constexpr char SetRequiredExport[] = "TextureQuestSetManualDepthDemandV1";
inline constexpr char QueryExport[] = "TextureQuestGetDepthStatusV1";
} // namespace tq_depth_v1
