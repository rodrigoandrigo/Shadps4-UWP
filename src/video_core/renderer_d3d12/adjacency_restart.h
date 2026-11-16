// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <stdexcept>
#include <vector>

namespace D3D12 {

inline std::vector<std::uint32_t> KeepCompleteAdjacencyGroups(
    std::span<const std::uint32_t> indices, std::optional<std::uint32_t> restart,
    std::uint32_t group_size) {
    if (group_size != 4 && group_size != 6)
        throw std::runtime_error("Invalid adjacency-list primitive group size");

    std::vector<std::uint32_t> output;
    const auto append_segment = [&](std::span<const std::uint32_t> segment) {
        const std::size_t complete_size = segment.size() / group_size * group_size;
        if (output.size() + complete_size > 4 * 1024 * 1024)
            throw std::runtime_error("Segmented D3D12 adjacency indices exceed upload budget");
        output.insert(output.end(), segment.begin(), segment.begin() + complete_size);
    };

    std::size_t first{};
    for (std::size_t i = 0; i < indices.size(); ++i) {
        if (restart && indices[i] == *restart) {
            append_segment(indices.subspan(first, i - first));
            first = i + 1;
        }
    }
    append_segment(indices.subspan(first));
    return output;
}

} // namespace D3D12
