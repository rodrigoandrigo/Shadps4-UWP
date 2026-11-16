// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdint>

#include "shader_recompiler/profile.h"

namespace Shader::Gcn {

struct Vop3CommonFields {
    std::uint16_t dispatch_opcode{};
    std::uint16_t raw_opcode{};
    std::uint16_t src0{};
    std::uint16_t src1{};
    std::uint16_t src2{};
    std::uint8_t vdst{};
    std::uint8_t omod{};
    std::uint8_t neg{};
    bool bit16{};
};

struct Vop3aControlFields {
    std::uint8_t abs{};
    std::uint8_t op_sel{};
    bool clamp{};
};

struct Vop3bControlFields {
    std::uint8_t sdst{};
    bool clamp{};
};

// GCN2 Sea Islands defines OP as [25:17] and reserves bit 16. Unknown profiles
// retain the checked-out shared decoder's 10-bit dispatch key for compatibility.
constexpr Vop3CommonFields DecodeVop3CommonFields(
    std::uint64_t word, GuestIsaGeneration generation = GuestIsaGeneration::Unknown) noexcept {
    const auto raw_opcode = static_cast<std::uint16_t>((word >> 17) & 0x1FFu);
    const bool bit16 = ((word >> 16) & 1u) != 0;
    const auto dispatch_opcode = static_cast<std::uint16_t>(
        raw_opcode | ((generation == GuestIsaGeneration::Unknown && bit16) ? 0x200u : 0u));
    return {
        .dispatch_opcode = dispatch_opcode,
        .raw_opcode = raw_opcode,
        .src0 = static_cast<std::uint16_t>((word >> 32) & 0x1FFu),
        .src1 = static_cast<std::uint16_t>((word >> 41) & 0x1FFu),
        .src2 = static_cast<std::uint16_t>((word >> 50) & 0x1FFu),
        .vdst = static_cast<std::uint8_t>(word & 0xFFu),
        .omod = static_cast<std::uint8_t>((word >> 59) & 0x3u),
        .neg = static_cast<std::uint8_t>((word >> 61) & 0x7u),
        .bit16 = bit16,
    };
}

// VOP3a uses ABS[10:8], CLMP[11], and OPSEL[15:12].
constexpr Vop3aControlFields DecodeVop3aControl(std::uint64_t word) noexcept {
    return {
        .abs = static_cast<std::uint8_t>((word >> 8) & 0x7u),
        .op_sel = static_cast<std::uint8_t>((word >> 12) & 0xFu),
        .clamp = ((word >> 11) & 1u) != 0,
    };
}

// VOP3b instead uses SDST[14:8] and CLMP[15].
constexpr Vop3bControlFields DecodeVop3bControl(std::uint64_t word) noexcept {
    return {
        .sdst = static_cast<std::uint8_t>((word >> 8) & 0x7Fu),
        .clamp = ((word >> 15) & 1u) != 0,
    };
}

constexpr bool IsGcn2Vop3aMad64(GuestIsaGeneration generation, std::uint16_t raw_opcode) noexcept {
    return generation == GuestIsaGeneration::Gcn2SeaIslands &&
           (raw_opcode == 374 || raw_opcode == 375);
}

} // namespace Shader::Gcn
