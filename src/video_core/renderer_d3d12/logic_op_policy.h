// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdint>
#include <span>

namespace D3D12::LogicOpPolicy {

// The sixteen repeated-byte truth tables map exactly to D3D12_LOGIC_OP. The
// guest's BrushXor (0x5a) is not one of them and must not be approximated as XOR.
[[nodiscard]] constexpr bool HasExactRop3(std::uint32_t rop3) noexcept {
    switch (rop3) {
    case 0x00:
    case 0x11:
    case 0x22:
    case 0x33:
    case 0x44:
    case 0x55:
    case 0x66:
    case 0x77:
    case 0x88:
    case 0x99:
    case 0xaa:
    case 0xbb:
    case 0xcc:
    case 0xdd:
    case 0xee:
    case 0xff:
        return true;
    default:
        return false;
    }
}

[[nodiscard]] constexpr bool CanUseNativeLogicOps(bool output_merger_logic_op,
                                                  bool uint4_render_target) noexcept {
    return output_merger_logic_op && uint4_render_target;
}

// A target with no guest color-write channels cannot expose a per-target ROP
// difference. Callers must pass the raw guest mask, not an internal suppression.
[[nodiscard]] constexpr bool HasVisiblePerTargetRop3Conflict(
    bool logic_op, bool disable_rop3, std::uint32_t guest_write_mask) noexcept {
    return logic_op && disable_rop3 && (guest_write_mask & 0x0fu) != 0;
}

struct PackedMaskTarget {
    bool active{};
    // Bit c is set if the guest attachment format has component c.
    std::uint8_t format_components{};
    // Guest channel mask used to pack the PS output before the native logic op.
    std::uint8_t source_mask{};
    // Outcome-specific guest mask that F13 must enforce in the shared PSO.
    std::uint8_t outcome_mask{};
};

[[nodiscard]] constexpr std::uint8_t PackedWriteMask(PackedMaskTarget target) noexcept {
    std::uint8_t packed_component{};
    std::uint8_t packed_mask{};
    for (std::uint8_t component = 0; component < 4; ++component) {
        const std::uint8_t bit = std::uint8_t(1u << component);
        if (!(target.format_components & bit) || !(target.source_mask & bit))
            continue;
        if (target.outcome_mask & bit)
            packed_mask |= std::uint8_t(1u << packed_component);
        ++packed_component;
    }
    return packed_mask;
}

// D3D12 requires IndependentBlendEnable=FALSE for logic ops, so F13 can express
// per-attachment color outcomes only when each target projects to one shared
// packed write mask. This models exactly the channel-to-uint4 mapping used by
// ShaderCache::CompileLogicOp; it does not emulate a differing mask in a shader.
[[nodiscard]] constexpr bool SharedPackedWriteMask(std::span<const PackedMaskTarget> targets,
                                                   std::uint8_t& result) noexcept {
    bool found{};
    for (const auto target : targets) {
        if (!target.active)
            continue;
        const auto mask = PackedWriteMask(target);
        if (found && mask != result)
            return false;
        result = mask;
        found = true;
    }
    return found;
}

} // namespace D3D12::LogicOpPolicy
