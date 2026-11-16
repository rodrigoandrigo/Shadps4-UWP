#pragma once
#include <cstdint>

namespace D3D12 {

// The shader-side guest stream-out lowering uses a private per-GS-invocation
// vertex index. Until ordered compaction exists, multiple invocations can write
// overlapping ranges in the same guest buffer. Keep this policy independent of
// API types so it can be exercised by a host test.
constexpr bool CanCaptureStreamOutWithoutCrossInvocationOverwrite(
    bool has_geometry, bool has_tessellation, bool primitive_restart, bool simple_list_topology,
    bool has_index_expansion, std::uint64_t input_primitives, std::uint32_t instances,
    std::uint32_t geometry_invocations) noexcept {
    return has_geometry && !has_tessellation && !primitive_restart && simple_list_topology &&
           !has_index_expansion && input_primitives == 1 && instances == 1 &&
           geometry_invocations == 1;
}

} // namespace D3D12
