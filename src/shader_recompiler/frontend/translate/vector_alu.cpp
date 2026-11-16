// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <array>
#include <limits>
#include <stdexcept>
#include "shader_recompiler/frontend/opcodes.h"
#include "shader_recompiler/frontend/translate/translate.h"
#include "shader_recompiler/ir/attribute.h"
#include "shader_recompiler/profile.h"

namespace Shader::Gcn {

namespace {

bool HasUnsupportedVop3InputModifier(const InstOperand& operand) {
    return operand.input_modifier.neg || operand.input_modifier.neg_hi ||
           operand.input_modifier.abs || operand.input_modifier.sext || operand.op_sel.op_sel ||
           operand.op_sel.op_sel_hi || operand.sdwa_present || operand.sdwa_reserved_bits ||
           operand.dpp.has_value();
}

bool HasUnsupportedVop3OutputModifier(const InstOperand& operand, bool allow_clamp) {
    return operand.op_sel.op_sel || operand.op_sel.op_sel_hi || operand.sdwa_present ||
           operand.sdwa_reserved_bits || operand.sdwa_sel != SdwaSelector::Invalid ||
           operand.dpp.has_value() || operand.output_modifier.multiplier != 0.0f ||
           (!allow_clamp && operand.output_modifier.clamp);
}

bool IsCarryMaskRegister(OperandField field) {
    return field == OperandField::ScalarGPR || field == OperandField::VccLo;
}

void ValidateVop3CoU32(const GcnInst& inst) {
    if (inst.encoding != InstEncoding::VOP3 || inst.src_count != 2 || inst.dst_count != 2 ||
        inst.dst[0].field != OperandField::VectorGPR || !IsCarryMaskRegister(inst.dst[1].field)) {
        throw std::runtime_error("VOP3B CO_U32 requires two sources, a VGPR result, and an "
                                 "SGPR-pair/VCC mask destination");
    }
    if (HasUnsupportedVop3InputModifier(inst.src[0]) ||
        HasUnsupportedVop3InputModifier(inst.src[1]) ||
        HasUnsupportedVop3OutputModifier(inst.dst[0], true)) {
        throw std::runtime_error(
            "VOP3B CO_U32 does not accept input modifiers or output modifiers other than CLMP");
    }
}

void ValidateVop3Mad64(const GcnInst& inst) {
    if (inst.encoding != InstEncoding::VOP3 || inst.src_count != 3 || inst.dst_count != 2 ||
        inst.dst[0].field != OperandField::VectorGPR || inst.dst[1].field != OperandField::VccLo) {
        throw std::runtime_error("GCN2 VOP3a MAD64 requires three sources, a VGPR-pair result, "
                                 "and its implicit VCC_OUT carry destination");
    }
    if (HasUnsupportedVop3InputModifier(inst.src[0]) ||
        HasUnsupportedVop3InputModifier(inst.src[1]) ||
        HasUnsupportedVop3InputModifier(inst.src[2]) ||
        HasUnsupportedVop3OutputModifier(inst.dst[0], false)) {
        throw std::runtime_error("GCN2 VOP3a MAD64 does not accept modifiers outside the "
                                 "unmodified subset");
    }
}

void ValidateVop3Sad(const GcnInst& inst) {
    if (inst.encoding != InstEncoding::VOP3 || inst.src_count != 3 || inst.dst_count != 1 ||
        inst.dst[0].field != OperandField::VectorGPR) {
        throw std::runtime_error("VOP3 SAD requires three sources and one VGPR destination");
    }
    if (HasUnsupportedVop3InputModifier(inst.src[0]) ||
        HasUnsupportedVop3InputModifier(inst.src[1]) ||
        HasUnsupportedVop3InputModifier(inst.src[2]) ||
        HasUnsupportedVop3OutputModifier(inst.dst[0], false)) {
        throw std::runtime_error(
            "VOP3 SAD does not accept modifiers outside the unmodified subset");
    }
}

bool IsSupportedVop3LerpU8SourceField(OperandField field) {
    switch (field) {
    case OperandField::ScalarGPR:
    case OperandField::VectorGPR:
    case OperandField::ConstZero:
    case OperandField::SignedConstIntPos:
    case OperandField::SignedConstIntNeg:
    case OperandField::ConstFloatPos_0_5:
    case OperandField::ConstFloatNeg_0_5:
    case OperandField::ConstFloatPos_1_0:
    case OperandField::ConstFloatNeg_1_0:
    case OperandField::ConstFloatPos_2_0:
    case OperandField::ConstFloatNeg_2_0:
    case OperandField::ConstFloatPos_4_0:
    case OperandField::ConstFloatNeg_4_0:
    case OperandField::VccLo:
    case OperandField::VccHi:
    case OperandField::M0:
    case OperandField::ExecLo:
    case OperandField::ExecHi:
    case OperandField::Scc:
        return true;
    default:
        return false;
    }
}

void ValidateVop3LerpU8(const GcnInst& inst) {
    if (inst.encoding != InstEncoding::VOP3 || inst.src_count != 3 || inst.dst_count != 1 ||
        inst.src[0].type != ScalarType::Uint32 || inst.src[1].type != ScalarType::Uint32 ||
        inst.src[2].type != ScalarType::Uint32 || inst.dst[0].type != ScalarType::Uint32 ||
        inst.dst[0].field != OperandField::VectorGPR) {
        throw std::runtime_error(
            "GCN2 VOP3a V_LERP_U8 requires three U32 sources and one U32 VGPR destination");
    }

    bool has_scalar_source = false;
    u32 scalar_source_code = 0;
    for (u32 index = 0; index < 3; ++index) {
        const auto& source = inst.src[index];
        if (!IsSupportedVop3LerpU8SourceField(source.field)) {
            throw std::runtime_error(
                "GCN2 VOP3a V_LERP_U8 source field is unsupported by the U32 IR path");
        }
        if (source.field == OperandField::ScalarGPR) {
            if (has_scalar_source && scalar_source_code != source.code) {
                throw std::runtime_error("GCN2 VOP3a V_LERP_U8 may read at most one distinct SGPR");
            }
            has_scalar_source = true;
            scalar_source_code = source.code;
        }
        if (HasUnsupportedVop3InputModifier(source)) {
            throw std::runtime_error(
                "GCN2 VOP3a V_LERP_U8 does not accept input modifiers or extensions");
        }
    }
    if (HasUnsupportedVop3OutputModifier(inst.dst[0], false)) {
        throw std::runtime_error(
            "GCN2 VOP3a V_LERP_U8 does not accept output modifiers or extensions");
    }
}

void ValidateVop3PackedSad(const GcnInst& inst) {
    if (inst.encoding != InstEncoding::VOP3 || inst.src_count != 3 || inst.dst_count != 1 ||
        inst.src[0].type != ScalarType::Uint64 || inst.src[1].type != ScalarType::Uint32 ||
        inst.src[2].type != ScalarType::Uint64 || inst.dst[0].type != ScalarType::Uint64 ||
        inst.dst[0].field != OperandField::VectorGPR) {
        throw std::runtime_error(
            "VOP3 packed SAD requires U64/U32/U64 sources and one U64 VGPR-pair destination");
    }
    if (HasUnsupportedVop3InputModifier(inst.src[0]) ||
        HasUnsupportedVop3InputModifier(inst.src[1]) ||
        HasUnsupportedVop3InputModifier(inst.src[2]) ||
        HasUnsupportedVop3OutputModifier(inst.dst[0], false)) {
        throw std::runtime_error(
            "VOP3 packed SAD does not accept modifiers outside the unmodified subset");
    }
}

void ValidateVop3MqsadU32U8(const GcnInst& inst) {
    constexpr u32 MaxVgprIndex{VectorGPRMax - VectorGPRMin};
    const bool src0_pair_out_of_range =
        (inst.src[0].field == OperandField::VectorGPR && inst.src[0].code > MaxVgprIndex - 1) ||
        (inst.src[0].field == OperandField::ScalarGPR && inst.src[0].code >= ScalarGPRMax);
    // B128 S2/D are four contiguous dwords, represented as individual U32 VGPRs.
    if (inst.encoding != InstEncoding::VOP3 || inst.src_count != 3 || inst.dst_count != 1 ||
        inst.src[0].type != ScalarType::Uint64 || inst.src[1].type != ScalarType::Uint32 ||
        inst.src[2].type != ScalarType::Uint32 || inst.dst[0].type != ScalarType::Uint32 ||
        inst.src[2].field != OperandField::VectorGPR || inst.src[2].code > MaxVgprIndex - 3 ||
        inst.dst[0].field != OperandField::VectorGPR || inst.dst[0].code > MaxVgprIndex - 3 ||
        src0_pair_out_of_range) {
        throw std::runtime_error(
            "VOP3 MQSAD_U32_U8 requires a valid B64 source and four-dword VGPR accumulator/result");
    }
    if (HasUnsupportedVop3InputModifier(inst.src[0]) ||
        HasUnsupportedVop3InputModifier(inst.src[1]) ||
        HasUnsupportedVop3InputModifier(inst.src[2]) ||
        HasUnsupportedVop3OutputModifier(inst.dst[0], false)) {
        throw std::runtime_error(
            "VOP3 MQSAD_U32_U8 does not accept modifiers outside the unmodified subset");
    }
}

void ValidateCarryInU32(const GcnInst& inst) {
    const bool vop2 =
        inst.encoding == InstEncoding::VOP2 && inst.src_count == 2 && inst.dst_count == 1;
    const bool vop3b = inst.encoding == InstEncoding::VOP3 && inst.src_count == 3 &&
                       inst.dst_count == 2 && IsCarryMaskRegister(inst.dst[1].field) &&
                       IsCarryMaskRegister(inst.src[2].field);
    if ((!vop2 && !vop3b) || inst.dst[0].field != OperandField::VectorGPR) {
        throw std::runtime_error(
            "U32 carry-in operation requires VOP2/VCC or VOP3B/SGPR-pair mask operands");
    }
    if (HasUnsupportedVop3InputModifier(inst.src[0]) ||
        HasUnsupportedVop3InputModifier(inst.src[1]) ||
        (vop3b && HasUnsupportedVop3InputModifier(inst.src[2])) ||
        HasUnsupportedVop3OutputModifier(inst.dst[0], true)) {
        throw std::runtime_error("U32 carry-in operation does not accept input modifiers or output "
                                 "modifiers other than CLMP");
    }
}

} // namespace

void Translator::EmitVectorAlu(const GcnInst& inst) {
    switch (inst.opcode) {
        // VOP2
    case Opcode::V_CNDMASK_B32:
        return V_CNDMASK_B32(inst);
    case Opcode::V_READLANE_B32:
        return V_READLANE_B32(inst);
    case Opcode::V_WRITELANE_B32:
        return V_WRITELANE_B32(inst);
    case Opcode::V_ADD_F32:
        return V_ADD_F32(inst);
    case Opcode::V_SUB_F32:
        return V_SUB_F32(inst);
    case Opcode::V_SUBREV_F32:
        return V_SUBREV_F32(inst);
    case Opcode::V_MAC_LEGACY_F32:
        return V_MAC_LEGACY_F32(inst);
    case Opcode::V_MUL_LEGACY_F32:
        return V_MUL_LEGACY_F32(inst);
    case Opcode::V_MUL_F32:
        return V_MUL_F32(inst);
    case Opcode::V_MUL_I32_I24:
        return V_MUL_I32_I24(inst, true);
    case Opcode::V_MUL_U32_U24:
        return V_MUL_I32_I24(inst, false);
    case Opcode::V_MUL_HI_I32_I24:
        if (!profile.supports_mul_hi_24_subset) {
            return LogMissingOpcode(inst);
        }
        return V_MUL_HI_24(inst, true);
    case Opcode::V_MUL_HI_U32_U24:
        if (!profile.supports_mul_hi_24_subset) {
            return LogMissingOpcode(inst);
        }
        return V_MUL_HI_24(inst, false);
    case Opcode::V_MIN_LEGACY_F32:
        return V_MIN_F32(inst, true);
    case Opcode::V_MAX_LEGACY_F32:
        return V_MAX_F32(inst, true);
    case Opcode::V_MIN_F32:
        return V_MIN_F32(inst, false);
    case Opcode::V_MAX_F32:
        return V_MAX_F32(inst);
    case Opcode::V_MIN_I32:
        return V_MIN_I32(inst);
    case Opcode::V_MAX_I32:
        return V_MAX_U32(true, inst);
    case Opcode::V_MIN_U32:
        return V_MIN_U32(inst);
    case Opcode::V_MAX_U32:
        return V_MAX_U32(false, inst);
    case Opcode::V_LSHR_B32:
        return V_LSHR_B32(inst);
    case Opcode::V_LSHRREV_B32:
        return V_LSHRREV_B32(inst);
    case Opcode::V_ASHR_I32:
        return V_ASHR_I32(inst);
    case Opcode::V_ASHRREV_I32:
        return V_ASHRREV_I32(inst);
    case Opcode::V_LSHL_B32:
        return V_LSHL_B32(inst);
    case Opcode::V_LSHLREV_B32:
        return V_LSHLREV_B32(inst);
    case Opcode::V_AND_B32:
        return V_AND_B32(inst);
    case Opcode::V_OR_B32:
        return V_OR_B32(false, inst);
    case Opcode::V_XOR_B32:
        return V_OR_B32(true, inst);
    case Opcode::V_BFM_B32:
        return V_BFM_B32(inst);
    case Opcode::V_MAC_F32:
        return V_MAC_F32(inst);
    case Opcode::V_MADMK_F32:
        return V_MADMK_F32(inst);
    case Opcode::V_MADAK_F32:
        return V_FMA_F32(inst);
    case Opcode::V_BCNT_U32_B32:
        return V_BCNT_U32_B32(inst);
    case Opcode::V_MBCNT_LO_U32_B32:
        return V_MBCNT_U32_B32(false, inst);
    case Opcode::V_MBCNT_HI_U32_B32:
        return V_MBCNT_U32_B32(true, inst);
    case Opcode::V_ADD_I32:
        return V_ADD_I32(inst);
    case Opcode::V_SUB_I32:
        return V_SUB_I32(inst);
    case Opcode::V_SUBREV_I32:
        return V_SUBREV_I32(inst);
    case Opcode::V_ADD_CO_U32:
        if (!profile.supports_vop3_co_u32_subset) {
            return LogMissingOpcode(inst);
        }
        return V_ADD_CO_U32(inst);
    case Opcode::V_SUB_CO_U32:
        if (!profile.supports_vop3_co_u32_subset) {
            return LogMissingOpcode(inst);
        }
        return V_SUB_CO_U32(inst);
    case Opcode::V_SUBREV_CO_U32:
        if (!profile.supports_vop3_co_u32_subset) {
            return LogMissingOpcode(inst);
        }
        return V_SUBREV_CO_U32(inst);
    case Opcode::V_ADDC_U32:
        return V_ADDC_U32(inst);
    case Opcode::V_SUBB_U32:
        return V_SUBB_U32(inst);
    case Opcode::V_SUBBREV_U32:
        return V_SUBBREV_U32(inst);
    case Opcode::V_LDEXP_F32:
        return V_LDEXP_F32(inst);
    case Opcode::V_CVT_PKNORM_U16_F32:
        return V_CVT_PKNORM_U16_F32(inst);
    case Opcode::V_CVT_PKNORM_I16_F32:
        return V_CVT_PKNORM_I16_F32(inst);
    case Opcode::V_CVT_PKRTZ_F16_F32:
        return V_CVT_PKRTZ_F16_F32(inst);
    case Opcode::V_ADD_F16:
        return V_ADD_F16(inst);
    case Opcode::V_SUB_F16:
        return V_SUB_F16(inst);
    case Opcode::V_SUBREV_F16:
        return V_SUBREV_F16(inst);
    case Opcode::V_MUL_F16:
        return V_MUL_F16(inst);
    case Opcode::V_MAX_F16:
        return V_MAX_F16(inst);
    case Opcode::V_MIN_F16:
        return V_MIN_F16(inst);

        // VOP1
    case Opcode::V_MOV_B32:
        return V_MOV(inst);
    case Opcode::V_READFIRSTLANE_B32:
        return V_READFIRSTLANE_B32(inst);
    case Opcode::V_CVT_I32_F64:
        return V_CVT_I32_F64(inst);
    case Opcode::V_CVT_F64_I32:
        return V_CVT_F64_I32(inst);
    case Opcode::V_CVT_F64_U32:
        return V_CVT_F64_U32(inst);
    case Opcode::V_CVT_F32_I32:
        return V_CVT_F32_I32(inst);
    case Opcode::V_CVT_F32_U32:
        return V_CVT_F32_U32(inst);
    case Opcode::V_CVT_U32_F32:
        return V_CVT_U32_F32(inst);
    case Opcode::V_CVT_I32_F32:
        return V_CVT_I32_F32(inst);
    case Opcode::V_CVT_F16_F32:
        return V_CVT_F16_F32(inst);
    case Opcode::V_CVT_F32_F16:
        return V_CVT_F32_F16(inst);
    case Opcode::V_CVT_FLR_I32_F32:
        return V_CVT_FLR_I32_F32(inst);
    case Opcode::V_CVT_F32_F64:
        return V_CVT_F32_F64(inst);
    case Opcode::V_CVT_F64_F32:
        return V_CVT_F64_F32(inst);
    case Opcode::V_CVT_RPI_I32_F32:
        return V_CVT_RPI_I32_F32(inst);
    case Opcode::V_CVT_OFF_F32_I4:
        return V_CVT_OFF_F32_I4(inst);
    case Opcode::V_CVT_F32_UBYTE0:
        return V_CVT_F32_UBYTE(0, inst);
    case Opcode::V_CVT_F32_UBYTE1:
        return V_CVT_F32_UBYTE(1, inst);
    case Opcode::V_CVT_F32_UBYTE2:
        return V_CVT_F32_UBYTE(2, inst);
    case Opcode::V_CVT_F32_UBYTE3:
        return V_CVT_F32_UBYTE(3, inst);
    case Opcode::V_TRUNC_F64:
        return V_TRUNC_F64(inst);
    case Opcode::V_FLOOR_F64:
        return V_FLOOR_F64(inst);
    case Opcode::V_FRACT_F32:
        return V_FRACT_F32(inst);
    case Opcode::V_TRUNC_F32:
        return V_TRUNC_F32(inst);
    case Opcode::V_CEIL_F32:
        return V_CEIL_F32(inst);
    case Opcode::V_RNDNE_F32:
        return V_RNDNE_F32(inst);
    case Opcode::V_FLOOR_F32:
        return V_FLOOR_F32(inst);
    case Opcode::V_EXP_F32:
        return V_EXP_F32(inst);
    case Opcode::V_LOG_CLAMP_F32:
        return V_LOG_CLAMP_F32(inst);
    case Opcode::V_LOG_F32:
        return V_LOG_F32(inst);
    case Opcode::V_RCP_F32:
        return V_RCP_F32(inst);
    case Opcode::V_RCP_LEGACY_F32:
        return V_RCP_LEGACY_F32(inst);
    case Opcode::V_RCP_F64:
        return V_RCP_F64(inst);
    case Opcode::V_RCP_IFLAG_F32:
        return V_RCP_F32(inst);
    case Opcode::V_RCP_CLAMP_F32:
        return V_RCP_CLAMP_F32(inst);
    case Opcode::V_RSQ_CLAMP_F32:
        return V_RSQ_CLAMP_F32(inst);
    case Opcode::V_RSQ_LEGACY_F32:
        return V_RSQ_LEGACY_F32(inst);
    case Opcode::V_RSQ_F32:
        return V_RSQ_F32(inst);
    case Opcode::V_SQRT_F32:
        return V_SQRT_F32(inst);
    case Opcode::V_SIN_F32:
        return V_SIN_F32(inst);
    case Opcode::V_COS_F32:
        return V_COS_F32(inst);
    case Opcode::V_NOT_B32:
        return V_NOT_B32(inst);
    case Opcode::V_BFREV_B32:
        return V_BFREV_B32(inst);
    case Opcode::V_FFBH_U32:
        return V_FFBH_U32(inst);
    case Opcode::V_FFBL_B32:
        return V_FFBL_B32(inst);
    case Opcode::V_FFBH_I32:
        return V_FFBH_I32(inst);
    case Opcode::V_FREXP_EXP_I32_F64:
        return V_FREXP_EXP_I32_F64(inst);
    case Opcode::V_FREXP_MANT_F64:
        return V_FREXP_MANT_F64(inst);
    case Opcode::V_FRACT_F64:
        return V_FRACT_F64(inst);
    case Opcode::V_FREXP_EXP_I32_F32:
        return V_FREXP_EXP_I32_F32(inst);
    case Opcode::V_FREXP_MANT_F32:
        return V_FREXP_MANT_F32(inst);
    case Opcode::V_MOVRELD_B32:
        return V_MOVRELD_B32(inst);
    case Opcode::V_MOVRELS_B32:
        return V_MOVRELS_B32(inst);
    case Opcode::V_MOVRELSD_B32:
        return V_MOVRELSD_B32(inst);

        // VOPC
        //     V_CMP_{OP16}_F32
    case Opcode::V_CMP_F_F32:
        return V_CMP_F32(ConditionOp::F, true, false, inst);
    case Opcode::V_CMP_LT_F32:
        return V_CMP_F32(ConditionOp::LT, true, false, inst);
    case Opcode::V_CMP_EQ_F32:
        return V_CMP_F32(ConditionOp::EQ, true, false, inst);
    case Opcode::V_CMP_LE_F32:
        return V_CMP_F32(ConditionOp::LE, true, false, inst);
    case Opcode::V_CMP_GT_F32:
        return V_CMP_F32(ConditionOp::GT, true, false, inst);
    case Opcode::V_CMP_LG_F32:
        return V_CMP_F32(ConditionOp::LG, true, false, inst);
    case Opcode::V_CMP_GE_F32:
        return V_CMP_F32(ConditionOp::GE, true, false, inst);
    case Opcode::V_CMP_U_F32:
        return V_CMP_F32(ConditionOp::U, true, false, inst);
    case Opcode::V_CMP_NGE_F32:
        return V_CMP_F32(ConditionOp::LT, false, false, inst);
    case Opcode::V_CMP_NGT_F32:
        return V_CMP_F32(ConditionOp::LE, false, false, inst);
    case Opcode::V_CMP_NLE_F32:
        return V_CMP_F32(ConditionOp::GT, false, false, inst);
    case Opcode::V_CMP_NEQ_F32:
        return V_CMP_F32(ConditionOp::LG, false, false, inst);
    case Opcode::V_CMP_NLT_F32:
        return V_CMP_F32(ConditionOp::GE, false, false, inst);

        //     V_CMPX_{OP16}_F32
    case Opcode::V_CMPX_F_F32:
        return V_CMP_F32(ConditionOp::F, true, true, inst);
    case Opcode::V_CMPX_LT_F32:
        return V_CMP_F32(ConditionOp::LT, true, true, inst);
    case Opcode::V_CMPX_EQ_F32:
        return V_CMP_F32(ConditionOp::EQ, true, true, inst);
    case Opcode::V_CMPX_LE_F32:
        return V_CMP_F32(ConditionOp::LE, true, true, inst);
    case Opcode::V_CMPX_GT_F32:
        return V_CMP_F32(ConditionOp::GT, true, true, inst);
    case Opcode::V_CMPX_LG_F32:
        return V_CMP_F32(ConditionOp::LG, true, true, inst);
    case Opcode::V_CMPX_GE_F32:
        return V_CMP_F32(ConditionOp::GE, true, true, inst);
    case Opcode::V_CMPX_NGE_F32:
        return V_CMP_F32(ConditionOp::LT, false, true, inst);
    case Opcode::V_CMPX_NLG_F32:
        return V_CMP_F32(ConditionOp::EQ, false, true, inst);
    case Opcode::V_CMPX_NGT_F32:
        return V_CMP_F32(ConditionOp::LE, false, true, inst);
    case Opcode::V_CMPX_NLE_F32:
        return V_CMP_F32(ConditionOp::GT, false, true, inst);
    case Opcode::V_CMPX_NEQ_F32:
        return V_CMP_F32(ConditionOp::LG, false, true, inst);
    case Opcode::V_CMPX_NLT_F32:
        return V_CMP_F32(ConditionOp::GE, false, true, inst);

        //     V_CMP_{OP16}_F64
    case Opcode::V_CMP_F_F64:
        return V_CMP_F64(ConditionOp::F, true, false, inst);
    case Opcode::V_CMP_LT_F64:
        return V_CMP_F64(ConditionOp::LT, true, false, inst);
    case Opcode::V_CMP_EQ_F64:
        return V_CMP_F64(ConditionOp::EQ, true, false, inst);
    case Opcode::V_CMP_LE_F64:
        return V_CMP_F64(ConditionOp::LE, true, false, inst);
    case Opcode::V_CMP_GT_F64:
        return V_CMP_F64(ConditionOp::GT, true, false, inst);
    case Opcode::V_CMP_LG_F64:
        return V_CMP_F64(ConditionOp::LG, true, false, inst);
    case Opcode::V_CMP_GE_F64:
        return V_CMP_F64(ConditionOp::GE, true, false, inst);
    case Opcode::V_CMP_U_F64:
        return V_CMP_F64(ConditionOp::U, true, false, inst);
    case Opcode::V_CMP_NGE_F64:
        return V_CMP_F64(ConditionOp::LT, false, false, inst);
    case Opcode::V_CMP_NGT_F64:
        return V_CMP_F64(ConditionOp::LE, false, false, inst);
    case Opcode::V_CMP_NLE_F64:
        return V_CMP_F64(ConditionOp::GT, false, false, inst);
    case Opcode::V_CMP_NEQ_F64:
        return V_CMP_F64(ConditionOp::LG, false, false, inst);
    case Opcode::V_CMP_NLT_F64:
        return V_CMP_F64(ConditionOp::GE, false, false, inst);

        //     V_CMP_{OP8}_I32
    case Opcode::V_CMP_LT_I32:
        return V_CMP_U32(ConditionOp::LT, true, false, inst);
    case Opcode::V_CMP_EQ_I32:
        return V_CMP_U32(ConditionOp::EQ, true, false, inst);
    case Opcode::V_CMP_LE_I32:
        return V_CMP_U32(ConditionOp::LE, true, false, inst);
    case Opcode::V_CMP_GT_I32:
        return V_CMP_U32(ConditionOp::GT, true, false, inst);
    case Opcode::V_CMP_NE_I32:
        return V_CMP_U32(ConditionOp::LG, true, false, inst);
    case Opcode::V_CMP_GE_I32:
        return V_CMP_U32(ConditionOp::GE, true, false, inst);
    case Opcode::V_CMPX_LE_I32:
        return V_CMP_U32(ConditionOp::LE, true, true, inst);

        //     V_CMPX_{OP8}_I32
    case Opcode::V_CMPX_LT_I32:
        return V_CMP_U32(ConditionOp::LT, true, true, inst);
    case Opcode::V_CMPX_EQ_I32:
        return V_CMP_U32(ConditionOp::EQ, true, true, inst);
    case Opcode::V_CMPX_GT_I32:
        return V_CMP_U32(ConditionOp::GT, true, true, inst);
    case Opcode::V_CMPX_LG_I32:
        return V_CMP_U32(ConditionOp::LG, true, true, inst);
    case Opcode::V_CMPX_GE_I32:
        return V_CMP_U32(ConditionOp::GE, true, true, inst);

        //     V_CMP_{OP8}_U32
    case Opcode::V_CMP_F_U32:
        return V_CMP_U32(ConditionOp::F, false, false, inst);
    case Opcode::V_CMP_LT_U32:
        return V_CMP_U32(ConditionOp::LT, false, false, inst);
    case Opcode::V_CMP_EQ_U32:
        return V_CMP_U32(ConditionOp::EQ, false, false, inst);
    case Opcode::V_CMP_LE_U32:
        return V_CMP_U32(ConditionOp::LE, false, false, inst);
    case Opcode::V_CMP_GT_U32:
        return V_CMP_U32(ConditionOp::GT, false, false, inst);
    case Opcode::V_CMP_NE_U32:
        return V_CMP_U32(ConditionOp::LG, false, false, inst);
    case Opcode::V_CMP_GE_U32:
        return V_CMP_U32(ConditionOp::GE, false, false, inst);
    case Opcode::V_CMP_TRU_U32:
        return V_CMP_U32(ConditionOp::TRU, false, false, inst);

        //     V_CMPX_{OP8}_U32
    case Opcode::V_CMPX_F_U32:
        return V_CMP_U32(ConditionOp::F, false, true, inst);
    case Opcode::V_CMPX_LT_U32:
        return V_CMP_U32(ConditionOp::LT, false, true, inst);
    case Opcode::V_CMPX_EQ_U32:
        return V_CMP_U32(ConditionOp::EQ, false, true, inst);
    case Opcode::V_CMPX_LE_U32:
        return V_CMP_U32(ConditionOp::LE, false, true, inst);
    case Opcode::V_CMPX_GT_U32:
        return V_CMP_U32(ConditionOp::GT, false, true, inst);
    case Opcode::V_CMPX_NE_U32:
        return V_CMP_U32(ConditionOp::LG, false, true, inst);
    case Opcode::V_CMPX_GE_U32:
        return V_CMP_U32(ConditionOp::GE, false, true, inst);
    case Opcode::V_CMPX_TRU_U32:
        return V_CMP_U32(ConditionOp::TRU, false, true, inst);

        //     V_CMP_{OP8}_I64
    case Opcode::V_CMP_F_I64:
        return V_CMP_U64(ConditionOp::F, true, false, inst);
    case Opcode::V_CMP_LT_I64:
        return V_CMP_U64(ConditionOp::LT, true, false, inst);
    case Opcode::V_CMP_EQ_I64:
        return V_CMP_U64(ConditionOp::EQ, true, false, inst);
    case Opcode::V_CMP_LE_I64:
        return V_CMP_U64(ConditionOp::LE, true, false, inst);
    case Opcode::V_CMP_GT_I64:
        return V_CMP_U64(ConditionOp::GT, true, false, inst);
    case Opcode::V_CMP_NE_I64:
        return V_CMP_U64(ConditionOp::LG, true, false, inst);
    case Opcode::V_CMP_GE_I64:
        return V_CMP_U64(ConditionOp::GE, true, false, inst);
    case Opcode::V_CMP_TRU_I64:
        return V_CMP_U64(ConditionOp::TRU, true, false, inst);

        //     V_CMPX_{OP8}_I64
    case Opcode::V_CMPX_EQ_I64:
        return V_CMP_U64(ConditionOp::EQ, true, true, inst);

        //     V_CMP_{OP8}_U64
    case Opcode::V_CMP_EQ_U64:
        return V_CMP_U64(ConditionOp::EQ, false, false, inst);
    case Opcode::V_CMP_NE_U64:
        return V_CMP_U64(ConditionOp::LG, false, false, inst);
    case Opcode::V_CMP_GT_U64:
        return V_CMP_U64(ConditionOp::GT, false, false, inst);
    case Opcode::V_CMP_LT_U64:
        return V_CMP_U64(ConditionOp::LT, false, false, inst);

        //     V_CMPX_{OP8}_U64
    case Opcode::V_CMPX_EQ_U64:
        return V_CMP_U64(ConditionOp::EQ, false, true, inst);
    case Opcode::V_CMPX_LG_U64:
        return V_CMP_U64(ConditionOp::LG, false, true, inst);

    case Opcode::V_CMP_CLASS_F32:
        return V_CMP_CLASS_F32(inst);

        // VOP3a
    case Opcode::V_MAD_LEGACY_F32:
        return V_MAD_LEGACY_F32(inst);
    case Opcode::V_MAD_F32:
        return V_MAD_F32(inst);
    case Opcode::V_MAD_I32_I24:
        return V_MAD_I32_I24(inst);
    case Opcode::V_MAD_U32_U24:
        return V_MAD_U32_U24(inst);
    case Opcode::V_CUBEID_F32:
        return V_CUBEID_F32(inst);
    case Opcode::V_CUBESC_F32:
        return V_CUBESC_F32(inst);
    case Opcode::V_CUBETC_F32:
        return V_CUBETC_F32(inst);
    case Opcode::V_CUBEMA_F32:
        return V_CUBEMA_F32(inst);
    case Opcode::V_BFE_U32:
        return V_BFE_U32(false, inst);
    case Opcode::V_BFE_I32:
        return V_BFE_U32(true, inst);
    case Opcode::V_BFI_B32:
        return V_BFI_B32(inst);
    case Opcode::V_FMA_F32:
        return V_FMA_F32(inst);
    case Opcode::V_FMA_F64:
        return V_FMA_F64(inst);
    case Opcode::V_LERP_U8:
        if (!profile.is_d3d12_profile ||
            profile.guest_isa_generation != GuestIsaGeneration::Gcn2SeaIslands ||
            !profile.supports_vop3_lerp_u8_subset) {
            return LogMissingOpcode(inst);
        }
        ValidateVop3LerpU8(inst);
        return V_LERP_U8(inst);
    case Opcode::V_MIN3_F32:
        return V_MIN3_F32(inst);
    case Opcode::V_MIN3_I32:
        return V_MIN3_U32(true, inst);
    case Opcode::V_MIN3_U32:
        return V_MIN3_U32(false, inst);
    case Opcode::V_MAX3_F32:
        return V_MAX3_F32(inst);
    case Opcode::V_MAX3_I32:
        return V_MAX3_U32(true, inst);
    case Opcode::V_MAX3_U32:
        return V_MAX3_U32(false, inst);
    case Opcode::V_MED3_F32:
        return V_MED3_F32(inst);
    case Opcode::V_MED3_I32:
        return V_MED3_U32(true, inst);
    case Opcode::V_MED3_U32:
        return V_MED3_U32(false, inst);
    case Opcode::V_SAD_U32:
        return V_SAD_U32(inst);
    case Opcode::V_MSAD_U8:
    case Opcode::V_SAD_U8:
    case Opcode::V_SAD_HI_U8:
    case Opcode::V_SAD_U16:
        if (!profile.is_d3d12_profile || !profile.supports_vop3_sad_subset) {
            return LogMissingOpcode(inst);
        }
        ValidateVop3Sad(inst);
        if (inst.opcode == Opcode::V_MSAD_U8) {
            return V_MSAD_U8(inst);
        }
        if (inst.opcode == Opcode::V_SAD_U16) {
            return V_SAD_U16(inst);
        }
        return V_SAD_U8(inst, inst.opcode == Opcode::V_SAD_HI_U8);
    case Opcode::V_QSAD_U8:
    case Opcode::V_MQSAD_U8:
        if (!profile.is_d3d12_profile || !profile.supports_vop3_sad_packed_subset) {
            return LogMissingOpcode(inst);
        }
        ValidateVop3PackedSad(inst);
        return V_QSAD_PK_U16_U8(inst, inst.opcode == Opcode::V_MQSAD_U8);
    case Opcode::V_MQSAD_U32_U8:
        if (!profile.is_d3d12_profile || !profile.supports_vop3_sad_packed_subset) {
            return LogMissingOpcode(inst);
        }
        ValidateVop3MqsadU32U8(inst);
        return V_MQSAD_U32_U8(inst);
    case Opcode::V_CVT_PK_U16_U32:
        return V_CVT_PK_U16_U32(inst);
    case Opcode::V_CVT_PK_I16_I32:
        return V_CVT_PK_I16_I32(inst);
    case Opcode::V_CVT_PK_U8_F32:
        return V_CVT_PK_U8_F32(inst);
    case Opcode::V_LSHL_B64:
        return V_LSHL_B64(inst);
    case Opcode::V_LSHR_B64:
        return V_LSHR_B64(inst);
    case Opcode::V_ASHR_I64:
        return V_ASHR_I64(inst);
    case Opcode::V_ADD_F64:
        return V_ADD_F64(inst);
    case Opcode::V_ALIGNBIT_B32:
        return V_ALIGNBIT_B32(inst);
    case Opcode::V_ALIGNBYTE_B32:
        return V_ALIGNBYTE_B32(inst);
    case Opcode::V_MUL_F64:
        return V_MUL_F64(inst);
    case Opcode::V_MIN_F64:
        return V_MIN_F64(inst);
    case Opcode::V_MAX_F64:
        return V_MAX_F64(inst);
    case Opcode::V_MUL_LO_U32:
        return V_MUL_LO_U32(inst);
    case Opcode::V_MUL_HI_U32:
        return V_MUL_HI_U32(false, inst);
    case Opcode::V_MUL_LO_I32:
        return V_MUL_LO_U32(inst);
    case Opcode::V_MUL_HI_I32:
        return V_MUL_HI_U32(true, inst);
    case Opcode::V_MAD_U64_U32:
        return V_MAD_U64_U32(inst);
    case Opcode::V_MAD_I64_I32:
        if (!profile.is_d3d12_profile || !profile.supports_vop3_mad64_subset) {
            return LogMissingOpcode(inst);
        }
        return V_MAD_I64_I32(inst);
    case Opcode::V_LSHRREV_B16:
        return V_LSHRREV_B16(inst);
    case Opcode::V_ASHRREV_I16:
        return V_ASHRREV_I16(inst);
    case Opcode::V_LSHLREV_B16:
        return V_LSHLREV_B16(inst);
    case Opcode::V_ADD_LSHL_U32:
        return V_ADD_LSHL_U32(inst);
    case Opcode::V_LSHL_ADD_U32:
        return V_LSHL_ADD_U32(inst);
    case Opcode::V_MIN3_F16:
        return V_MIN3_F16(inst);
    case Opcode::V_MAX3_F16:
        return V_MAX3_F16(inst);
    case Opcode::V_MED3_F16:
        return V_MED3_F16(inst);
    case Opcode::V_ADD3_U32:
        return V_ADD3_U32(inst);
    case Opcode::V_LSHL_OR_B32:
        return V_LSHL_OR_B32(inst);
    case Opcode::V_AND_OR_B32:
        return V_AND_OR_B32(inst);
    case Opcode::V_OR3_B32:
        return V_OR3_B32(inst);
    case Opcode::V_NOP:
        return;

    // VOP3P
    case Opcode::V_PK_MUL_LO_U16:
        return V_PK_MUL_LO_U16(inst);
    case Opcode::V_PK_ADD_I16:
        return V_PK_ADD_I16(inst);
    case Opcode::V_PK_SUB_I16:
        return V_PK_SUB_I16(inst);
    case Opcode::V_PK_LSHRREV_B16:
        return V_PK_LSHRREV_B16(inst);
    case Opcode::V_PK_LSHLREV_B16:
        return V_PK_LSHLREV_B16(inst);
    case Opcode::V_PK_MAD_U16:
        return V_PK_MAD_U16(inst);
    case Opcode::V_PK_ADD_U16:
        return V_PK_ADD_U16(inst);
    case Opcode::V_PK_SUB_U16:
        return V_PK_SUB_U16(inst);
    case Opcode::V_PK_MAX_U16:
        return V_PK_MAX_U16(inst);
    case Opcode::V_PK_MIN_U16:
        return V_PK_MIN_U16(inst);
    case Opcode::V_PK_FMA_F16:
        return V_PK_FMA_F16(inst);
    case Opcode::V_PK_ADD_F16:
        return V_PK_ADD_F16(inst);
    case Opcode::V_PK_MUL_F16:
        return V_PK_MUL_F16(inst);
    case Opcode::V_PK_MIN_F16:
        return V_PK_MIN_F16(inst);
    case Opcode::V_PK_MAX_F16:
        return V_PK_MAX_F16(inst);
    case Opcode::V_MAD_MIX_F32:
        return V_MAD_MIX_F32(inst);
    case Opcode::V_MAD_MIXLO_F16:
        return V_MAD_MIXLO_F16(inst);
    case Opcode::V_MAD_MIXHI_F16:
        return V_MAD_MIXHI_F16(inst);

    default:
        LogMissingOpcode(inst);
    }
}

// VOP2

void Translator::V_CNDMASK_B32(const GcnInst& inst) {
    IR::U64 mask;
    if (inst.src[2].field == OperandField::Undefined) {
        mask = ir.PackUint2x32(ir.CompositeConstruct(ir.GetVccLo(), ir.GetVccHi()));
    } else {
        mask = GetSrc64(inst.src[2]);
    }
    const IR::Value result = ir.Select(ir.InverseBallot(mask), GetSrc<IR::F32>(inst.src[1]),
                                       GetSrc<IR::F32>(inst.src[0]));
    SetDst(inst.dst[0], IR::U32F32{result});
}

void Translator::V_ADD_F32(const GcnInst& inst) {
    const IR::F32 src0{GetSrc<IR::F32>(inst.src[0])};
    const IR::F32 src1{GetSrc<IR::F32>(inst.src[1])};
    const IR::F32 result{ir.FPAdd(src0, src1)};
    SetDst(inst.dst[0], result);
}

void Translator::V_ADD_F64(const GcnInst& inst) {
    const IR::F64 src0{GetSrc64<IR::F64>(inst.src[0])};
    const IR::F64 src1{GetSrc64<IR::F64>(inst.src[1])};
    SetDst64(inst.dst[0], ir.FPAdd(src0, src1));
}

void Translator::V_SUB_F32(const GcnInst& inst) {
    const IR::F32 src0{GetSrc<IR::F32>(inst.src[0])};
    const IR::F32 src1{GetSrc<IR::F32>(inst.src[1])};
    SetDst(inst.dst[0], ir.FPSub(src0, src1));
}

void Translator::V_SUBREV_F32(const GcnInst& inst) {
    const IR::F32 src0{GetSrc<IR::F32>(inst.src[0])};
    const IR::F32 src1{GetSrc<IR::F32>(inst.src[1])};
    SetDst(inst.dst[0], ir.FPSub(src1, src0));
}

void Translator::V_MUL_F32(const GcnInst& inst) {
    SetDst(inst.dst[0], ir.FPMul(GetSrc<IR::F32>(inst.src[0]), GetSrc<IR::F32>(inst.src[1])));
}

void Translator::V_MUL_LEGACY_F32(const GcnInst& inst) {
    const IR::F32 src0{GetSrc<IR::F32>(inst.src[0])};
    const IR::F32 src1{GetSrc<IR::F32>(inst.src[1])};
    SetDst(inst.dst[0], LegacyMul(src0, src1));
}

void Translator::V_MAC_LEGACY_F32(const GcnInst& inst) {
    const IR::F32 src0{GetSrc<IR::F32>(inst.src[0])};
    const IR::F32 src1{GetSrc<IR::F32>(inst.src[1])};
    const IR::F32 dst0{GetSrc<IR::F32>(inst.dst[0])};
    SetDst(inst.dst[0], ir.FPAdd(LegacyMul(src0, src1), dst0));
}

void Translator::V_MAD_LEGACY_F32(const GcnInst& inst) {
    const IR::F32 src0{GetSrc<IR::F32>(inst.src[0])};
    const IR::F32 src1{GetSrc<IR::F32>(inst.src[1])};
    const IR::F32 src2{GetSrc<IR::F32>(inst.src[2])};
    SetDst(inst.dst[0], ir.FPAdd(LegacyMul(src0, src1), src2));
}

void Translator::V_MUL_I32_I24(const GcnInst& inst, bool is_signed) {
    const IR::U32 src0{
        ir.BitFieldExtract(GetSrc(inst.src[0]), ir.Imm32(0), ir.Imm32(24), is_signed)};
    const IR::U32 src1{
        ir.BitFieldExtract(GetSrc(inst.src[1]), ir.Imm32(0), ir.Imm32(24), is_signed)};
    SetDst(inst.dst[0], ir.IMul(src0, src1));
}

void Translator::V_MIN_F32(const GcnInst& inst, bool is_legacy) {
    const IR::F32 src0{GetSrc<IR::F32>(inst.src[0])};
    const IR::F32 src1{GetSrc<IR::F32>(inst.src[1])};

    const IR::F32 fpmin = ir.FPMin(src0, src1);
    const IR::F32 result =
        is_legacy
            ? IR::F32{ir.Select(ir.LogicalOr(ir.FPIsNan(src0), ir.FPIsNan(src1)), src1, fpmin)}
            : fpmin;
    SetDst(inst.dst[0], result);
}

void Translator::V_MAX_F32(const GcnInst& inst, bool is_legacy) {
    const IR::F32 src0{GetSrc<IR::F32>(inst.src[0])};
    const IR::F32 src1{GetSrc<IR::F32>(inst.src[1])};

    const IR::F32 fpmax = ir.FPMax(src0, src1);
    const IR::F32 result =
        is_legacy
            ? IR::F32{ir.Select(ir.LogicalOr(ir.FPIsNan(src0), ir.FPIsNan(src1)), src1, fpmax)}
            : fpmax;
    SetDst(inst.dst[0], result);
}

void Translator::V_MIN_I32(const GcnInst& inst) {
    const IR::U32 src0{GetSrc(inst.src[0])};
    const IR::U32 src1{GetSrc(inst.src[1])};
    SetDst(inst.dst[0], ir.SMin(src0, src1));
}

void Translator::V_MIN_U32(const GcnInst& inst) {
    const IR::U32 src0{GetSrc(inst.src[0])};
    const IR::U32 src1{GetSrc(inst.src[1])};
    SetDst(inst.dst[0], ir.IMin(src0, src1, false));
}

void Translator::V_MAX_U32(bool is_signed, const GcnInst& inst) {
    const IR::U32 src0{GetSrc(inst.src[0])};
    const IR::U32 src1{GetSrc(inst.src[1])};
    SetDst(inst.dst[0], ir.IMax(src0, src1, is_signed));
}

void Translator::V_LSHR_B32(const GcnInst& inst) {
    const IR::U32 src0{GetSrc(inst.src[0])};
    const IR::U32 src1{GetSrc(inst.src[1])};
    SetDst(inst.dst[0], ir.ShiftRightLogical(src0, ir.BitwiseAnd(src1, ir.Imm32(0x1F))));
}

void Translator::V_LSHRREV_B32(const GcnInst& inst) {
    const IR::U32 src0{GetSrc(inst.src[0])};
    const IR::U32 src1{GetSrc(inst.src[1])};
    SetDst(inst.dst[0], ir.ShiftRightLogical(src1, ir.BitwiseAnd(src0, ir.Imm32(0x1F))));
}

void Translator::V_ASHR_I32(const GcnInst& inst) {
    const IR::U32 src0{GetSrc(inst.src[0])};
    const IR::U32 src1{GetSrc(inst.src[1])};
    SetDst(inst.dst[0], ir.ShiftRightArithmetic(src0, ir.BitwiseAnd(src1, ir.Imm32(0x1F))));
}

void Translator::V_ASHRREV_I32(const GcnInst& inst) {
    const IR::U32 src0{GetSrc(inst.src[0])};
    const IR::U32 src1{GetSrc(inst.src[1])};
    SetDst(inst.dst[0], ir.ShiftRightArithmetic(src1, ir.BitwiseAnd(src0, ir.Imm32(0x1F))));
}

void Translator::V_LSHL_B32(const GcnInst& inst) {
    const IR::U32 src0{GetSrc(inst.src[0])};
    const IR::U32 src1{GetSrc(inst.src[1])};
    SetDst(inst.dst[0], ir.ShiftLeftLogical(src0, ir.BitwiseAnd(src1, ir.Imm32(0x1F))));
}

void Translator::V_LSHLREV_B32(const GcnInst& inst) {
    const IR::U32 src0{GetSrc(inst.src[0])};
    const IR::U32 src1{GetSrc(inst.src[1])};
    SetDst(inst.dst[0], ir.ShiftLeftLogical(src1, ir.BitwiseAnd(src0, ir.Imm32(0x1F))));
}

void Translator::V_AND_B32(const GcnInst& inst) {
    const IR::U32 src0{GetSrc(inst.src[0])};
    const IR::U32 src1{GetSrc(inst.src[1])};
    SetDst(inst.dst[0], ir.BitwiseAnd(src0, src1));
}

void Translator::V_OR_B32(bool is_xor, const GcnInst& inst) {
    const IR::U32 src0{GetSrc(inst.src[0])};
    const IR::U32 src1{GetSrc(inst.src[1])};
    SetDst(inst.dst[0],
           is_xor ? IR::U32{ir.BitwiseXor(src0, src1)} : IR::U32{ir.BitwiseOr(src0, src1)});
}

void Translator::V_BFM_B32(const GcnInst& inst) {
    // bitmask width, S0[4:0]
    const IR::U32 src0{ir.BitFieldExtract(GetSrc(inst.src[0]), ir.Imm32(0), ir.Imm32(5))};
    // bitmask offset, S1[4:0]
    const IR::U32 src1{ir.BitFieldExtract(GetSrc(inst.src[1]), ir.Imm32(0), ir.Imm32(5))};
    const IR::U32 ones = ir.ISub(ir.ShiftLeftLogical(ir.Imm32(1), src0), ir.Imm32(1));
    SetDst(inst.dst[0], ir.ShiftLeftLogical(ones, src1));
}

void Translator::V_MAC_F32(const GcnInst& inst) {
    const auto src0 = GetSrc<IR::F32>(inst.src[0]);
    const auto src1 = GetSrc<IR::F32>(inst.src[1]);
    const auto dst0 = GetSrc<IR::F32>(inst.dst[0]);
    SetDst(inst.dst[0], ir.FPAdd(ir.FPMul(src0, src1), dst0));
}

void Translator::V_MADMK_F32(const GcnInst& inst) {
    const IR::F32 src0{GetSrc<IR::F32>(inst.src[0])};
    const IR::F32 src1{GetSrc<IR::F32>(inst.src[1])};
    const IR::F32 k{GetSrc<IR::F32>(inst.src[2])};
    SetDst(inst.dst[0], ir.FPAdd(ir.FPMul(src0, k), src1));
}

void Translator::V_BCNT_U32_B32(const GcnInst& inst) {
    const IR::U32 src0{GetSrc(inst.src[0])};
    const IR::U32 src1{GetSrc(inst.src[1])};
    SetDst(inst.dst[0], ir.IAdd(ir.BitCount(src0), src1));
}

void Translator::V_MBCNT_U32_B32(bool hi, const GcnInst& inst) {
    const IR::U32 src0{GetSrc(inst.src[0])};
    const IR::U32 src1{GetSrc(inst.src[1])};
    SetDst(inst.dst[0], ir.MaskedBitCount(src0, src1, hi));
}

void Translator::V_ADD_I32(const GcnInst& inst) {
    if (HasUnsupportedVop3InputModifier(inst.src[0]) ||
        HasUnsupportedVop3InputModifier(inst.src[1]) ||
        HasUnsupportedVop3OutputModifier(inst.dst[0], false)) {
        throw std::runtime_error("V_ADD_I32 carry-out subset rejects unrepresented modifiers");
    }
    const IR::U32 src0{GetSrc(inst.src[0])};
    const IR::U32 src1{GetSrc(inst.src[1])};
    const IR::U32 result{ir.IAdd(src0, src1)};
    SetDst(inst.dst[0], result);

    SetCarryOut(inst, ir.ILessThan(result, src0, false));
}

void Translator::V_SUB_I32(const GcnInst& inst) {
    // Unsigned components
    if (HasUnsupportedVop3InputModifier(inst.src[0]) ||
        HasUnsupportedVop3InputModifier(inst.src[1]) ||
        HasUnsupportedVop3OutputModifier(inst.dst[0], false)) {
        throw std::runtime_error("V_SUB_I32 carry-out subset rejects unrepresented modifiers");
    }
    const IR::U32 src0{GetSrc(inst.src[0])};
    const IR::U32 src1{GetSrc(inst.src[1])};
    const IR::U32 result{ir.ISub(src0, src1)};
    SetDst(inst.dst[0], result);

    const IR::U1 did_underflow{ir.IGreaterThan(src1, src0, false)};
    SetCarryOut(inst, did_underflow);
}

void Translator::V_SUBREV_I32(const GcnInst& inst) {
    // Unsigned components
    if (HasUnsupportedVop3InputModifier(inst.src[0]) ||
        HasUnsupportedVop3InputModifier(inst.src[1]) ||
        HasUnsupportedVop3OutputModifier(inst.dst[0], false)) {
        throw std::runtime_error("V_SUBREV_I32 carry-out subset rejects unrepresented modifiers");
    }
    const IR::U32 src0{GetSrc(inst.src[0])};
    const IR::U32 src1{GetSrc(inst.src[1])};
    const IR::U32 result{ir.ISub(src1, src0)};
    SetDst(inst.dst[0], result);

    const IR::U1 did_underflow{ir.IGreaterThan(src0, src1, false)};
    SetCarryOut(inst, did_underflow);
}

void Translator::V_ADD_CO_U32(const GcnInst& inst) {
    if (!profile.supports_vop3_co_u32_subset) {
        throw std::runtime_error("VOP3B ADD_CO_U32 is disabled for this shader profile");
    }
    ValidateVop3CoU32(inst);
    const IR::U32 src0{GetSrc(inst.src[0])};
    const IR::U32 src1{GetSrc(inst.src[1])};
    const IR::U32 result{ir.IAdd(src0, src1)};
    const IR::U1 did_carry{ir.ILessThan(result, src0, false)};
    const IR::U32 clamped_result{inst.dst[0].output_modifier.clamp
                                     ? IR::U32{ir.Select(did_carry, ir.Imm32(0xFFFFFFFFU), result)}
                                     : result};
    SetDst(inst.dst[0], clamped_result);
    SetCarryOut(inst, did_carry);
}

void Translator::V_SUB_CO_U32(const GcnInst& inst) {
    if (!profile.supports_vop3_co_u32_subset) {
        throw std::runtime_error("VOP3B SUB_CO_U32 is disabled for this shader profile");
    }
    ValidateVop3CoU32(inst);
    const IR::U32 src0{GetSrc(inst.src[0])};
    const IR::U32 src1{GetSrc(inst.src[1])};
    const IR::U32 result{ir.ISub(src0, src1)};
    // The ISA's unsigned borrow mask is true when src1 > src0. CLMP saturates
    // only the vector result; the scalar mask still reports the original borrow.
    const IR::U1 did_borrow{ir.IGreaterThan(src1, src0, false)};
    const IR::U32 clamped_result{inst.dst[0].output_modifier.clamp
                                     ? IR::U32{ir.Select(did_borrow, ir.Imm32(0), result)}
                                     : result};
    SetDst(inst.dst[0], clamped_result);
    SetCarryOut(inst, did_borrow);
}

void Translator::V_SUBREV_CO_U32(const GcnInst& inst) {
    if (!profile.supports_vop3_co_u32_subset) {
        throw std::runtime_error("VOP3B SUBREV_CO_U32 is disabled for this shader profile");
    }
    ValidateVop3CoU32(inst);
    const IR::U32 src0{GetSrc(inst.src[0])};
    const IR::U32 src1{GetSrc(inst.src[1])};
    const IR::U32 result{ir.ISub(src1, src0)};
    const IR::U1 did_borrow{ir.IGreaterThan(src0, src1, false)};
    const IR::U32 clamped_result{inst.dst[0].output_modifier.clamp
                                     ? IR::U32{ir.Select(did_borrow, ir.Imm32(0), result)}
                                     : result};
    SetDst(inst.dst[0], clamped_result);
    SetCarryOut(inst, did_borrow);
}

void Translator::V_ADDC_U32(const GcnInst& inst) {
    ValidateCarryInU32(inst);
    const IR::U32 src0{GetSrc(inst.src[0])};
    const IR::U32 src1{GetSrc(inst.src[1])};
    const IR::U32 carry{GetCarryIn(inst)};
    const IR::U32 result1{ir.IAdd(src0, src1)};
    const IR::U32 result2{ir.IAdd(result1, carry)};
    const IR::U1 carry_out1{ir.ILessThan(result1, src0, false)};
    const IR::U1 carry_out2{ir.ILessThan(result2, result1, false)};
    const IR::U1 did_overflow{ir.LogicalOr(carry_out1, carry_out2)};
    const IR::U32 clamped_result{
        inst.dst[0].output_modifier.clamp
            ? IR::U32{ir.Select(did_overflow, ir.Imm32(0xFFFFFFFFU), result2)}
            : result2};
    SetDst(inst.dst[0], clamped_result);
    SetCarryOut(inst, did_overflow);
}

void Translator::V_SUBB_U32(const GcnInst& inst) {
    ValidateCarryInU32(inst);
    const IR::U32 src0{GetSrc(inst.src[0])};
    const IR::U32 src1{GetSrc(inst.src[1])};
    const IR::U32 carry{GetCarryIn(inst)};
    const IR::U32 result{ir.ISub(ir.ISub(src0, src1), carry)};
    const IR::U1 underflow{ir.IGreaterThan(src1, src0, false)};
    const IR::U32 difference{ir.ISub(src0, src1)};
    const IR::U1 borrow_underflow{ir.IGreaterThan(carry, difference, false)};
    const IR::U1 did_borrow{ir.LogicalOr(underflow, borrow_underflow)};
    const IR::U32 clamped_result{inst.dst[0].output_modifier.clamp
                                     ? IR::U32{ir.Select(did_borrow, ir.Imm32(0), result)}
                                     : result};
    SetDst(inst.dst[0], clamped_result);
    SetCarryOut(inst, did_borrow);
}

void Translator::V_SUBBREV_U32(const GcnInst& inst) {
    ValidateCarryInU32(inst);
    const IR::U32 src0{GetSrc(inst.src[0])};
    const IR::U32 src1{GetSrc(inst.src[1])};
    const IR::U32 carry{GetCarryIn(inst)};
    const IR::U32 result{ir.ISub(ir.ISub(src1, src0), carry)};
    const IR::U1 underflow{ir.IGreaterThan(src0, src1, false)};
    const IR::U32 difference{ir.ISub(src1, src0)};
    const IR::U1 borrow_underflow{ir.IGreaterThan(carry, difference, false)};
    const IR::U1 did_borrow{ir.LogicalOr(underflow, borrow_underflow)};
    const IR::U32 clamped_result{inst.dst[0].output_modifier.clamp
                                     ? IR::U32{ir.Select(did_borrow, ir.Imm32(0), result)}
                                     : result};
    SetDst(inst.dst[0], clamped_result);
    SetCarryOut(inst, did_borrow);
}

void Translator::V_LDEXP_F32(const GcnInst& inst) {
    const IR::F32 src0{GetSrc<IR::F32>(inst.src[0])};
    const IR::U32 src1{GetSrc(inst.src[1])};
    SetDst(inst.dst[0], ir.FPLdexp(src0, src1));
}

void Translator::V_CVT_PKNORM_U16_F32(const GcnInst& inst) {
    const IR::Value vec_f32 =
        ir.CompositeConstruct(GetSrc<IR::F32>(inst.src[0]), GetSrc<IR::F32>(inst.src[1]));
    SetDst(inst.dst[0], ir.Pack2x16(AmdGpu::NumberFormat::Unorm, vec_f32));
}

void Translator::V_CVT_PKNORM_I16_F32(const GcnInst& inst) {
    const IR::Value vec_f32 =
        ir.CompositeConstruct(GetSrc<IR::F32>(inst.src[0]), GetSrc<IR::F32>(inst.src[1]));
    SetDst(inst.dst[0], ir.Pack2x16(AmdGpu::NumberFormat::Snorm, vec_f32));
}

void Translator::V_CVT_PKRTZ_F16_F32(const GcnInst& inst) {
    const IR::Value vec_f32 =
        ir.CompositeConstruct(GetSrc<IR::F32>(inst.src[0]), GetSrc<IR::F32>(inst.src[1]));
    SetDst(inst.dst[0], ir.Pack2x16(AmdGpu::NumberFormat::Float, vec_f32));
}

void Translator::V_ADD_F16(const GcnInst& inst) {
    const auto src0 = GetSrc16<IR::F32>(inst.src[0], true);
    const auto src1 = GetSrc16<IR::F32>(inst.src[1], true);

    const auto result = ir.FPAdd(src0, src1);

    SetDst16(inst.dst[0], result, true);
}

void Translator::V_SUB_F16(const GcnInst& inst) {
    const auto src0 = GetSrc16<IR::F32>(inst.src[0], true);
    const auto src1 = GetSrc16<IR::F32>(inst.src[1], true);

    const auto result = ir.FPSub(src0, src1);

    SetDst16(inst.dst[0], result, true);
}

void Translator::V_SUBREV_F16(const GcnInst& inst) {
    const auto src0 = GetSrc16<IR::F32>(inst.src[0], true);
    const auto src1 = GetSrc16<IR::F32>(inst.src[1], true);

    const auto result = ir.FPSub(src1, src0);

    SetDst16(inst.dst[0], result, true);
}

void Translator::V_MUL_F16(const GcnInst& inst) {
    const auto src0 = GetSrc16<IR::F32>(inst.src[0], true);
    const auto src1 = GetSrc16<IR::F32>(inst.src[1], true);

    const auto result = ir.FPMul(src0, src1);

    SetDst16(inst.dst[0], result, true);
}

void Translator::V_MAX_F16(const GcnInst& inst) {
    const auto src0 = GetSrc16<IR::F32>(inst.src[0], true);
    const auto src1 = GetSrc16<IR::F32>(inst.src[1], true);

    const auto result = ir.FPMax(src0, src1);

    SetDst16(inst.dst[0], result, true);
}

void Translator::V_MIN_F16(const GcnInst& inst) {
    const auto src0 = GetSrc16<IR::F32>(inst.src[0], true);
    const auto src1 = GetSrc16<IR::F32>(inst.src[1], true);

    const auto result = ir.FPMin(src0, src1);

    SetDst16(inst.dst[0], result, true);
}

// VOP1

void Translator::V_MOV(const GcnInst& inst) {
    SetDst(inst.dst[0], GetSrc<IR::F32>(inst.src[0]));
}

void Translator::V_CVT_I32_F64(const GcnInst& inst) {
    const IR::F64 src0{GetSrc64<IR::F64>(inst.src[0])};
    SetDst(inst.dst[0], ir.ConvertFToS(32, src0));
}

void Translator::V_CVT_F64_I32(const GcnInst& inst) {
    const IR::U32 src0{GetSrc(inst.src[0])};
    SetDst64(inst.dst[0], ir.ConvertSToF(64, 32, src0));
}

void Translator::V_CVT_F64_U32(const GcnInst& inst) {
    const IR::U32 src0{GetSrc(inst.src[0])};
    SetDst64(inst.dst[0], ir.ConvertUToF(64, 32, src0));
}

void Translator::V_CVT_F32_I32(const GcnInst& inst) {
    const IR::U32 src0{GetSrc(inst.src[0])};
    SetDst(inst.dst[0], ir.ConvertSToF(32, 32, src0));
}

void Translator::V_CVT_F32_U32(const GcnInst& inst) {
    const IR::U32 src0{GetSrc(inst.src[0])};
    SetDst(inst.dst[0], ir.ConvertUToF(32, 32, src0));
}

void Translator::V_CVT_U32_F32(const GcnInst& inst) {
    const IR::F32 src0{GetSrc<IR::F32>(inst.src[0])};
    SetDst(inst.dst[0], ir.ConvertFToU(32, src0));
}

void Translator::V_CVT_I32_F32(const GcnInst& inst) {
    const IR::F32 src0{GetSrc<IR::F32>(inst.src[0])};
    SetDst(inst.dst[0], ir.ConvertFToS(32, src0));
}

void Translator::V_CVT_F16_F32(const GcnInst& inst) {
    const IR::F32 src0 = GetSrc<IR::F32>(inst.src[0]);

    IR::U32 src0fp16;
    if (profile.support_float16) {
        const IR::F16 converted = ir.FPConvert(16, src0);
        src0fp16 = ir.UConvert(32, ir.BitCast<IR::U16>(converted));
    } else {
        const IR::U32 packed =
            ir.Pack2x16(AmdGpu::NumberFormat::Float, ir.CompositeConstruct(src0, ir.Imm32(0.f)));
        src0fp16 = ir.BitFieldExtract(packed, ir.Imm32(0U), ir.Imm32(16U));
    }

    SetDst(inst.dst[0], src0fp16);
}

void Translator::V_CVT_F32_F16(const GcnInst& inst) {
    const IR::U32 src0 = GetSrc(inst.src[0]);

    IR::F32 src0l;
    if (profile.support_float16) {
        const IR::U16 converted = ir.UConvert(16, src0);
        src0l = ir.FPConvert(32, ir.BitCast<IR::F16>(converted));
    } else {
        const IR::Value unpacked = ir.Unpack2x16(AmdGpu::NumberFormat::Float, src0);
        src0l = IR::F32{ir.CompositeExtract(unpacked, 0)};
    }

    SetDst(inst.dst[0], src0l);
}

void Translator::V_CVT_RPI_I32_F32(const GcnInst& inst) {
    const IR::F32 src0{GetSrc<IR::F32>(inst.src[0])};
    SetDst(inst.dst[0], ir.ConvertFToI(32, true, ir.FPFloor(ir.FPAdd(src0, ir.Imm32(0.5f)))));
}

void Translator::V_CVT_FLR_I32_F32(const GcnInst& inst) {
    const IR::F32 src0{GetSrc<IR::F32>(inst.src[0])};
    SetDst(inst.dst[0], ir.ConvertFToI(32, true, ir.FPFloor(src0)));
}

void Translator::V_CVT_OFF_F32_I4(const GcnInst& inst) {
    const IR::U32 src0{GetSrc(inst.src[0])};
    ASSERT(src0.IsImmediate());
    static constexpr std::array IntToFloat = {
        0.0f,     0.0625f,  0.1250f,  0.1875f,  0.2500f,  0.3125f,  0.3750f,  0.4375f,
        -0.5000f, -0.4375f, -0.3750f, -0.3125f, -0.2500f, -0.1875f, -0.1250f, -0.0625f};
    SetDst(inst.dst[0], ir.Imm32(IntToFloat[src0.U32() & 0xF]));
}

void Translator::V_CVT_F32_F64(const GcnInst& inst) {
    const IR::F64 src0{GetSrc64<IR::F64>(inst.src[0])};
    SetDst(inst.dst[0], ir.FPConvert(32, src0));
}

void Translator::V_CVT_F64_F32(const GcnInst& inst) {
    const IR::F32 src0{GetSrc<IR::F32>(inst.src[0])};
    SetDst64(inst.dst[0], ir.FPConvert(64, src0));
}

void Translator::V_CVT_F32_UBYTE(u32 index, const GcnInst& inst) {
    const IR::U32 src0{GetSrc(inst.src[0])};
    const IR::U32 byte = ir.BitFieldExtract(src0, ir.Imm32(8 * index), ir.Imm32(8));
    SetDst(inst.dst[0], ir.ConvertUToF(32, 32, byte));
}

void Translator::V_TRUNC_F64(const GcnInst& inst) {
    const IR::F64 src0{GetSrc64<IR::F64>(inst.src[0])};
    SetDst64(inst.dst[0], ir.FPTrunc(src0));
}

void Translator::V_FLOOR_F64(const GcnInst& inst) {
    const IR::F64 src0{GetSrc64<IR::F64>(inst.src[0])};
    SetDst64(inst.dst[0], ir.FPFloor(src0));
}

void Translator::V_FRACT_F32(const GcnInst& inst) {
    const IR::F32 src0{GetSrc<IR::F32>(inst.src[0])};
    SetDst(inst.dst[0], ir.FPFract(src0));
}

void Translator::V_TRUNC_F32(const GcnInst& inst) {
    const IR::F32 src0{GetSrc<IR::F32>(inst.src[0])};
    SetDst(inst.dst[0], ir.FPTrunc(src0));
}

void Translator::V_CEIL_F32(const GcnInst& inst) {
    const IR::F32 src0{GetSrc<IR::F32>(inst.src[0])};
    SetDst(inst.dst[0], ir.FPCeil(src0));
}

void Translator::V_RNDNE_F32(const GcnInst& inst) {
    const IR::F32 src0{GetSrc<IR::F32>(inst.src[0])};
    SetDst(inst.dst[0], ir.FPRoundEven(src0));
}

void Translator::V_FLOOR_F32(const GcnInst& inst) {
    const IR::F32 src0{GetSrc<IR::F32>(inst.src[0])};
    SetDst(inst.dst[0], ir.FPFloor(src0));
}

void Translator::V_EXP_F32(const GcnInst& inst) {
    const IR::F32 src0{GetSrc<IR::F32>(inst.src[0])};
    SetDst(inst.dst[0], ir.FPExp2(src0));
}

void Translator::V_LOG_CLAMP_F32(const GcnInst& inst) {
    const IR::F32 src0{GetSrc<IR::F32>(inst.src[0])};
    const IR::F32 result{ir.FPLog2(src0)};
    const IR::F32 max_val{ir.Imm32(std::numeric_limits<float>::max())};
    const IR::F32 min_val{ir.Imm32(-std::numeric_limits<float>::max())};
    SetDst(inst.dst[0], ir.FPClamp(result, min_val, max_val));
}

void Translator::V_LOG_F32(const GcnInst& inst) {
    const IR::F32 src0{GetSrc<IR::F32>(inst.src[0])};
    SetDst(inst.dst[0], ir.FPLog2(src0));
}

void Translator::V_RCP_F32(const GcnInst& inst) {
    const IR::F32 src0{GetSrc<IR::F32>(inst.src[0])};
    SetDst(inst.dst[0], ir.FPRecip(src0));
}

void Translator::V_RCP_LEGACY_F32(const GcnInst& inst) {
    const IR::F32 src0{GetSrc<IR::F32>(inst.src[0])};
    const auto result = ir.FPRecip(src0);
    const auto inf = ir.FPIsInf(result);

    const auto raw_result = ir.ConvertFToU(32, result);
    const auto sign_bit = ir.ShiftRightLogical(raw_result, ir.Imm32(31u));
    const auto sign_bit_set = ir.INotEqual(sign_bit, ir.Imm32(0u));
    const IR::F32 inf_result{ir.Select(sign_bit_set, ir.Imm32(-0.0f), ir.Imm32(0.0f))};
    const IR::F32 val{ir.Select(inf, inf_result, result)};

    SetDst(inst.dst[0], val);
}

void Translator::V_RCP_F64(const GcnInst& inst) {
    const IR::F64 src0{GetSrc64<IR::F64>(inst.src[0])};
    SetDst64(inst.dst[0], ir.FPRecip(src0));
}

void Translator::V_RSQ_LEGACY_F32(const GcnInst& inst) {
    const IR::F32 src0{GetSrc<IR::F32>(inst.src[0])};
    const IR::F32 result{ir.FPRecipSqrt(src0)};
    const IR::F32 max_val{ir.Imm32(std::numeric_limits<float>::max())};
    const IR::F32 min_val{ir.Imm32(-std::numeric_limits<float>::max())};
    const IR::F32 clamped{ir.FPClamp(result, min_val, max_val)};
    const IR::F32 zero{ir.Imm32(0.0f)};
    const IR::F32 output{ir.Select(ir.FPEqual(src0, zero), zero, clamped)};
    SetDst(inst.dst[0], output);
}

void Translator::V_RSQ_F32(const GcnInst& inst) {
    const IR::F32 src0{GetSrc<IR::F32>(inst.src[0])};
    SetDst(inst.dst[0], ir.FPRecipSqrt(src0));
}

void Translator::V_RCP_CLAMP_F32(const GcnInst& inst) {
    const IR::F32 src0{GetSrc<IR::F32>(inst.src[0])};
    const IR::F32 result{ir.FPRecip(src0)};
    const IR::F32 max_val{ir.Imm32(std::numeric_limits<float>::max())};
    const IR::F32 min_val{ir.Imm32(-std::numeric_limits<float>::max())};
    SetDst(inst.dst[0], ir.FPClamp(result, min_val, max_val));
}

void Translator::V_RSQ_CLAMP_F32(const GcnInst& inst) {
    const IR::F32 src0{GetSrc<IR::F32>(inst.src[0])};
    const IR::F32 result{ir.FPRecipSqrt(src0)};
    const IR::F32 max_val{ir.Imm32(std::numeric_limits<float>::max())};
    const IR::F32 min_val{ir.Imm32(-std::numeric_limits<float>::max())};
    SetDst(inst.dst[0], ir.FPClamp(result, min_val, max_val));
}

void Translator::V_SQRT_F32(const GcnInst& inst) {
    const IR::F32 src0{GetSrc<IR::F32>(inst.src[0])};
    SetDst(inst.dst[0], ir.FPSqrt(src0));
}

void Translator::V_SIN_F32(const GcnInst& inst) {
    const IR::F32 src0{GetSrc<IR::F32>(inst.src[0])};
    SetDst(inst.dst[0], ir.FPSin(src0));
}

void Translator::V_COS_F32(const GcnInst& inst) {
    const IR::F32 src0{GetSrc<IR::F32>(inst.src[0])};
    SetDst(inst.dst[0], ir.FPCos(src0));
}

void Translator::V_NOT_B32(const GcnInst& inst) {
    const IR::U32 src0{GetSrc(inst.src[0])};
    SetDst(inst.dst[0], ir.BitwiseNot(src0));
}

void Translator::V_BFREV_B32(const GcnInst& inst) {
    const IR::U32 src0{GetSrc(inst.src[0])};
    SetDst(inst.dst[0], ir.BitReverse(src0));
}

void Translator::V_FFBH_U32(const GcnInst& inst) {
    const IR::U32 src0{GetSrc(inst.src[0])};
    // Gcn wants the MSB position counting from the left, but SPIR-V counts from the rightmost (LSB)
    // position
    const IR::U32 msb_pos = ir.FindUMsb(src0);
    const IR::U32 pos_from_left = ir.ISub(ir.Imm32(31), msb_pos);
    // Select 0xFFFFFFFF if src0 was 0
    const IR::U1 cond = ir.INotEqual(src0, ir.Imm32(0));
    SetDst(inst.dst[0], IR::U32{ir.Select(cond, pos_from_left, ir.Imm32(~0U))});
}

void Translator::V_FFBL_B32(const GcnInst& inst) {
    const IR::U32 src0{GetSrc(inst.src[0])};
    SetDst(inst.dst[0], ir.FindILsb(src0));
}

void Translator::V_FFBH_I32(const GcnInst& inst) {
    const IR::U32 src0{GetSrc(inst.src[0])};
    // Gcn wants the MSB position counting from the left, but SPIR-V counts from the rightmost (LSB)
    // position
    const IR::U32 msb_pos = ir.FindSMsb(src0);
    const IR::U32 pos_from_left = ir.ISub(ir.Imm32(31), msb_pos);
    // Select 0xFFFFFFFF if src0 was 0 or -1
    const IR::U32 minusOne = ir.Imm32(~0U);
    const IR::U1 cond =
        ir.LogicalAnd(ir.INotEqual(src0, ir.Imm32(0)), ir.INotEqual(src0, minusOne));
    SetDst(inst.dst[0], IR::U32{ir.Select(cond, pos_from_left, minusOne)});
}

void Translator::V_FREXP_EXP_I32_F64(const GcnInst& inst) {
    const IR::F64 src0{GetSrc64<IR::F64>(inst.src[0])};
    SetDst(inst.dst[0], ir.FPFrexpExp(src0));
}

void Translator::V_FREXP_MANT_F64(const GcnInst& inst) {
    const IR::F64 src0{GetSrc64<IR::F64>(inst.src[0])};
    SetDst64(inst.dst[0], ir.FPFrexpSig(src0));
}

void Translator::V_FRACT_F64(const GcnInst& inst) {
    const IR::F64 src0{GetSrc64<IR::F64>(inst.src[0])};
    SetDst64(inst.dst[0], ir.FPFract(src0));
}

void Translator::V_FREXP_EXP_I32_F32(const GcnInst& inst) {
    const IR::F32 src0{GetSrc<IR::F32>(inst.src[0])};
    SetDst(inst.dst[0], ir.FPFrexpExp(src0));
}

void Translator::V_FREXP_MANT_F32(const GcnInst& inst) {
    const IR::F32 src0{GetSrc<IR::F32>(inst.src[0])};
    SetDst(inst.dst[0], ir.FPFrexpSig(src0));
}

void Translator::V_MOVRELD_B32(const GcnInst& inst) {
    const IR::U32 src_val{GetSrc(inst.src[0])};
    u32 dst_vgprno = inst.dst[0].code - static_cast<u32>(IR::VectorReg::V0);
    IR::U32 m0 = ir.GetM0();

    VMovRelDHelper(dst_vgprno, src_val, m0);
}

void Translator::V_MOVRELS_B32(const GcnInst& inst) {
    u32 src_vgprno = inst.src[0].code - static_cast<u32>(IR::VectorReg::V0);
    const IR::U32 m0 = ir.GetM0();

    const IR::U32 src_val = VMovRelSHelper(src_vgprno, m0);
    SetDst(inst.dst[0], src_val);
}

void Translator::V_MOVRELSD_B32(const GcnInst& inst) {
    u32 src_vgprno = inst.src[0].code - static_cast<u32>(IR::VectorReg::V0);
    u32 dst_vgprno = inst.dst[0].code - static_cast<u32>(IR::VectorReg::V0);
    IR::U32 m0 = ir.GetM0();

    const IR::U32 src_val = VMovRelSHelper(src_vgprno, m0);
    VMovRelDHelper(dst_vgprno, src_val, m0);
}

// VOPC

void Translator::V_CMP_F32(ConditionOp op, bool ordered, bool set_exec, const GcnInst& inst) {
    const IR::F32 src0{GetSrc<IR::F32>(inst.src[0])};
    const IR::F32 src1{GetSrc<IR::F32>(inst.src[1])};
    const IR::U1 result = [&] {
        switch (op) {
        case ConditionOp::F:
            return ir.Imm1(false);
        case ConditionOp::EQ:
            return ir.FPEqual(src0, src1, ordered);
        case ConditionOp::LG:
            return ir.FPNotEqual(src0, src1, ordered);
        case ConditionOp::GT:
            return ir.FPGreaterThan(src0, src1, ordered);
        case ConditionOp::LT:
            return ir.FPLessThan(src0, src1, ordered);
        case ConditionOp::LE:
            return ir.FPLessThanEqual(src0, src1, ordered);
        case ConditionOp::GE:
            return ir.FPGreaterThanEqual(src0, src1, ordered);
        case ConditionOp::U:
            return ir.LogicalOr(ir.FPIsNan(src0), ir.FPIsNan(src1));
        default:
            UNREACHABLE();
        }
    }();
    if (set_exec) {
        // V_CMPX evaluates on active lanes only; hardware writes exec & result to both EXEC
        // and the VCC/SDST destination, zeroing inactive lanes' bits.
        const IR::U1 masked{ir.LogicalAnd(ir.GetExec(), result)};
        ir.SetExec(masked);
        SetDst64(inst.dst[1], ir.Ballot(masked));
        return;
    }
    SetDst64(inst.dst[1], ir.Ballot(result));
}

void Translator::V_CMP_F64(ConditionOp op, bool ordered, bool set_exec, const GcnInst& inst) {
    const IR::F64 src0{GetSrc64<IR::F64>(inst.src[0])};
    const IR::F64 src1{GetSrc64<IR::F64>(inst.src[1])};
    const IR::U1 result = [&] {
        switch (op) {
        case ConditionOp::F:
            return ir.Imm1(false);
        case ConditionOp::EQ:
            return ir.FPEqual(src0, src1, ordered);
        case ConditionOp::LG:
            return ir.FPNotEqual(src0, src1, ordered);
        case ConditionOp::GT:
            return ir.FPGreaterThan(src0, src1, ordered);
        case ConditionOp::LT:
            return ir.FPLessThan(src0, src1, ordered);
        case ConditionOp::LE:
            return ir.FPLessThanEqual(src0, src1, ordered);
        case ConditionOp::GE:
            return ir.FPGreaterThanEqual(src0, src1, ordered);
        case ConditionOp::U:
            return ir.LogicalOr(ir.FPIsNan(src0), ir.FPIsNan(src1));
        default:
            UNREACHABLE();
        }
    }();
    if (set_exec) {
        // See the V_CMPX note in V_CMP_F32.
        const IR::U1 masked{ir.LogicalAnd(ir.GetExec(), result)};
        ir.SetExec(masked);
        SetDst64(inst.dst[1], ir.Ballot(masked));
        return;
    }
    SetDst64(inst.dst[1], ir.Ballot(result));
}

void Translator::V_CMP_U32(ConditionOp op, bool is_signed, bool set_exec, const GcnInst& inst) {
    const IR::U32 src0{GetSrc(inst.src[0])};
    const IR::U32 src1{GetSrc(inst.src[1])};
    const IR::U1 result = [&] {
        switch (op) {
        case ConditionOp::F:
            return ir.Imm1(false);
        case ConditionOp::TRU:
            return ir.Imm1(true);
        case ConditionOp::EQ:
            return ir.IEqual(src0, src1);
        case ConditionOp::LG:
            return ir.INotEqual(src0, src1);
        case ConditionOp::GT:
            return ir.IGreaterThan(src0, src1, is_signed);
        case ConditionOp::LT:
            return ir.ILessThan(src0, src1, is_signed);
        case ConditionOp::LE:
            return ir.ILessThanEqual(src0, src1, is_signed);
        case ConditionOp::GE:
            return ir.IGreaterThanEqual(src0, src1, is_signed);
        default:
            UNREACHABLE();
        }
    }();
    if (set_exec) {
        // See the V_CMPX note in V_CMP_F32.
        const IR::U1 masked{ir.LogicalAnd(ir.GetExec(), result)};
        ir.SetExec(masked);
        SetDst64(inst.dst[1], ir.Ballot(masked));
        return;
    }
    SetDst64(inst.dst[1], ir.Ballot(result));
}

void Translator::V_CMP_U64(ConditionOp op, bool is_signed, bool set_exec, const GcnInst& inst) {
    const IR::U64 src0{GetSrc64(inst.src[0])};
    const IR::U64 src1{GetSrc64(inst.src[1])};
    const IR::U1 result = [&] {
        switch (op) {
        case ConditionOp::F:
            return ir.Imm1(false);
        case ConditionOp::TRU:
            return ir.Imm1(true);
        case ConditionOp::EQ:
            return ir.IEqual(src0, src1);
        case ConditionOp::LG:
            return ir.INotEqual(src0, src1);
        case ConditionOp::GT:
            return ir.IGreaterThan(src0, src1, is_signed);
        case ConditionOp::LT:
            return ir.ILessThan(src0, src1, is_signed);
        case ConditionOp::LE:
            return ir.ILessThanEqual(src0, src1, is_signed);
        case ConditionOp::GE:
            return ir.IGreaterThanEqual(src0, src1, is_signed);
        default:
            UNREACHABLE();
        }
    }();
    if (set_exec) {
        // See the V_CMPX note in V_CMP_F32.
        const IR::U1 masked{ir.LogicalAnd(ir.GetExec(), result)};
        ir.SetExec(masked);
        SetDst64(inst.dst[1], ir.Ballot(masked));
        return;
    }
    SetDst64(inst.dst[1], ir.Ballot(result));
}

void Translator::V_CMP_CLASS_F32(const GcnInst& inst) {
    const IR::F32 src0{GetSrc<IR::F32>(inst.src[0])};
    const IR::U32 src1{GetSrc(inst.src[1])};
    IR::U1 value;
    if (src1.IsImmediate()) {
        const auto class_mask = static_cast<IR::FloatClassFunc>(src1.U32());
        if ((class_mask & IR::FloatClassFunc::NaN) == IR::FloatClassFunc::NaN) {
            value = ir.FPIsNan(src0);
        } else if ((class_mask & IR::FloatClassFunc::Infinity) == IR::FloatClassFunc::Infinity) {
            value = ir.FPIsInf(src0);
        } else if ((class_mask & IR::FloatClassFunc::Negative) == IR::FloatClassFunc::Negative) {
            value = ir.FPLessThanEqual(src0, ir.Imm32(-0.f));
        } else {
            UNREACHABLE_MSG("Unsupported float class mask: {:#x}", static_cast<u32>(class_mask));
        }
    } else {
        // We don't know the type yet, delay its resolution.
        value = ir.FPCmpClass32(src0, src1);
    }
    SetDst64(inst.dst[1], ir.Ballot(value));
}

// VOP3a

void Translator::V_MAD_F32(const GcnInst& inst) {
    const IR::F32 src0{GetSrc<IR::F32>(inst.src[0])};
    const IR::F32 src1{GetSrc<IR::F32>(inst.src[1])};
    const IR::F32 src2{GetSrc<IR::F32>(inst.src[2])};
    SetDst(inst.dst[0], ir.FPAdd(ir.FPMul(src0, src1), src2));
}

void Translator::V_MAD_I32_I24(const GcnInst& inst, bool is_signed) {
    const IR::U32 src0{
        ir.BitFieldExtract(GetSrc(inst.src[0]), ir.Imm32(0), ir.Imm32(24), is_signed)};
    const IR::U32 src1{
        ir.BitFieldExtract(GetSrc(inst.src[1]), ir.Imm32(0), ir.Imm32(24), is_signed)};
    const IR::U32 src2{GetSrc(inst.src[2])};
    SetDst(inst.dst[0], ir.IAdd(ir.IMul(src0, src1), src2));
}

void Translator::V_MAD_U32_U24(const GcnInst& inst) {
    V_MAD_I32_I24(inst, false);
}

void Translator::V_CUBEID_F32(const GcnInst& inst) {
    const auto x = GetSrc<IR::F32>(inst.src[0]);
    const auto y = GetSrc<IR::F32>(inst.src[1]);
    const auto z = GetSrc<IR::F32>(inst.src[2]);

    SetDst(inst.dst[0], ir.CubeFaceIndex(x, y, z));
}

void Translator::V_CUBESC_F32(const GcnInst& inst) {
    const auto x = GetSrc<IR::F32>(inst.src[0]);
    const auto y = GetSrc<IR::F32>(inst.src[1]);
    const auto z = GetSrc<IR::F32>(inst.src[2]);

    SetDst(inst.dst[0], ir.CubeFaceCoordS(x, y, z));
}

void Translator::V_CUBETC_F32(const GcnInst& inst) {
    const auto x = GetSrc<IR::F32>(inst.src[0]);
    const auto y = GetSrc<IR::F32>(inst.src[1]);
    const auto z = GetSrc<IR::F32>(inst.src[2]);

    SetDst(inst.dst[0], ir.CubeFaceCoordT(x, y, z));
}

void Translator::V_CUBEMA_F32(const GcnInst& inst) {
    const auto x = GetSrc<IR::F32>(inst.src[0]);
    const auto y = GetSrc<IR::F32>(inst.src[1]);
    const auto z = GetSrc<IR::F32>(inst.src[2]);

    SetDst(inst.dst[0], ir.CubeFaceMajorAxis(x, y, z));
}

void Translator::V_BFE_U32(bool is_signed, const GcnInst& inst) {
    const IR::U32 src0{GetSrc(inst.src[0])};
    IR::U32 src1{GetSrc(inst.src[1])};
    IR::U32 src2{GetSrc(inst.src[2])};
    if (!src1.IsImmediate()) {
        src1 = ir.BitwiseAnd(src1, ir.Imm32(0x1F));
    }
    if (!src2.IsImmediate()) {
        src2 = ir.BitwiseAnd(src2, ir.Imm32(0x1F));
    }
    SetDst(inst.dst[0], ir.BitFieldExtract(src0, src1, src2, is_signed));
}

void Translator::V_BFI_B32(const GcnInst& inst) {
    const IR::U32 src0{GetSrc(inst.src[0])};
    const IR::U32 src1{GetSrc(inst.src[1])};
    const IR::U32 src2{GetSrc(inst.src[2])};
    SetDst(inst.dst[0],
           ir.BitwiseOr(ir.BitwiseAnd(src0, src1), ir.BitwiseAnd(ir.BitwiseNot(src0), src2)));
}

void Translator::V_FMA_F32(const GcnInst& inst) {
    const IR::F32 src0{GetSrc<IR::F32>(inst.src[0])};
    const IR::F32 src1{GetSrc<IR::F32>(inst.src[1])};
    const IR::F32 src2{GetSrc<IR::F32>(inst.src[2])};
    SetDst(inst.dst[0], ir.FPFma(src0, src1, src2));
}

void Translator::V_FMA_F64(const GcnInst& inst) {
    const IR::F64 src0{GetSrc64<IR::F64>(inst.src[0])};
    const IR::F64 src1{GetSrc64<IR::F64>(inst.src[1])};
    const IR::F64 src2{GetSrc64<IR::F64>(inst.src[2])};
    SetDst64(inst.dst[0], ir.FPFma(src0, src1, src2));
}

void Translator::V_LERP_U8(const GcnInst& inst) {
    const IR::U32 src0{GetSrc<IR::U32>(inst.src[0])};
    const IR::U32 src1{GetSrc<IR::U32>(inst.src[1])};
    const IR::U32 src2{GetSrc<IR::U32>(inst.src[2])};
    IR::U32 result{ir.Imm32(0U)};
    for (u32 shift = 0; shift < 32; shift += 8) {
        const IR::U32 lhs{ir.BitFieldExtract(src0, ir.Imm32(shift), ir.Imm32(8U))};
        const IR::U32 rhs{ir.BitFieldExtract(src1, ir.Imm32(shift), ir.Imm32(8U))};
        // S2 contributes the low bit of each corresponding byte as the tie-rounding control.
        const IR::U32 round_up{ir.BitFieldExtract(src2, ir.Imm32(shift), ir.Imm32(1U))};
        const IR::U32 sum{ir.IAdd(ir.IAdd(lhs, rhs), round_up)};
        const IR::U32 average{ir.ShiftRightLogical(sum, ir.Imm32(1U))};
        result = ir.BitwiseOr(result, ir.ShiftLeftLogical(average, ir.Imm32(shift)));
    }
    SetDst(inst.dst[0], result);
}

void Translator::V_MIN3_F32(const GcnInst& inst) {
    const IR::F32 src0{GetSrc<IR::F32>(inst.src[0])};
    const IR::F32 src1{GetSrc<IR::F32>(inst.src[1])};
    const IR::F32 src2{GetSrc<IR::F32>(inst.src[2])};
    SetDst(inst.dst[0], ir.FPMinTri(src0, src1, src2));
}

void Translator::V_MIN3_U32(bool is_signed, const GcnInst& inst) {
    const IR::U32 src0{GetSrc(inst.src[0])};
    const IR::U32 src1{GetSrc(inst.src[1])};
    const IR::U32 src2{GetSrc(inst.src[2])};
    SetDst(inst.dst[0], ir.IMinTri(src0, src1, src2, is_signed));
}

void Translator::V_MAX3_F32(const GcnInst& inst) {
    const IR::F32 src0{GetSrc<IR::F32>(inst.src[0])};
    const IR::F32 src1{GetSrc<IR::F32>(inst.src[1])};
    const IR::F32 src2{GetSrc<IR::F32>(inst.src[2])};
    SetDst(inst.dst[0], ir.FPMaxTri(src0, src1, src2));
}

void Translator::V_MAX3_U32(bool is_signed, const GcnInst& inst) {
    const IR::U32 src0{GetSrc(inst.src[0])};
    const IR::U32 src1{GetSrc(inst.src[1])};
    const IR::U32 src2{GetSrc(inst.src[2])};
    SetDst(inst.dst[0], ir.IMaxTri(src0, src1, src2, is_signed));
}

void Translator::V_MED3_F32(const GcnInst& inst) {
    const IR::F32 src0{GetSrc<IR::F32>(inst.src[0])};
    const IR::F32 src1{GetSrc<IR::F32>(inst.src[1])};
    const IR::F32 src2{GetSrc<IR::F32>(inst.src[2])};
    SetDst(inst.dst[0], ir.FPMedTri(src0, src1, src2));
}

void Translator::V_MED3_U32(bool is_signed, const GcnInst& inst) {
    const IR::U32 src0{GetSrc(inst.src[0])};
    const IR::U32 src1{GetSrc(inst.src[1])};
    const IR::U32 src2{GetSrc(inst.src[2])};
    SetDst(inst.dst[0], ir.IMedTri(src0, src1, src2, is_signed));
}

void Translator::V_SAD(const GcnInst& inst) {
    const IR::U32 abs_diff = ir.IAbs(ir.ISub(GetSrc(inst.src[0]), GetSrc(inst.src[1])));
    SetDst(inst.dst[0], ir.IAdd(abs_diff, GetSrc(inst.src[2])));
}

void Translator::V_SAD_U32(const GcnInst& inst) {
    const IR::U32 src0{GetSrc(inst.src[0])};
    const IR::U32 src1{GetSrc(inst.src[1])};
    const IR::U32 src2{GetSrc(inst.src[2])};
    IR::U32 result;
    if (src0.IsImmediate() && src0.U32() == 0U) {
        result = src1;
    } else if (src1.IsImmediate() && src1.U32() == 0U) {
        result = src0;
    } else {
        const IR::U32 max{ir.IMax(src0, src1, false)};
        const IR::U32 min{ir.IMin(src0, src1, false)};
        result = ir.ISub(max, min);
    }
    SetDst(inst.dst[0], ir.IAdd(result, src2));
}

void Translator::V_SAD_U8(const GcnInst& inst, bool high_accumulate) {
    const IR::U32 src0{GetSrc<IR::U32>(inst.src[0])};
    const IR::U32 src1{GetSrc<IR::U32>(inst.src[1])};
    const IR::U32 src2{GetSrc<IR::U32>(inst.src[2])};
    const auto byte_at = [this](const IR::U32& value, u32 shift) {
        return IR::U32{ir.BitwiseAnd(ir.ShiftRightLogical(value, ir.Imm32(shift)), ir.Imm32(0xFF))};
    };
    const auto abs_diff = [this](const IR::U32& lhs, const IR::U32& rhs) {
        const IR::U32 max{ir.IMax(lhs, rhs, false)};
        const IR::U32 min{ir.IMin(lhs, rhs, false)};
        return IR::U32{ir.ISub(max, min)};
    };
    IR::U32 sad{ir.Imm32(0)};
    for (u32 shift = 0; shift < 32; shift += 8) {
        sad = ir.IAdd(sad, abs_diff(byte_at(src0, shift), byte_at(src1, shift)));
    }
    if (high_accumulate) {
        sad = ir.ShiftLeftLogical(sad, ir.Imm32(16));
    }
    SetDst(inst.dst[0], ir.IAdd(sad, src2));
}

void Translator::V_MSAD_U8(const GcnInst& inst) {
    const IR::U32 src0{GetSrc<IR::U32>(inst.src[0])};
    const IR::U32 src1{GetSrc<IR::U32>(inst.src[1])};
    const IR::U32 src2{GetSrc<IR::U32>(inst.src[2])};
    const auto byte_at = [this](const IR::U32& value, u32 shift) {
        return IR::U32{ir.BitwiseAnd(ir.ShiftRightLogical(value, ir.Imm32(shift)), ir.Imm32(0xFF))};
    };
    const auto abs_diff = [this](const IR::U32& lhs, const IR::U32& rhs) {
        const IR::U32 max{ir.IMax(lhs, rhs, false)};
        const IR::U32 min{ir.IMin(lhs, rhs, false)};
        return IR::U32{ir.ISub(max, min)};
    };
    IR::U32 sad{ir.Imm32(0)};
    for (u32 shift = 0; shift < 32; shift += 8) {
        const IR::U32 input_byte{byte_at(src0, shift)};
        const IR::U32 reference_byte{byte_at(src1, shift)};
        const IR::U32 term{ir.Select(ir.IEqual(reference_byte, ir.Imm32(0)), ir.Imm32(0),
                                     abs_diff(input_byte, reference_byte))};
        sad = ir.IAdd(sad, term);
    }
    SetDst(inst.dst[0], ir.IAdd(sad, src2));
}

void Translator::V_QSAD_PK_U16_U8(const GcnInst& inst, bool masked) {
    const IR::U64 src0{GetSrc64<IR::U64>(inst.src[0])};
    const IR::U32 src1{GetSrc<IR::U32>(inst.src[1])};
    const IR::U64 src2{GetSrc64<IR::U64>(inst.src[2])};
    const auto byte_at_u64 = [this](const IR::U64& value, u32 shift) {
        const IR::U64 shifted{ir.ShiftRightLogical(value, ir.Imm32(shift))};
        return IR::U32{ir.BitwiseAnd(ir.UConvert(32, shifted), ir.Imm32(0xFF))};
    };
    const auto byte_at_u32 = [this](const IR::U32& value, u32 shift) {
        return IR::U32{ir.BitwiseAnd(ir.ShiftRightLogical(value, ir.Imm32(shift)), ir.Imm32(0xFF))};
    };
    const auto abs_diff = [this](const IR::U32& lhs, const IR::U32& rhs) {
        const IR::U32 max{ir.IMax(lhs, rhs, false)};
        const IR::U32 min{ir.IMin(lhs, rhs, false)};
        return IR::U32{ir.ISub(max, min)};
    };

    IR::U64 packed{ir.Imm64(u64{0})};
    for (u32 lane = 0; lane < 4; ++lane) {
        IR::U32 sad{ir.Imm32(0)};
        for (u32 byte = 0; byte < 4; ++byte) {
            const IR::U32 input_byte{byte_at_u64(src0, (lane + byte) * 8)};
            const IR::U32 reference_byte{byte_at_u32(src1, byte * 8)};
            const IR::U32 difference{abs_diff(input_byte, reference_byte)};
            const IR::U32 term{masked ? IR::U32{ir.Select(ir.IEqual(reference_byte, ir.Imm32(0)),
                                                          ir.Imm32(0), difference)}
                                      : difference};
            sad = ir.IAdd(sad, term);
        }
        const IR::U32 accumulator{ir.BitwiseAnd(
            ir.UConvert(32, ir.ShiftRightLogical(src2, ir.Imm32(lane * 16))), ir.Imm32(0xFFFF))};
        const IR::U32 result16{ir.BitwiseAnd(ir.IAdd(sad, accumulator), ir.Imm32(0xFFFF))};
        const IR::U64 packed_lane{
            ir.ShiftLeftLogical(ir.UConvert(64, result16), ir.Imm32(lane * 16))};
        packed = ir.BitwiseOr(packed, packed_lane);
    }
    SetDst64(inst.dst[0], packed);
}

void Translator::V_MQSAD_U32_U8(const GcnInst& inst) {
    const IR::U64 src0{GetSrc64<IR::U64>(inst.src[0])};
    const IR::U32 src1{GetSrc<IR::U32>(inst.src[1])};
    const auto byte_at_u64 = [this](const IR::U64& value, u32 shift) {
        const IR::U64 shifted{ir.ShiftRightLogical(value, ir.Imm32(shift))};
        return IR::U32{ir.BitwiseAnd(ir.UConvert(32, shifted), ir.Imm32(0xFF))};
    };
    const auto byte_at_u32 = [this](const IR::U32& value, u32 shift) {
        return IR::U32{ir.BitwiseAnd(ir.ShiftRightLogical(value, ir.Imm32(shift)), ir.Imm32(0xFF))};
    };
    const auto abs_diff = [this](const IR::U32& lhs, const IR::U32& rhs) {
        const IR::U32 max{ir.IMax(lhs, rhs, false)};
        const IR::U32 min{ir.IMin(lhs, rhs, false)};
        return IR::U32{ir.ISub(max, min)};
    };
    const auto read_dword = [this](const InstOperand& operand, u32 index) {
        InstOperand dword = operand;
        dword.code += index;
        dword.type = ScalarType::Uint32;
        return IR::U32{GetSrc<IR::U32>(dword)};
    };

    // Snapshot all four source accumulators and compute all results before writing, so legal
    // source/destination VGPR aliasing cannot change a later lane's input.
    const std::array accumulators{read_dword(inst.src[2], 0), read_dword(inst.src[2], 1),
                                  read_dword(inst.src[2], 2), read_dword(inst.src[2], 3)};
    const auto calculate_lane = [&](u32 lane, const IR::U32& accumulator) {
        IR::U32 sad{ir.Imm32(0)};
        for (u32 byte = 0; byte < 4; ++byte) {
            const IR::U32 input_byte{byte_at_u64(src0, (lane + byte) * 8)};
            const IR::U32 reference_byte{byte_at_u32(src1, byte * 8)};
            const IR::U32 term{ir.Select(ir.IEqual(reference_byte, ir.Imm32(0)), ir.Imm32(0),
                                         abs_diff(input_byte, reference_byte))};
            sad = ir.IAdd(sad, term);
        }
        return IR::U32{ir.IAdd(sad, accumulator)};
    };
    const std::array results{calculate_lane(0, accumulators[0]), calculate_lane(1, accumulators[1]),
                             calculate_lane(2, accumulators[2]),
                             calculate_lane(3, accumulators[3])};

    for (u32 lane = 0; lane < 4; ++lane) {
        InstOperand destination = inst.dst[0];
        destination.code += lane;
        destination.type = ScalarType::Uint32;
        SetDst(destination, results[lane]);
    }
}

void Translator::V_SAD_U16(const GcnInst& inst) {
    const IR::U32 src0{GetSrc<IR::U32>(inst.src[0])};
    const IR::U32 src1{GetSrc<IR::U32>(inst.src[1])};
    const IR::U32 src2{GetSrc<IR::U32>(inst.src[2])};
    const auto abs_diff = [this](const IR::U32& lhs, const IR::U32& rhs) {
        const IR::U32 max{ir.IMax(lhs, rhs, false)};
        const IR::U32 min{ir.IMin(lhs, rhs, false)};
        return IR::U32{ir.ISub(max, min)};
    };
    const IR::U32 lo0{ir.BitwiseAnd(src0, ir.Imm32(0xFFFF))};
    const IR::U32 hi0{ir.ShiftRightLogical(src0, ir.Imm32(16))};
    const IR::U32 lo1{ir.BitwiseAnd(src1, ir.Imm32(0xFFFF))};
    const IR::U32 hi1{ir.ShiftRightLogical(src1, ir.Imm32(16))};
    const IR::U32 sad{ir.IAdd(abs_diff(lo0, lo1), abs_diff(hi0, hi1))};
    SetDst(inst.dst[0], ir.IAdd(sad, src2));
}

void Translator::V_CVT_PK_U16_U32(const GcnInst& inst) {
    const IR::Value vec_u32 =
        ir.CompositeConstruct(ir.BitCast<IR::F32>(GetSrc<IR::U32>(inst.src[0])),
                              ir.BitCast<IR::F32>(GetSrc<IR::U32>(inst.src[1])));
    SetDst(inst.dst[0], ir.Pack2x16(AmdGpu::NumberFormat::Uint, vec_u32));
}

void Translator::V_CVT_PK_I16_I32(const GcnInst& inst) {
    const IR::Value vec_u32 =
        ir.CompositeConstruct(ir.BitCast<IR::F32>(GetSrc<IR::U32>(inst.src[0])),
                              ir.BitCast<IR::F32>(GetSrc<IR::U32>(inst.src[1])));
    SetDst(inst.dst[0], ir.Pack2x16(AmdGpu::NumberFormat::Sint, vec_u32));
}

void Translator::V_CVT_PK_U8_F32(const GcnInst& inst) {
    const IR::F32 src0{GetSrc<IR::F32>(inst.src[0])};
    const IR::U32 src1{GetSrc(inst.src[1])};
    const IR::U32 src2{GetSrc(inst.src[2])};

    const IR::F32 clamped{ir.FPClamp(ir.FPRoundEven(src0), ir.Imm32(0.0f), ir.Imm32(255.0f))};
    const IR::F32 value{ir.Select(ir.FPIsNan(src0), ir.Imm32(0.0f), clamped)};
    const IR::U32 value_uint{ir.ConvertFToU(32, value)};
    const IR::U32 offset{ir.ShiftLeftLogical(ir.BitwiseAnd(src1, ir.Imm32(3)), ir.Imm32(3))};
    SetDst(inst.dst[0], ir.BitFieldInsert(src2, value_uint, offset, ir.Imm32(8)));
}

void Translator::V_LSHL_B64(const GcnInst& inst) {
    const IR::U64 src0{GetSrc64(inst.src[0])};
    const IR::U64 src1{GetSrc64(inst.src[1])};
    SetDst64(inst.dst[0], ir.ShiftLeftLogical(src0, ir.BitwiseAnd(src1, ir.Imm64(u64(0x3F)))));
}

void Translator::V_LSHR_B64(const GcnInst& inst) {
    const IR::U64 src0{GetSrc64(inst.src[0])};
    const IR::U64 src1{GetSrc64(inst.src[1])};
    SetDst64(inst.dst[0], ir.ShiftRightLogical(src0, ir.BitwiseAnd(src1, ir.Imm64(u64(0x3F)))));
}

void Translator::V_ASHR_I64(const GcnInst& inst) {
    const IR::U64 src0{GetSrc64(inst.src[0])};
    const IR::U64 src1{GetSrc64(inst.src[1])};
    SetDst64(inst.dst[0], ir.ShiftRightArithmetic(src0, ir.BitwiseAnd(src1, ir.Imm64(u64(0x3F)))));
}

void Translator::V_ALIGNBIT_B32(const GcnInst& inst) {
    const IR::U32 src0{GetSrc(inst.src[0])};
    const IR::U32 src1{GetSrc(inst.src[1])};
    const IR::U32 src2{ir.BitwiseAnd(GetSrc(inst.src[2]), ir.Imm32(0x1F))};
    const IR::U32 lo{ir.ShiftRightLogical(src1, src2)};
    const IR::U32 hi{
        ir.ShiftLeftLogical(ir.ShiftLeftLogical(src0, ir.Imm32(1)), ir.ISub(ir.Imm32(31), src2))};
    SetDst(inst.dst[0], ir.BitwiseOr(lo, hi));
}

void Translator::V_ALIGNBYTE_B32(const GcnInst& inst) {
    const IR::U32 src0{GetSrc(inst.src[0])};
    const IR::U32 src1{GetSrc(inst.src[1])};
    const IR::U32 src2{ir.BitwiseAnd(GetSrc(inst.src[2]), ir.Imm32(0x3))};
    const IR::U32 shift{ir.ShiftLeftLogical(src2, ir.Imm32(3))};
    const IR::U32 lo{ir.ShiftRightLogical(src1, shift)};
    const IR::U32 hi{
        ir.ShiftLeftLogical(ir.ShiftLeftLogical(src0, ir.Imm32(1)), ir.ISub(ir.Imm32(31), shift))};
    SetDst(inst.dst[0], ir.BitwiseOr(lo, hi));
}

void Translator::V_MUL_F64(const GcnInst& inst) {
    const IR::F64 src0{GetSrc64<IR::F64>(inst.src[0])};
    const IR::F64 src1{GetSrc64<IR::F64>(inst.src[1])};
    SetDst64(inst.dst[0], ir.FPMul(src0, src1));
}

void Translator::V_MIN_F64(const GcnInst& inst) {
    const IR::F64 src0{GetSrc64<IR::F64>(inst.src[0])};
    const IR::F64 src1{GetSrc64<IR::F64>(inst.src[1])};
    SetDst64(inst.dst[0], ir.FPMin(src0, src1));
}

void Translator::V_MAX_F64(const GcnInst& inst) {
    const IR::F64 src0{GetSrc64<IR::F64>(inst.src[0])};
    const IR::F64 src1{GetSrc64<IR::F64>(inst.src[1])};
    SetDst64(inst.dst[0], ir.FPMax(src0, src1));
}

void Translator::V_MUL_LO_U32(const GcnInst& inst) {
    const IR::U32 src0{GetSrc(inst.src[0])};
    const IR::U32 src1{GetSrc(inst.src[1])};
    SetDst(inst.dst[0], ir.IMul(src0, src1));
}

void Translator::V_MUL_HI_U32(bool is_signed, const GcnInst& inst) {
    const IR::U32 src0{GetSrc(inst.src[0])};
    const IR::U32 src1{GetSrc(inst.src[1])};
    const IR::U32 hi{ir.IMulHi(src0, src1, is_signed)};
    SetDst(inst.dst[0], hi);
}
void Translator::V_MUL_HI_24(const GcnInst& inst, bool is_signed) {
    const IR::U32 src0{
        ir.BitFieldExtract(GetSrc(inst.src[0]), ir.Imm32(0), ir.Imm32(24), is_signed)};
    const IR::U32 src1{
        ir.BitFieldExtract(GetSrc(inst.src[1]), ir.Imm32(0), ir.Imm32(24), is_signed)};
    const IR::U32 hi{ir.IMulHi(src0, src1, is_signed)};
    SetDst(inst.dst[0], hi);
}
void Translator::V_MAD_U64_U32(const GcnInst& inst) {
    if (profile.is_d3d12_profile && !profile.supports_vop3_mad64_subset) {
        LogMissingOpcode(inst);
        return;
    }
    const bool d3d12_mad64 = profile.is_d3d12_profile;
    if (d3d12_mad64) {
        ValidateVop3Mad64(inst);
    }
    const auto src0 = GetSrc<IR::U32>(inst.src[0]);
    const auto src1 = GetSrc<IR::U32>(inst.src[1]);
    const auto src2 = GetSrc64<IR::U64>(inst.src[2]);

    const IR::U64 mul_result = ir.IMul(ir.UConvert(64, src0), ir.UConvert(64, src1));
    const IR::U64 sum_result = ir.IAdd(mul_result, src2);

    SetDst64(inst.dst[0], sum_result);

    const IR::U1 less_src0 = ir.ILessThan(sum_result, mul_result, false);
    const IR::U1 less_src1 = ir.ILessThan(sum_result, src2, false);
    const IR::U1 did_overflow = ir.LogicalOr(less_src0, less_src1);

    if (d3d12_mad64) {
        SetCarryOut(inst, did_overflow);
    } else {
        const auto unpacked = ir.UnpackUint2x32(ir.Ballot(did_overflow));
        const IR::U32 lo{ir.CompositeExtract(unpacked, 0U)};
        const IR::U32 hi{ir.CompositeExtract(unpacked, 1U)};
        ir.SetVccLo(lo);
        ir.SetVccHi(hi);
    }
}

void Translator::V_MAD_I64_I32(const GcnInst& inst) {
    if (!profile.is_d3d12_profile || !profile.supports_vop3_mad64_subset) {
        throw std::runtime_error("GCN2 VOP3a MAD_I64_I32 is disabled for this shader profile");
    }
    ValidateVop3Mad64(inst);

    const IR::U32 src0{GetSrc<IR::U32>(inst.src[0])};
    const IR::U32 src1{GetSrc<IR::U32>(inst.src[1])};
    const IR::U32 src0_sign{ir.ShiftRightArithmetic(src0, ir.Imm32(31))};
    const IR::U32 src1_sign{ir.ShiftRightArithmetic(src1, ir.Imm32(31))};
    const IR::U64 src0_signed{ir.PackUint2x32(ir.CompositeConstruct(src0, src0_sign))};
    const IR::U64 src1_signed{ir.PackUint2x32(ir.CompositeConstruct(src1, src1_sign))};
    const IR::U64 src2{GetSrc64<IR::U64>(inst.src[2])};

    const IR::U64 mul_result{ir.IMul(src0_signed, src1_signed)};
    const IR::U64 sum_result{ir.IAdd(mul_result, src2)};
    SetDst64(inst.dst[0], sum_result);

    const IR::U1 less_src0{ir.ILessThan(sum_result, mul_result, false)};
    const IR::U1 less_src1{ir.ILessThan(sum_result, src2, false)};
    const IR::U1 did_overflow{ir.LogicalOr(less_src0, less_src1)};
    SetCarryOut(inst, did_overflow);
}

void Translator::V_LSHLREV_B16(const GcnInst& inst) {
    const auto shift = GetSrc16<IR::U32>(inst.src[0]);
    const auto src = GetSrc16<IR::U32>(inst.src[1]);

    const auto result = ir.ShiftLeftLogical(src, ir.BitwiseAnd(shift, ir.Imm32(0xF)));

    SetDst16(inst.dst[0], result);
}

void Translator::V_LSHRREV_B16(const GcnInst& inst) {
    const auto shift = GetSrc16<IR::U32>(inst.src[0]);
    const auto src = GetSrc16<IR::U32>(inst.src[1]);

    const auto result = ir.ShiftRightLogical(src, ir.BitwiseAnd(shift, ir.Imm32(0xF)));

    SetDst16(inst.dst[0], result);
}

void Translator::V_ASHRREV_I16(const GcnInst& inst) {
    const auto shift = GetSrc16<IR::U32, true>(inst.src[0]);
    const auto src = GetSrc16<IR::U32, true>(inst.src[1]);

    const auto result = ir.ShiftRightArithmetic(src, ir.BitwiseAnd(shift, ir.Imm32(0xF)));

    SetDst16<true>(inst.dst[0], result);
}

void Translator::V_ADD_LSHL_U32(const GcnInst& inst) {
    const auto src0 = GetSrc<IR::U32>(inst.src[0]);
    const auto src1 = GetSrc<IR::U32>(inst.src[1]);
    const auto src2 = GetSrc<IR::U32>(inst.src[2]);

    const auto shift = ir.BitwiseAnd(src2, ir.Imm32(0x1F));

    const auto result = ir.ShiftLeftLogical(ir.IAdd(src0, src1), shift);

    SetDst(inst.dst[0], result);
}

void Translator::V_LSHL_ADD_U32(const GcnInst& inst) {
    const auto src0 = GetSrc<IR::U32>(inst.src[0]);
    const auto src1 = GetSrc<IR::U32>(inst.src[1]);
    const auto src2 = GetSrc<IR::U32>(inst.src[2]);

    const auto shift = ir.BitwiseAnd(src1, ir.Imm32(0x1F));

    const auto result = ir.IAdd(ir.ShiftLeftLogical(src0, shift), src2);

    SetDst(inst.dst[0], result);
}

void Translator::V_MIN3_F16(const GcnInst& inst) {
    const auto src0 = GetSrc16<IR::F32>(inst.src[0]);
    const auto src1 = GetSrc16<IR::F32>(inst.src[1]);
    const auto src2 = GetSrc16<IR::F32>(inst.src[2]);

    const auto result = ir.FPMinTri(src0, src1, src2);

    SetDst16(inst.dst[0], result);
}

void Translator::V_MAX3_F16(const GcnInst& inst) {
    const auto src0 = GetSrc16<IR::F32>(inst.src[0]);
    const auto src1 = GetSrc16<IR::F32>(inst.src[1]);
    const auto src2 = GetSrc16<IR::F32>(inst.src[2]);

    const auto result = ir.FPMaxTri(src0, src1, src2);

    SetDst16(inst.dst[0], result);
}

void Translator::V_MED3_F16(const GcnInst& inst) {
    const auto src0 = GetSrc16<IR::F32>(inst.src[0]);
    const auto src1 = GetSrc16<IR::F32>(inst.src[1]);
    const auto src2 = GetSrc16<IR::F32>(inst.src[2]);

    const auto result = ir.FPMedTri(src0, src1, src2);

    SetDst16(inst.dst[0], result);
}

void Translator::V_ADD3_U32(const GcnInst& inst) {
    const auto src0 = GetSrc<IR::U32>(inst.src[0]);
    const auto src1 = GetSrc<IR::U32>(inst.src[1]);
    const auto src2 = GetSrc<IR::U32>(inst.src[2]);

    SetDst(inst.dst[0], ir.IAdd(src0, ir.IAdd(src1, src2)));
}

void Translator::V_PK_MUL_LO_U16(const GcnInst& inst) {
    const auto src0 = GetSrcPk<IR::U32>(inst.src[0]);
    const auto src1 = GetSrcPk<IR::U32>(inst.src[1]);

    const auto result_lo = ir.IMul(src0.first, src1.first);
    const auto result_hi = ir.IMul(src0.second, src1.second);

    SetDstPk<IR::U32, false>(inst.dst[0], {result_lo, result_hi});
}

void Translator::V_PK_ADD_I16(const GcnInst& inst) {
    const auto src0 = GetSrcPk<IR::U32, true>(inst.src[0]);
    const auto src1 = GetSrcPk<IR::U32, true>(inst.src[1]);

    const auto result_lo = ir.IAdd(src0.first, src1.first);
    const auto result_hi = ir.IAdd(src0.second, src1.second);

    SetDstPk<IR::U32, true>(inst.dst[0], {result_lo, result_hi});
}

void Translator::V_PK_SUB_I16(const GcnInst& inst) {
    const auto src0 = GetSrcPk<IR::U32, true>(inst.src[0]);
    const auto src1 = GetSrcPk<IR::U32, true>(inst.src[1]);

    const auto result_lo = ir.ISub(src0.first, src1.first);
    const auto result_hi = ir.ISub(src0.second, src1.second);

    SetDstPk<IR::U32, true>(inst.dst[0], {result_lo, result_hi});
}

void Translator::V_PK_LSHLREV_B16(const GcnInst& inst) {
    const auto shift = GetSrcPk<IR::U32>(inst.src[0]);
    const auto src = GetSrcPk<IR::U32>(inst.src[1]);

    const auto result_lo = ir.ShiftLeftLogical(src.first, shift.first);
    const auto result_hi = ir.ShiftLeftLogical(src.second, shift.second);

    SetDstPk<IR::U32, false>(inst.dst[0], {result_lo, result_hi});
}

void Translator::V_PK_LSHRREV_B16(const GcnInst& inst) {
    const auto shift = GetSrcPk<IR::U32>(inst.src[0]);
    const auto src = GetSrcPk<IR::U32>(inst.src[1]);

    const auto result_lo = ir.ShiftRightLogical(src.first, shift.first);
    const auto result_hi = ir.ShiftRightLogical(src.second, shift.second);

    SetDstPk<IR::U32, false>(inst.dst[0], {result_lo, result_hi});
}

void Translator::V_PK_MAD_U16(const GcnInst& inst) {
    const auto src0 = GetSrcPk<IR::U32>(inst.src[0]);
    const auto src1 = GetSrcPk<IR::U32>(inst.src[1]);
    const auto src2 = GetSrcPk<IR::U32>(inst.src[2]);

    const auto result_lo = ir.IAdd(ir.IMul(src0.first, src1.first), src2.first);
    const auto result_hi = ir.IAdd(ir.IMul(src0.second, src1.second), src2.second);

    SetDstPk<IR::U32, false>(inst.dst[0], {result_lo, result_hi});
}

void Translator::V_PK_ADD_U16(const GcnInst& inst) {
    const auto src0 = GetSrcPk<IR::U32>(inst.src[0]);
    const auto src1 = GetSrcPk<IR::U32>(inst.src[1]);

    const auto result_lo = ir.IAdd(src0.first, src1.first);
    const auto result_hi = ir.IAdd(src0.second, src1.second);

    SetDstPk<IR::U32, false>(inst.dst[0], {result_lo, result_hi});
}

void Translator::V_PK_SUB_U16(const GcnInst& inst) {
    const auto src0 = GetSrcPk<IR::U32>(inst.src[0]);
    const auto src1 = GetSrcPk<IR::U32>(inst.src[1]);

    const auto result_lo = ir.ISub(src0.first, src1.first);
    const auto result_hi = ir.ISub(src0.second, src1.second);

    SetDstPk<IR::U32, false>(inst.dst[0], {result_lo, result_hi});
}

void Translator::V_PK_MAX_U16(const GcnInst& inst) {
    const auto src0 = GetSrcPk<IR::U32>(inst.src[0]);
    const auto src1 = GetSrcPk<IR::U32>(inst.src[1]);

    const auto result_lo = ir.UMax(src0.first, src1.first);
    const auto result_hi = ir.UMax(src0.second, src1.second);

    SetDstPk<IR::U32, false>(inst.dst[0], {result_lo, result_hi});
}

void Translator::V_PK_MIN_U16(const GcnInst& inst) {
    const auto src0 = GetSrcPk<IR::U32>(inst.src[0]);
    const auto src1 = GetSrcPk<IR::U32>(inst.src[1]);

    const auto result_lo = ir.UMin(src0.first, src1.first);
    const auto result_hi = ir.UMin(src0.second, src1.second);

    SetDstPk<IR::U32, false>(inst.dst[0], {result_lo, result_hi});
}

void Translator::V_PK_FMA_F16(const GcnInst& inst) {
    const auto src0 = GetSrcPk<IR::F32>(inst.src[0]);
    const auto src1 = GetSrcPk<IR::F32>(inst.src[1]);
    const auto src2 = GetSrcPk<IR::F32>(inst.src[2]);

    const auto result_lo = ir.FPFma(src0.first, src1.first, src2.first);
    const auto result_hi = ir.FPFma(src0.second, src1.second, src2.second);

    SetDstPk<IR::F32>(inst.dst[0], {result_lo, result_hi});
}

void Translator::V_PK_ADD_F16(const GcnInst& inst) {
    const auto src0 = GetSrcPk<IR::F32>(inst.src[0]);
    const auto src1 = GetSrcPk<IR::F32>(inst.src[1]);

    const auto result_lo = ir.FPAdd(src0.first, src1.first);
    const auto result_hi = ir.FPAdd(src0.second, src1.second);

    SetDstPk<IR::F32>(inst.dst[0], {result_lo, result_hi});
}

void Translator::V_PK_MUL_F16(const GcnInst& inst) {
    const auto src0 = GetSrcPk<IR::F32>(inst.src[0]);
    const auto src1 = GetSrcPk<IR::F32>(inst.src[1]);

    const auto result_lo = ir.FPMul(src0.first, src1.first);
    const auto result_hi = ir.FPMul(src0.second, src1.second);

    SetDstPk<IR::F32>(inst.dst[0], {result_lo, result_hi});
}

void Translator::V_PK_MIN_F16(const GcnInst& inst) {
    const auto src0 = GetSrcPk<IR::F32>(inst.src[0]);
    const auto src1 = GetSrcPk<IR::F32>(inst.src[1]);

    const auto result_lo = ir.FPMin(src0.first, src1.first);
    const auto result_hi = ir.FPMin(src0.second, src1.second);

    SetDstPk<IR::F32>(inst.dst[0], {result_lo, result_hi});
}

void Translator::V_PK_MAX_F16(const GcnInst& inst) {
    const auto src0 = GetSrcPk<IR::F32>(inst.src[0]);
    const auto src1 = GetSrcPk<IR::F32>(inst.src[1]);

    const auto result_lo = ir.FPMax(src0.first, src1.first);
    const auto result_hi = ir.FPMax(src0.second, src1.second);

    SetDstPk<IR::F32>(inst.dst[0], {result_lo, result_hi});
}

void Translator::V_LSHL_OR_B32(const GcnInst& inst) {
    const auto src0 = GetSrc<IR::U32>(inst.src[0]);
    const auto src1 = GetSrc<IR::U32>(inst.src[1]);
    const auto src2 = GetSrc<IR::U32>(inst.src[2]);

    const auto shift = ir.BitwiseAnd(src1, ir.Imm32(0x1F));

    const auto result = ir.BitwiseOr(ir.ShiftLeftLogical(src0, shift), src2);

    SetDst(inst.dst[0], result);
}

void Translator::V_AND_OR_B32(const GcnInst& inst) {
    const auto src0 = GetSrc<IR::U32>(inst.src[0]);
    const auto src1 = GetSrc<IR::U32>(inst.src[1]);
    const auto src2 = GetSrc<IR::U32>(inst.src[2]);

    const auto result = ir.BitwiseOr(ir.BitwiseAnd(src0, src1), src2);

    SetDst(inst.dst[0], result);
}

void Translator::V_OR3_B32(const GcnInst& inst) {
    const auto src0 = GetSrc<IR::U32>(inst.src[0]);
    const auto src1 = GetSrc<IR::U32>(inst.src[1]);
    const auto src2 = GetSrc<IR::U32>(inst.src[2]);

    const auto result = ir.BitwiseOr(ir.BitwiseOr(src0, src1), src2);

    SetDst(inst.dst[0], result);
}

void Translator::V_MAD_MIX_F32(const GcnInst& inst) {
    const auto src0 = GetSrcMix(inst.src[0]);
    const auto src1 = GetSrcMix(inst.src[1]);
    const auto src2 = GetSrcMix(inst.src[2]);

    const IR::F32 result = ir.FPAdd(ir.FPMul(src0, src1), src2);

    SetDst(inst.dst[0], result);
}

void Translator::V_MAD_MIXLO_F16(const GcnInst& inst) {
    const auto src0 = GetSrcMix(inst.src[0]);
    const auto src1 = GetSrcMix(inst.src[1]);
    const auto src2 = GetSrcMix(inst.src[2]);

    const IR::F32 result = ir.FPAdd(ir.FPMul(src0, src1), src2);
    const IR::F16 result_f16 = ir.FPConvert(16, result);
    const IR::U16 result_f16_u16 = ir.BitCast<IR::U16, IR::F16>(result_f16);

    const IR::U32 old_value{GetSrc(inst.dst[0])};
    const IR::U32 new_value{
        ir.BitFieldInsert(old_value, ir.UConvert(32, result_f16_u16), ir.Imm32(0U), ir.Imm32(16U))};
    SetDst(inst.dst[0], new_value);
}

void Translator::V_MAD_MIXHI_F16(const GcnInst& inst) {
    const auto src0 = GetSrcMix(inst.src[0]);
    const auto src1 = GetSrcMix(inst.src[1]);
    const auto src2 = GetSrcMix(inst.src[2]);

    const IR::F32 result = ir.FPAdd(ir.FPMul(src0, src1), src2);
    const IR::F16 result_f16 = ir.FPConvert(16, result);
    const IR::U16 result_f16_u16 = ir.BitCast<IR::U16, IR::F16>(result_f16);

    const IR::U32 old_value{GetSrc(inst.dst[0])};
    const IR::U32 new_value{ir.BitFieldInsert(old_value, ir.UConvert(32, result_f16_u16),
                                              ir.Imm32(16U), ir.Imm32(16U))};
    SetDst(inst.dst[0], new_value);
}

IR::U32 Translator::GetCarryIn(const GcnInst& inst) {
    IR::U64 carry;
    if (inst.encoding == InstEncoding::VOP3 && inst.src_count == 3) {
        if (!IsCarryMaskRegister(inst.src[2].field)) {
            throw std::runtime_error("VOP3B carry-in source must be an SGPR-pair or VCC mask");
        }
        InstOperand carry_operand = inst.src[2];
        // The guest ISA is wave64 even when the backend emulates it on physical wave32.
        if (carry_operand.field == OperandField::ScalarGPR) {
            carry_operand.code &= ~u32{1};
        }
        carry = GetSrc64(carry_operand);
    } else if (inst.encoding == InstEncoding::VOP2 && inst.src_count == 2) {
        carry = ir.PackUint2x32(ir.CompositeConstruct(ir.GetVccLo(), ir.GetVccHi()));
    } else {
        throw std::runtime_error("Unsupported VOP2/VOP3B carry-in encoding");
    }

    return IR::U32{ir.Select(ir.InverseBallot(carry), ir.Imm32(1), ir.Imm32(0))};
}

void Translator::SetCarryOut(const GcnInst& inst, const IR::U1& carry) {
    if (inst.encoding == InstEncoding::VOP3 && inst.dst_count == 2) {
        if (!IsCarryMaskRegister(inst.dst[1].field)) {
            throw std::runtime_error("VOP3 carry-out destination must be an SGPR-pair or VCC mask");
        }
        InstOperand carry_destination = inst.dst[1];
        // Keep the guest wave64 SGPR-pair semantics independent of physical subgroup size.
        if (carry_destination.field == OperandField::ScalarGPR) {
            carry_destination.code &= ~u32{1};
        }
        SetDst64(carry_destination, ir.Ballot(carry));
    } else if (inst.encoding == InstEncoding::VOP2 && inst.dst_count == 1) {
        const auto unpacked = ir.UnpackUint2x32(ir.Ballot(carry));
        const IR::U32 lo{ir.CompositeExtract(unpacked, 0U)};
        const IR::U32 hi{ir.CompositeExtract(unpacked, 1U)};
        ir.SetVccLo(lo);
        ir.SetVccHi(hi);
    } else {
        throw std::runtime_error("Unsupported VOP2/VOP3 carry-out encoding");
    }
}

IR::F32 Translator::LegacyMul(const IR::F32& a, const IR::F32& b) {
    // DX9 rules, 0.0 * x = 0.0
    const IR::F32 zero{ir.Imm32(0.0f)};
    const IR::U1 either_zero{ir.LogicalOr(ir.FPEqual(a, zero), ir.FPEqual(b, zero))};
    return IR::F32{ir.Select(either_zero, zero, ir.FPMul(a, b))};
}

// TODO: add range analysis pass to hopefully put an upper bound on m0, and only select one of
// [src_vgprno, src_vgprno + max_m0]. Same for dst regs we may write back to

IR::U32 Translator::VMovRelSHelper(u32 src_vgprno, const IR::U32 m0) {
    // Read from VGPR0 by default when src_vgprno + m0 > num_allocated_vgprs
    IR::U32 src_val = ir.GetVectorReg<IR::U32>(IR::VectorReg::V0);
    for (u32 i = src_vgprno; i < runtime_info.props.num_allocated_vgprs; i++) {
        const IR::U1 cond = ir.IEqual(m0, ir.Imm32(i - src_vgprno));
        src_val =
            IR::U32{ir.Select(cond, ir.GetVectorReg<IR::U32>(IR::VectorReg::V0 + i), src_val)};
    }
    return src_val;
}

void Translator::VMovRelDHelper(u32 dst_vgprno, const IR::U32 src_val, const IR::U32 m0) {
    for (u32 i = dst_vgprno; i < runtime_info.props.num_allocated_vgprs; i++) {
        const IR::U1 cond = ir.IEqual(m0, ir.Imm32(i - dst_vgprno));
        const IR::U32 dst_val =
            IR::U32{ir.Select(cond, src_val, ir.GetVectorReg<IR::U32>(IR::VectorReg::V0 + i))};
        ir.SetVectorReg(IR::VectorReg::V0 + i, dst_val);
    }
}

} // namespace Shader::Gcn
