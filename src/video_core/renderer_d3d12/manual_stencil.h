// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <algorithm>
#include <cstdint>

namespace D3D12::ManualStencil {
using OpCode = std::uint32_t;
using Byte = std::uint8_t;

// The native D3D12 stencil operations do not express all guest operations or
// independent test/op references. Keep this decision pure so the runtime gate
// and the host contract test exercise the same classification. Guest enum values
// are checked against this code table by manual_stencil_structure.py.
constexpr bool NeedsLowering(OpCode op, Byte test_ref, Byte op_ref) noexcept {
    switch (op) {
    case 0: // Keep
    case 1: // Zero
    case 3: // ReplaceTest
    case 7: // Invert
        return false;
    case 2:  // Ones
    case 10: // And
    case 11: // Or
    case 12: // Xor
    case 13: // Nand
    case 14: // Nor
    case 15: // Xnor
        return true;
    case 4: // ReplaceOp
        return test_ref != op_ref;
    case 5: // AddClamp
    case 6: // SubClamp
    case 8: // AddWrap
    case 9: // SubWrap
        return op_ref != 1;
    default:
        return true;
    }
}

// Guest CompareFunc::Never is 0; StencilFunc::ReplaceOp is 4 and Keep is 0.
constexpr bool CanUseNativeNeverFailReplace(OpCode compare, OpCode fail, OpCode zfail, OpCode zpass,
                                            Byte test_ref, Byte op_ref, Byte write_mask,
                                            bool backface_state_matches, bool stencil_ref_export,
                                            bool direct_single_draw) noexcept {
    return compare == 0 && fail == 4 && zfail == 0 && zpass == 0 && test_ref != op_ref &&
           write_mask != 0 && backface_state_matches && !stencil_ref_export && direct_single_draw;
}

// CPU specification of the byte operation used by the D3D12 CAS helper. A
// false result means the input code is outside the guest's four-bit operation
// range; callers must not silently choose an operation for it.
constexpr bool TryApply(OpCode op, Byte old, Byte test_ref, Byte op_ref, Byte& result) noexcept {
    switch (op) {
    case 0: // Keep
        result = old;
        return true;
    case 1: // Zero
        result = 0;
        return true;
    case 2: // Ones
        result = 0xff;
        return true;
    case 3: // ReplaceTest
        result = test_ref;
        return true;
    case 4: // ReplaceOp
        result = op_ref;
        return true;
    case 5: // AddClamp
        result = static_cast<Byte>(std::min<std::uint32_t>(std::uint32_t(old) + op_ref, 0xff));
        return true;
    case 6: // SubClamp
        result = static_cast<Byte>(old > op_ref ? old - op_ref : 0);
        return true;
    case 7: // Invert
        result = static_cast<Byte>(~old);
        return true;
    case 8: // AddWrap
        result = static_cast<Byte>(std::uint32_t(old) + op_ref);
        return true;
    case 9: // SubWrap
        result = static_cast<Byte>(std::uint32_t(old) - op_ref);
        return true;
    case 10: // And
        result = static_cast<Byte>(old & op_ref);
        return true;
    case 11: // Or
        result = static_cast<Byte>(old | op_ref);
        return true;
    case 12: // Xor
        result = static_cast<Byte>(old ^ op_ref);
        return true;
    case 13: // Nand
        result = static_cast<Byte>(~(old & op_ref));
        return true;
    case 14: // Nor
        result = static_cast<Byte>(~(old | op_ref));
        return true;
    case 15: // Xnor
        result = static_cast<Byte>(~(old ^ op_ref));
        return true;
    default:
        return false;
    }
}

constexpr Byte MergeWriteMask(Byte old, Byte updated, Byte write_mask) noexcept {
    return static_cast<Byte>((old & static_cast<Byte>(~write_mask)) | (updated & write_mask));
}

constexpr bool SupportsOrderedPrimitiveReplay(bool has_guest_geometry_shader, bool has_tessellation,
                                              bool rectangles, bool expanded) noexcept {
    // The injected capture GS emits one complete record per IA primitive in submitted order.
    // Guest GS output decomposition and tessellator ordering are not proven by this replay path.
    return !has_guest_geometry_shader && !has_tessellation && !rectangles && !expanded;
}

constexpr bool SupportsSingleSampleRasterReplay(std::uint32_t samples, bool alpha_to_coverage,
                                                bool expanded, bool sample_shading = false,
                                                bool sample_semantics = false) noexcept {
    return samples == 1 && !alpha_to_coverage && !expanded && !sample_shading && !sample_semantics;
}

struct FragmentReplayFeatures {
    bool has_discard{};
    bool writes_guest_memory{};
    bool writes_depth{};
    bool writes_sample_mask{};
    bool writes_stencil_ref{};
    bool depth_stencil_feedback{};
    bool invocation_sensitive{};
};

constexpr bool SupportsFragmentReplay(FragmentReplayFeatures features) noexcept {
    return !(features.has_discard || features.writes_guest_memory || features.writes_depth ||
             features.writes_sample_mask || features.writes_stencil_ref ||
             features.depth_stencil_feedback || features.invocation_sensitive);
}

constexpr bool SupportsSingleLayerAttachment(bool has_stencil, std::uint32_t native_layers,
                                             std::uint32_t mip_levels) noexcept {
    return has_stencil && native_layers == 1 && mip_levels == 1;
}

} // namespace D3D12::ManualStencil
