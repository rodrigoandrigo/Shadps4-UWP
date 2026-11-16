#pragma once

#include <array>
#include <cstdint>
#include <span>

namespace D3D12::BlendConstantPolicy {

enum class Factor : std::uint8_t {
    Other,
    ConstantColor,
    OneMinusConstantColor,
    ConstantAlpha,
    OneMinusConstantAlpha,
};

struct Target {
    bool blend_enabled{};
    std::uint8_t write_mask{};
    bool factors_ignored{};
    Factor source{Factor::Other};
    Factor destination{Factor::Other};
};

struct Plan {
    bool has_alpha_factor{};
    bool compatible{true};
    std::array<std::uint32_t, 4> constants{};
};

// D3D12_BLEND_ALPHA_FACTOR replicates blend_constants.a over RGB, whereas
// D3D12_BLEND_BLEND_FACTOR uses the per-channel vector. They are interchangeable
// only on writable channels where the constants are bit-identical. This helper
// finds a single global vector that preserves both factors across all targets.
[[nodiscard]] inline Plan TryPlanAlphaFactorFallback(
    std::span<const Target> targets, std::array<std::uint32_t, 4> constants) noexcept {
    Plan plan{.constants = constants};
    std::array<std::uint32_t, 3> required{};
    std::array<bool, 3> constrained{};
    const auto constrain = [&](std::size_t channel, std::uint32_t value) {
        if (constrained[channel] && required[channel] != value) {
            plan.compatible = false;
            return;
        }
        constrained[channel] = true;
        required[channel] = value;
    };

    for (const auto& target : targets) {
        if (!target.blend_enabled || target.factors_ignored)
            continue;
        const std::array factors{target.source, target.destination};
        for (std::size_t channel = 0; channel < required.size(); ++channel) {
            if ((target.write_mask & (1u << channel)) == 0)
                continue;
            for (const auto factor : factors) {
                switch (factor) {
                case Factor::ConstantColor:
                case Factor::OneMinusConstantColor:
                    constrain(channel, constants[channel]);
                    break;
                case Factor::ConstantAlpha:
                case Factor::OneMinusConstantAlpha:
                    plan.has_alpha_factor = true;
                    constrain(channel, constants[3]);
                    break;
                case Factor::Other:
                    break;
                }
            }
        }
    }

    if (plan.compatible)
        for (std::size_t channel = 0; channel < constrained.size(); ++channel)
            if (constrained[channel])
                plan.constants[channel] = required[channel];
    return plan;
}

} // namespace D3D12::BlendConstantPolicy
