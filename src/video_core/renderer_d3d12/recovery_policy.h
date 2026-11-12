#pragma once

#include <cstdint>

namespace D3D12::RecoveryPolicy {

enum class FailureKind : std::uint8_t {
    OutOfMemory,
    DeviceLost,
    Other,
};

enum class Decision : std::uint8_t {
    RetryCheckpoint,
    Fail,
    FailDeviceLoss,
};

// Device loss is never recoverable by rebuilding only the guest command runtime:
// the UWP presenter owns a separate device, queue, composition swapchain and backbuffers.
constexpr Decision Decide(FailureKind failure, bool retry_used, bool operation_completed,
                          bool guest_memory_written, bool safe_checkpoint) noexcept {
    if (failure == FailureKind::DeviceLost)
        return Decision::FailDeviceLoss;
    if (failure != FailureKind::OutOfMemory || retry_used || operation_completed ||
        guest_memory_written || !safe_checkpoint)
        return Decision::Fail;
    return Decision::RetryCheckpoint;
}

} // namespace D3D12::RecoveryPolicy
