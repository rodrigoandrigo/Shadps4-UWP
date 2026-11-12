#pragma once

#include <d3d12.h>

#include "video_core/renderer_d3d12/native_capabilities.h"

namespace D3D12 {

[[nodiscard]] NativeCapabilitySnapshot QueryNativeDeviceCapabilities(
    ID3D12Device* device, D3D_SHADER_MODEL maximum_model = D3D_SHADER_MODEL_6_6);

} // namespace D3D12
