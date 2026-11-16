// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

namespace D3D12::DepthGate {

// F13 materializes depth-fail and depth-pass results as distinct PS-bearing draws.
// It cannot capture one guest PS result and consume it in both outcomes. Never
// infer single-invocation semantics from early depth testing or from a cached PSO.
struct ReplayFeatures {
    bool exports_depth{};
    bool writes_guest_memory{};
    bool depth_stencil_feedback{};
    bool has_discard{};
    bool writes_sample_mask{};
    bool writes_stencil_ref{};
    bool multisampled{};
    bool alpha_to_coverage{};
    bool sample_shading{};
    bool invocation_sensitive{};
};

[[nodiscard]] constexpr bool CanReplayFragment(ReplayFeatures features) noexcept {
    return !(features.exports_depth || features.writes_guest_memory ||
             features.depth_stencil_feedback || features.has_discard ||
             features.writes_sample_mask || features.writes_stencil_ref || features.multisampled ||
             features.alpha_to_coverage || features.sample_shading ||
             features.invocation_sensitive);
}

} // namespace D3D12::DepthGate
