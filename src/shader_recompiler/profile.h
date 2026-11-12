// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "common/types.h"

namespace Shader {
enum class GuestIsaGeneration : u8 {
    Unknown,
    Gcn2SeaIslands,
};

struct Profile {
    u32 max_viewport_width{};
    u32 max_viewport_height{};
    u32 max_shared_memory_size{};
    u32 supported_spirv{0x00010000};
    u32 subgroup_size{};
    u32 sparse_page_shift{};
    GuestIsaGeneration guest_isa_generation{GuestIsaGeneration::Unknown};
    bool support_int8{};
    bool support_int16{};
    bool support_int64{};
    bool support_bda{};
    bool support_float16{};
    bool support_float64{};
    // D3D12 wave64 gate for VOP3B unsigned add/sub with explicit carry/borrow masks.
    bool supports_vop3_co_u32_subset{};
    // D3D12 gate for GCN2 VOP3a MAD64 with implicit VCC_OUT, requiring Int64ShaderOps and wave64
    // ballot.
    bool supports_vop3_mad64_subset{};
    // D3D12-only GCN2 VOP3a V_LERP_U8 lowering using baseline U32 arithmetic.
    bool supports_vop3_lerp_u8_subset{};
    // D3D12-only exact VOP3 SAD byte/short expansion using baseline U32 arithmetic.
    bool supports_vop3_sad_subset{};
    // D3D12-only QSAD/MQSAD expansion with B64 S0 reads; requires device Int64ShaderOps.
    bool supports_vop3_sad_packed_subset{};
    // Narrow D3D12 compute wave64 DPP gate: SM6.4 with fixed wave64 or SM6.6 with WaveSize.
    bool supports_dpp_subset{};
    // Narrow D3D12-only gate for exact signed/unsigned 24-bit multiply-high lowering.
    bool supports_mul_hi_24_subset{};
    // Narrow D3D12-only gate for SDWA WORD_0/WORD_1 F16 VOP2 lowering.
    bool supports_sdwa_f16_subset{};
    // Narrow D3D12-only gate for validated 32-bit SDWA VOP1 source lowering.
    bool supports_sdwa_vop1_subset{};
    // Narrow D3D12-only gate for VOPC SDWAB with VCC destination and wave64.
    bool supports_sdwa_vopc_subset{};
    // D3D12-only wave64 gates for correct GCN DS_APPEND/DS_CONSUME lowering.
    bool is_d3d12_profile{};
    bool supports_d3d12_data_share_compute{};
    bool supports_d3d12_data_share_graphics{};
    bool allow_fp64_to_fp32_lowering{true};
    bool supports_denorm_behavior_independence{};
    bool supports_rounding_mode_independence{};
    bool support_fp16_denorm_preserve{};
    bool support_fp16_denorm_flush{};
    bool support_fp16_round_to_zero{};
    bool support_fp32_denorm_preserve{};
    bool support_fp32_denorm_flush{};
    bool support_fp32_round_to_zero{};
    bool support_fp64_denorm_preserve{};
    bool support_fp64_denorm_flush{};
    bool support_fp64_round_to_zero{};
    bool support_fp16_signed_zero_inf_nan_preserve{};
    bool support_fp32_signed_zero_inf_nan_preserve{};
    bool support_fp64_signed_zero_inf_nan_preserve{};
    bool supports_image_load_store_lod{};
    bool supports_image_sample_min_lod{};
    bool supports_native_cube_calc{};
    bool supports_trinary_minmax{};
    bool supports_buffer_fp32_atomic_min_max{};
    bool supports_image_fp32_atomic_min_max{};
    bool supports_buffer_int64_atomics{};
    bool supports_shared_int64_atomics{};
    bool supports_workgroup_explicit_memory_layout{};
    bool supports_amd_shader_explicit_vertex_parameter{};
    bool supports_fragment_shader_barycentric{};
    bool supports_shader_subgroup_clock{};
    bool has_broken_spirv_clamp{};
    bool lower_left_origin_mode{};
    bool needs_manual_interpolation{};
    bool needs_lds_barriers{};
    bool needs_buffer_offsets{};
    bool needs_unorm_fixup{};
    bool needs_clip_distance_emulation{};
    bool supports_shader_stencil_export{};
    bool needs_image_load_feedback{};
    bool needs_subword_buffer_emulation{};
    bool needs_signed_atomic32_emulation{};
    bool needs_inline_vertex_fetch{};
    bool needs_unified_buffer_types{};
    bool fast_shader_math{};
    bool reject_approximate_fmask_access{};
    bool supports_fmask_storage_lowering{};

    bool operator==(const Profile&) const = default;
};

} // namespace Shader
