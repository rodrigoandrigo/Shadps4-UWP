#pragma once

#include <cstdint>
#include <string_view>

namespace D3D12 {

// Values are the stable D3D enum encodings. Keeping this policy header free of
// Windows SDK types lets CI test the acceptance rules on non-Windows hosts.
struct NativeCapabilitySnapshot {
    std::uint32_t shader_model{};
    bool wave_ops{};
    std::uint32_t wave_lane_count_min{};
    std::uint32_t wave_lane_count_max{};
    std::uint32_t feature_level{};
    std::uint32_t resource_binding_tier{};
    bool int64_shader_ops{};
    bool double_precision_float_shader_ops{};
    bool viewport_index_from_any_shader_without_gs{};
};

enum class NativeCapabilityFailure {
    None,
    ShaderModel,
    WaveOps,
    Wave64,
    FeatureLevel,
    ResourceBindingTier,
};

[[nodiscard]] constexpr NativeCapabilityFailure RequiredCapabilityFailure(
    const NativeCapabilitySnapshot& capabilities) noexcept {
    if (capabilities.shader_model < 0x64)
        return NativeCapabilityFailure::ShaderModel;
    if (!capabilities.wave_ops)
        return NativeCapabilityFailure::WaveOps;
    if (capabilities.wave_lane_count_min > 64 || capabilities.wave_lane_count_max < 64)
        return NativeCapabilityFailure::Wave64;
    if (capabilities.feature_level < 0xb000)
        return NativeCapabilityFailure::FeatureLevel;
    if (capabilities.resource_binding_tier < 1 || capabilities.resource_binding_tier > 3)
        return NativeCapabilityFailure::ResourceBindingTier;
    return NativeCapabilityFailure::None;
}

[[nodiscard]] constexpr std::string_view CapabilityFailureName(
    NativeCapabilityFailure failure) noexcept {
    switch (failure) {
    case NativeCapabilityFailure::None:
        return {};
    case NativeCapabilityFailure::ShaderModel:
        return "Shader Model 6.4";
    case NativeCapabilityFailure::WaveOps:
        return "WaveOps";
    case NativeCapabilityFailure::Wave64:
        return "wave64";
    case NativeCapabilityFailure::FeatureLevel:
        return "Feature Level 11.0";
    case NativeCapabilityFailure::ResourceBindingTier:
        return "a valid Resource Binding Tier";
    }
    return "an unknown D3D12 capability";
}

} // namespace D3D12
