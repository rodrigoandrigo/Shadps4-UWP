// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

namespace D3D12::RasterizerLogic {

[[nodiscard]] constexpr bool RequiresTessellationProvokingOrderGuard(
    bool guest_requests_last, bool has_tessellation, bool has_geometry_shader,
    bool tessellation_emits_points) noexcept {
    // With no guest GS, line and triangle primitives are ordered by the
    // implementation's tessellator. Vulkan exposes the requested provoking
    // mode, but the project has no cross-backend proof of that output order.
    // Point primitives contain one vertex, so first/last selection is identical.
    return guest_requests_last && has_tessellation && !has_geometry_shader &&
           !tessellation_emits_points;
}

} // namespace D3D12::RasterizerLogic
