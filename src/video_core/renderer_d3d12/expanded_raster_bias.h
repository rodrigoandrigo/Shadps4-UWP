// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

namespace D3D12::RasterizerLogic {

[[nodiscard]] constexpr bool RequiresOriginalPrimitiveDepthBias(
    bool depth_enabled, bool primitive_expanded, int depth_bias,
    float slope_scaled_depth_bias) noexcept {
    return depth_enabled && primitive_expanded &&
           (depth_bias != 0 || slope_scaled_depth_bias != 0.0f);
}

} // namespace D3D12::RasterizerLogic
