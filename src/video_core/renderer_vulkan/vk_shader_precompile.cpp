// SPDX-License-Identifier: GPL-2.0-or-later
#include <algorithm>
#include <array>
#include <bit>
#include <cstring>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <unordered_set>
#include <miniz.h>
#include <sirit/sirit.h>
#include <xxhash.h>
#include "common/elf_info.h"
#include "common/io_file.h"
#include "common/logging/log.h"
#include "common/scope_exit.h"
#include "common/scoped_assert.h"
#include "core/file_sys/fs.h"
#include "core/emulator_settings.h"
#include "shader_recompiler/backend/spirv/emit_spirv.h"
#include "shader_recompiler/frontend/fetch_shader.h"
#include "shader_recompiler/ir/passes/srt.h"
#include "shader_recompiler/runtime_info.h"
#include "video_core/cache_storage.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_pipeline_cache.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"
#ifdef _WIN32
#include <memoryapi.h>
#include <windows.h>
#else
#include <sys/mman.h>
#endif
#ifdef SHADPS4_UWP
#include "uwp/session.h"
#include "uwp/shadps4_api.h"
#endif

namespace Vulkan {
namespace {
constexpr u32 kSpirvVersion1_5 = 0x00010500u;
constexpr u32 kMaxPrecompileVertexAttribs = 28;
constexpr size_t MaxContainerBytes = 128 * 1024 * 1024;
constexpr u64 ManifestVersion = 0x4152434853480001ULL;

u16 ReadLe16(const u8* p) {
    u16 value;
    std::memcpy(&value, p, 2);
    return value;
}
u32 ReadLe32(const u8* p) {
    u32 value;
    std::memcpy(&value, p, 4);
    return value;
}
u64 ReadLe64(const u8* p) {
    u64 value;
    std::memcpy(&value, p, 8);
    return value;
}
u32 ReadBe32(const u8* p) {
    return std::byteswap(ReadLe32(p));
}
std::string ToLowerAscii(std::string text) {
    for (char& ch : text)
        if (ch >= 'A' && ch <= 'Z')
            ch += 'a' - 'A';
    return text;
}
void* AllocateDescriptorMemory(size_t bytes) {
    for (uintptr_t address = 0x10000; address + bytes <= 0x400000; address += 0x10000) {
#ifdef SHADPS4_UWP
        if (auto memory = VirtualAllocFromApp(reinterpret_cast<void*>(address), bytes,
                                              MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE))
            return memory;
#elif defined(_WIN32)
        if (auto memory = VirtualAlloc(reinterpret_cast<void*>(address), bytes,
                                       MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE))
            return memory;
#elif defined(MAP_FIXED_NOREPLACE)
        auto memory = mmap(reinterpret_cast<void*>(address), bytes, PROT_READ | PROT_WRITE,
                           MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
        if (memory != MAP_FAILED)
            return memory;
#else
        break;
#endif
    }
    return nullptr;
}
void FreeDescriptorMemory(void* memory, size_t bytes) {
#ifdef _WIN32
    VirtualFree(memory, 0, MEM_RELEASE);
#else
    munmap(memory, bytes);
#endif
}

bool DecompressDcxSpan(std::span<const u8> raw, std::vector<u8>& out_data) {
    out_data.clear();
    if (raw.size() < 16) {
        return false;
    }
    if (std::memcmp(raw.data(), "DCX\0", 4) != 0) {
        out_data.assign(raw.begin(), raw.end());
        return true;
    }
    if (raw.size() < 0x4C) {
        return false;
    }
    const u32 dcs_off = ReadBe32(raw.data() + 0x08);
    const u32 dcp_off = ReadBe32(raw.data() + 0x0C);
    if (dcs_off > raw.size() - 12 || dcp_off > raw.size() - 0x20) {
        return false;
    }
    if (std::memcmp(raw.data() + dcs_off, "DCS\0", 4) != 0 ||
        std::memcmp(raw.data() + dcp_off, "DCP\0", 4) != 0 ||
        std::memcmp(raw.data() + dcp_off + 4, "DFLT", 4) != 0) {
        return false;
    }
    const u32 uncomp_size = ReadBe32(raw.data() + dcs_off + 4);
    const u32 comp_size = ReadBe32(raw.data() + dcs_off + 8);
    if (uncomp_size == 0 || uncomp_size > 128u * 1024 * 1024 || comp_size == 0) {
        return false;
    }

    size_t zlib_off = 0;
    const size_t scan_limit = std::min<size_t>(raw.size() - 8, 256);
    for (size_t pos = dcp_off + 8; pos <= scan_limit; ++pos) {
        if (std::memcmp(raw.data() + pos, "DCA\0", 4) == 0) {
            const u32 dca_len = ReadBe32(raw.data() + pos + 4);
            if (dca_len >= 8 && pos + dca_len < raw.size()) {
                zlib_off = pos + dca_len;
                break;
            }
        }
    }
    if (zlib_off == 0 || zlib_off >= raw.size()) {
        return false;
    }
    const size_t avail_comp = std::min<size_t>(comp_size, raw.size() - zlib_off);
    out_data.resize(uncomp_size);
    mz_ulong dest_len = static_cast<mz_ulong>(uncomp_size);
    const int rc = mz_uncompress(out_data.data(), &dest_len, raw.data() + zlib_off,
                                 static_cast<mz_ulong>(avail_comp));
    if (rc != MZ_OK || dest_len != uncomp_size) {
        out_data.clear();
        return false;
    }
    return true;
}

std::string NormalizeShaderStem(std::string name) {
    for (char& c : name) {
        if (c == '\\') {
            c = '/';
        }
    }
    if (const size_t slash = name.rfind('/'); slash != std::string::npos) {
        name.erase(0, slash + 1);
    }
    name = ToLowerAscii(std::move(name));
    if (const size_t dot = name.find('.'); dot != std::string::npos) {
        name.erase(dot);
    }
    for (const char* suf : {"_vs", "_ps", "_vv", "_p", "_v", "_f"}) {
        const size_t len = std::strlen(suf);
        if (name.size() > len && name.ends_with(suf)) {
            name.erase(name.size() - len);
            break;
        }
    }
    return name;
}

std::string ReadBnd4EntryName(std::span<const u8> bnd, size_t entry_off) {
    if (entry_off + 0x24 > bnd.size() || bnd.size() < 0x40) {
        return {};
    }
    const u32 name_off = ReadLe32(bnd.data() + entry_off + 0x20);
    if (name_off >= bnd.size()) {
        return {};
    }
    std::string out;
    if (bnd[0x30] != 0) {
        for (size_t p = name_off; p + 1 < bnd.size() && out.size() < 256; p += 2) {
            const u16 ch = ReadLe16(bnd.data() + p);
            if (ch == 0) {
                break;
            }
            out.push_back(ch < 128 ? static_cast<char>(ch) : '_');
        }
    } else {
        for (size_t p = name_off; p < bnd.size() && out.size() < 256; ++p) {
            const u8 ch = bnd[p];
            if (ch == 0) {
                break;
            }
            out.push_back(ch < 128 ? static_cast<char>(ch) : '_');
        }
    }
    return NormalizeShaderStem(std::move(out));
}

template <typename Fn>
void ForEachContainerSlice(std::span<const u8> payload, Fn&& fn) {
    if (payload.size() >= 0x40 && std::memcmp(payload.data(), "BND4", 4) == 0) {
        const u32 count = ReadLe32(payload.data() + 0x0C);
        const u64 entry_size = ReadLe64(payload.data() + 0x20);
        if (entry_size >= 0x20 && entry_size <= 0x40 &&
            0x40ull + u64(count) * entry_size <= payload.size()) {
            std::vector<u8> nested_buf;
            for (u32 i = 0; i < count; ++i) {
                const size_t entry = 0x40 + size_t(i) * static_cast<size_t>(entry_size);
                const u64 len = ReadLe64(payload.data() + entry + 0x08);
                const u32 off = ReadLe32(payload.data() + entry + 0x18);
                if (len == 0 || off > payload.size() || len > payload.size() - off) {
                    continue;
                }
                const std::string stem =
                    entry_size >= 0x24 ? ReadBnd4EntryName(payload, entry) : std::string{};
                std::span<const u8> slice{payload.data() + off, static_cast<size_t>(len)};
                if (slice.size() >= 0x4C && std::memcmp(slice.data(), "DCX\0", 4) == 0) {
                    if (DecompressDcxSpan(slice, nested_buf)) {
                        fn(std::span<const u8>{nested_buf}, stem);
                    }
                } else {
                    fn(slice, stem);
                }
            }
            return;
        }
    }
    fn(payload, std::string{});
}

struct OrbProgram {
    size_t start = 0;
    size_t footer = 0;
    u64 hash = 0;
    u32 bytes = 0;
    u32 type = 0;
    u32 usages = 0;
    bool is_srt = false;
};

std::optional<OrbProgram> ParseOrbProgramAt(std::span<const u8> data, size_t at) {
    if (at + 8 > data.size() || ReadLe32(data.data() + at) != 0xbeeb03ffu) {
        return std::nullopt;
    }
    const u64 delta = (u64(ReadLe32(data.data() + at + 4)) + 1ull) * 8ull;
    if (delta < 8 || delta > 262144 || at + delta + 28 > data.size()) {
        return std::nullopt;
    }
    const u8* footer = data.data() + at + delta;
    if (std::memcmp(footer, "OrbShdr", 7) != 0) {
        return std::nullopt;
    }
    const u32 flags_len = ReadLe32(footer + 8);
    const u32 length = flags_len >> 8;
    if (length == 0 || (length % 4) != 0 || length > delta) {
        return std::nullopt;
    }
    return OrbProgram{
        .start = at,
        .footer = at + static_cast<size_t>(delta),
        .hash = ReadLe64(footer + 16),
        .bytes = length,
        .type = (flags_len >> 2) & 0xFu,
        .usages = footer[13],
        .is_srt = (footer[14] & 1u) != 0,
    };
}

enum class StageKind : u8 {
    Compute = 0,
    Fragment = 1,
    Vertex = 2,
};

struct ExtractedShader {
    StageKind kind{};
    u64 hash = 0;
    bool is_srt = false;
    std::array<u32, 13> regs{};
    std::vector<u32> usages;
    std::vector<u32> semantics;
    std::vector<std::pair<u8, u8>> vs_exports;
    std::vector<u32> code;
};

bool MapVsOutputsSafe(std::span<Shader::OutputMap, 3> outputs, const AmdGpu::VsOutputControl& ctl,
                      u32& out_count) {
    out_count = 0;
    if (ctl.use_vtx_edge_flag || ctl.use_vtx_kill_flag || ctl.use_vtx_gs_cut_flag) {
        return false;
    }
    if (ctl.vs_out_misc_enable) {
        auto& misc_vec = outputs[out_count++];
        misc_vec[0] = ctl.use_vtx_point_size ? Shader::Output::PointSize : Shader::Output::None;
        misc_vec[1] = Shader::Output::None;
        misc_vec[2] = ctl.use_vtx_render_target_idx ? Shader::Output::RenderTargetIndex
                                                    : Shader::Output::None;
        misc_vec[3] =
            ctl.use_vtx_viewport_idx ? Shader::Output::ViewportIndex : Shader::Output::None;
    }
    if (ctl.vs_out_ccdist0_enable) {
        auto& ccdist0 = outputs[out_count++];
        ccdist0[0] =
            ctl.IsClipDistEnabled(0)
                ? Shader::Output::ClipDist0
                : (ctl.IsCullDistEnabled(0) ? Shader::Output::CullDist0 : Shader::Output::None);
        ccdist0[1] =
            ctl.IsClipDistEnabled(1)
                ? Shader::Output::ClipDist1
                : (ctl.IsCullDistEnabled(1) ? Shader::Output::CullDist1 : Shader::Output::None);
        ccdist0[2] =
            ctl.IsClipDistEnabled(2)
                ? Shader::Output::ClipDist2
                : (ctl.IsCullDistEnabled(2) ? Shader::Output::CullDist2 : Shader::Output::None);
        ccdist0[3] =
            ctl.IsClipDistEnabled(3)
                ? Shader::Output::ClipDist3
                : (ctl.IsCullDistEnabled(3) ? Shader::Output::CullDist3 : Shader::Output::None);
    }
    if (ctl.vs_out_ccdist1_enable) {
        auto& ccdist1 = outputs[out_count++];
        ccdist1[0] =
            ctl.IsClipDistEnabled(4)
                ? Shader::Output::ClipDist4
                : (ctl.IsCullDistEnabled(4) ? Shader::Output::CullDist4 : Shader::Output::None);
        ccdist1[1] =
            ctl.IsClipDistEnabled(5)
                ? Shader::Output::ClipDist5
                : (ctl.IsCullDistEnabled(5) ? Shader::Output::CullDist5 : Shader::Output::None);
        ccdist1[2] =
            ctl.IsClipDistEnabled(6)
                ? Shader::Output::ClipDist6
                : (ctl.IsCullDistEnabled(6) ? Shader::Output::CullDist6 : Shader::Output::None);
        ccdist1[3] =
            ctl.IsClipDistEnabled(7)
                ? Shader::Output::ClipDist7
                : (ctl.IsCullDistEnabled(7) ? Shader::Output::CullDist7 : Shader::Output::None);
    }
    return out_count <= 3u;
}

bool TryExtractCs(std::span<const u8> data, const OrbProgram& program, ExtractedShader& out) {
    for (const size_t base : {32u, 36u, 40u, 44u, 48u, 52u, 56u, 60u}) {
        const size_t size = base + size_t(program.usages) * 4;
        if (size > program.start) continue;
        const u8* header = data.data() + program.start - size;
        if (ReadLe32(header) != size + 8 || ReadLe32(header + 4) != 0xffffffffu) continue;
        for (const size_t settings : {8u, 12u}) {
            if (settings + 20 > base || (settings == 12 && ReadLe32(header + 8) != 0)) continue;
            if (((ReadLe32(header + settings + 4) >> 1) & 31u) > 16u) continue;
            const u32 x = ReadLe32(header + settings + 8) & 0xffffu;
            const u32 y = ReadLe32(header + settings + 12) & 0xffffu;
            const u32 z = ReadLe32(header + settings + 16) & 0xffffu;
            if (!x || !y || !z || u64(x) * y * z > 1024) continue;
            out = {};
            out.kind = StageKind::Compute;
            out.hash = program.hash;
            out.is_srt = program.is_srt;
            std::memcpy(out.regs.data() + 3, header + settings, 8);
            std::memcpy(out.regs.data() + 5, header + settings + 8, 12);
            out.usages.resize(program.usages);
            std::memcpy(out.usages.data(), header + base, size_t(program.usages) * 4);
            out.code.resize(program.bytes / 4);
            std::memcpy(out.code.data(), data.data() + program.start, program.bytes);
            return true;
        }
    }
    return false;
}

bool TryExtractPs(std::span<const u8> data, const OrbProgram& p, ExtractedShader& out) {
    for (u32 count = 0; count <= 32; ++count) {
        const size_t size = 52 + size_t(p.usages) * 4 + ((size_t(count) * 2 + 3) & ~size_t(3));
        if (size > p.start) {
            continue;
        }
        const u8* h = data.data() + p.start - size;
        if (ReadLe32(h) != size + 8 || ReadLe32(h + 4) != 0xffffffffu ||
            ReadLe32(h + 48) != count || (ReadLe32(h + 32) & 63u) != count ||
            ((ReadLe32(h + 12) >> 1) & 31u) > 16u) {
            continue;
        }
        out = {};
        out.kind = StageKind::Fragment;
        out.hash = p.hash;
        out.is_srt = p.is_srt;
        std::memcpy(out.regs.data(), h, 52);
        out.usages.resize(p.usages);
        if (p.usages > 0) {
            std::memcpy(out.usages.data(), h + 52, size_t(p.usages) * 4);
        }
        out.semantics.resize(count);
        const u8* sem_table = h + 52 + size_t(p.usages) * 4;
        for (u32 i = 0; i < count; ++i) {
            out.semantics[i] = ReadLe16(sem_table + i * 2);
        }
        out.code.resize(p.bytes / 4);
        std::memcpy(out.code.data(), data.data() + p.start, p.bytes);
        return true;
    }
    return false;
}

bool TryExtractVs(std::span<const u8> data, const OrbProgram& p, ExtractedShader& out) {
    for (size_t size = 32; size <= std::min<size_t>(p.start, 1280); size += 4) {
        const u8* h = data.data() + p.start - size;
        if (ReadLe32(h) != size + 8 || ReadLe32(h + 4) != 0xffffffffu) {
            continue;
        }
        const u32 counts = ReadLe32(h + 28);
        const u32 inputs = counts & 255u;
        const u32 outputs = (counts >> 8) & 255u;
        if ((counts >> 16) != 0 || inputs > kMaxPrecompileVertexAttribs || outputs > 32) {
            continue;
        }
        const size_t expected = 32 + size_t(p.usages) * 4 + size_t(inputs) * 4 +
                                ((size_t(outputs) * 2 + 3) & ~size_t(3));
        if (size != expected || ((ReadLe32(h + 12) >> 1) & 31u) > 16u) {
            continue;
        }
        std::array<Shader::OutputMap, 3> dummy_outputs{};
        u32 mapped_out = 0;
        if (!MapVsOutputsSafe(dummy_outputs,
                              std::bit_cast<AmdGpu::VsOutputControl>(ReadLe32(h + 24)),
                              mapped_out)) {
            return false;
        }

        const u8* exp_table = h + 32 + size_t(p.usages) * 4 + size_t(inputs) * 4;
        std::vector<std::pair<u8, u8>> exports;
        exports.reserve(outputs);
        for (u32 i = 0; i < outputs; ++i) {
            const u16 val = ReadLe16(exp_table + i * 2);
            const u8 sem = static_cast<u8>(val & 0xFFu);
            const u8 idx = static_cast<u8>((val >> 8) & 0xFFu);
            if (idx >= 32u) {
                return false;
            }
            exports.emplace_back(sem, idx);
        }

        out = {};
        out.kind = StageKind::Vertex;
        out.hash = p.hash;
        out.is_srt = p.is_srt;
        std::memcpy(out.regs.data(), h, 32);
        out.usages.resize(p.usages);
        if (p.usages > 0) {
            std::memcpy(out.usages.data(), h + 32, size_t(p.usages) * 4);
        }
        out.semantics.resize(inputs);
        if (inputs > 0) {
            std::memcpy(out.semantics.data(), h + 32 + size_t(p.usages) * 4, size_t(inputs) * 4);
        }
        out.vs_exports = std::move(exports);
        out.code.resize(p.bytes / 4);
        std::memcpy(out.code.data(), data.data() + p.start, p.bytes);
        return true;
    }
    return false;
}

class DescriptorArena {
public:
    static constexpr size_t kArenaBytes = 2 * 1024 * 1024;
    static constexpr size_t kExtendedOff = 0x00000;
    static constexpr size_t kSrtLeafOff = 0x20000;
    static constexpr size_t kResourceOff = 0x40000;
    static constexpr size_t kConstBufOff = 0x80000;
    static constexpr size_t kSamplerOff = 0xC0000;
    static constexpr size_t kVertexOff = 0xE0000;
    static constexpr size_t kFetchCodeOff = 0xF0000;

    DescriptorArena() {
        base_ = static_cast<u8*>(AllocateDescriptorMemory(kArenaBytes));
        if (!base_) {
            return;
        }
        std::memset(base_, 0, kArenaBytes);

        AmdGpu::Image img = AmdGpu::Image::Null(false);
        img.base_address = 1;
        img.width = 63;
        img.height = 63;
        img.pitch = 63;

        AmdGpu::Buffer cbuf = AmdGpu::Buffer::Placeholder(65536);
        cbuf.base_address = PtrAt(0x100000);
        cbuf.data_format = u32(AmdGpu::DataFormat::Format32_32_32_32);
        cbuf.num_format = u32(AmdGpu::NumberFormat::Float);
        cbuf.stride = 16;
        cbuf.dst_sel_x = u32(AmdGpu::CompSwizzle::Red);
        cbuf.dst_sel_y = u32(AmdGpu::CompSwizzle::Green);
        cbuf.dst_sel_z = u32(AmdGpu::CompSwizzle::Blue);
        cbuf.dst_sel_w = u32(AmdGpu::CompSwizzle::Alpha);

        AmdGpu::Sampler smp{};

        for (size_t off = 0; off + sizeof(img) <= 0x40000; off += sizeof(img)) {
            std::memcpy(base_ + kResourceOff + off, &img, sizeof(img));
        }
        for (size_t off = 0; off + sizeof(cbuf) <= 0x40000; off += sizeof(cbuf)) {
            std::memcpy(base_ + kConstBufOff + off, &cbuf, sizeof(cbuf));
        }
        for (size_t off = 0; off + sizeof(cbuf) <= 0x20000; off += sizeof(cbuf)) {
            std::memcpy(base_ + kSrtLeafOff + off, &cbuf, sizeof(cbuf));
        }
        for (size_t off = 0; off + sizeof(smp) <= 0x20000; off += sizeof(smp)) {
            std::memcpy(base_ + kSamplerOff + off, &smp, sizeof(smp));
        }
        for (size_t off = 0; off + sizeof(cbuf) <= 0x10000; off += sizeof(cbuf)) {
            std::memcpy(base_ + kVertexOff + off, &cbuf, sizeof(cbuf));
        }
    }

    ~DescriptorArena() {
        if (base_) {
            FreeDescriptorMemory(base_, kArenaBytes);
        }
    }

    VAddr CodeBase() const noexcept {
        return PtrAt(0x140000);
    }

    bool Valid() const noexcept {
        return base_ != nullptr && reinterpret_cast<uintptr_t>(base_) >= 0x10000u;
    }

    bool Populate(const ExtractedShader& sh, std::array<u32, 16>& user_regs) {
        if (!Valid()) {
            return false;
        }
        if (sh.code.size() * sizeof(u32) > 0x40000)
            return false;
        std::memcpy(base_ + 0x140000, sh.code.data(), sh.code.size() * sizeof(u32));
        user_regs.fill(0);

        const u64 leaf_ptr = PtrAt(kSrtLeafOff);
        for (size_t off = 0; off + sizeof(u64) <= 0x20000; off += sizeof(u64)) {
            std::memcpy(base_ + kExtendedOff + off, &leaf_ptr, sizeof(u64));
        }
        for (u32 r = 0; r + 1 < 16; r += 2) {
            std::memcpy(&user_regs[r], &leaf_ptr, sizeof(u64));
        }

        AmdGpu::Image img = AmdGpu::Image::Null(false);
        img.base_address = 1;
        img.width = 63;
        img.height = 63;
        img.pitch = 63;

        AmdGpu::Buffer buf = AmdGpu::Buffer::Placeholder(65536);
        buf.base_address = PtrAt(0x100000);
        buf.data_format = u32(AmdGpu::DataFormat::Format32_32_32_32);
        buf.num_format = u32(AmdGpu::NumberFormat::Float);
        buf.stride = 16;
        buf.dst_sel_x = u32(AmdGpu::CompSwizzle::Red);
        buf.dst_sel_y = u32(AmdGpu::CompSwizzle::Green);
        buf.dst_sel_z = u32(AmdGpu::CompSwizzle::Blue);
        buf.dst_sel_w = u32(AmdGpu::CompSwizzle::Alpha);

        AmdGpu::Sampler smp{};

        auto put = [&](u32 reg, const void* val, size_t nbytes) -> bool {
            if (reg < 16) {
                if (size_t(reg) * 4 + nbytes > sizeof(user_regs)) {
                    return false;
                }
                std::memcpy(reinterpret_cast<u8*>(user_regs.data()) + size_t(reg) * 4, val, nbytes);
                return true;
            }
            const size_t ext_byte = size_t(reg - 16) * 4;
            if (ext_byte + nbytes > 0x20000) {
                return false;
            }
            std::memcpy(base_ + kExtendedOff + ext_byte, val, nbytes);
            return true;
        };

        std::optional<u32> vtx_table_reg;
        std::optional<u32> fetch_subptr_reg;

        for (const u32 usage : sh.usages) {
            const u32 type = usage & 0xFFu;
            const u32 reg = (usage >> 16) & 0xFFu;
            const u32 flags = (usage >> 24) & 0xFFu;
            switch (type) {
            case 0x00:
            case 0x04:
                if (flags & 2u) {
                    if (!put(reg, &img, sizeof(img)))
                        return false;
                } else {
                    if (!put(reg, &buf, sizeof(buf)))
                        return false;
                }
                break;
            case 0x01:
                if (!put(reg, &smp, sizeof(smp)))
                    return false;
                break;
            case 0x02:
            case 0x03:
                if (!put(reg, &buf, sizeof(buf)))
                    return false;
                break;
            case 0x05:
            case 0x06:
            case 0x07:
            case 0x08:
            case 0x09:
                break;
            case 0x0D:
            case 0x1D:
                if (!put(reg, &leaf_ptr, sizeof(u64)))
                    return false;
                break;
            case 0x12:
                fetch_subptr_reg = reg;
                break;
            case 0x13:
            case 0x14:
            case 0x19: {
                const u64 p = PtrAt(kResourceOff);
                if (!put(reg, &p, sizeof(u64)))
                    return false;
                break;
            }
            case 0x15: {
                const u64 p = PtrAt(kSamplerOff);
                if (!put(reg, &p, sizeof(u64)))
                    return false;
                break;
            }
            case 0x16:
            case 0x18: {
                const u64 p = PtrAt(kConstBufOff);
                if (!put(reg, &p, sizeof(u64)))
                    return false;
                break;
            }
            case 0x17: {
                vtx_table_reg = reg;
                const u64 p = PtrAt(kVertexOff);
                if (!put(reg, &p, sizeof(u64)))
                    return false;
                break;
            }
            case 0x1B: {
                const u64 p = PtrAt(kExtendedOff);
                if (!put(reg, &p, sizeof(u64)))
                    return false;
                break;
            }
            default:
                return false;
            }
        }

        if (sh.kind == StageKind::Vertex) {
            if (fetch_subptr_reg) {
                const u32 vtable = vtx_table_reg.value_or(2u);
                if (!vtx_table_reg && vtable < 15) {
                    const u64 p = PtrAt(kVertexOff);
                    put(vtable, &p, sizeof(u64));
                }
                if (vtable >= 128 || (vtable & 1u) != 0) {
                    return false;
                }
                if (!BuildFetchShader(sh.semantics, vtable)) {
                    return false;
                }
                const u64 fetch_ptr = PtrAt(kFetchCodeOff + fetch_gen_offset_);
                if (!put(*fetch_subptr_reg, &fetch_ptr, sizeof(u64))) {
                    return false;
                }
            } else if (!sh.semantics.empty()) {
                return false;
            }
        }
        return true;
    }

private:
    u64 PtrAt(size_t offset) const noexcept {
        return static_cast<u64>(reinterpret_cast<uintptr_t>(base_ + offset));
    }

    bool BuildFetchShader(std::span<const u32> semantics, u32 vtable_sgpr) {
        if (semantics.size() > kMaxPrecompileVertexAttribs) {
            return false;
        }
        fetch_cursor_ = (fetch_cursor_ + 512) % 0xF000u;
        fetch_gen_offset_ = fetch_cursor_;
        u32* out = reinterpret_cast<u32*>(base_ + kFetchCodeOff + fetch_gen_offset_);
        size_t w = 0;
        for (size_t i = 0; i < semantics.size(); ++i) {
            const u32 packed = semantics[i];
            const u32 vgpr = (packed >> 8) & 0xFFu;
            const u32 comps = std::clamp<u32>((packed >> 16) & 0xFFu, 1u, 4u);
            if (vgpr > 252) {
                return false;
            }

            out[w++] = 0xC0820100u | (((vtable_sgpr / 2u) & 0x3Fu) << 9) | (u32(i * 4) & 0xFFu);

            out[w++] = 0xE0002000u | (((comps - 1u) & 3u) << 18);
            out[w++] = 0x80010000u | ((vgpr & 0xFFu) << 8);
        }

        out[w++] = 0xBE802000u;
        return true;
    }

    u8* base_ = nullptr;
    size_t fetch_cursor_ = 0;
    size_t fetch_gen_offset_ = 0;
};

std::vector<u32> EmitPassthroughVertexSpirv(u32 num_outputs) {
    Sirit::Module m{kSpirvVersion1_5};
    m.AddCapability(spv::Capability::Shader);
    m.SetMemoryModel(spv::AddressingModel::Logical, spv::MemoryModel::GLSL450);

    const Sirit::Id void_ty = m.TypeVoid();
    const Sirit::Id f32_ty = m.TypeFloat(32);
    const Sirit::Id vec4_ty = m.TypeVector(f32_ty, 4);
    const Sirit::Id ptr_out_vec4 = m.TypePointer(spv::StorageClass::Output, vec4_ty);
    const Sirit::Id fn_ty = m.TypeFunction(void_ty);

    const Sirit::Id f0 = m.Constant(f32_ty, 0.0f);
    const Sirit::Id f1 = m.Constant(f32_ty, 1.0f);
    const Sirit::Id zero4 = m.ConstantComposite(vec4_ty, f0, f0, f0, f0);
    const Sirit::Id pos4 = m.ConstantComposite(vec4_ty, f0, f0, f0, f1);

    std::vector<Sirit::Id> iface;
    const Sirit::Id gl_pos = m.AddGlobalVariable(ptr_out_vec4, spv::StorageClass::Output);
    m.Decorate(gl_pos, spv::Decoration::BuiltIn, spv::BuiltIn::Position);
    iface.push_back(gl_pos);

    std::vector<Sirit::Id> outs;
    outs.reserve(num_outputs);
    for (u32 i = 0; i < num_outputs; ++i) {
        const Sirit::Id v = m.AddGlobalVariable(ptr_out_vec4, spv::StorageClass::Output);
        m.Decorate(v, spv::Decoration::Location, i);
        iface.push_back(v);
        outs.push_back(v);
    }

    const Sirit::Id main_fn = m.OpFunction(void_ty, spv::FunctionControlMask::MaskNone, fn_ty);
    m.AddEntryPoint(spv::ExecutionModel::Vertex, main_fn, "main",
                    std::span<const Sirit::Id>{iface});
    m.AddLabel(m.OpLabel());
    m.OpStore(gl_pos, pos4);
    for (const Sirit::Id v : outs) {
        m.OpStore(v, zero4);
    }
    m.OpReturn();
    m.OpFunctionEnd();
    return m.Assemble();
}

std::vector<u32> EmitPassthroughFragmentSpirv() {
    Sirit::Module m{kSpirvVersion1_5};
    m.AddCapability(spv::Capability::Shader);
    m.SetMemoryModel(spv::AddressingModel::Logical, spv::MemoryModel::GLSL450);

    const Sirit::Id void_ty = m.TypeVoid();
    const Sirit::Id f32_ty = m.TypeFloat(32);
    const Sirit::Id vec4_ty = m.TypeVector(f32_ty, 4);
    const Sirit::Id ptr_out_vec4 = m.TypePointer(spv::StorageClass::Output, vec4_ty);
    const Sirit::Id fn_ty = m.TypeFunction(void_ty);

    const Sirit::Id f0 = m.Constant(f32_ty, 0.0f);
    const Sirit::Id zero4 = m.ConstantComposite(vec4_ty, f0, f0, f0, f0);

    const Sirit::Id out0 = m.AddGlobalVariable(ptr_out_vec4, spv::StorageClass::Output);
    m.Decorate(out0, spv::Decoration::Location, 0u);
    const std::array<Sirit::Id, 1> iface{out0};

    const Sirit::Id main_fn = m.OpFunction(void_ty, spv::FunctionControlMask::MaskNone, fn_ty);
    m.AddEntryPoint(spv::ExecutionModel::Fragment, main_fn, "main",
                    std::span<const Sirit::Id>{iface});
    m.AddExecutionMode(main_fn, spv::ExecutionMode::OriginUpperLeft);
    m.AddLabel(m.OpLabel());
    m.OpStore(out0, zero4);
    m.OpReturn();
    m.OpFunctionEnd();
    return m.Assemble();
}

struct PrecompileContext {
    Vulkan::PipelineCache& cache;
    DescriptorArena& arena;
    std::array<std::vector<u32>, 33> passthrough_vs_spv;
    std::vector<u32> passthrough_fs_spv;
    Shader::Info dummy_vs_info{};
    Shader::Info dummy_fs_info{};
    Shader::RuntimeInfo dummy_vs_rt{};
    Shader::RuntimeInfo dummy_fs_rt{};

    PrecompileContext(Vulkan::PipelineCache& cache_, DescriptorArena& arena_)
        : cache{cache_}, arena{arena_} {
        dummy_vs_info.hw_stage = Shader::HwStage::Vertex;
        dummy_vs_info.sw_stage = Shader::SwStage::Vertex;
        dummy_fs_info.hw_stage = Shader::HwStage::Fragment;
        dummy_fs_info.sw_stage = Shader::SwStage::Fragment;
        dummy_fs_info.mrt_mask = 1u;
        dummy_vs_rt.Initialize(Shader::HwStage::Vertex, Shader::SwStage::Vertex);
        dummy_fs_rt.Initialize(Shader::HwStage::Fragment, Shader::SwStage::Fragment);
        passthrough_fs_spv = EmitPassthroughFragmentSpirv();
    }

    const std::vector<u32>& GetPassthroughVs(u32 num_outputs) {
        const u32 clamped = std::min<u32>(num_outputs, 32u);
        if (passthrough_vs_spv[clamped].empty()) {
            passthrough_vs_spv[clamped] = EmitPassthroughVertexSpirv(clamped);
        }
        return passthrough_vs_spv[clamped];
    }
};

void BuildFragmentRuntimeAndKey(const ExtractedShader& sh,
                                const std::vector<std::pair<u8, u8>>* vs_exports,
                                Shader::RuntimeInfo& rt, Vulkan::GraphicsPipelineKey& key,
                                u32& max_param_loc) {
    rt = {};
    rt.Initialize(Shader::HwStage::Fragment, Shader::SwStage::Fragment);
    AmdGpu::ShaderProgram pgm{};
    std::memcpy(&pgm.settings, sh.regs.data() + 2, 8);
    rt.props.fp_denorm_mode32 = pgm.settings.fp_denorm_mode32;
    rt.props.fp_denorm_mode16_64 = pgm.settings.fp_denorm_mode64;
    rt.props.fp_round_mode32 = pgm.settings.fp_round_mode32;
    rt.props.fp_round_mode16_64 = pgm.settings.fp_round_mode64;
    rt.props.num_user_data = pgm.settings.num_user_regs;
    rt.props.num_input_vgprs = pgm.settings.vgpr_comp_cnt;
    rt.props.num_allocated_vgprs = pgm.NumVgprs();
    rt.props.num_allocated_sgprs = pgm.NumSgprs();
    rt.props.dx10_clamp = pgm.settings.dx10_clamp;
    auto& fs = rt.hw.fs;
    fs.en_flags = std::bit_cast<AmdGpu::PsInput>(sh.regs[6]);
    fs.addr_flags = std::bit_cast<AmdGpu::PsInput>(sh.regs[7]);
    fs.front_face_all_bits =
        std::bit_cast<AmdGpu::BarycentricControl>(sh.regs[9]).front_face_all_bits;
    fs.z_export_format = static_cast<AmdGpu::ShaderExportFormat>(sh.regs[4] & 0xFu);
    const auto depth = std::bit_cast<AmdGpu::DepthShaderControl>(sh.regs[10]);
    fs.mrtz_mask =
        depth.z_export_enable |
        ((depth.stencil_op_val_export_enable || depth.stencil_test_val_export_enable) << 1) |
        (depth.mask_export_enable << 2) | (depth.coverage_to_mask_enable << 3);

    const u32 num_in = std::min<u32>(static_cast<u32>(sh.semantics.size()), 32u);
    fs.num_inputs = num_in;
    max_param_loc = 0;
    for (u32 i = 0; i < num_in; ++i) {
        const u32 raw_sem = sh.semantics[i];
        const bool flat = (raw_sem & 0x0400u) != 0;
        if (vs_exports) {
            const u8 sem_id = static_cast<u8>(raw_sem & 0xFFu);
            auto it = std::ranges::find_if(*vs_exports,
                                           [sem_id](const auto& kv) { return kv.first == sem_id; });
            if (it != vs_exports->end() && it->second < 32u) {
                fs.inputs[i] = {it->second, false, flat, 0};
                max_param_loc = std::max<u32>(max_param_loc, u32(it->second) + 1u);
            } else {
                fs.inputs[i] = {0, true, flat, static_cast<u8>((raw_sem >> 8) & 3u)};
            }
        } else {
            fs.inputs[i] = {static_cast<u8>(i), false, flat, 0};
            max_param_loc = std::max<u32>(max_param_loc, i + 1u);
        }
    }

    key = {};
    key.prim_type = AmdGpu::PrimitiveType::TriangleList;
    key.polygon_mode = AmdGpu::PolygonMode::Fill;
    key.z_format = AmdGpu::DepthBuffer::ZFormat::Z32Float;
    if (fs.mrtz_mask & 2u) {
        key.stencil_format = AmdGpu::DepthBuffer::StencilFormat::Stencil8;
    }
    key.depth_clip_enable = 1;
    key.num_samples = 1;
    key.depth_samples = 1;
    key.color_samples.fill(1);

    for (u32 cb = 0; cb < Shader::MaxColorBuffers; ++cb) {
        const u32 exp_fmt = (sh.regs[5] >> (cb * 4)) & 0xFu;
        if (exp_fmt == 0) {
            continue;
        }
        const auto sef = static_cast<AmdGpu::ShaderExportFormat>(exp_fmt);
        auto& col = fs.color_buffers[cb];
        if (sef == AmdGpu::ShaderExportFormat::ABGR_FP16 ||
            sef == AmdGpu::ShaderExportFormat::ABGR_32) {
            col.data_format = AmdGpu::DataFormat::Format16_16_16_16;
            col.num_format = AmdGpu::NumberFormat::Float;
        } else {
            col.data_format = AmdGpu::DataFormat::Format8_8_8_8;
            col.num_format = AmdGpu::NumberFormat::Unorm;
        }
        col.export_format = sef;
        col.swizzle = {.r = AmdGpu::CompSwizzle::Red,
                       .g = AmdGpu::CompSwizzle::Green,
                       .b = AmdGpu::CompSwizzle::Blue,
                       .a = AmdGpu::CompSwizzle::Alpha};
        key.color_buffers[cb] = col;
        key.write_masks[cb] = vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG |
                              vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA;
    }
}

bool PopulateVertexInputState(const std::optional<Shader::Gcn::FetchShaderData>& fetch,
                              Vulkan::GraphicsPipelineKey& key,
                              Vulkan::GraphicsPipeline::SerializationSupport& sdata) {
    if (!fetch) {
        return true;
    }
    if (fetch->attributes.size() > kMaxPrecompileVertexAttribs) {
        return false;
    }
    u32 slot = 0;
    for (const auto& attrib : fetch->attributes) {
        if (slot >= Vulkan::MaxVertexBufferCount) {
            return false;
        }
        key.vertex_buffer_formats[slot] = vk::Format::eR32G32B32A32Sfloat;
        sdata.vertex_bindings.push_back(vk::VertexInputBindingDescription{
            .binding = slot,
            .stride = 16,
            .inputRate = vk::VertexInputRate::eVertex,
        });
        sdata.vertex_attributes.push_back(vk::VertexInputAttributeDescription{
            .location = slot,
            .binding = slot,
            .format = vk::Format::eR32G32B32A32Sfloat,
            .offset = 0,
        });
        ++slot;
    }
    return true;
}

bool ValidateSpirvModule(std::span<const u32> spv, u64 shader_hash) noexcept {
    if (spv.size() < 6 || spv[0] != 0x07230203u) {
        return false;
    }
    const u32 bound = spv[3];
    if (bound < 2u || bound > (1u << 22)) {
        return false;
    }
    auto ok_id = [bound](u32 id) noexcept { return id != 0u && id < bound; };

    size_t pos = 5;
    while (pos < spv.size()) {
        const u32 hdr = spv[pos];
        const u32 wc = hdr >> 16;
        const u32 op = hdr & 0xFFFFu;
        if (wc == 0 || pos + wc > spv.size()) {
            return false;
        }
        auto fail_at = [&](u32 slot) { return false; };
        auto check_range = [&](u32 first_slot, u32 end_slot) {
            for (u32 s = first_slot; s < end_slot && s < wc; ++s) {
                if (!ok_id(spv[pos + s])) {
                    return fail_at(s);
                }
            }
            return true;
        };

        switch (op) {
        case 12:
            if (wc < 4 || !check_range(1, 4) || !check_range(5, wc))
                return false;
            break;
        case 15:
            if (wc < 4 || !ok_id(spv[pos + 2]))
                return fail_at(2);
            break;
        case 16:
        case 71:
        case 72:
        case 247:
        case 248:
        case 249:
            if (wc < 2 || !ok_id(spv[pos + 1]))
                return fail_at(1);
            break;
        case 59:
            if (wc < 4 || !check_range(1, 3) || (wc >= 5 && !ok_id(spv[pos + 4])))
                return false;
            break;
        case 61:
        case 81:
        case 250:
            if (wc < 4 || !check_range(1, 4))
                return false;
            break;
        case 62:
        case 246:
        case 251:
            if (wc < 3 || !check_range(1, 3))
                return false;
            break;
        case 79:
        case 82:
            if (wc < 5 || !check_range(1, 5))
                return false;
            break;
        case 57:
        case 65:
        case 66:
        case 80:
        case 83:
        case 84:
        case 245:
            if (wc < 3 || !check_range(1, wc))
                return false;
            break;
        case 99:
            if (wc < 4 || !check_range(1, 4))
                return false;
            break;
        default:
            if ((op >= 87 && op <= 98) || op == 100 || op == 101) {

                if (wc < 5 || !check_range(1, 5))
                    return false;
            } else if ((op >= 102 && op <= 107) || (op >= 109 && op <= 124) ||
                       (op >= 126 && op <= 191) || (op >= 201 && op <= 205)) {

                if (wc < 3 || !check_range(1, wc))
                    return false;
            }
            break;
        }
        pos += wc;
    }
    return true;
}

SHAD_NO_INLINE bool CompileSingleExtractedInner(PrecompileContext& ctx, const ExtractedShader& sh) {

    Common::ScopedAssertExceptions assertion_scope;
    const auto watermark = Shader::GetSrtWalkerWatermark();
    SCOPE_EXIT {
        Shader::RewindSrtWalker(watermark);
    };
    try {
        std::array<u32, 16> user_regs{};
        if (!ctx.arena.Populate(sh, user_regs)) {
            return false;
        }

        const Shader::Profile& profile = ctx.cache.GetProfile();
        Shader::Pools pools;

        if (sh.kind == StageKind::Compute) {
            Shader::RuntimeInfo rt{};
            rt.Initialize(Shader::HwStage::Compute, Shader::SwStage::Compute);
            AmdGpu::ComputeProgram cs_pgm{};
            std::memcpy(&cs_pgm.settings, sh.regs.data() + 3, 8);
            std::memcpy(&cs_pgm.num_thread_x, sh.regs.data() + 5, 12);
            rt.props.fp_denorm_mode32 = cs_pgm.settings.fp_denorm_mode32;
            rt.props.fp_denorm_mode16_64 = cs_pgm.settings.fp_denorm_mode64;
            rt.props.fp_round_mode32 = cs_pgm.settings.fp_round_mode32;
            rt.props.fp_round_mode16_64 = cs_pgm.settings.fp_round_mode64;
            rt.props.num_user_data = std::min<u32>(cs_pgm.settings.num_user_regs, 16u);
            rt.props.num_allocated_vgprs = cs_pgm.settings.num_vgprs * 4;
            rt.props.num_allocated_sgprs = cs_pgm.NumSgprs();
            rt.props.dx10_clamp = cs_pgm.settings.dx10_clamp;
            rt.hw.cs.workgroup_size = {cs_pgm.num_thread_x.full, cs_pgm.num_thread_y.full,
                                       cs_pgm.num_thread_z.full};
            rt.hw.cs.tgid_enable = {cs_pgm.IsTgidEnabled(0), cs_pgm.IsTgidEnabled(1),
                                    cs_pgm.IsTgidEnabled(2)};
            rt.hw.cs.shared_memory_size = cs_pgm.SharedMemSize();

            Shader::ShaderParams params{
                .user_data = user_regs,
                .code = sh.code,
                .hash = sh.hash,
                .guest_base = ctx.arena.CodeBase(),
            };
            Shader::Info info{Shader::HwStage::Compute, Shader::SwStage::Compute, params};
            Shader::Backend::Bindings binding{};
            auto ir = Shader::TranslateProgram(sh.code, pools, info, rt, profile);
            auto spv = Shader::Backend::SPIRV::EmitSPIRV(profile, rt, ir, binding);
            pools.ReleaseContents();
            if (!ValidateSpirvModule(spv, sh.hash)) {
                return false;
            }
            return ctx.cache.PrecompileArchiveCompute(info, spv);
        }

        if (sh.kind == StageKind::Fragment) {
            Shader::RuntimeInfo rt{};
            Vulkan::GraphicsPipelineKey key{};
            u32 max_param_loc = 0;
            BuildFragmentRuntimeAndKey(sh, nullptr, rt, key, max_param_loc);

            Shader::ShaderParams params{
                .user_data = user_regs,
                .code = sh.code,
                .hash = sh.hash,
                .guest_base = ctx.arena.CodeBase(),
            };
            Shader::Info info{Shader::HwStage::Fragment, Shader::SwStage::Fragment, params};
            Shader::Backend::Bindings binding{};
            auto ir = Shader::TranslateProgram(sh.code, pools, info, rt, profile);
            if (!info.fmasks.empty()) {
                return false;
            }
            auto spv = Shader::Backend::SPIRV::EmitSPIRV(profile, rt, ir, binding);
            pools.ReleaseContents();
            if (!ValidateSpirvModule(spv, sh.hash)) {
                return false;
            }

            key.mrt_mask = info.mrt_mask;
            key.num_color_attachments = std::bit_width(info.mrt_mask);
            for (u32 cb = 0; cb < Shader::MaxColorBuffers; ++cb) {
                if ((key.mrt_mask & (1u << cb)) == 0) {
                    std::memset(&key.color_buffers[cb], 0, sizeof(Shader::PsColorBuffer));
                    key.write_masks[cb] = {};
                }
            }

            Vulkan::GraphicsPipeline::SerializationSupport sdata{};
            sdata.multisampling.rasterizationSamples = vk::SampleCountFlagBits::e1;
            const auto& vs_spv = ctx.GetPassthroughVs(max_param_loc);
            return ctx.cache.PrecompileArchiveGraphics(&ctx.dummy_vs_info, ctx.dummy_vs_rt, vs_spv,
                                                       std::nullopt, &info, rt, spv, key, sdata);
        }

        if (sh.kind == StageKind::Vertex) {
            Shader::RuntimeInfo rt{};
            rt.Initialize(Shader::HwStage::Vertex, Shader::SwStage::Vertex);
            AmdGpu::ShaderProgram pgm{};
            std::memcpy(&pgm.settings, sh.regs.data() + 2, 8);
            rt.props.fp_denorm_mode32 = pgm.settings.fp_denorm_mode32;
            rt.props.fp_denorm_mode16_64 = pgm.settings.fp_denorm_mode64;
            rt.props.fp_round_mode32 = pgm.settings.fp_round_mode32;
            rt.props.fp_round_mode16_64 = pgm.settings.fp_round_mode64;
            rt.props.num_user_data = pgm.settings.num_user_regs;
            rt.props.num_input_vgprs = pgm.settings.vgpr_comp_cnt;
            rt.props.num_allocated_vgprs = pgm.NumVgprs();
            rt.props.num_allocated_sgprs = pgm.NumSgprs();
            rt.props.dx10_clamp = pgm.settings.dx10_clamp;
            u32 mapped_out = 0;
            if (!MapVsOutputsSafe(rt.hw.vs.outputs,
                                  std::bit_cast<AmdGpu::VsOutputControl>(sh.regs[6]), mapped_out)) {
                return false;
            }
            rt.hw.vs.num_outputs = mapped_out;
            rt.sw.vs.step_rate_0 = 1;
            rt.sw.vs.step_rate_1 = 1;

            Shader::ShaderParams params{
                .user_data = user_regs,
                .code = sh.code,
                .hash = sh.hash,
                .guest_base = ctx.arena.CodeBase(),
            };
            Shader::Info info{Shader::HwStage::Vertex, Shader::SwStage::Vertex, params};
            Shader::Backend::Bindings binding{};
            auto ir = Shader::TranslateProgram(sh.code, pools, info, rt, profile);
            if (!info.fmasks.empty()) {
                return false;
            }
            Shader::Gcn::FetchShaderData fetch_data;
            std::optional<Shader::Gcn::FetchShaderData> fetch;
            if (Shader::Gcn::ParseFetchShader(info, fetch_data))
                fetch = std::move(fetch_data);
            auto spv = Shader::Backend::SPIRV::EmitSPIRV(profile, rt, ir, binding);
            pools.ReleaseContents();
            if (!ValidateSpirvModule(spv, sh.hash)) {
                return false;
            }

            Vulkan::GraphicsPipelineKey key{};
            key.prim_type = AmdGpu::PrimitiveType::TriangleList;
            key.polygon_mode = AmdGpu::PolygonMode::Fill;
            key.z_format = AmdGpu::DepthBuffer::ZFormat::Z32Float;
            key.depth_clip_enable = 1;
            key.num_samples = 1;
            key.depth_samples = 1;
            key.color_samples.fill(1);
            key.mrt_mask = 0u;
            key.num_color_attachments = 0u;

            Vulkan::GraphicsPipeline::SerializationSupport sdata{};
            sdata.multisampling.rasterizationSamples = vk::SampleCountFlagBits::e1;
            if (!PopulateVertexInputState(fetch, key, sdata)) {
                return false;
            }

            return ctx.cache.PrecompileArchiveGraphics(&info, rt, spv, fetch, nullptr,
                                                       ctx.dummy_fs_rt, {}, key, sdata);
        }
    } catch (const std::bad_alloc&) {
        throw;
    } catch (const std::exception& error) {
        const std::string_view message{error.what()};
        if (message.find("ErrorDeviceLost") != std::string_view::npos ||
            message.find("OutOf") != std::string_view::npos)
            throw;
        LOG_DEBUG(Render_Vulkan, "Archive shader skipped: {}", message);
        return false;
    }
    return false;
}

SHAD_NO_INLINE bool CompileLinkedPairInner(PrecompileContext& ctx, const ExtractedShader& vs_sh,
                                           const ExtractedShader& ps_sh) {

    Common::ScopedAssertExceptions assertion_scope;
    const auto watermark = Shader::GetSrtWalkerWatermark();
    SCOPE_EXIT {
        Shader::RewindSrtWalker(watermark);
    };
    try {
        const Shader::Profile& profile = ctx.cache.GetProfile();
        Shader::Backend::Bindings binding{};

        std::array<u32, 16> ps_user_regs{};
        if (!ctx.arena.Populate(ps_sh, ps_user_regs)) {
            return false;
        }
        Shader::RuntimeInfo ps_rt{};
        Vulkan::GraphicsPipelineKey key{};
        u32 max_param_loc = 0;
        BuildFragmentRuntimeAndKey(ps_sh, &vs_sh.vs_exports, ps_rt, key, max_param_loc);

        Shader::ShaderParams ps_params{
            .user_data = ps_user_regs,
            .code = ps_sh.code,
            .hash = ps_sh.hash,
            .guest_base = ctx.arena.CodeBase(),
        };
        Shader::Info ps_info{Shader::HwStage::Fragment, Shader::SwStage::Fragment, ps_params};
        std::vector<u32> ps_spv;
        {
            Shader::Pools ps_pools;
            auto ps_ir = Shader::TranslateProgram(ps_sh.code, ps_pools, ps_info, ps_rt, profile);
            if (!ps_info.fmasks.empty()) {
                return false;
            }
            ps_spv = Shader::Backend::SPIRV::EmitSPIRV(profile, ps_rt, ps_ir, binding);
            ps_pools.ReleaseContents();
            if (!ValidateSpirvModule(ps_spv, ps_sh.hash)) {
                return false;
            }
        }

        std::array<u32, 16> vs_user_regs{};
        if (!ctx.arena.Populate(vs_sh, vs_user_regs)) {
            return false;
        }
        Shader::RuntimeInfo vs_rt{};
        vs_rt.Initialize(Shader::HwStage::Vertex, Shader::SwStage::Vertex);
        AmdGpu::ShaderProgram vs_pgm{};
        std::memcpy(&vs_pgm.settings, vs_sh.regs.data() + 2, 8);
        vs_rt.props.fp_denorm_mode32 = vs_pgm.settings.fp_denorm_mode32;
        vs_rt.props.fp_denorm_mode16_64 = vs_pgm.settings.fp_denorm_mode64;
        vs_rt.props.fp_round_mode32 = vs_pgm.settings.fp_round_mode32;
        vs_rt.props.fp_round_mode16_64 = vs_pgm.settings.fp_round_mode64;
        vs_rt.props.num_user_data = vs_pgm.settings.num_user_regs;
        vs_rt.props.num_input_vgprs = vs_pgm.settings.vgpr_comp_cnt;
        vs_rt.props.num_allocated_vgprs = vs_pgm.NumVgprs();
        vs_rt.props.num_allocated_sgprs = vs_pgm.NumSgprs();
        vs_rt.props.dx10_clamp = vs_pgm.settings.dx10_clamp;
        u32 mapped_out = 0;
        if (!MapVsOutputsSafe(vs_rt.hw.vs.outputs,
                              std::bit_cast<AmdGpu::VsOutputControl>(vs_sh.regs[6]), mapped_out)) {
            return false;
        }
        vs_rt.hw.vs.num_outputs = mapped_out;
        vs_rt.sw.vs.step_rate_0 = 1;
        vs_rt.sw.vs.step_rate_1 = 1;

        Shader::ShaderParams vs_params{
            .user_data = vs_user_regs,
            .code = vs_sh.code,
            .hash = vs_sh.hash,
            .guest_base = ctx.arena.CodeBase(),
        };
        Shader::Info vs_info{Shader::HwStage::Vertex, Shader::SwStage::Vertex, vs_params};
        std::optional<Shader::Gcn::FetchShaderData> fetch;
        std::vector<u32> vs_spv;
        {
            Shader::Pools vs_pools;
            auto vs_ir = Shader::TranslateProgram(vs_sh.code, vs_pools, vs_info, vs_rt, profile);
            if (!vs_info.fmasks.empty()) {
                return false;
            }
            Shader::Gcn::FetchShaderData fetch_data;
            if (Shader::Gcn::ParseFetchShader(vs_info, fetch_data))
                fetch = std::move(fetch_data);
            vs_spv = Shader::Backend::SPIRV::EmitSPIRV(profile, vs_rt, vs_ir, binding);
            vs_pools.ReleaseContents();
            if (!ValidateSpirvModule(vs_spv, vs_sh.hash)) {
                return false;
            }
        }

        if (u64(ps_info.buffers.size() + vs_info.buffers.size()) > 30ull) {
            return false;
        }

        key.mrt_mask = ps_info.mrt_mask;
        key.num_color_attachments = std::bit_width(ps_info.mrt_mask);
        for (u32 cb = 0; cb < Shader::MaxColorBuffers; ++cb) {
            if ((key.mrt_mask & (1u << cb)) == 0) {
                std::memset(&key.color_buffers[cb], 0, sizeof(Shader::PsColorBuffer));
                key.write_masks[cb] = {};
            }
        }

        Vulkan::GraphicsPipeline::SerializationSupport sdata{};
        sdata.multisampling.rasterizationSamples = vk::SampleCountFlagBits::e1;
        if (!PopulateVertexInputState(fetch, key, sdata)) {
            return false;
        }
        return ctx.cache.PrecompileArchiveGraphics(&vs_info, vs_rt, vs_spv, fetch, &ps_info, ps_rt,
                                                   ps_spv, key, sdata);
    } catch (const std::bad_alloc&) {
        throw;
    } catch (const std::exception& error) {
        const std::string_view message{error.what()};
        if (message.find("ErrorDeviceLost") != std::string_view::npos ||
            message.find("OutOf") != std::string_view::npos)
            throw;
        LOG_DEBUG(Render_Vulkan, "Archive shader skipped: {}", message);
        return false;
    }
}

bool IsShaderFile(std::string name, u64 size) {
    name = ToLowerAscii(std::move(name));
    if (name.find("shader") != std::string::npos)
        return true;
    for (const auto* suffix : {".sb", ".sbin", ".gxsh", ".gsh", ".vsh", ".fsh", ".csh", ".bnd",
                               ".bnd.dcx", ".elf", ".prx", ".sprx", ".vsb", ".psb", ".csb"})
        if (name.ends_with(suffix))
            return true;
    return name == "eboot.bin" || (name.ends_with(".bin") && size <= 1024 * 1024);
}

std::vector<std::string> FindShaderFiles() {
    auto* mounts = Common::Singleton<Core::FileSys::MntPoints>::Instance();
    std::vector<std::pair<std::string, u32>> directories{{"/app0", 0}};
    std::vector<std::string> files;
    std::unordered_set<std::string> seen;
    while (!directories.empty()) {
        auto [path, depth] = std::move(directories.back());
        directories.pop_back();
        if (!seen.insert(path).second)
            continue;
        auto directory = mounts->OpenDir(path);
        if (!directory)
            continue;
        Core::FileSys::DirEntry entry;
        while (directory->Next(entry)) {
            if (entry.name == "." || entry.name == ".." ||
                entry.name.find_first_of("/\\") != std::string::npos)
                continue;
            const auto child = path + "/" + entry.name;
            if (entry.is_directory) {
                if (depth < 64)
                    directories.emplace_back(child, depth + 1);
            } else if (IsShaderFile(entry.name, entry.size)) {
                files.push_back(child);
            }
        }
#ifdef SHADPS4_UWP
        if (Core::Uwp::StopRequested())
            break;
#endif
    }
    std::ranges::sort(files);
    return files;
}

using Extracted = std::pair<ExtractedShader, std::string>;

void ExtractPayload(std::span<const u8> payload, const std::string& name,
                    std::vector<Extracted>& shaders, u32 depth = 0) {
    if (depth > 8 || payload.size() < 8)
        return;
    if (std::memcmp(payload.data(), "DCX\0", 4) == 0) {
        std::vector<u8> decoded;
        if (DecompressDcxSpan(payload, decoded))
            ExtractPayload(decoded, name, shaders, depth + 1);
        else
            LOG_WARNING(Render_Vulkan, "[ArchivePrecompile] Unsupported or invalid DCX: {}", name);
        return;
    }
    if (std::memcmp(payload.data(), "BND4", 4) == 0) {
        ForEachContainerSlice(payload, [&](std::span<const u8> slice, const std::string& stem) {
            if (slice.data() != payload.data())
                ExtractPayload(slice, stem, shaders, depth + 1);
        });
        return;
    }
    if (payload[0] == 'P' && payload[1] == 'K') {
        mz_zip_archive zip{};
        if (!mz_zip_reader_init_mem(&zip, payload.data(), payload.size(), 0))
            return;
        SCOPE_EXIT {
            mz_zip_reader_end(&zip);
        };
        for (mz_uint index = 0; index < mz_zip_reader_get_num_files(&zip); ++index) {
            mz_zip_archive_file_stat entry{};
            if (!mz_zip_reader_file_stat(&zip, index, &entry) || entry.m_is_directory ||
                entry.m_uncomp_size > MaxContainerBytes ||
                !IsShaderFile(entry.m_filename, entry.m_uncomp_size))
                continue;
            std::vector<u8> bytes(entry.m_uncomp_size);
            if (mz_zip_reader_extract_to_mem(&zip, index, bytes.data(), bytes.size(), 0))
                ExtractPayload(bytes, NormalizeShaderStem(entry.m_filename), shaders, depth + 1);
        }
        return;
    }
    for (size_t offset = 0; offset + 8 <= payload.size(); offset += 4) {
        const auto program = ParseOrbProgramAt(payload, offset);
        if (!program)
            continue;
        ExtractedShader shader;
        const bool extracted = program->type == 4   ? TryExtractCs(payload, *program, shader)
                               : program->type == 0 ? TryExtractPs(payload, *program, shader)
                               : program->type == 1 ? TryExtractVs(payload, *program, shader)
                                                    : false;
        if (extracted)
            shaders.emplace_back(std::move(shader), name);
        offset += static_cast<size_t>(program->footer - offset);
    }
}

std::vector<Extracted> ReadShaders(const std::string& path) {
    auto* mounts = Common::Singleton<Core::FileSys::MntPoints>::Instance();
    auto file = mounts->Open(path, Common::FS::FileAccessMode::Read);
    std::vector<Extracted> shaders;
    if (!file || !file->Size())
        return shaders;
    if (file->Size() <= MaxContainerBytes) {
        std::vector<u8> bytes(file->Size());
        u64 offset{};
        while (offset < bytes.size()) {
            const auto count = file->Read(bytes.data() + offset, bytes.size() - offset);
            if (count <= 0)
                return {};
            offset += count;
        }
        ExtractPayload(bytes, NormalizeShaderStem(path), shaders);
    } else {
        std::vector<u8> bytes(1024 * 1024);
        for (u64 offset = 0; offset < file->Size();) {
            if (!file->Seek(offset, Common::FS::SeekOrigin::SetOrigin))
                break;
            const auto count = file->Read(bytes.data(), bytes.size());
            if (count <= 0)
                break;
            ExtractPayload(std::span{bytes.data(), static_cast<size_t>(count)},
                           NormalizeShaderStem(path), shaders);
            if (static_cast<u64>(count) < bytes.size())
                break;
            offset += bytes.size() - 0x50000;
        }
    }
    return shaders;
}

u64 Fingerprint(const ExtractedShader& shader) {
    u64 hash = XXH3_64bits(shader.code.data(), shader.code.size() * sizeof(u32));
    hash ^= XXH3_64bits_withSeed(shader.regs.data(), sizeof(shader.regs), hash);
    hash ^= XXH3_64bits_withSeed(shader.usages.data(), shader.usages.size() * sizeof(u32), hash);
    hash ^=
        XXH3_64bits_withSeed(shader.semantics.data(), shader.semantics.size() * sizeof(u32), hash);
    hash ^= XXH3_64bits_withSeed(shader.vs_exports.data(),
                                 shader.vs_exports.size() * sizeof(shader.vs_exports[0]), hash);
    return hash ^ static_cast<u64>(shader.kind) ^ (u64(shader.is_srt) << 8);
}

using ShaderPair = std::pair<size_t, size_t>;
std::vector<ShaderPair> FindPairs(const std::vector<Extracted>& shaders) {
    std::map<std::string, std::vector<size_t>> vertices;
    for (size_t index = 0; index < shaders.size(); ++index)
        if (shaders[index].first.kind == StageKind::Vertex && !shaders[index].second.empty())
            vertices[shaders[index].second].push_back(index);
    std::vector<ShaderPair> pairs;
    for (size_t index = 0; index < shaders.size(); ++index) {
        if (shaders[index].first.kind != StageKind::Fragment)
            continue;
        if (auto found = vertices.find(shaders[index].second); found != vertices.end())
            for (auto vertex : found->second)
                pairs.emplace_back(vertex, index);
    }
    return pairs;
}
u64 PairFingerprint(u64 vertex, u64 fragment) {
    const std::array hashes{vertex, fragment};
    return XXH3_64bits_withSeed(hashes.data(), sizeof(hashes), 0x4c494e4b);
}
u64 PairFingerprint(const ExtractedShader& vs, const ExtractedShader& ps) {
    return PairFingerprint(Fingerprint(vs), Fingerprint(ps));
}

} // namespace

bool PipelineCache::PrecompileArchiveCompute(const Shader::Info& info, std::span<const u32> spv) {
    const auto device = instance.GetDevice();
    const auto module = CompileSPV(spv, device);
    SCOPE_EXIT {
        device.destroyShaderModule(module);
    };
    ComputePipelineKey key{.value = info.pgm_hash};
    ComputePipeline::SerializationSupport data{};
    ComputePipeline pipeline{instance, scheduler, desc_heap, nullptr, profile, *pipeline_cache,
                             key,      info,      module,    data,    true};
    return static_cast<bool>(pipeline.Handle());
}

bool PipelineCache::PrecompileArchiveGraphics(
    const Shader::Info* vs_info, const Shader::RuntimeInfo& vs_rt, std::span<const u32> vs_spv,
    const std::optional<Shader::Gcn::FetchShaderData>& fetch, const Shader::Info* fs_info,
    const Shader::RuntimeInfo& fs_rt, std::span<const u32> fs_spv, const GraphicsPipelineKey& key,
    GraphicsPipeline::SerializationSupport& data) {
    const auto device = instance.GetDevice();
    vk::ShaderModule vs{}, fs{};
    SCOPE_EXIT {
        if (fs)
            device.destroyShaderModule(fs);
        if (vs)
            device.destroyShaderModule(vs);
    };
    if (vs_info)
        vs = CompileSPV(vs_spv, device);
    if (fs_info)
        fs = CompileSPV(fs_spv, device);
    std::array<const Shader::Info*, MaxShaderStages> stages{};
    std::array<Shader::RuntimeInfo, MaxShaderStages> runtime{};
    std::array<vk::ShaderModule, MaxShaderStages> modules{};
    stages[u32(Shader::SwStage::Vertex)] = vs_info;
    stages[u32(Shader::SwStage::Fragment)] = fs_info;
    runtime[u32(Shader::SwStage::Vertex)] = vs_rt;
    runtime[u32(Shader::SwStage::Fragment)] = fs_rt;
    modules[u32(Shader::SwStage::Vertex)] = vs;
    modules[u32(Shader::SwStage::Fragment)] = fs;
    GraphicsPipeline pipeline{instance,
                              scheduler,
                              desc_heap,
                              nullptr,
                              profile,
                              key,
                              *pipeline_cache,
                              stages,
                              runtime,
                              fetch ? &*fetch : nullptr,
                              modules,
                              data,
                              true};
    return static_cast<bool>(pipeline.Handle());
}

void PipelineCache::PrecompileShaderFiles(const std::function<void(u32, u32, u32)>& progress) {
    try {
        if (!pipeline_cache)
            return;
        const auto files = FindShaderFiles();
        if (files.empty())
            return;
        std::unordered_set<u64> candidates;
        struct LooseShader {
            std::string path;
            u64 fingerprint;
        };
        struct LooseStages {
            std::vector<LooseShader> vertices, fragments;
        };
        std::map<std::string, LooseStages> loose;
        u32 file_index{};
        for (const auto& file : files) {
#ifdef SHADPS4_UWP
            if (Core::Uwp::StopRequested())
                return;
            Core::Uwp::UpdateShaderCompilation(SHADPS4_SHADERS_LOADING_CACHE, file_index++,
                                               files.size());
#endif
            const auto shaders = ReadShaders(file);
            if (shaders.size() == 1) {
                const auto& shader = shaders[0].first;
                const auto slash = file.find_last_of('/');
                const auto name = file.substr(0, slash + 1) + NormalizeShaderStem(file);
                if (shader.kind == StageKind::Vertex)
                    loose[name].vertices.push_back({file, Fingerprint(shader)});
                else if (shader.kind == StageKind::Fragment)
                    loose[name].fragments.push_back({file, Fingerprint(shader)});
            }
            for (const auto& [shader, name] : shaders)
                candidates.insert(Fingerprint(shader));
            for (const auto& [vs, ps] : FindPairs(shaders))
                candidates.insert(PairFingerprint(shaders[vs].first, shaders[ps].first));
        }
        for (const auto& [name, stages] : loose)
            for (const auto& vertex : stages.vertices)
                for (const auto& fragment : stages.fragments)
                    candidates.insert(PairFingerprint(vertex.fingerprint, fragment.fingerprint));
        if (candidates.empty())
            return;
        DescriptorArena arena;
        if (!arena.Valid()) {
            LOG_WARNING(Render_Vulkan,
                        "[ArchivePrecompile] Descriptor scratch allocation unavailable");
            return;
        }
        PrecompileContext context{*this, arena};
        const auto uuid = instance.GetPipelineCacheUUID();
        const std::array<u64, 4> header{
            ManifestVersion, XXH3_64bits(&profile, sizeof(profile)),
            XXH3_64bits(uuid.data(), uuid.size()),
            XXH3_64bits_withSeed(__DATE__ " " __TIME__, sizeof(__DATE__ " " __TIME__),
                u64(EmulatorSettings.IsInlineFetchShader()) |
                (u64(EmulatorSettings.IsDirectMemoryAccessEnabled()) << 1))};
        auto& storage = Storage::DataBase::Instance();
        std::map<u64, u64> results;
        std::vector<u8> saved;
        storage.Load(Storage::BlobType::ShaderProfile, "archive_precompile", saved);
        if (driver_cache_hash && saved.size() >= sizeof(header) &&
            saved.size() <= 4 * 1024 * 1024 && (saved.size() - sizeof(header)) % 16 == 0 &&
            std::memcmp(saved.data(), header.data(), sizeof(header)) == 0)
            for (size_t offset = sizeof(header); offset < saved.size(); offset += 16)
                results.emplace(ReadLe64(saved.data() + offset),
                                ReadLe64(saved.data() + offset + 8));
        const auto& serial = Common::ElfInfo::Instance().GameSerial();
        const auto marker_path = Common::FS::GetUserPath(Common::FS::PathType::CacheDir) /
                                 fmt::format("archive-precompile-{:016x}.pending",
                                             XXH3_64bits(serial.data(), serial.size()));
        {
            Common::FS::IOFile marker{marker_path, Common::FS::FileAccessMode::Read};
            std::array<u64, 5> previous{};
            if (marker.IsOpen() && marker.GetSize() == sizeof(previous) &&
                marker.ReadRaw<u64>(previous.data(), previous.size()) == previous.size() &&
                std::equal(header.begin(), header.end(), previous.begin())) {
                results[previous.back()] = 2;
                LOG_WARNING(Render_Vulkan,
                            "[ArchivePrecompile] Previous attempt interrupted on {:016x}; using "
                            "runtime compilation",
                            previous.back());
            }
        }
        const auto clear_marker = [&] {
            std::error_code error;
            std::filesystem::remove(marker_path, error);
        };
        u32 done{}, ready{}, reused{};
        const auto total = static_cast<u32>(candidates.size());
        progress(0, total, 0);
        std::unordered_set<u64> visited;
        const auto attempt = [&](u64 id, auto&& compile) {
            if (!candidates.contains(id) || !visited.insert(id).second)
                return;
            if (const auto found = results.find(id); found != results.end()) {
                ready += found->second == 1;
                ++reused;
            } else {
                const std::array<u64, 5> record{header[0], header[1], header[2], header[3], id};
                {
                    Common::FS::IOFile marker{marker_path, Common::FS::FileAccessMode::Create};
                    if (marker.IsOpen()) {
                        marker.WriteRaw<u64>(record.data(), record.size());
                        marker.Flush();
                    }
                }
                bool ok{};
                try {
                    ok = compile();
                } catch (const std::bad_alloc&) {
                    clear_marker();
                    throw;
                } catch (const std::exception& error) {
                    if (std::string_view{error.what()}.find("OutOf") != std::string_view::npos)
                        clear_marker();
                    throw;
                }
                clear_marker();
                results[id] = ok ? 1 : 2;
                ready += ok;
            }
            progress(++done, total, ready);
        };
        for (const auto& file : files) {
#ifdef SHADPS4_UWP
            if (Core::Uwp::StopRequested())
                break;
#endif
            const auto shaders = ReadShaders(file);
            for (const auto& [shader, name] : shaders) {
                attempt(Fingerprint(shader),
                        [&] { return CompileSingleExtractedInner(context, shader); });
            }
            for (const auto& [vs, ps] : FindPairs(shaders)) {
                const auto& vertex = shaders[vs].first;
                const auto& fragment = shaders[ps].first;
                attempt(PairFingerprint(vertex, fragment),
                        [&] { return CompileLinkedPairInner(context, vertex, fragment); });
            }
        }
        for (const auto& [name, stages] : loose) {
            for (const auto& vertex : stages.vertices) {
                const auto vs = ReadShaders(vertex.path);
                if (vs.size() != 1 || Fingerprint(vs[0].first) != vertex.fingerprint)
                    continue;
                for (const auto& fragment : stages.fragments) {
#ifdef SHADPS4_UWP
                    if (Core::Uwp::StopRequested())
                        break;
#endif
                    const auto ps = ReadShaders(fragment.path);
                    if (ps.size() != 1 || Fingerprint(ps[0].first) != fragment.fingerprint)
                        continue;
                    attempt(PairFingerprint(vertex.fingerprint, fragment.fingerprint), [&] {
                        return CompileLinkedPairInner(context, vs[0].first, ps[0].first);
                    });
                }
            }
        }
        SaveDriverCache();
        if (driver_cache_hash) {
            std::vector<u8> manifest(sizeof(header) + results.size() * 16);
            std::memcpy(manifest.data(), header.data(), sizeof(header));
            size_t offset = sizeof(header);
            for (const auto& [id, result] : results) {
                std::memcpy(manifest.data() + offset, &id, 8);
                std::memcpy(manifest.data() + offset + 8, &result, 8);
                offset += 16;
            }
            storage.Save(Storage::BlobType::ShaderProfile, "archive_precompile",
                         std::move(manifest));
        }
        LOG_INFO(Render_Vulkan,
                 "[ArchivePrecompile] files={} candidates={} ready={} skipped={} reused={}",
                 files.size(), total, ready, done - ready, reused);
    } catch (const std::bad_alloc&) {
        LOG_WARNING(Render_Vulkan, "[ArchivePrecompile] Stopped under host memory pressure; "
                                   "remaining shaders compile at runtime");
    } catch (const std::exception& error) {
        const std::string_view reason{error.what()};
        if (reason.find("OutOf") == std::string_view::npos)
            throw;
        LOG_WARNING(Render_Vulkan, "[ArchivePrecompile] Stopped under device memory pressure: {}",
                    reason);
    }
}

} // namespace Vulkan
