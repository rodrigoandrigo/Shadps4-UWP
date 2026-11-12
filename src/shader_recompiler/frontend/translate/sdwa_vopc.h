// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

namespace Shader::Gcn::SdwaVopcPolicy {

template <typename OpcodeType>
constexpr bool IsFloatCompare(OpcodeType opcode) noexcept {
    switch (opcode) {
    case OpcodeType::V_CMP_F_F32:
    case OpcodeType::V_CMP_LT_F32:
    case OpcodeType::V_CMP_EQ_F32:
    case OpcodeType::V_CMP_LE_F32:
    case OpcodeType::V_CMP_GT_F32:
    case OpcodeType::V_CMP_LG_F32:
    case OpcodeType::V_CMP_GE_F32:
    case OpcodeType::V_CMP_U_F32:
    case OpcodeType::V_CMP_NGE_F32:
    case OpcodeType::V_CMP_NGT_F32:
    case OpcodeType::V_CMP_NLE_F32:
    case OpcodeType::V_CMP_NEQ_F32:
    case OpcodeType::V_CMP_NLT_F32:
    case OpcodeType::V_CMPX_F_F32:
    case OpcodeType::V_CMPX_LT_F32:
    case OpcodeType::V_CMPX_EQ_F32:
    case OpcodeType::V_CMPX_LE_F32:
    case OpcodeType::V_CMPX_GT_F32:
    case OpcodeType::V_CMPX_LG_F32:
    case OpcodeType::V_CMPX_GE_F32:
    case OpcodeType::V_CMPX_NGE_F32:
    case OpcodeType::V_CMPX_NLG_F32:
    case OpcodeType::V_CMPX_NGT_F32:
    case OpcodeType::V_CMPX_NLE_F32:
    case OpcodeType::V_CMPX_NEQ_F32:
    case OpcodeType::V_CMPX_NLT_F32:
        return true;
    default:
        return false;
    }
}

template <typename OpcodeType>
constexpr bool IsIntegerCompare(OpcodeType opcode) noexcept {
    switch (opcode) {
    case OpcodeType::V_CMP_LT_I32:
    case OpcodeType::V_CMP_EQ_I32:
    case OpcodeType::V_CMP_LE_I32:
    case OpcodeType::V_CMP_GT_I32:
    case OpcodeType::V_CMP_NE_I32:
    case OpcodeType::V_CMP_GE_I32:
    case OpcodeType::V_CMPX_LT_I32:
    case OpcodeType::V_CMPX_EQ_I32:
    case OpcodeType::V_CMPX_LE_I32:
    case OpcodeType::V_CMPX_GT_I32:
    case OpcodeType::V_CMPX_LG_I32:
    case OpcodeType::V_CMPX_GE_I32:
    case OpcodeType::V_CMP_F_U32:
    case OpcodeType::V_CMP_LT_U32:
    case OpcodeType::V_CMP_EQ_U32:
    case OpcodeType::V_CMP_LE_U32:
    case OpcodeType::V_CMP_GT_U32:
    case OpcodeType::V_CMP_NE_U32:
    case OpcodeType::V_CMP_GE_U32:
    case OpcodeType::V_CMP_TRU_U32:
    case OpcodeType::V_CMPX_F_U32:
    case OpcodeType::V_CMPX_LT_U32:
    case OpcodeType::V_CMPX_EQ_U32:
    case OpcodeType::V_CMPX_LE_U32:
    case OpcodeType::V_CMPX_GT_U32:
    case OpcodeType::V_CMPX_NE_U32:
    case OpcodeType::V_CMPX_GE_U32:
    case OpcodeType::V_CMPX_TRU_U32:
        return true;
    default:
        return false;
    }
}

template <typename OpcodeType>
constexpr bool IsClassCompare(OpcodeType opcode) noexcept {
    return opcode == OpcodeType::V_CMP_CLASS_F32;
}

template <typename OpcodeType>
constexpr bool IsSupportedOpcode(OpcodeType opcode) noexcept {
    return IsFloatCompare(opcode) || IsIntegerCompare(opcode) || IsClassCompare(opcode);
}

} // namespace Shader::Gcn::SdwaVopcPolicy
