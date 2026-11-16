// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdint>

namespace Shader::Gcn::SdwaSelectorLayout {

struct BitRange {
    std::uint32_t offset;
    std::uint32_t width;
};

// SDWA selectors 0..6 select byte 0..3, word 0..1, or the full dword.
constexpr bool TryGetBitRange(std::uint32_t selector, BitRange& range) noexcept {
    if (selector < 4) {
        range = {selector * 8, 8};
        return true;
    }
    if (selector < 6) {
        range = {(selector - 4) * 16, 16};
        return true;
    }
    if (selector == 6) {
        range = {0, 32};
        return true;
    }
    return false;
}

} // namespace Shader::Gcn::SdwaSelectorLayout
