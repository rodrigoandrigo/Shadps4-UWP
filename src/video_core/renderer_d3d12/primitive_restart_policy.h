#pragma once

#include <cstdint>

namespace D3D12 {
inline constexpr bool CanUseNativeStripCut(bool indexed, bool primitive_restart,
                                           bool strip_topology,
                                           std::uint32_t restart_index) noexcept {
    return indexed && primitive_restart && strip_topology &&
           (restart_index == 0xffffu || restart_index == 0xffffffffu);
}

inline constexpr bool PreserveAdjacencyListForRestartConversion(bool indexed,
                                                                bool primitive_restart,
                                                                bool adjacency_list,
                                                                bool has_geometry) noexcept {
    return indexed && primitive_restart && adjacency_list && has_geometry;
}
} // namespace D3D12
