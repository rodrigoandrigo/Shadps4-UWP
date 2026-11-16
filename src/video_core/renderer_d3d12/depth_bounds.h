// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

namespace D3D12::RasterizerLogic {

// D3D12 viewport depth and depth-bounds values are limited to [0, 1]. Keep the
// native path only when the guest range is ordered and needs no implementation clamp.
[[nodiscard]] constexpr bool IsDepthRangeWithinZeroToOne(float min_depth,
                                                         float max_depth) noexcept {
    return min_depth >= 0.0f && min_depth <= max_depth && max_depth <= 1.0f;
}

// Depth bounds is an output-merger test over stored depth. Its enable state is
// independent of whether the guest also enables depth comparison or stencil.
[[nodiscard]] constexpr bool ShouldEnableDepthBoundsTest(bool depth_attachment_valid,
                                                         bool guest_enabled) noexcept {
    return depth_attachment_valid && guest_enabled;
}

} // namespace D3D12::RasterizerLogic
