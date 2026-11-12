// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstdint>
#include <stdexcept>
#include <string>
namespace D3D12 {
inline void RejectUnverifiedDrawIndexWrite(bool enabled, std::uint16_t draw_index_loc) {
    if (!enabled)
        return;
    throw std::runtime_error(
        "D3D12 indirect draw-index write is not implemented for DRAW_INDEX_LOC=" +
        std::to_string(draw_index_loc) + "; the per-draw guest SGPR mapping is not verified");
}
} // namespace D3D12
