// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdint>

namespace D3D12::StorageImageMsaaPolicy {

constexpr std::uint32_t SampleMask(std::uint32_t sample_count) noexcept {
    return sample_count - 1;
}

constexpr std::uint32_t WrapSampleIndex(std::uint32_t sample, std::uint32_t sample_count) noexcept {
    return sample & SampleMask(sample_count);
}

constexpr std::uint32_t PlaneIndex(std::uint32_t absolute_slice, std::uint32_t sample,
                                   std::uint32_t sample_count) noexcept {
    return absolute_slice * sample_count + WrapSampleIndex(sample, sample_count);
}

} // namespace D3D12::StorageImageMsaaPolicy
