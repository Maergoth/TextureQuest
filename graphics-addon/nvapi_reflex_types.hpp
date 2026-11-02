#pragma once
// Narrow attributed subset of NVIDIA NVAPI SDK61683, commit70d337db9186e968eab622f7e786de7e437faf3d.
// SPDX-FileCopyrightText: Copyright (c) 2019-2026 NVIDIA CORPORATION & AFFILIATES.
// SPDX-License-Identifier: MIT
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies
// of the Software, and to permit persons to whom the Software is furnished to
// do so, subject to the following conditions: The above copyright notice and
// this permission notice shall be included in all copies or substantial portions
// of the Software. THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY
// KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
// MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO
// EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES
// OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
// ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
// DEALINGS IN THE SOFTWARE.
#include <cstdint>
#include <cstddef>
namespace tq::reflex {
using NvStatus = int32_t;
inline constexpr NvStatus Ok = 0, NotSupported = -104, LibraryMissing = -2, Error = -1;
inline constexpr uint32_t InitializeId = 0x0150e828, UnloadId = 0xd22bdd7e;
inline constexpr uint32_t GetSleepStatusId = 0xaef96ca1, SetSleepModeId = 0xac1ca9e0;
#pragma pack(push,8)
struct SleepMode {
 uint32_t version; uint8_t bLowLatencyMode, bLowLatencyBoost;
 uint32_t minimumIntervalUs;
 uint8_t bUseMarkersToOptimize, bUseMinQueueTime, rsvd[30];
};
struct SleepStatus {
 uint32_t version; uint8_t bLowLatencyMode, bFsVrr, bCplVsyncOn;
 uint32_t sleepIntervalUs;
 uint8_t bUseGameSleep, bFullscreenIFlip, fgMultiplier, bDfgControl;
 uint32_t dfgFrameTimeTargetUs; uint8_t rsvd[114];
};
#pragma pack(pop)
static_assert(sizeof(SleepMode)==44 && sizeof(SleepStatus)==136);
static_assert(offsetof(SleepMode,minimumIntervalUs)==8 && offsetof(SleepMode,bUseMarkersToOptimize)==12);
static_assert(offsetof(SleepStatus,sleepIntervalUs)==8 && offsetof(SleepStatus,bUseGameSleep)==12 && offsetof(SleepStatus,dfgFrameTimeTargetUs)==16);
inline constexpr uint32_t SleepModeVersion = uint32_t(sizeof(SleepMode)) | (1u<<16);
inline constexpr uint32_t SleepStatusVersion = uint32_t(sizeof(SleepStatus)) | (1u<<16);
inline SleepMode Request(bool enabled) {
 SleepMode result{}; result.version=SleepModeVersion; result.bLowLatencyMode=enabled?1:0; return result;
}
inline SleepStatus EmptyStatus() { SleepStatus result{};result.version=SleepStatusVersion;return result; }
}
