#pragma once
// TextureQuest private AIO ABI1. Copied status only; no driver/resource pointers.
#include <stdint.h>
#define TEXTUREQUEST_AIO_ABI 1u
struct TextureQuestAIOStatusV1 {
    uint32_t Abi, Bytes, Flags, ActiveCpuJobs;
    uint32_t SourceWidth, SourceHeight, OutputWidth, OutputHeight;
    uint64_t MasterEpoch, SourceFrames, NrFrames, SrFrames, FgFrames, PresentedFrames;
};
static_assert(sizeof(TextureQuestAIOStatusV1) == 80, "TextureQuest AIO ABI1 layout");
enum TextureQuestAIOFlags : uint32_t {
    TQAIO_MasterRequested=1u, TQAIO_MasterAdmitted=2u, TQAIO_UserEnabled=4u,
    TQAIO_NeuralReady=8u, TQAIO_Failed=16u, TQAIO_NrFeature=32u,
    TQAIO_SrFeature=64u, TQAIO_FgFeature=128u, TQAIO_NvofReady=256u,
    TQAIO_UsingNvof=512u, TQAIO_OutputReady=1024u, TQAIO_Draining=2048u
};
using TextureQuestAIOSetMasterFn = int32_t (__cdecl *)(uint32_t abi, uint32_t enabled);
using TextureQuestAIOGetStatusFn = int32_t (__cdecl *)(uint32_t abi, void *output, uint32_t bytes);
// Exact exports return nonzero on accepted ABI/arguments, zero otherwise.
// SetMaster is atomic request only; managed build starts OFF. Calls never persist config.
// GetStatus returns a copied, most-recent-Present snapshot plus current master/CPU facts.
// Feature flags mean handles exist, never successful GPU/visual proof; counters are separate.

// ABI2 controls supplement ABI1 master/status exports; the latter stay available.
#define TEXTUREQUEST_AIO_CONTROLS_ABI 2u
enum TextureQuestAioEnabledMask : uint32_t { TQAIO_NR=1u,TQAIO_SR=2u,TQAIO_FG=4u };
enum TextureQuestAioSrMode : uint32_t {
 TQAIO_SR_Current=1, TQAIO_SR_Quality=2,
 TQAIO_SR_Balanced=3, TQAIO_SR_Performance=4, TQAIO_SR_UltraPerformance=5
};
struct TextureQuestAIOControlsV2 {
 uint32_t Abi, Bytes, EnabledMask, SrMode, FrameMultiplier, NrModel, NrPasses, Reserved;
};
static_assert(sizeof(TextureQuestAIOControlsV2)==32,"AIO ABI2 controls32");
struct TextureQuestAIOControlStatusV2 {
 uint32_t Abi, Bytes, Flags, Reserved;
 uint64_t RequestedGeneration, AppliedGeneration;
 TextureQuestAIOControlsV2 Requested, Applied;
};
static_assert(sizeof(TextureQuestAIOControlStatusV2)==96,"AIO ABI2 control status96");
enum TextureQuestAIOControlStatusFlags : uint32_t {
 TQAIO_ControlsLoaded=1u, TQAIO_ControlsPending=2u,
 TQAIO_FGCapabilitiesKnown=4u, TQAIO_Supported4x=8u, TQAIO_Unsupported4x=16u,
 TQAIO_Effective4x=32u, TQAIO_Effective2x=64u,
 TQAIO_Supported3x=128u, TQAIO_Unsupported3x=256u, TQAIO_Effective3x=512u
};
using TextureQuestAIOSetControlsFn = int32_t (__cdecl *)(uint32_t abi,const void *controls,uint32_t bytes);
using TextureQuestAIOGetControlsFn = int32_t (__cdecl *)(uint32_t abi,void *output,uint32_t bytes);
// Exports: TextureQuestAIOSetControls / TextureQuestAIOGetControls. ABI exactly2.
// Set receives32bytes, validates EnabledMask0..7 (NR1/SR2/FG4), multiplier2..4,
// SRmode1..5, model/pass1..3, reserved0. Off changes mask only, retaining presets.
// Atomic request only; current master remains authoritative. Owner Present drains
// admitted jobs, applies and persists via normal ReShade config, then readmits.
// Get returns96bytes with copied atomic requested/applied controls and generations.
// Since TextureQuest 1.13.15 SR is retired: its bit and mode enum remain in ABI2
// for compatibility, but the managed provider clears MaskSR and uses native
// capture regardless of older saved settings or control requests. No NGX SR call.
// MaskFG off bypasses FG;selected2/3/4 request one/two/three intermediates. Actual
// runtime support/evaluations/status must prove4x; request alone is not evidence.
// NR model1..3/pass1..3 retain upstream style/pass contracts.
