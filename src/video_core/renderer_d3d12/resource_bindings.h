#pragma once

#include "d3d12_pipeline_cache.h"
#include "video_core/renderer_d3d12/image_cache.h"
#include "video_core/renderer_d3d12/shader_cache.h"

namespace D3D12 {
class ResourceBindings {
public:
    struct StreamOutSlot {
        VAddr address{};
        u64 size{};
    };
    ResourceBindings(D3D12DeviceContext& device, ResourceCache& buffers, ImageCache& images,
                     std::span<CompiledShader*> shaders,
                     std::span<const StreamOutSlot> strmout_slots,
                     D3D12DescriptorArena* resource_heap, D3D12DescriptorArena* sampler_heap,
                     bool fence_safe, DescriptorBindingTier descriptor_tier,
                     bool feature_level_11_1_or_higher);
    void Upload(ID3D12GraphicsCommandList* commands, Shader::PushData& push,
                std::span<const float, 24> clip_planes, VAddr border_colors,
                const ResourceCache::Read& read, std::span<const u8, 65536> gds,
                std::span<const u8> shared_memory = {});
    void PrepareImages();
    void SnapshotAttachmentFeedback(std::span<ImageCache::Image*> attachments,
                                    std::span<const bool> attachment_writes,
                                    bool writes_only_first_layer,
                                    ID3D12GraphicsCommandList* commands);
    const std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>>& FeedbackSnapshots() const {
        return feedback_snapshots;
    }
    u64 FeedbackSnapshotBytes() const {
        return feedback_snapshot_bytes;
    }
    std::span<const ResourceCache::Range> GuestRanges() const {
        return guest_ranges;
    }
    std::vector<ResourceCache::Range> ImageRanges() const;
    std::vector<ResourceCache::Range> CpuRanges(VAddr border_colors,
                                                bool written_only = false) const;
    void CommitGds(std::span<u8, 65536> gds);
    ResourceCache::Buffer* GdsBuffer() const {
        return gds_buffer;
    }
    ResourceCache::Buffer* StreamOutCounter() const {
        return strmout_counter;
    }
    const std::vector<D3D12_DESCRIPTOR_RANGE>& ResourceRanges() const {
        return resource_ranges;
    }
    const std::vector<D3D12_DESCRIPTOR_RANGE>& SamplerRanges() const {
        return sampler_ranges;
    }
    void Bind(ID3D12GraphicsCommandList* commands, bool graphics, u32 resource_slot,
              u32 sampler_slot);
    void ValidateInputRange(VAddr address, u64 size) const;
    void ValidateTargetRange(VAddr address, u64 size, bool written = true,
                             bool allow_sampled_attachment_feedback = false) const;
    std::vector<ResourceCache::Range> AccessRanges(VAddr border_colors) const;

private:
    bool Aliases(VAddr first, u64 first_size, VAddr second, u64 second_size) const;
    struct Entry {
        CompiledShader* shader;
        ShaderBinding binding;
        u32 index, element, heap_index;
    };
    D3D12DeviceContext& device;
    ResourceCache& buffers;
    ImageCache& images;
    std::vector<Entry> entries;
    std::vector<ResourceCache::Range> guest_ranges;
    std::vector<D3D12_DESCRIPTOR_RANGE> resource_ranges, sampler_ranges;
    std::vector<ID3D12Resource*> feedback_resource_views;
    std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>> feedback_snapshots;
    u64 feedback_snapshot_bytes{};
    D3D12DescriptorArena owned_resources, owned_samplers;
    D3D12DescriptorArena& resources;
    D3D12DescriptorArena& samplers;
    ResourceCache::Buffer* gds_buffer{};
    ResourceCache::Buffer* strmout_counter{};
};
} // namespace D3D12
