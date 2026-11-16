// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

namespace D3D12::StorageImagePolicy {

struct Operations {
    bool load{};
    bool store{};
    bool feedback{};
    bool atomic{};
};

struct TypedUavSupport {
    bool view{};
    bool load{};
    bool store{};
};

constexpr bool IsWriteOnlyTypedStoreCandidate(bool format_supported, bool is_depth,
                                              Operations operations) noexcept {
    return format_supported && !is_depth && operations.store && !operations.load &&
           !operations.feedback && !operations.atomic;
}

constexpr bool RequiresSharedR16StoreFallback(bool is_r16, bool format_supported, bool is_depth,
                                              bool single_sample, Operations operations) noexcept {
    return is_r16 && single_sample && !is_depth && operations.store && !format_supported;
}

constexpr bool SupportsTypedUav(TypedUavSupport support, bool require_load,
                                bool require_store) noexcept {
    return support.view && (!require_load || support.load) && (!require_store || support.store);
}

constexpr bool SupportsWriteOnlyTypedUav(bool device_available, TypedUavSupport support) noexcept {
    return device_available && SupportsTypedUav(support, false, true);
}

} // namespace D3D12::StorageImagePolicy
