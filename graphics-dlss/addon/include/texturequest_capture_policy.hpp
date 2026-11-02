#pragma once
namespace tq_capture_policy {
// Selection and transport are separate from actual FG feature evaluation.
// A failed/unchecked FG component cannot push healthy NR/SR back to the old
// source-Present-driven NVOF queue. Other unsupported guide/transport paths
// retain their existing fallback unchanged.
constexpr bool UseCompletionMailbox(bool d3d11, bool direct_output,
    bool nr_selected, bool sr_selected, bool fg_effective, bool fg_failed,
    bool vort_guides, bool flow_depth, bool shared_capture_fences) noexcept {
    return d3d11 && direct_output && shared_capture_fences &&
        !vort_guides && !flow_depth &&
        (nr_selected || sr_selected || (fg_effective && !fg_failed));
}
}
