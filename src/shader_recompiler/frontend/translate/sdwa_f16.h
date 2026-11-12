// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdint>

namespace Shader::Gcn::SdwaF16 {

// Encodings are defined by SdwaSelector/SdwaDstUnused in instruction.h and the
// AMD GCN3 ISA's VOP_SDWA format. This header stays dependency-free for host tests.
inline constexpr std::uint32_t Word0Selector = 4;
inline constexpr std::uint32_t Word1Selector = 5;

enum class DestinationMode : std::uint32_t {
    Pad = 0,
    SignExtend = 1,
    Preserve = 2,
};

constexpr bool TryWordOffset(std::uint32_t selector, std::uint32_t& offset) noexcept {
    if (selector == Word0Selector) {
        offset = 0;
        return true;
    }
    if (selector == Word1Selector) {
        offset = 16;
        return true;
    }
    return false;
}

constexpr bool TryDestinationMode(std::uint32_t mode, DestinationMode& result) noexcept {
    if (mode > static_cast<std::uint32_t>(DestinationMode::Preserve)) {
        return false;
    }
    result = static_cast<DestinationMode>(mode);
    return true;
}

constexpr bool TrySelectWord(std::uint32_t packed, std::uint32_t selector,
                             std::uint16_t& result) noexcept {
    std::uint32_t offset{};
    if (!TryWordOffset(selector, offset)) {
        return false;
    }
    result = static_cast<std::uint16_t>((packed >> offset) & 0xFFFFu);
    return true;
}

// Models the architectural 32-bit VGPR write after a 16-bit result has been
// rounded to its F16 bit pattern. SEXT sign-extends only into unused upper bits
// and pads unused lower bits with zero, per the VOP_SDWA definition.
constexpr bool TryWriteback(std::uint32_t original, std::uint16_t value, std::uint32_t selector,
                            std::uint32_t mode, std::uint32_t& result) noexcept {
    std::uint32_t offset{};
    DestinationMode destination_mode{};
    if (!TryWordOffset(selector, offset) || !TryDestinationMode(mode, destination_mode)) {
        return false;
    }

    const std::uint32_t shifted = static_cast<std::uint32_t>(value) << offset;
    const std::uint32_t mask = 0xFFFFu << offset;
    switch (destination_mode) {
    case DestinationMode::Pad:
        result = shifted;
        return true;
    case DestinationMode::SignExtend:
        result = offset == 0 && (value & 0x8000u) != 0 ? (0xFFFF0000u | value) : shifted;
        return true;
    case DestinationMode::Preserve:
        result = (original & ~mask) | shifted;
        return true;
    }
    return false;
}

} // namespace Shader::Gcn::SdwaF16
