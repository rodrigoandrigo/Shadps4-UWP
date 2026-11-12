#pragma once

#include <cstdint>
#include <span>

namespace D3D12 {

inline constexpr unsigned MaxResourceDescriptorCount = 512;
inline constexpr unsigned MaxSamplerDescriptorCount = 128;
inline constexpr unsigned MaxColorAttachmentCount = 8;
inline constexpr unsigned MaxDepthAttachmentCount = 1;

// Avoid rejecting a legal resource descriptor table solely because of the
// per-context image count; byte budgets remain enforced by ImageCache.
inline constexpr unsigned MinImageCacheEntriesForMaxDraw =
    MaxResourceDescriptorCount + MaxColorAttachmentCount + MaxDepthAttachmentCount;

enum class DescriptorBindingTier : unsigned {
    Tier1 = 1,
    Tier2 = 2,
    Tier3 = 3,
};

struct StageDescriptorUsage {
    std::uint64_t cbv{};
    std::uint64_t srv{};
    std::uint64_t uav{};
    std::uint64_t sampler{};
};

constexpr bool CanAppendDescriptorCount(std::uint64_t current, std::uint64_t added,
                                        unsigned capacity) {
    return current <= capacity && added <= capacity - current;
}

constexpr bool FitsDescriptorHeapUsage(std::uint64_t resources, std::uint64_t samplers) {
    return resources <= MaxResourceDescriptorCount && samplers <= MaxSamplerDescriptorCount;
}

constexpr bool IsValidDescriptorBindingTier(DescriptorBindingTier tier) {
    return tier == DescriptorBindingTier::Tier1 || tier == DescriptorBindingTier::Tier2 ||
           tier == DescriptorBindingTier::Tier3;
}

constexpr bool FitsDescriptorBindingTier(DescriptorBindingTier tier,
                                         bool feature_level_11_1_or_higher,
                                         std::span<const StageDescriptorUsage> stages) {
    if (!IsValidDescriptorBindingTier(tier))
        return false;

    const std::uint64_t max_cbv_per_stage =
        tier == DescriptorBindingTier::Tier3 ? MaxResourceDescriptorCount : 14;
    const std::uint64_t max_srv_per_stage =
        tier == DescriptorBindingTier::Tier1 ? 128 : MaxResourceDescriptorCount;
    const std::uint64_t max_sampler_per_stage =
        tier == DescriptorBindingTier::Tier1 ? 16 : MaxSamplerDescriptorCount;
    const std::uint64_t max_uav_per_pipeline =
        tier == DescriptorBindingTier::Tier1   ? (feature_level_11_1_or_higher ? 64 : 8)
        : tier == DescriptorBindingTier::Tier2 ? 64
                                               : MaxResourceDescriptorCount;

    // ResourceBindings emits one table per heap with ShaderVisibility=ALL, so
    // every shader stage sees every stage's register-space ranges.
    StageDescriptorUsage visible_usage{};
    for (const auto& stage : stages) {
        if (!CanAppendDescriptorCount(visible_usage.cbv, stage.cbv,
                                      static_cast<unsigned>(max_cbv_per_stage)) ||
            !CanAppendDescriptorCount(visible_usage.srv, stage.srv,
                                      static_cast<unsigned>(max_srv_per_stage)) ||
            !CanAppendDescriptorCount(visible_usage.sampler, stage.sampler,
                                      static_cast<unsigned>(max_sampler_per_stage)) ||
            !CanAppendDescriptorCount(visible_usage.uav, stage.uav,
                                      static_cast<unsigned>(max_uav_per_pipeline)))
            return false;
        visible_usage.cbv += stage.cbv;
        visible_usage.srv += stage.srv;
        visible_usage.sampler += stage.sampler;
        visible_usage.uav += stage.uav;
    }
    return true;
}

} // namespace D3D12
