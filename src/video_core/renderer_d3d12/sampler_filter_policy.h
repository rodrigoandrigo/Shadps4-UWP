#pragma once

namespace D3D12::SamplerFilterPolicy {
template <typename MipFilter>
[[nodiscard]] constexpr bool UsesPointMip(MipFilter filter) noexcept {
    return filter == MipFilter::None || filter == MipFilter::Point;
}

template <typename MipFilter>
[[nodiscard]] constexpr bool UseAnisotropicPointMip(bool anisotropic, bool capability_supported,
                                                    MipFilter mip_filter) noexcept {
    return anisotropic && capability_supported && UsesPointMip(mip_filter);
}
} // namespace D3D12::SamplerFilterPolicy
