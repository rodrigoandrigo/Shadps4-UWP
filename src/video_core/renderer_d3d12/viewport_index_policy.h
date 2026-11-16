#pragma once

namespace D3D12 {

[[nodiscard]] constexpr bool CanUseNativeViewportIndexPath(
    bool viewport_index_enabled, bool has_geometry_shader,
    bool from_any_shader_supported_without_gs) noexcept {
    return !viewport_index_enabled || has_geometry_shader || from_any_shader_supported_without_gs;
}

} // namespace D3D12
