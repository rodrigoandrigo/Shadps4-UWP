// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "shader_recompiler/frontend/translate/sdwa_selector.h"

namespace Shader::Gcn::SdwaVop1 {

// Exact VOP1 handlers with one VGPR source and a dword or halfword VGPR result.
// intentionally an opcode list, not a VOP1-family or enum-range test.
template <typename OpcodeType>
constexpr bool IsSupportedOpcode(OpcodeType opcode) noexcept {
    switch (opcode) {
    case OpcodeType::V_MOV_B32:
    case OpcodeType::V_CVT_F32_I32:
    case OpcodeType::V_CVT_F32_U32:
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
    case OpcodeType::V_RCP_IFLAG_F32:
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
    case OpcodeType::V_CVT_F32_F16:
    case OpcodeType::V_CVT_F16_F32:
    case OpcodeType::V_CVT_F32_UBYTE0:
    case OpcodeType::V_CVT_F32_UBYTE1:
    case OpcodeType::V_CVT_F32_UBYTE2:
    case OpcodeType::V_CVT_F32_UBYTE3:
        return true;
    default:
        return false;
    }
}

// SDWA ABS/NEG describe floating-point modifiers. Only handlers that actually
// consume src0 as FP32 may use them; integer and raw-bit handlers use SEXT only.
template <typename OpcodeType>
constexpr bool SupportsFloatInputModifiers(OpcodeType opcode) noexcept {
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
    case OpcodeType::V_RCP_IFLAG_F32:
    case OpcodeType::V_RSQ_CLAMP_F32:
    case OpcodeType::V_RSQ_LEGACY_F32:
    case OpcodeType::V_RSQ_F32:
    case OpcodeType::V_SQRT_F32:
    case OpcodeType::V_SIN_F32:
    case OpcodeType::V_COS_F32:
    case OpcodeType::V_FREXP_EXP_I32_F32:
    case OpcodeType::V_FREXP_MANT_F32:
    case OpcodeType::V_CVT_F16_F32:
        return true;
    default:
        return false;
    }
}

} // namespace Shader::Gcn::SdwaVop1
