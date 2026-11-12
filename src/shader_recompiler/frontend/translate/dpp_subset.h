// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <type_traits>

namespace Shader::Gcn {

// This is an intentionally explicit subset of ordinary 32-bit VOP2 instructions.
// DPP itself is an encoding modifier, not a general-purpose modifier for every GCN opcode.
template <typename OpcodeType>
constexpr bool IsSupportedDppVop2Opcode(OpcodeType opcode) noexcept {
    switch (opcode) {
    case OpcodeType::V_CNDMASK_B32:
    case OpcodeType::V_ADD_F32:
    case OpcodeType::V_SUB_F32:
    case OpcodeType::V_SUBREV_F32:
    case OpcodeType::V_MAC_LEGACY_F32:
    case OpcodeType::V_MUL_LEGACY_F32:
    case OpcodeType::V_MUL_F32:
    case OpcodeType::V_MUL_I32_I24:
    case OpcodeType::V_MUL_U32_U24:
    case OpcodeType::V_MIN_LEGACY_F32:
    case OpcodeType::V_MAX_LEGACY_F32:
    case OpcodeType::V_MIN_F32:
    case OpcodeType::V_MAX_F32:
    case OpcodeType::V_MIN_I32:
    case OpcodeType::V_MAX_I32:
    case OpcodeType::V_MIN_U32:
    case OpcodeType::V_MAX_U32:
    case OpcodeType::V_LSHR_B32:
    case OpcodeType::V_LSHRREV_B32:
    case OpcodeType::V_ASHR_I32:
    case OpcodeType::V_ASHRREV_I32:
    case OpcodeType::V_LSHL_B32:
    case OpcodeType::V_LSHLREV_B32:
    case OpcodeType::V_AND_B32:
    case OpcodeType::V_OR_B32:
    case OpcodeType::V_XOR_B32:
    case OpcodeType::V_BFM_B32:
    case OpcodeType::V_MAC_F32:
    case OpcodeType::V_BCNT_U32_B32:
    case OpcodeType::V_LDEXP_F32:
    case OpcodeType::V_CVT_PKNORM_I16_F32:
    case OpcodeType::V_CVT_PKNORM_U16_F32:
    case OpcodeType::V_CVT_PKRTZ_F16_F32:
        return true;
    default:
        return false;
    }
}

// Narrow whitelist of implemented VOP1 instructions whose source is one VGPR
// dword and whose result is one VGPR dword. Special-state, lane-indexed,
// packed-result, and 64-bit operations intentionally remain unsupported.
template <typename OpcodeType>
constexpr bool IsSupportedDppVop1Opcode(OpcodeType opcode) noexcept {
    switch (opcode) {
    case OpcodeType::V_MOV_B32:
    case OpcodeType::V_CVT_F32_I32:
    case OpcodeType::V_CVT_F32_U32:
    case OpcodeType::V_CVT_U32_F32:
    case OpcodeType::V_CVT_I32_F32:
    case OpcodeType::V_CVT_F32_F16:
    case OpcodeType::V_CVT_RPI_I32_F32:
    case OpcodeType::V_CVT_FLR_I32_F32:
    case OpcodeType::V_CVT_F32_UBYTE0:
    case OpcodeType::V_CVT_F32_UBYTE1:
    case OpcodeType::V_CVT_F32_UBYTE2:
    case OpcodeType::V_CVT_F32_UBYTE3:
    case OpcodeType::V_FRACT_F32:
    case OpcodeType::V_TRUNC_F32:
    case OpcodeType::V_CEIL_F32:
    case OpcodeType::V_RNDNE_F32:
    case OpcodeType::V_FLOOR_F32:
    case OpcodeType::V_EXP_F32:
    case OpcodeType::V_LOG_CLAMP_F32:
    case OpcodeType::V_LOG_F32:
    case OpcodeType::V_RCP_CLAMP_F32:
    case OpcodeType::V_RCP_LEGACY_F32:
    case OpcodeType::V_RCP_F32:
    case OpcodeType::V_RSQ_CLAMP_F32:
    case OpcodeType::V_RSQ_LEGACY_F32:
    case OpcodeType::V_RSQ_F32:
    case OpcodeType::V_SQRT_F32:
    case OpcodeType::V_SIN_F32:
    case OpcodeType::V_COS_F32:
    case OpcodeType::V_NOT_B32:
    case OpcodeType::V_BFREV_B32:
    case OpcodeType::V_FFBH_U32:
    case OpcodeType::V_FFBL_B32:
    case OpcodeType::V_FFBH_I32:
    case OpcodeType::V_FREXP_EXP_I32_F32:
    case OpcodeType::V_FREXP_MANT_F32:
    case OpcodeType::V_LOG_LEGACY_F32:
    case OpcodeType::V_EXP_LEGACY_F32:
        return true;
    default:
        return false;
    }
}

// ABS/NEG on DPP operands are currently lowered only for operations whose
// source interpretation is explicitly floating point. Integer/bitwise operations
// remain available with unmodified operands; other modifier combinations fail closed.
template <typename OpcodeType>
constexpr bool SupportsDppFloatInputModifiers(OpcodeType opcode) noexcept {
    switch (opcode) {
    case OpcodeType::V_ADD_F32:
    case OpcodeType::V_SUB_F32:
    case OpcodeType::V_SUBREV_F32:
    case OpcodeType::V_MAC_LEGACY_F32:
    case OpcodeType::V_MUL_LEGACY_F32:
    case OpcodeType::V_MUL_F32:
    case OpcodeType::V_MIN_LEGACY_F32:
    case OpcodeType::V_MAX_LEGACY_F32:
    case OpcodeType::V_MIN_F32:
    case OpcodeType::V_MAX_F32:
    case OpcodeType::V_MAC_F32:
    case OpcodeType::V_LDEXP_F32:
    case OpcodeType::V_CVT_PKNORM_I16_F32:
    case OpcodeType::V_CVT_PKNORM_U16_F32:
    case OpcodeType::V_CVT_PKRTZ_F16_F32:
        return true;
    default:
        return false;
    }
}

// VOP1 has only src0. This allowlist additionally excludes operations whose
// source is integer bits, packed FP16, or otherwise not lowered as an FP32 value.
template <typename OpcodeType>
constexpr bool SupportsDppVop1FloatInputModifiers(OpcodeType opcode) noexcept {
    switch (opcode) {
    case OpcodeType::V_CVT_U32_F32:
    case OpcodeType::V_CVT_I32_F32:
    case OpcodeType::V_CVT_RPI_I32_F32:
    case OpcodeType::V_CVT_FLR_I32_F32:
    case OpcodeType::V_FRACT_F32:
    case OpcodeType::V_TRUNC_F32:
    case OpcodeType::V_CEIL_F32:
    case OpcodeType::V_RNDNE_F32:
    case OpcodeType::V_FLOOR_F32:
    case OpcodeType::V_EXP_F32:
    case OpcodeType::V_LOG_CLAMP_F32:
    case OpcodeType::V_LOG_F32:
    case OpcodeType::V_RCP_CLAMP_F32:
    case OpcodeType::V_RCP_LEGACY_F32:
    case OpcodeType::V_RCP_F32:
    case OpcodeType::V_RSQ_CLAMP_F32:
    case OpcodeType::V_RSQ_LEGACY_F32:
    case OpcodeType::V_RSQ_F32:
    case OpcodeType::V_SQRT_F32:
    case OpcodeType::V_SIN_F32:
    case OpcodeType::V_COS_F32:
    case OpcodeType::V_FREXP_EXP_I32_F32:
    case OpcodeType::V_FREXP_MANT_F32:
    case OpcodeType::V_LOG_LEGACY_F32:
    case OpcodeType::V_EXP_LEGACY_F32:
        return true;
    default:
        return false;
    }
}

template <typename Operand>
constexpr bool HasDppInputModifier(const Operand& operand) noexcept {
    return operand.input_modifier.abs || operand.input_modifier.neg ||
           operand.input_modifier.neg_hi || operand.input_modifier.sext;
}

template <typename Dpp>
constexpr bool IsSupportedDppControl(const Dpp& dpp) noexcept {
    using Control = std::remove_cvref_t<decltype(dpp.op)>;
    const bool row_shift = dpp.op == Control::DppRowShl || dpp.op == Control::DppRowShr;
    const bool row_rotate = dpp.op == Control::DppRowRor && dpp.value >= 1 && dpp.value <= 15;
    const bool row_mirror = dpp.op == Control::DppRowMirror || dpp.op == Control::DppRowHalfMirror;
    const bool row_broadcast = dpp.op == Control::DppRowBcast15 || dpp.op == Control::DppRowBcast31;
    const bool wave_shift = dpp.op == Control::DppWaveShl || dpp.op == Control::DppWaveShr;
    const bool wave_rotate = dpp.op == Control::DppWaveRol || dpp.op == Control::DppWaveRor;
    const bool supported_operation =
        dpp.op == Control::DppQuadPerm ||
        ((row_shift || row_rotate) && dpp.value >= 1 && dpp.value <= 15) || row_mirror ||
        row_broadcast || wave_shift || wave_rotate;
    return supported_operation && dpp.row_mask <= 0xF && dpp.bank_mask <= 0xF && !dpp.fi;
}

} // namespace Shader::Gcn
