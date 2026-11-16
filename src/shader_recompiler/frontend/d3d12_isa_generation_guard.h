// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdint>
#include <optional>

#include "shader_recompiler/profile.h"

namespace Shader::Gcn {

// These are raw instruction encodings, not values from OpcodeVOP2/OpcodeVOP3.
// The GCN2 exception below applies only when the D3D12 guest profile was
// selected from the non-Neo PS4 compatibility mode. Unknown/Neo guests retain
// the full fail-closed candidate quarantine.
enum class D3D12IsaRawFormat : std::uint8_t {
    VOP1,
    VOP2,
    VOPC,
    VOP3,
};

enum class D3D12IsaHazardKind : std::uint8_t {
    VOP2Carry,
    VOP3Carry,
    MAD64,
    SAD,
    MULHI24VOP2,
    MULHI24VOP3,
    VOP3GenerationMismatch,
    VOP3ReservedBit16,
    SDWAExtension,
    DPPExtension,
};

struct D3D12IsaRawHazard {
    D3D12IsaRawFormat format;
    D3D12IsaHazardKind kind;
    std::uint16_t raw_opcode;
};

namespace detail {

enum class RawFormat : std::uint8_t { Other, VOP1, VOP2, VOPC, VOP3 };

constexpr RawFormat GetRawFormat(std::uint32_t word) noexcept {
    if ((word & 0xFE000000u) == 0x7E000000u) {
        return RawFormat::VOP1;
    }
    if ((word & 0xFE000000u) == 0x7C000000u) {
        return RawFormat::VOPC;
    }
    // The six-bit mask is specific to VOP3 (110100); VOP3P has a different
    // prefix and is intentionally not classified as VOP3 here.
    if ((word & 0xFC000000u) == 0xD0000000u) {
        return RawFormat::VOP3;
    }
    // VOP2 is selected by the 1-bit encoding mask after the more-specific
    // SOP/VOP formats above; its top bit is zero.
    if ((word & 0x80000000u) == 0) {
        return RawFormat::VOP2;
    }
    return RawFormat::Other;
}

constexpr std::uint16_t GetRawOpcode(RawFormat format, std::uint32_t word) noexcept {
    switch (format) {
    case RawFormat::VOP1:
        return static_cast<std::uint16_t>((word >> 9) & 0xFFu);
    case RawFormat::VOP2:
        return static_cast<std::uint16_t>((word >> 25) & 0x3Fu);
    case RawFormat::VOPC:
        return static_cast<std::uint16_t>((word >> 17) & 0xFFu);
    case RawFormat::VOP3:
        // For an unknown guest, mirror the checked-out decoder's 10-bit
        // dispatch key only to recognize candidate conflicts. Sea Islands
        // has a 9-bit OP field and reserves bit 16; that case is rejected
        // explicitly before this helper is used for a qualified GCN2 guest.
        return static_cast<std::uint16_t>(((word >> 17) & 0x1FFu) | (((word >> 16) & 1u) << 9));
    case RawFormat::Other:
        return 0;
    }
    return 0;
}

constexpr bool InRange(std::uint16_t value, std::uint16_t first, std::uint16_t last) noexcept {
    return value >= first && value <= last;
}

constexpr bool IsVOP2Carry(std::uint16_t op) noexcept {
    return InRange(op, 25, 30) || InRange(op, 37, 42);
}

constexpr bool IsVOP3Carry(std::uint16_t op) noexcept {
    return InRange(op, 281, 286) || InRange(op, 293, 298) || op == 783 || op == 784 || op == 793;
}

constexpr bool IsVOP3Sad(std::uint16_t op) noexcept {
    return InRange(op, 346, 349) || op == 369 || op == 370 || op == 371 || op == 373 ||
           InRange(op, 473, 476) || InRange(op, 484, 487);
}

constexpr bool IsVOP3GenerationMismatch(std::uint16_t op) noexcept {
    return op == 333 || op == 490 || op == 492 || op == 644 || op == 771 || op == 781 ||
           op == 782 || op == 833 || op == 862;
}

constexpr std::optional<D3D12IsaHazardKind> GetHazardKind(RawFormat format,
                                                          std::uint16_t op) noexcept {
    switch (format) {
    case RawFormat::VOP3:
        if (op == 374 || op == 375 || op == 488 || op == 489) {
            return D3D12IsaHazardKind::MAD64;
        }
        if (IsVOP3Sad(op)) {
            return D3D12IsaHazardKind::SAD;
        }
        if (op == 263 || op == 265 || op == 266 || op == 268) {
            return D3D12IsaHazardKind::MULHI24VOP3;
        }
        if (IsVOP3Carry(op)) {
            return D3D12IsaHazardKind::VOP3Carry;
        }
        if (IsVOP3GenerationMismatch(op)) {
            return D3D12IsaHazardKind::VOP3GenerationMismatch;
        }
        break;
    case RawFormat::VOP2:
        if (IsVOP2Carry(op)) {
            return D3D12IsaHazardKind::VOP2Carry;
        }
        if (op == 7 || op == 9 || op == 10 || op == 12) {
            return D3D12IsaHazardKind::MULHI24VOP2;
        }
        break;
    case RawFormat::VOP1:
    case RawFormat::VOPC:
    case RawFormat::Other:
        break;
    }
    return std::nullopt;
}

constexpr bool IsQualifiedGcn2Opcode(RawFormat format, D3D12IsaHazardKind kind,
                                     std::uint16_t op) noexcept {
    switch (kind) {
    case D3D12IsaHazardKind::VOP2Carry:
        // GCN2 carry VOP2 is 37-42; 25-30 are other valid GCN2 VOP2 slots.
        return format == RawFormat::VOP2;
    case D3D12IsaHazardKind::VOP3Carry:
        // 281-286 map to GCN2 VOP2 slots 25-30; 293-298 map to GCN2 carry
        // VOP2 slots 37-42 and preserve the scalar carry destination in decode.
        return format == RawFormat::VOP3 && (InRange(op, 281, 286) || InRange(op, 293, 298));
    case D3D12IsaHazardKind::SAD:
        return format == RawFormat::VOP3 &&
               (InRange(op, 346, 349) || InRange(op, 369, 371) || op == 373);
    case D3D12IsaHazardKind::MULHI24VOP2:
    case D3D12IsaHazardKind::MULHI24VOP3:
        // Under the GCN2 table these raw slots are decoded using their GCN2
        // meanings; 10/12 and 266/268 are the documented MUL_HI forms.
        return true;
    case D3D12IsaHazardKind::VOP3GenerationMismatch:
        // AMD documents GCN2 V_LERP_U8 as raw 333. GCN3 moves it to 644.
        return format == RawFormat::VOP3 && op == 333;
    case D3D12IsaHazardKind::MAD64:
        // Sea Islands 374/375 are VOP3a with implicit VCC_OUT; later 488/489
        // candidates do not share this qualified GCN2 raw identity.
        return format == RawFormat::VOP3 && (op == 374 || op == 375);
    case D3D12IsaHazardKind::VOP3ReservedBit16:
    case D3D12IsaHazardKind::SDWAExtension:
    case D3D12IsaHazardKind::DPPExtension:
        return false;
    }
    return false;
}

} // namespace detail

constexpr std::optional<D3D12IsaRawHazard> FindD3D12IsaRawHazard(
    std::uint32_t first_word,
    GuestIsaGeneration guest_generation = GuestIsaGeneration::Unknown) noexcept {
    using detail::RawFormat;
    const RawFormat raw_format = detail::GetRawFormat(first_word);
    if (raw_format == RawFormat::Other) {
        return std::nullopt;
    }
    if (raw_format == RawFormat::VOP3 && guest_generation == GuestIsaGeneration::Gcn2SeaIslands &&
        (first_word & (1u << 16)) != 0) {
        // AMD Sea Islands VOP3a/VOP3b layouts label bit 16 as reserved (r),
        // not OP[9]. The shared decoder folds it into its local 10-bit key.
        return D3D12IsaRawHazard{D3D12IsaRawFormat::VOP3, D3D12IsaHazardKind::VOP3ReservedBit16,
                                 static_cast<std::uint16_t>((first_word >> 17) & 0x1FFu)};
    }
    if ((raw_format == RawFormat::VOP1 || raw_format == RawFormat::VOP2 ||
         raw_format == RawFormat::VOPC) &&
        (first_word & 0x1FFu) == 249u) {
        D3D12IsaRawFormat format = D3D12IsaRawFormat::VOP1;
        if (raw_format == RawFormat::VOP2) {
            format = D3D12IsaRawFormat::VOP2;
        } else if (raw_format == RawFormat::VOPC) {
            format = D3D12IsaRawFormat::VOPC;
        }
        return D3D12IsaRawHazard{format, D3D12IsaHazardKind::SDWAExtension,
                                 detail::GetRawOpcode(raw_format, first_word)};
    }
    if ((raw_format == RawFormat::VOP1 || raw_format == RawFormat::VOP2) &&
        (first_word & 0x1FFu) == 250u) {
        const auto format =
            raw_format == RawFormat::VOP1 ? D3D12IsaRawFormat::VOP1 : D3D12IsaRawFormat::VOP2;
        return D3D12IsaRawHazard{format, D3D12IsaHazardKind::DPPExtension,
                                 detail::GetRawOpcode(raw_format, first_word)};
    }

    const auto opcode = detail::GetRawOpcode(raw_format, first_word);
    const auto kind = detail::GetHazardKind(raw_format, opcode);
    if (!kind) {
        return std::nullopt;
    }
    if (guest_generation == GuestIsaGeneration::Gcn2SeaIslands &&
        detail::IsQualifiedGcn2Opcode(raw_format, *kind, opcode)) {
        return std::nullopt;
    }

    D3D12IsaRawFormat format = D3D12IsaRawFormat::VOP1;
    switch (raw_format) {
    case RawFormat::VOP1:
        format = D3D12IsaRawFormat::VOP1;
        break;
    case RawFormat::VOP2:
        format = D3D12IsaRawFormat::VOP2;
        break;
    case RawFormat::VOPC:
        format = D3D12IsaRawFormat::VOPC;
        break;
    case RawFormat::VOP3:
        format = D3D12IsaRawFormat::VOP3;
        break;
    case RawFormat::Other:
        return std::nullopt;
    }
    return D3D12IsaRawHazard{format, *kind, opcode};
}

constexpr const char* D3D12IsaHazardName(D3D12IsaHazardKind kind) noexcept {
    switch (kind) {
    case D3D12IsaHazardKind::VOP2Carry:
        return "carry VOP2";
    case D3D12IsaHazardKind::VOP3Carry:
        return "carry VOP3B";
    case D3D12IsaHazardKind::MAD64:
        return "MAD64 VOP3";
    case D3D12IsaHazardKind::SAD:
        return "SAD-family VOP3";
    case D3D12IsaHazardKind::MULHI24VOP2:
        return "MUL_HI 24-bit VOP2";
    case D3D12IsaHazardKind::MULHI24VOP3:
        return "MUL_HI 24-bit VOP3";
    case D3D12IsaHazardKind::VOP3GenerationMismatch:
        return "generation-sensitive VOP3";
    case D3D12IsaHazardKind::VOP3ReservedBit16:
        return "reserved VOP3 bit 16 in GCN2";
    case D3D12IsaHazardKind::SDWAExtension:
        return "SDWA extension";
    case D3D12IsaHazardKind::DPPExtension:
        return "DPP extension";
    }
    return "unknown ISA hazard";
}

} // namespace Shader::Gcn
