#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <map>
#include <stdexcept>
#include <winrt/base.h>
#include "core/uwp/memory_pressure.h"
#include "video_core/renderer_d3d12/attachment_feedback.h"
#include "video_core/renderer_d3d12/image_buffer_alias.h"
#include "video_core/renderer_d3d12/resource_bindings.h"
#include "video_core/renderer_d3d12/sampler_filter_policy.h"

namespace D3D12 {
namespace {
void CheckWrapMode(AmdGpu::ClampMode mode) {
    switch (mode) {
    case AmdGpu::ClampMode::Wrap:
    case AmdGpu::ClampMode::Mirror:
    case AmdGpu::ClampMode::ClampLastTexel:
    case AmdGpu::ClampMode::MirrorOnceLastTexel:
    case AmdGpu::ClampMode::ClampBorder:
    case AmdGpu::ClampMode::ClampHalfBorder:
    case AmdGpu::ClampMode::MirrorOnceHalfBorder:
    case AmdGpu::ClampMode::MirrorOnceBorder:
        return;
    default:
        throw std::runtime_error(
            "Guest sampler border/mirror mode requires shader coordinate emulation");
    }
}

D3D12_TEXTURE_ADDRESS_MODE AddressMode(AmdGpu::ClampMode mode) {
    switch (mode) {
    case AmdGpu::ClampMode::Wrap:
        return D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    case AmdGpu::ClampMode::Mirror:
        return D3D12_TEXTURE_ADDRESS_MODE_MIRROR;
    case AmdGpu::ClampMode::ClampLastTexel:
        return D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    case AmdGpu::ClampMode::MirrorOnceLastTexel:
        return D3D12_TEXTURE_ADDRESS_MODE_MIRROR_ONCE;
    case AmdGpu::ClampMode::ClampBorder:
    case AmdGpu::ClampMode::ClampHalfBorder:
    case AmdGpu::ClampMode::MirrorOnceHalfBorder:
    case AmdGpu::ClampMode::MirrorOnceBorder:
        return D3D12_TEXTURE_ADDRESS_MODE_BORDER;
    default:
        throw std::runtime_error(
            "Guest sampler border/mirror mode requires shader coordinate emulation");
    }
}
UINT Component(AmdGpu::CompSwizzle component) {
    const u32 value = u32(component);
    if (value >= 4 && value <= 7)
        return value - 4;
    if (component == AmdGpu::CompSwizzle::Zero)
        return D3D12_SHADER_COMPONENT_MAPPING_FORCE_VALUE_0;
    if (component == AmdGpu::CompSwizzle::One)
        return D3D12_SHADER_COMPONENT_MAPPING_FORCE_VALUE_1;
    throw std::runtime_error("Invalid guest texture component selector");
}
DXGI_FORMAT TypelessFormat(DXGI_FORMAT format) {
    switch (format) {
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
    case DXGI_FORMAT_R8G8B8A8_UINT:
    case DXGI_FORMAT_R8G8B8A8_SINT:
    case DXGI_FORMAT_R8G8B8A8_SNORM:
        return DXGI_FORMAT_R8G8B8A8_TYPELESS;
    case DXGI_FORMAT_R32_FLOAT:
    case DXGI_FORMAT_R32_UINT:
    case DXGI_FORMAT_R32_SINT:
    case DXGI_FORMAT_D32_FLOAT:
        return DXGI_FORMAT_R32_TYPELESS;
    case DXGI_FORMAT_D16_UNORM:
        return DXGI_FORMAT_R16_TYPELESS;
    default:
        return format;
    }
}
} // namespace
ResourceBindings::ResourceBindings(D3D12DeviceContext& context, ResourceCache& buffer_cache,
                                   ImageCache& image_cache, std::span<CompiledShader*> shaders,
                                   std::span<const StreamOutSlot> strmout_slots,
                                   D3D12DescriptorArena* resource_heap,
                                   D3D12DescriptorArena* sampler_heap, bool fence_safe,
                                   DescriptorBindingTier descriptor_tier,
                                   bool feature_level_11_1_or_higher)
    : device{context}, buffers{buffer_cache}, images{image_cache},
      resources{resource_heap ? *resource_heap : owned_resources},
      samplers{sampler_heap ? *sampler_heap : owned_samplers} {
    if (!fence_safe && (resource_heap || sampler_heap))
        device.Wait(device.LastSignaledValue());
    u32 resource_count{}, sampler_count{}, strmout_used{};
    std::array<StageDescriptorUsage, Shader::MaxStageTypes> descriptor_usage{};
    for (auto* shader : shaders) {
        const u32 stage = static_cast<u32>(shader->info.sw_stage);
        if (stage >= descriptor_usage.size())
            throw std::runtime_error("Invalid D3D12 guest shader stage");
        auto& stage_usage = descriptor_usage[stage];
        for (const auto& binding : shader->binary->bindings) {
            if (binding.binding < shader->start_bindings.unified)
                throw std::runtime_error("Invalid guest shader binding base");
            if (binding.space != stage + 1)
                throw std::runtime_error(
                    "Guest shader descriptor register space does not match stage");
            const bool class_matches_type = (binding.kind == ResourceKind::Buffer &&
                                             binding.type == D3D12_DESCRIPTOR_RANGE_TYPE_UAV) ||
                                            (binding.kind == ResourceKind::Constants &&
                                             binding.type == D3D12_DESCRIPTOR_RANGE_TYPE_CBV) ||
                                            (binding.kind == ResourceKind::Image &&
                                             (binding.type == D3D12_DESCRIPTOR_RANGE_TYPE_SRV ||
                                              binding.type == D3D12_DESCRIPTOR_RANGE_TYPE_UAV)) ||
                                            (binding.kind == ResourceKind::Sampler &&
                                             binding.type == D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER);
            if (!class_matches_type)
                throw std::runtime_error("Guest shader descriptor class/type mismatch");
            switch (binding.type) {
            case D3D12_DESCRIPTOR_RANGE_TYPE_CBV:
                stage_usage.cbv += binding.count;
                break;
            case D3D12_DESCRIPTOR_RANGE_TYPE_SRV:
                stage_usage.srv += binding.count;
                break;
            case D3D12_DESCRIPTOR_RANGE_TYPE_UAV:
                stage_usage.uav += binding.count;
                break;
            case D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER:
                stage_usage.sampler += binding.count;
                break;
            default:
                throw std::runtime_error("Invalid guest shader descriptor type");
            }
            u32 index = binding.binding - shader->start_bindings.unified;
            if (binding.kind == ResourceKind::Buffer) {
                if (index >= shader->info.buffers.size() || binding.count != 1)
                    throw std::runtime_error("Invalid guest buffer descriptor mapping");
            } else {
                if (binding.kind == ResourceKind::Constants)
                    throw std::runtime_error(
                        "D3D12 guest uniform-buffer CBV bindings are not implemented");
                if (index < shader->info.buffers.size())
                    throw std::runtime_error("Guest shader resource class mismatch");
                index -= shader->info.buffers.size();
                u32 image_index{};
                while (image_index < shader->info.images.size()) {
                    const u32 count = shader->info.images[image_index].NumBindings(shader->info);
                    if (index < count)
                        break;
                    index -= count;
                    ++image_index;
                }
                if (binding.kind == ResourceKind::Image) {
                    if (image_index >= shader->info.images.size() || index ||
                        binding.count != shader->info.images[image_index].NumBindings(shader->info))
                        throw std::runtime_error("Invalid guest image descriptor mapping");
                    index = image_index;
                } else if (binding.kind != ResourceKind::Sampler ||
                           image_index != shader->info.images.size() ||
                           index >= shader->info.samplers.size() || binding.count != 1)
                    throw std::runtime_error("Invalid guest sampler descriptor mapping");
            }
            if (!binding.count)
                throw std::runtime_error("Guest shader descriptor range is empty");
            const bool sampler = binding.kind == ResourceKind::Sampler;
            auto& count = sampler ? sampler_count : resource_count;
            auto& ranges = sampler ? sampler_ranges : resource_ranges;
            const u32 capacity = sampler ? MaxSamplerDescriptorCount : MaxResourceDescriptorCount;
            if (!CanAppendDescriptorCount(count, binding.count, capacity))
                throw std::runtime_error("Guest shader descriptor budget exceeded");
            ranges.push_back({binding.type, binding.count, binding.binding, binding.space, count});
            for (u32 element = 0; element < binding.count; ++element) {
                entries.push_back({shader, binding, index, element, count++});
                ResourceCache::Range range{};
                if (binding.kind == ResourceKind::Buffer) {
                    const auto& resource = shader->info.buffers[index];
                    if (resource.buffer_type == Shader::BufferType::Guest) {
                        const auto sharp = resource.GetSharp(shader->info);
                        range = {sharp.base_address,
                                 u64(sharp.num_records) * std::max<u32>(1, sharp.stride)};
                        if (range.address > UINT64_MAX - range.size)
                            throw std::runtime_error("Guest shader buffer address overflow");
                    } else if (resource.buffer_type == Shader::BufferType::Strmout) {
                        if (strmout_used >= 4)
                            throw std::runtime_error(
                                "D3D12 stream-out capture binding count exceeds slots");
                        range = {strmout_slots[strmout_used].address,
                                 strmout_slots[strmout_used].size};
                        ++strmout_used;
                    } else if (resource.buffer_type != Shader::BufferType::Flatbuf &&
                               resource.buffer_type != Shader::BufferType::ClipPlanes &&
                               resource.buffer_type != Shader::BufferType::GdsBuffer &&
                               resource.buffer_type != Shader::BufferType::ImageAddressTable &&
                               resource.buffer_type != Shader::BufferType::BufferAddressTable &&
                               resource.buffer_type != Shader::BufferType::SamplerTable &&
                               resource.buffer_type != Shader::BufferType::StrmoutCounter &&
                               resource.buffer_type != Shader::BufferType::SharedMemory)
                        throw std::runtime_error(
                            "D3D12 shader special buffer requires additional execution state");
                }
                guest_ranges.push_back(range);
            }
        }
    }
    if (!FitsDescriptorBindingTier(descriptor_tier, feature_level_11_1_or_higher, descriptor_usage))
        throw std::runtime_error(
            "Guest shader descriptors exceed the device ResourceBindingTier limits");
    if (!FitsDescriptorHeapUsage(resource_count, sampler_count))
        throw std::runtime_error("Guest shader descriptor budget exceeded");
    feedback_resource_views.resize(entries.size());
    if (!resource_heap) {
        resources.Initialize(device.Device(), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
                             std::max(1u, resource_count), true);
        static_cast<void>(resources.Allocate(std::max(1u, resource_count)));
    }
    if (!sampler_heap) {
        samplers.Initialize(device.Device(), D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER,
                            std::max(1u, sampler_count), true);
        static_cast<void>(samplers.Allocate(std::max(1u, sampler_count)));
    }
    static_cast<void>(resources.Cpu(std::max(1u, resource_count) - 1));
    static_cast<void>(samplers.Cpu(std::max(1u, sampler_count) - 1));
}
bool ResourceBindings::Aliases(VAddr first, u64 first_size, VAddr second, u64 second_size) const {
    return buffers.Aliases(first, first_size, second, second_size);
}
std::vector<ResourceCache::Range> ResourceBindings::AccessRanges(VAddr border_colors) const {
    auto ranges = guest_ranges;
    for (const auto& entry : entries) {
        if (entry.binding.kind == ResourceKind::Image) {
            const auto& resource = entry.shader->info.images[entry.index];
            const auto sharp = resource.GetSharp(entry.shader->info);
            if (sharp) {
                const auto info = ShaderImageLayout(sharp, resource);
                ranges.push_back({info.guest_address, info.guest_size});
                ranges.push_back({info.stencil_addr, info.stencil_size});
            }
        } else if (entry.binding.kind == ResourceKind::Sampler) {
            const auto sharp =
                entry.shader->info.samplers[entry.index].GetSharp(entry.shader->info);
            if (sharp.border_color_type.Value() == AmdGpu::BorderColor::Custom) {
                const u64 offset = sharp.border_color_ptr.Value() * 16;
                if (!border_colors || border_colors > UINT64_MAX - offset - 16)
                    throw std::runtime_error("Invalid guest custom sampler border table");
                ranges.push_back({border_colors + offset, 16});
            }
        }
    }
    return ranges;
}
std::vector<ResourceCache::Range> ResourceBindings::ImageRanges() const {
    std::vector<ResourceCache::Range> result;
    for (const auto& entry : entries)
        if (entry.binding.kind == ResourceKind::Image) {
            const auto& resource = entry.shader->info.images[entry.index];
            const auto sharp = resource.GetSharp(entry.shader->info);
            if (sharp) {
                const auto info = ShaderImageLayout(sharp, resource);
                result.push_back({info.guest_address, info.guest_size});
                result.push_back({info.stencil_addr, info.stencil_size});
            }
        }
    return result;
}
void ResourceBindings::ValidateInputRange(VAddr address, u64 size) const {
    if (address > UINT64_MAX - size)
        throw std::runtime_error("Guest input range overflow");
    for (u32 i = 0; i < entries.size(); ++i) {
        const auto& entry = entries[i];
        if (entry.binding.kind == ResourceKind::Buffer &&
            entry.shader->info.buffers[entry.index].is_written &&
            Aliases(address, size, guest_ranges[i].address, guest_ranges[i].size))
            throw std::runtime_error(
                "D3D12 writable shader/input buffer alias requires coherent fetching");
        if (entry.binding.kind == ResourceKind::Image) {
            const auto& resource = entry.shader->info.images[entry.index];
            const auto sharp = resource.GetSharp(entry.shader->info);
            if (sharp && resource.is_written) {
                const auto info = ShaderImageLayout(sharp, resource);
                const bool aliases_depth =
                    Aliases(address, size, info.guest_address, info.guest_size);
                const bool aliases_stencil =
                    Aliases(address, size, info.stencil_addr, info.stencil_size);
                if (aliases_depth || aliases_stencil)
                    throw std::runtime_error(
                        "D3D12 writable image/input alias requires coherent fetching");
            }
        }
    }
}
void ResourceBindings::ValidateTargetRange(VAddr address, u64 size, bool written,
                                           bool allow_sampled_attachment_feedback) const {
    if (address > UINT64_MAX - size)
        throw std::runtime_error("Guest render target range overflow");
    for (u32 i = 0; i < entries.size(); ++i) {
        const auto& entry = entries[i];
        if (Aliases(address, size, guest_ranges[i].address, guest_ranges[i].size)) {
            const auto& resource = entry.shader->info.buffers[entry.index];
            if (written || resource.is_written ||
                resource.buffer_type == Shader::BufferType::Strmout)
                throw std::runtime_error(
                    "D3D12 writable attachment/buffer feedback requires ordered access");
        }
        if (entry.binding.kind == ResourceKind::Image) {
            const auto& resource = entry.shader->info.images[entry.index];
            const auto sharp = resource.GetSharp(entry.shader->info);
            if (sharp) {
                const auto info = ShaderImageLayout(sharp, resource);
                const bool overlap = Aliases(address, size, info.guest_address, info.guest_size) ||
                                     Aliases(address, size, info.stencil_addr, info.stencil_size);
                const bool sampled_feedback = allow_sampled_attachment_feedback && written &&
                                              !resource.is_written && !resource.is_feedback &&
                                              !resource.is_atomic &&
                                              entry.binding.type == D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
                if (overlap && !sampled_feedback &&
                    (written || resource.is_written || !allow_sampled_attachment_feedback))
                    throw std::runtime_error(
                        "D3D12 writable attachment/image feedback requires ordered access");
            }
        }
    }
}
std::vector<ResourceCache::Range> ResourceBindings::CpuRanges(VAddr border_colors,
                                                              bool written_only) const {
    std::vector<ResourceCache::Range> ranges;
    for (u32 i = 0; i < entries.size(); ++i) {
        const auto& entry = entries[i];
        if (entry.binding.kind == ResourceKind::Image || !guest_ranges[i].address ||
            !guest_ranges[i].size)
            continue;
        if (written_only && (entry.binding.kind != ResourceKind::Buffer ||
                             !entry.shader->info.buffers[entry.index].is_written))
            continue;
        ranges.push_back(guest_ranges[i]);
    }
    if (!written_only)
        for (const auto& entry : entries) {
            if (entry.binding.kind != ResourceKind::Sampler)
                continue;
            const auto sharp =
                entry.shader->info.samplers[entry.index].GetSharp(entry.shader->info);
            if (sharp.border_color_type.Value() != AmdGpu::BorderColor::Custom)
                continue;
            const u64 offset = sharp.border_color_ptr.Value() * 16;
            if (!border_colors || border_colors > UINT64_MAX - offset - 16)
                throw std::runtime_error("Invalid guest custom sampler border table");
            ranges.push_back({border_colors + offset, 16});
        }
    return ranges;
}
void ResourceBindings::PrepareImages() {
    for (const auto& entry : entries) {
        if (entry.binding.kind != ResourceKind::Image)
            continue;
        const auto& resource = entry.shader->info.images[entry.index];
        const auto sharp = resource.GetSharp(entry.shader->info);
        if (!sharp)
            continue;
        const auto type = sharp.GetViewType(resource.is_array);
        const bool array =
            type == AmdGpu::ImageType::Color1DArray || type == AmdGpu::ImageType::Color2DArray ||
            type == AmdGpu::ImageType::Color2DMsaaArray || type == AmdGpu::ImageType::Cube;
        const auto info = ShaderImageLayout(sharp, resource);
        auto format = ConvertFormat(info.pixel_format);
        const bool writable = entry.binding.type == D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        if (resource.is_depth)
            format = sharp.GetDataFmt() == AmdGpu::DataFormat::Format16 ? DXGI_FORMAT_R16_UNORM
                                                                        : DXGI_FORMAT_R32_FLOAT;
        if (writable && format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB)
            format = DXGI_FORMAT_R8G8B8A8_UNORM;
        images.GetImage(info, TypelessFormat(format),
                        writable ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS
                                 : D3D12_RESOURCE_FLAG_NONE,
                        array ? 0 : sharp.base_array, array ? 0 : 1);
    }
}
void ResourceBindings::SnapshotAttachmentFeedback(std::span<ImageCache::Image*> attachments,
                                                  std::span<const bool> attachment_writes,
                                                  bool writes_only_first_layer,
                                                  ID3D12GraphicsCommandList* commands) {
    constexpr u64 MaxSnapshotBytes = 32ULL * 1024 * 1024;
    if (attachment_writes.size() != attachments.size())
        throw std::runtime_error("D3D12 attachment feedback write-mask count is invalid");
    feedback_resource_views.assign(entries.size(), nullptr);
    feedback_snapshot_bytes = 0;
    std::vector<ImageCache::Image*> source_targets(entries.size());
    const auto same_backing = [&](VAddr first, u64 first_size, VAddr second, u64 second_size) {
        if (first_size != second_size)
            return false;
        const auto a = buffers.Canonicalize(first, first_size);
        const auto b = buffers.Canonicalize(second, second_size);
        return a.address == b.address && a.physical == b.physical;
    };
    for (u32 i = 0; i < entries.size(); ++i) {
        const auto& entry = entries[i];
        if (entry.binding.kind != ResourceKind::Image)
            continue;
        const auto& resource = entry.shader->info.images[entry.index];
        const auto sharp = resource.GetSharp(entry.shader->info);
        if (!sharp)
            continue;
        const auto info = ShaderImageLayout(sharp, resource);
        ImageCache::Image* target{};
        u32 target_slot{};
        for (u32 slot = 0; slot < attachments.size(); ++slot) {
            auto* attachment = attachments[slot];
            if (!attachment || !Aliases(attachment->info.guest_address, attachment->info.guest_size,
                                        info.guest_address, info.guest_size))
                continue;
            if (target)
                throw std::runtime_error(
                    "D3D12 attachment feedback aliases multiple color targets");
            target = attachment;
            target_slot = slot;
        }
        if (!target)
            continue;
        if (entry.binding.type != D3D12_DESCRIPTOR_RANGE_TYPE_SRV || resource.is_written ||
            resource.is_feedback || resource.is_atomic)
            throw std::runtime_error(
                "D3D12 attachment feedback supports sampled reads only; ordered storage "
                "access is unsupported");
        const auto view_type = sharp.GetViewType(resource.is_array);
        const bool multisampled = info.num_samples > 1;
        const bool array_view = view_type == AmdGpu::ImageType::Color2DArray ||
                                view_type == AmdGpu::ImageType::Color2DMsaaArray;
        const bool supported_dimension = multisampled
                                             ? (view_type == AmdGpu::ImageType::Color2DMsaa ||
                                                view_type == AmdGpu::ImageType::Color2DMsaaArray)
                                             : (view_type == AmdGpu::ImageType::Color2D ||
                                                view_type == AmdGpu::ImageType::Color2DArray);
        const u32 view_layers = sharp.NumViewLayers(resource.is_array);
        const bool supported_array_view =
            array_view && AttachmentFeedback::IsSafeArrayReadRange(
                              sharp.base_array, view_layers, info.resources.layers,
                              attachment_writes[target_slot], writes_only_first_layer);
        const bool supported_2d_view = !attachment_writes[target_slot] && !array_view &&
                                       sharp.base_array == 0 &&
                                       view_layers == info.resources.layers;
        const bool supported_view =
            supported_dimension && sharp.base_level == 0 &&
            sharp.NumViewLevels(resource.is_array) == 1 &&
            (supported_array_view || supported_2d_view) && sharp.NumSamples() == info.num_samples &&
            resource.mip_fallback_mode == Shader::MipStorageFallbackMode::None;
        const bool matching_layout =
            !resource.is_depth && !info.props.is_depth && !info.props.is_block &&
            !info.props.is_volume && !target->info.props.is_depth && !target->info.props.is_block &&
            !target->info.props.is_volume && !target->has_stencil && !target->logic_op &&
            target->base_layer == 0 && target->native_layers == target->info.resources.layers &&
            target->info.resources.levels == 1 && info.resources.levels == 1 &&
            target->info.resources.layers == info.resources.layers &&
            target->info.num_samples == info.num_samples &&
            info.pixel_format == target->info.pixel_format &&
            info.guest_size == target->info.guest_size &&
            info.size.width == target->info.size.width &&
            info.size.height == target->info.size.height && info.pitch == target->info.pitch &&
            info.num_bits == target->info.num_bits && info.tile_mode == target->info.tile_mode &&
            info.bank_swizzle == target->info.bank_swizzle &&
            info.alt_tile == target->info.alt_tile;
        if (!supported_view || !matching_layout ||
            !same_backing(info.guest_address, info.guest_size, target->info.guest_address,
                          target->info.guest_size))
            throw std::runtime_error(
                "D3D12 attachment feedback requires a contained single-mip 2D color view; "
                "writable targets need a disjoint array slice and a proven first-layer draw");
        source_targets[i] = target;
    }

    std::map<ImageCache::Image*, ID3D12Resource*> snapshots;
    u64 snapshot_bytes{};
    for (u32 i = 0; i < source_targets.size(); ++i) {
        auto* target = source_targets[i];
        if (!target)
            continue;
        if (const auto found = snapshots.find(target); found != snapshots.end()) {
            feedback_resource_views[i] = found->second;
            continue;
        }
        auto description = target->Native()->GetDesc();
        if (description.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
            description.DepthOrArraySize != target->native_layers || description.MipLevels != 1 ||
            description.SampleDesc.Count != target->info.num_samples ||
            description.Format != ConvertFormat(target->info.pixel_format))
            throw std::runtime_error(
                "D3D12 attachment feedback target has an unsupported native resource shape");
        if (description.SampleDesc.Count > 1) {
            D3D12_FEATURE_DATA_FORMAT_SUPPORT format_support{description.Format};
            D3D12_FEATURE_DATA_MULTISAMPLE_QUALITY_LEVELS quality{description.Format,
                                                                  description.SampleDesc.Count};
            if (FAILED(device.Device()->CheckFeatureSupport(
                    D3D12_FEATURE_FORMAT_SUPPORT, &format_support, sizeof(format_support))) ||
                !(format_support.Support1 & D3D12_FORMAT_SUPPORT1_MULTISAMPLE_LOAD) ||
                FAILED(device.Device()->CheckFeatureSupport(
                    D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS, &quality, sizeof(quality))) ||
                description.SampleDesc.Quality >= quality.NumQualityLevels)
                throw std::runtime_error(
                    "D3D12 attachment feedback format lacks the required multisample shader load");
        }
        description.Flags = D3D12_RESOURCE_FLAG_NONE;
        const auto allocation = device.Device()->GetResourceAllocationInfo(0, 1, &description);
        if (allocation.SizeInBytes == UINT64_MAX ||
            allocation.SizeInBytes > MaxSnapshotBytes - snapshot_bytes)
            throw std::runtime_error(
                "D3D12 attachment feedback snapshots exceed the 32 MiB per-draw budget");
        snapshot_bytes += allocation.SizeInBytes;
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        feedback_snapshots.emplace_back();
        auto& snapshot = feedback_snapshots.back();
        winrt::check_hresult(Core::Uwp::AllocateMemory([&] {
            return device.Device()->CreateCommittedResource(
                &heap, D3D12_HEAP_FLAG_NONE, &description, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                IID_PPV_ARGS(&snapshot));
        }));
        images.Barrier(*target, commands, D3D12_RESOURCE_STATE_COPY_SOURCE);
        commands->CopyResource(snapshot.Get(), target->Native());
        images.Barrier(*target, commands, D3D12_RESOURCE_STATE_RENDER_TARGET);
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition = {snapshot.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                              D3D12_RESOURCE_STATE_COPY_DEST,
                              D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
                                  D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE};
        commands->ResourceBarrier(1, &barrier);
        snapshots.emplace(target, snapshot.Get());
        feedback_resource_views[i] = snapshot.Get();
    }
    feedback_snapshot_bytes = snapshot_bytes;
}
void ResourceBindings::Upload(ID3D12GraphicsCommandList* commands, Shader::PushData& push,
                              std::span<const float, 24> clip_planes, VAddr border_colors,
                              const ResourceCache::Read& read, std::span<const u8, 65536> gds,
                              std::span<const u8> shared_memory) {
    const auto views = buffers.GetViews(guest_ranges);
    std::map<const CompiledShader*, std::vector<u32>> address_tables;
    for (u32 i = 0; i < entries.size(); ++i) {
        const auto& entry = entries[i];
        if (entry.binding.kind != ResourceKind::Buffer ||
            entry.shader->info.buffers[entry.index].buffer_type != Shader::BufferType::Guest)
            continue;
        auto& table = address_tables[entry.shader];
        if (table.empty())
            table.resize(entry.shader->info.buffers.size() * 4);
        const auto& view = views[i];
        auto segments = view.segments;
        if (segments.empty() && view.buffer)
            segments.push_back({view.guest_address, view.offset, view.size});
        u32 shift = 14;
        const u32 bias = segments.empty() ? 0 : segments.front().offset & 3;
        for (const auto& segment : segments) {
            const u64 begin = segment.address - view.guest_address + bias;
            const u64 end = begin + segment.size;
            if ((segment.offset & 3) != (begin & 3))
                throw std::runtime_error("D3D12 buffer mapping splits a physical word");
            while (shift > 2 && ((begin != bias && (begin & ((1u << shift) - 1))) ||
                                 (end != view.size + bias && (end & ((1u << shift) - 1)))))
                --shift;
        }
        const u32 mask = (1u << shift) - 1;
        const u32 pages = std::max<u64>(1, (view.size + bias + mask) >> shift);
        const u32 start = table.size();
        if (u64(start) + pages > 4ULL * 1024 * 1024)
            throw std::runtime_error("D3D12 buffer address tables exceed allocation budget");
        table.resize(start + pages, UINT32_MAX);
        table[entry.index * 4] = start;
        table[entry.index * 4 + 1] = shift;
        table[entry.index * 4 + 2] = bias;
        table[entry.index * 4 + 3] = view.size;
        for (const auto& segment : segments) {
            const u64 begin = segment.address - view.guest_address + bias;
            const u64 end = begin + segment.size;
            for (u64 page = begin >> shift; page <= (end - 1) >> shift; ++page)
                table[start + page] = u32(segment.offset + (page << shift) - begin);
        }
    }
    for (u32 i = 0; i < entries.size(); ++i) {
        if (entries[i].binding.kind != ResourceKind::Buffer || !views[i].buffer)
            continue;
        for (u32 j = 0; j < i; ++j) {
            if (entries[j].binding.kind != ResourceKind::Buffer || !views[j].buffer ||
                views[i].buffer == views[j].buffer)
                continue;
            if ((entries[i].shader->info.buffers[entries[i].index].is_written ||
                 entries[j].shader->info.buffers[entries[j].index].is_written) &&
                Aliases(guest_ranges[i].address, guest_ranges[i].size, guest_ranges[j].address,
                        guest_ranges[j].size))
                throw std::runtime_error(
                    "D3D12 segmented writable aliases require a shared allocation");
        }
    }
    std::vector<ResourceCache::Buffer*> uploaded_buffers;
    std::vector<ImageCache::Image*> uploaded_images;
    struct BoundImage {
        struct BackingRanges {
            VAddr depth_address;
            u64 depth_size;
            VAddr stencil_address;
            u64 stencil_size;
        } ranges;
        bool writable;
        ImageCache::Image* image;
    };
    const auto image_overlaps_range = [&](const BoundImage::BackingRanges& image, VAddr address,
                                          u64 size) {
        return Aliases(image.depth_address, image.depth_size, address, size) ||
               Aliases(image.stencil_address, image.stencil_size, address, size);
    };
    const auto image_ranges_overlap = [&](const BoundImage::BackingRanges& first,
                                          const BoundImage::BackingRanges& second) {
        return image_overlaps_range(first, second.depth_address, second.depth_size) ||
               image_overlaps_range(first, second.stencil_address, second.stencil_size);
    };
    std::vector<BoundImage> bound_images;
    for (u32 i = 0; i < entries.size(); ++i) {
        const auto& entry = entries[i];
        const auto& info = entry.shader->info;
        if (entry.binding.kind == ResourceKind::Buffer) {
            const auto& resource = info.buffers[entry.index];
            auto view = views[i];
            if (resource.buffer_type == Shader::BufferType::Flatbuf) {
                const u64 bytes = (info.flattened_ud_buf.empty() ? Shader::NUM_USER_DATA_REGS
                                                                 : info.flattened_ud_buf.size()) *
                                  sizeof(u32);
                view = {&buffers.GetHostBuffer(0xffff000000000000ULL + (u64(info.sw_stage) << 32),
                                               {reinterpret_cast<const u8*>(info.flat_ud),
                                                static_cast<size_t>(bytes)}),
                        0, bytes};
            } else if (resource.buffer_type == Shader::BufferType::BufferAddressTable) {
                const auto& table = address_tables.at(entry.shader);
                const u64 identity = 0xfff9000000000000ULL + (u64(info.sw_stage) << 32);
                view = {&buffers.GetHostBuffer(identity, {reinterpret_cast<const u8*>(table.data()),
                                                          table.size() * sizeof(u32)}),
                        0, table.size() * sizeof(u32)};
            } else if (resource.buffer_type == Shader::BufferType::ImageAddressTable) {
                const auto& table = entry.shader->image_address_tables.at(entry.index);
                const u64 identity =
                    0xfffa000000000000ULL + (u64(info.sw_stage) << 32) + (u64(entry.index) << 26);
                view = {&buffers.GetHostBuffer(identity, {reinterpret_cast<const u8*>(table.data()),
                                                          table.size() * sizeof(u32)}),
                        0, table.size() * sizeof(u32)};
            } else if (resource.buffer_type == Shader::BufferType::ClipPlanes)
                view = {&buffers.GetHostBuffer(0xfffd000000000000ULL,
                                               {reinterpret_cast<const u8*>(clip_planes.data()),
                                                clip_planes.size_bytes()}),
                        0, clip_planes.size_bytes()};
            else if (resource.buffer_type == Shader::BufferType::GdsBuffer) {
                gds_buffer = &buffers.GetHostBuffer(0xfffc000000000000ULL, gds);
                view = {gds_buffer, 0, gds.size_bytes()};
            } else if (resource.buffer_type == Shader::BufferType::StrmoutCounter) {
                const std::array<u8, 16> zero{};
                strmout_counter = &buffers.GetHostBuffer(0xfff8000000000000ULL, zero);
                strmout_counter->snapshot.clear();
                view = {strmout_counter, 0, zero.size()};
            } else if (resource.buffer_type == Shader::BufferType::SharedMemory) {
                if (shared_memory.empty())
                    throw std::runtime_error("Missing D3D12 shared memory backing");
                auto& scratch = buffers.GetHostBuffer(0xfffb000000000000ULL, shared_memory);
                scratch.snapshot.clear();
                view = {&scratch, 0, shared_memory.size()};
            } else if (resource.buffer_type == Shader::BufferType::SamplerTable) {
                if (info.samplers.empty())
                    throw std::runtime_error("Missing guest sampler table for lowered sampling");
                std::vector<u32> words(info.samplers.size() * SharedSamplerWords, 0);
                for (u32 i = 0; i < info.samplers.size(); ++i) {
                    const auto sharp = info.samplers[i].GetSharp(info);
                    if (!sharp.Valid())
                        throw std::runtime_error("Invalid guest sampler");
                    CheckWrapMode(sharp.clamp_x);
                    CheckWrapMode(sharp.clamp_y);
                    CheckWrapMode(sharp.clamp_z);
                    const u32 xy_linear = sharp.xy_min_filter == AmdGpu::Filter::Bilinear ? 1U : 0U;
                    const u32 z_linear = sharp.z_filter == AmdGpu::ZFilter::Linear ? 1U : 0U;
                    u32 border = 0;
                    std::array<float, 4> border_float{};
                    switch (sharp.border_color_type.Value()) {
                    case AmdGpu::BorderColor::TransparentBlack:
                        break;
                    case AmdGpu::BorderColor::OpaqueBlack:
                        border = 0xff000000u;
                        border_float[3] = 1.f;
                        break;
                    case AmdGpu::BorderColor::White:
                        border = 0xffffffffu;
                        border_float.fill(1.f);
                        break;
                    case AmdGpu::BorderColor::Custom: {
                        const u64 offset = sharp.border_color_ptr.Value() * 16;
                        if (!border_colors || border_colors > UINT64_MAX - offset - 16)
                            throw std::runtime_error("Invalid guest custom sampler border table");
                        read(border_colors + offset,
                             {reinterpret_cast<u8*>(border_float.data()), sizeof(border_float)});
                        break;
                    }
                    }
                    const u32 entry = i * SharedSamplerWords;
                    words[entry] = u32(sharp.clamp_x.Value()) | u32(sharp.clamp_y.Value()) << 3 |
                                   u32(sharp.clamp_z.Value()) << 6 | xy_linear << 9 |
                                   z_linear << 10 | u32(sharp.depth_compare_func.Value()) << 11 |
                                   (sharp.trunc_coord ? 1U << 14 : 0U);
                    words[entry + 1] = border;
                    words[entry + 2] = std::bit_cast<u32>(sharp.MinLod());
                    words[entry + 3] = std::bit_cast<u32>(sharp.MaxLod());
                    words[entry + 4] = std::bit_cast<u32>(sharp.LodBias());
                    words[entry + 5] =
                        u32(sharp.xy_mag_filter.Value()) | (u32(sharp.xy_min_filter.Value()) << 2) |
                        (u32(sharp.mip_filter.Value()) << 4) |
                        (u32(sharp.filter_mode.Value()) << 6) |
                        (u32(sharp.max_aniso.Value()) << 8) | (u32(sharp.z_filter.Value()) << 11);
                    for (u32 c = 0; c < 4; ++c)
                        words[entry + 6 + c] = std::bit_cast<u32>(border_float[c]);
                    std::memcpy(words.data() + entry + 12, &sharp, sizeof(sharp));
                }
                const u64 identity = 0xfff7000000000000ULL + (u64(info.sw_stage) << 32);
                view = {&buffers.GetHostBuffer(identity, {reinterpret_cast<const u8*>(words.data()),
                                                          words.size() * sizeof(u32)}),
                        0, words.size() * sizeof(u32)};
            } else if (resource.buffer_type != Shader::BufferType::Guest)
                push.AddOffset(entry.shader->start_bindings.buffer + entry.index, view.offset & 15);
            D3D12_UNORDERED_ACCESS_VIEW_DESC descriptor{};
            descriptor.Format = DXGI_FORMAT_R32_TYPELESS;
            descriptor.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
            const bool translated = resource.buffer_type == Shader::BufferType::Guest;
            descriptor.Buffer.FirstElement = translated ? 0 : (view.offset & ~u64{15}) / 4;
            descriptor.Buffer.NumElements =
                view.buffer
                    ? ((translated ? view.buffer->size : view.size + (view.offset & 15)) + 3) / 4
                    : 1;
            descriptor.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
            device.Device()->CreateUnorderedAccessView(
                view.buffer ? view.buffer->native.Get() : nullptr, nullptr, &descriptor,
                resources.Cpu(entry.heap_index));
            if (view.buffer) {
                if (std::ranges::find(uploaded_buffers, view.buffer) == uploaded_buffers.end()) {
                    buffers.Upload(*view.buffer, commands);
                    uploaded_buffers.push_back(view.buffer);
                }
                if (resource.is_written && resource.buffer_type != Shader::BufferType::SharedMemory)
                    buffers.MarkWritten(view);
            }
        } else if (entry.binding.kind == ResourceKind::Sampler) {
            const auto& resource = info.samplers[entry.index];
            const auto sharp = resource.GetSharp(info);
            if (!sharp.Valid())
                throw std::runtime_error("Invalid guest sampler");
            D3D12_SAMPLER_DESC descriptor{};
            descriptor.AddressU = AddressMode(sharp.clamp_x);
            descriptor.AddressV = AddressMode(sharp.clamp_y);
            descriptor.AddressW = AddressMode(sharp.clamp_z);
            const bool aniso = AmdGpu::IsAnisoFilter(sharp.xy_min_filter) ||
                               AmdGpu::IsAnisoFilter(sharp.xy_mag_filter);
            const auto reduction = resource.is_depth ? D3D12_FILTER_REDUCTION_TYPE_COMPARISON
                                   : sharp.filter_mode.Value() == AmdGpu::FilterMode::Min
                                       ? D3D12_FILTER_REDUCTION_TYPE_MINIMUM
                                   : sharp.filter_mode.Value() == AmdGpu::FilterMode::Max
                                       ? D3D12_FILTER_REDUCTION_TYPE_MAXIMUM
                                       : D3D12_FILTER_REDUCTION_TYPE_STANDARD;
            const bool point_mip_aniso = SamplerFilterPolicy::UseAnisotropicPointMip(
                aniso, device.AnisoFilterWithPointMipSupported(), sharp.mip_filter.Value());
            const D3D12_FILTER anisotropic_filter =
                point_mip_aniso ? static_cast<D3D12_FILTER>(
                                      D3D12_ANISOTROPIC_FILTERING_BIT |
                                      D3D12_ENCODE_BASIC_FILTER(D3D12_FILTER_TYPE_LINEAR,
                                                                D3D12_FILTER_TYPE_LINEAR,
                                                                D3D12_FILTER_TYPE_POINT, reduction))
                                : D3D12_ENCODE_ANISOTROPIC_FILTER(reduction);
            descriptor.Filter =
                aniso
                    ? anisotropic_filter
                    : D3D12_ENCODE_BASIC_FILTER(
                          sharp.xy_min_filter == AmdGpu::Filter::Bilinear ? D3D12_FILTER_TYPE_LINEAR
                                                                          : D3D12_FILTER_TYPE_POINT,
                          sharp.xy_mag_filter == AmdGpu::Filter::Bilinear ? D3D12_FILTER_TYPE_LINEAR
                                                                          : D3D12_FILTER_TYPE_POINT,
                          sharp.mip_filter == AmdGpu::MipFilter::Linear ? D3D12_FILTER_TYPE_LINEAR
                                                                        : D3D12_FILTER_TYPE_POINT,
                          reduction);
            descriptor.MipLODBias = std::clamp(sharp.LodBias(), -16.0f, 15.99f);
            descriptor.MinLOD = sharp.MinLod();
            descriptor.MaxLOD = sharp.MaxLod();
            descriptor.MaxAnisotropy = static_cast<UINT>(sharp.MaxAniso());
            descriptor.ComparisonFunc =
                D3D12_COMPARISON_FUNC(u32(sharp.depth_compare_func.Value()) + 1);
            switch (sharp.border_color_type.Value()) {
            case AmdGpu::BorderColor::TransparentBlack:
                break;
            case AmdGpu::BorderColor::OpaqueBlack:
                descriptor.BorderColor[3] = 1;
                break;
            case AmdGpu::BorderColor::White:
                std::fill_n(descriptor.BorderColor, 4, 1.f);
                break;
            case AmdGpu::BorderColor::Custom: {
                const u64 offset = sharp.border_color_ptr.Value() * 16;
                if (!border_colors || border_colors > UINT64_MAX - offset - 16)
                    throw std::runtime_error("Invalid guest custom sampler border table");
                read(border_colors + offset, {reinterpret_cast<u8*>(descriptor.BorderColor),
                                              sizeof(descriptor.BorderColor)});
                break;
            }
            }
            device.Device()->CreateSampler(&descriptor, samplers.Cpu(entry.heap_index));
        } else {
            const auto& resource = info.images[entry.index];
            const auto sharp = resource.GetSharp(info);
            const bool writable = entry.binding.type == D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
            const auto type = sharp.GetViewType(resource.is_array);
            const bool array = type == AmdGpu::ImageType::Color1DArray ||
                               type == AmdGpu::ImageType::Color2DArray ||
                               type == AmdGpu::ImageType::Color2DMsaaArray ||
                               type == AmdGpu::ImageType::Cube;
            const bool volume = type == AmdGpu::ImageType::Color3D;
            const bool multisampled = type == AmdGpu::ImageType::Color2DMsaa ||
                                      type == AmdGpu::ImageType::Color2DMsaaArray;
            if (multisampled && (writable || sharp.NumSamples() == 1))
                throw std::runtime_error(
                    "D3D12 multisample images require read-only views and at least two samples");
            DXGI_FORMAT format = ConvertFormat(ShaderImageLayout(sharp, resource).pixel_format);
            if (resource.is_depth)
                format = sharp.GetDataFmt() == AmdGpu::DataFormat::Format16 ? DXGI_FORMAT_R16_UNORM
                                                                            : DXGI_FORMAT_R32_FLOAT;
            if (writable && format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB)
                format = DXGI_FORMAT_R8G8B8A8_UNORM;
            if (resource.is_feedback) {
                D3D12_FEATURE_DATA_FORMAT_SUPPORT support{format};
                winrt::check_hresult(device.Device()->CheckFeatureSupport(
                    D3D12_FEATURE_FORMAT_SUPPORT, &support, sizeof(support)));
                if (!(support.Support2 & D3D12_FORMAT_SUPPORT2_UAV_TYPED_LOAD))
                    throw std::runtime_error(
                        "D3D12 image feedback requires typed UAV load support");
            }
            ImageCache::Image* image{};
            if (sharp) {
                const auto image_info = ShaderImageLayout(sharp, resource);
                const BoundImage::BackingRanges image_ranges{
                    image_info.guest_address, image_info.guest_size, image_info.stencil_addr,
                    image_info.stencil_size};
                for (const auto& bound : bound_images)
                    if (image_ranges_overlap(image_ranges, bound.ranges) &&
                        bound.writable != writable)
                        throw std::runtime_error(
                            "D3D12 SRV/UAV image alias was not lowered to shared storage");
                for (u32 buffer_index = 0; buffer_index < entries.size(); ++buffer_index) {
                    const auto& buffer_entry = entries[buffer_index];
                    const auto& range = guest_ranges[buffer_index];
                    if (buffer_entry.binding.kind == ResourceKind::Buffer &&
                        (writable || resource.is_written ||
                         buffer_entry.shader->info.buffers[buffer_entry.index].is_written) &&
                        image_overlaps_range(image_ranges, range.address, range.size))
                        throw std::runtime_error(
                            "D3D12 simultaneous image/buffer aliases require coherent views");
                }
                image = &images.GetImage(image_info, TypelessFormat(format),
                                         writable ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS
                                                  : D3D12_RESOURCE_FLAG_NONE,
                                         array ? 0 : sharp.base_array, array ? 0 : 1);
                for (const auto& bound : bound_images)
                    if (image_ranges_overlap(image_ranges, bound.ranges) && bound.image != image &&
                        (writable || bound.writable))
                        throw std::runtime_error(
                            "D3D12 overlapping texture layouts require shared image views");
                bound_images.push_back({image_ranges, writable, image});
                if (!feedback_resource_views[i] &&
                    std::ranges::find(uploaded_images, image) == uploaded_images.end()) {
                    images.Upload(*image, commands);
                    uploaded_images.push_back(image);
                }
                if (resource.is_depth && !writable && image->has_stencil) {
                    switch (image->dsv_format) {
                    case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
                        format = DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
                        break;
                    case DXGI_FORMAT_D24_UNORM_S8_UINT:
                        format = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
                        break;
                    default:
                        throw std::runtime_error(
                            "D3D12 depth/stencil image has no compatible sampled view format");
                    }
                }
                if (!feedback_resource_views[i])
                    images.Barrier(*image, commands,
                                   writable ? D3D12_RESOURCE_STATE_UNORDERED_ACCESS
                                            : D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE |
                                                  D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
                image->written |= resource.is_written;
            }
            const u32 mip =
                sharp.base_level +
                (resource.mip_fallback_mode == Shader::MipStorageFallbackMode::DynamicIndex
                     ? entry.element
                 : resource.mip_fallback_mode == Shader::MipStorageFallbackMode::ConstantIndex
                     ? resource.constant_mip_index
                     : 0);
            if (mip >= sharp.NumLevels() || sharp.base_array >= sharp.NumLayers())
                throw std::runtime_error("Guest image view range exceeds image allocation");
            if (writable) {
                D3D12_UNORDERED_ACCESS_VIEW_DESC descriptor{};
                descriptor.Format = format;
                descriptor.ViewDimension = volume  ? D3D12_UAV_DIMENSION_TEXTURE3D
                                           : array ? D3D12_UAV_DIMENSION_TEXTURE2DARRAY
                                                   : D3D12_UAV_DIMENSION_TEXTURE2D;
                if (volume)
                    descriptor.Texture3D = {mip, 0, std::max(1u, u32(sharp.depth + 1) >> mip)};
                else if (array)
                    descriptor.Texture2DArray = {mip, sharp.base_array,
                                                 sharp.NumViewLayers(resource.is_array), 0};
                else
                    descriptor.Texture2D = {mip, 0};
                device.Device()->CreateUnorderedAccessView(image ? image->Native() : nullptr,
                                                           nullptr, &descriptor,
                                                           resources.Cpu(entry.heap_index));
            } else {
                D3D12_SHADER_RESOURCE_VIEW_DESC descriptor{};
                descriptor.Format = format;
                descriptor.ViewDimension = multisampled
                                               ? (array ? D3D12_SRV_DIMENSION_TEXTURE2DMSARRAY
                                                        : D3D12_SRV_DIMENSION_TEXTURE2DMS)
                                           : volume ? D3D12_SRV_DIMENSION_TEXTURE3D
                                           : array  ? D3D12_SRV_DIMENSION_TEXTURE2DARRAY
                                                    : D3D12_SRV_DIMENSION_TEXTURE2D;
                const auto mapping = sharp.DstSelect();
                const std::array<UINT, 4> components{Component(mapping.r), Component(mapping.g),
                                                     Component(mapping.b), Component(mapping.a)};
                descriptor.Shader4ComponentMapping = D3D12_ENCODE_SHADER_4_COMPONENT_MAPPING(
                    components[0], components[1], components[2], components[3]);
                if (multisampled && array)
                    descriptor.Texture2DMSArray = {sharp.base_array,
                                                   sharp.NumViewLayers(resource.is_array)};
                else if (multisampled)
                    descriptor.Texture2DMS.UnusedField_NothingToDefine = 0;
                else if (volume)
                    descriptor.Texture3D = {mip, sharp.NumViewLevels(resource.is_array),
                                            float(sharp.min_lod) / 256};
                else if (array)
                    descriptor.Texture2DArray = {mip,
                                                 sharp.NumViewLevels(resource.is_array),
                                                 sharp.base_array,
                                                 sharp.NumViewLayers(resource.is_array),
                                                 0,
                                                 float(sharp.min_lod) / 256};
                else
                    descriptor.Texture2D = {mip, sharp.NumViewLevels(resource.is_array), 0,
                                            float(sharp.min_lod) / 256};
                ID3D12Resource* resource_view = feedback_resource_views[i];
                if (!resource_view && image)
                    resource_view = image->Native();
                device.Device()->CreateShaderResourceView(resource_view, &descriptor,
                                                          resources.Cpu(entry.heap_index));
            }
        }
    }
}
void ResourceBindings::Bind(ID3D12GraphicsCommandList* commands, bool graphics, u32 resource_slot,
                            u32 sampler_slot) {
    ID3D12DescriptorHeap* heaps[]{resources.Heap(), samplers.Heap()};
    commands->SetDescriptorHeaps(2, heaps);
    if (!resource_ranges.empty()) {
        if (graphics)
            commands->SetGraphicsRootDescriptorTable(resource_slot, resources.Gpu(0));
        else
            commands->SetComputeRootDescriptorTable(resource_slot, resources.Gpu(0));
    }
    if (!sampler_ranges.empty()) {
        if (graphics)
            commands->SetGraphicsRootDescriptorTable(sampler_slot, samplers.Gpu(0));
        else
            commands->SetComputeRootDescriptorTable(sampler_slot, samplers.Gpu(0));
    }
}
void ResourceBindings::CommitGds(std::span<u8, 65536> gds) {
    if (gds_buffer)
        std::memcpy(gds.data(), gds_buffer->host_data.data(), gds.size_bytes());
}
} // namespace D3D12
