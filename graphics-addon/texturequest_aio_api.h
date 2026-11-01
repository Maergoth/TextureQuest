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
