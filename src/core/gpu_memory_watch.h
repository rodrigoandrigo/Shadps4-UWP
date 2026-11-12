// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstdint>
#include <limits>
namespace Core::GpuMemoryWatch {
// AddressSpace's Windows/UWP protection path changes permissions in 4 KiB pages.
inline constexpr unsigned ProtectionPageShift = 12;
constexpr bool ProtectionPageOverlapsRange(std::uint64_t fault_address, std::uint64_t range_address,
                                           std::uint64_t range_size) noexcept {
    constexpr auto max = std::numeric_limits<std::uint64_t>::max();
    if (!range_size || range_address > max - range_size)
        return false;
    const auto fault_page = fault_address >> ProtectionPageShift;
    const auto first_page = range_address >> ProtectionPageShift;
    const auto last_page = (range_address + range_size - 1) >> ProtectionPageShift;
    return fault_page >= first_page && fault_page <= last_page;
}
} // namespace Core::GpuMemoryWatch
