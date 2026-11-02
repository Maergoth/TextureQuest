#pragma once
#include <cstdint>

// NGX DLSS-G's count is the number of generated images, not total presents.
// Production policy and the CPU contract test share these exact helpers.
namespace tq_mfg {
constexpr std::uint32_t MaxGenerated = 3;
constexpr std::uint32_t MaxPresented = MaxGenerated + 1;
// Optional diagnostic ABI, separate from the established control/status ABI2.
// Values are individually atomic reads; query after the owner has drained for
// an exact stable tuple. Present counters mean DXGI accepted, not scanout.
struct CountersV2 {
    std::uint32_t Abi;
    std::uint32_t Bytes;
    std::uint64_t EvaluateOk;
    std::uint64_t GeneratedPresents;
    std::uint64_t RealPresents;
    std::uint64_t CompleteGeneratedGroups;
};
static_assert(sizeof(CountersV2) == 40);
struct Plan {
    std::uint32_t selected;
    std::uint32_t generated;
    bool unsupported;
    constexpr std::uint32_t Presented() const noexcept { return generated + 1; }
};
constexpr Plan Select(bool enabled, std::uint32_t selected,
    bool capability_known, std::uint32_t maximum_generated) noexcept
{
    if (selected < 2 || selected > MaxPresented) return {selected, 0, true};
    if (!enabled) return {selected, 0, false};
    const auto count = selected - 1;
    // A missing max key predates MFG: ordinary 2x remains admissible, but
    // multi-frame operation requires an affirmative device capability.
    if (count > 1 && (!capability_known || maximum_generated < count))
        return {selected, 0, true};
    return {selected, count, false};
}
constexpr double Fraction(std::uint32_t index, std::uint32_t count) noexcept
{
    return count >= 1 && count <= MaxGenerated && index >= 1 && index <= count ?
        static_cast<double>(index) / (count + 1) : 0.0;
}
template<class Pointer>
bool DistinctOutputs(Pointer const *outputs, std::uint32_t count) noexcept
{
    if (!outputs || count < 1 || count > MaxGenerated) return false;
    for (std::uint32_t i = 0; i < count; ++i) {
        if (!outputs[i]) return false;
        for (std::uint32_t j = 0; j < i; ++j)
            if (outputs[i] == outputs[j]) return false;
    }
    return true;
}
// Calls one genuine Evaluate per generated image. A failed sequence stops;
// its caller must discard the entire incomplete output group and reset NGX.
template<class Evaluate>
std::uint32_t EvaluateSequence(std::uint32_t count, bool reset, Evaluate evaluate)
{
    if (count < 1 || count > MaxGenerated) return 0;
    std::uint32_t completed = 0;
    for (std::uint32_t index = 1; index <= count; ++index) {
        if (!evaluate(count, index, reset && index == 1)) break;
        ++completed;
    }
    return completed;
}
constexpr bool IsGeneratedPresent(std::uint32_t index,
    std::uint32_t generated_count) noexcept
{
    return generated_count <= MaxGenerated && index < generated_count;
}
}
