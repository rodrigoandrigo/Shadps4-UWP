#pragma once

#include <cstdint>
#include <span>

namespace D3D12::BlendOutputRemapPolicy {

struct ActiveTarget {
    std::uint32_t guest_slot{};
    std::uint32_t format{};
    std::uint8_t write_mask{};
    bool blendable{};
};

struct ShaderOutput {
    std::uint32_t guest_slot{};
    std::uint8_t component_mask{};
    bool integer{};
};

struct Plan {
    bool supported{};
    std::uint32_t guest_slot{};
    std::uint32_t native_slot{};
    std::uint32_t format{};
    std::uint8_t write_mask{};
};

[[nodiscard]] inline constexpr bool SupportsDualSourceBlendOperations(
    std::uint32_t color_operation, std::uint32_t alpha_operation, std::uint32_t add,
    std::uint32_t subtract, std::uint32_t reverse_subtract) noexcept {
    const auto supported = [&](std::uint32_t operation) {
        return operation == add || operation == subtract || operation == reverse_subtract;
    };
    return supported(color_operation) && supported(alpha_operation);
}

// D3D12 dual-source blending consumes SV_Target0/SV_Target1 for one physical
// RTV at slot zero. Preserve the guest resource, format, and channel mask while
// compacting only a single, fully reflected floating-point guest output.
[[nodiscard]] inline Plan TryPlanSingleTargetToSlotZero(std::span<const ActiveTarget> targets,
                                                        std::span<const ShaderOutput> outputs,
                                                        bool guest_dual_source_factors) noexcept {
    if (targets.size() != 1 || guest_dual_source_factors)
        return {};

    const auto& target = targets.front();
    if (target.guest_slot >= 8 || !target.format || !target.write_mask || !target.blendable ||
        (target.write_mask & ~std::uint8_t{0x0f}))
        return {};

    const ShaderOutput* matching_output{};
    for (const auto& output : outputs) {
        if (output.guest_slot != target.guest_slot)
            continue;
        if (matching_output)
            return {};
        matching_output = &output;
    }
    if (!matching_output || matching_output->integer || matching_output->component_mask != 0x0f)
        return {};

    return {.supported = true,
            .guest_slot = target.guest_slot,
            .native_slot = 0,
            .format = target.format,
            .write_mask = target.write_mask};
}

} // namespace D3D12::BlendOutputRemapPolicy
