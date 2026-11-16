#pragma once

#include <cstdint>

namespace D3D12::AttachmentFeedback {

constexpr bool IsSafeArrayReadRange(std::uint32_t first_layer, std::uint32_t layer_count,
                                    std::uint32_t snapshot_layer_count, bool attachment_writes,
                                    bool writes_only_first_layer) noexcept {
    if (layer_count == 0 || first_layer >= snapshot_layer_count ||
        layer_count > snapshot_layer_count - first_layer)
        return false;
    if (!attachment_writes)
        return true;
    return writes_only_first_layer && first_layer > 0;
}

} // namespace D3D12::AttachmentFeedback
