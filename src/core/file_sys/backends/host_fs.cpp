// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <fstream>
#include <system_error>
#include <unordered_set>

#include "common/string_util.h"
#ifdef SHADPS4_UWP
#include "core/address_space.h"
#endif
#include "core/file_sys/backends/host_fs.h"
#include "core/file_sys/fs.h"

namespace Core::FileSys {

namespace {

class MetadataDirectory final : public IDirectory {
public:
    MetadataDirectory(std::unique_ptr<IDirectory> primary, std::unique_ptr<IDirectory> fallback)
        : primary(std::move(primary)), fallback(std::move(fallback)) {}

    bool Next(DirEntry& out) override {
        while ((primary && primary->Next(out)) || (fallback && fallback->Next(out))) {
            if (names.emplace(Common::ToLower(out.name)).second)
                return true;
        }
        return false;
    }

    void Rewind() override {
        if (primary)
            primary->Rewind();
        if (fallback)
            fallback->Rewind();
        names.clear();
    }

private:
    std::unique_ptr<IDirectory> primary, fallback;
    std::unordered_set<std::string> names;
};

FileStat MakeFileStat(const Common::FS::FileInfo& info, u32 inode) {
    return {
        .size = info.size,
        .inode = inode,
        .mtime_sec = info.mtime_sec,
        .mtime_nsec = info.mtime_nsec,
        .atime_sec = info.atime_sec,
        .atime_nsec = info.atime_nsec,
        .ctime_sec = info.ctime_sec,
        .ctime_nsec = info.ctime_nsec,
        .is_directory = info.is_directory,
    };
}

} // namespace

// Guest code expects POSIX sharing: opening a file already open for writing must not fail.
HostFile::HostFile(std::filesystem::path host_path, Common::FS::FileAccessMode mode, bool read_only)
    : m_path(std::move(host_path)), m_file(m_path, mode, Common::FS::FileType::BinaryFile,
                                           Common::FS::FileShareFlag::ShareReadWrite),
      m_read_only(read_only) {
    if (const auto info = m_file.GetInfo()) {
        m_inode = GetGuestInode(FileIdentity{.host = info->identity}).value_or(0);
    }
}

s64 HostFile::Read(void* dst, u64 size) {
#ifdef SHADPS4_UWP
    const auto residency = AddressSpace::AcquireHostAccess(dst, size);
#endif
    if (!m_file.IsOpen()) {
        return -1;
    }
    return static_cast<s64>(m_file.ReadRaw<u8>(dst, size));
}

s64 HostFile::Write(const void* src, u64 size) {
#ifdef SHADPS4_UWP
    const auto residency = AddressSpace::AcquireHostAccess(src, size);
#endif
    if (!m_file.IsOpen() || m_read_only) {
        return -1;
    }
    return static_cast<s64>(m_file.WriteRaw<u8>(src, size));
}

bool HostFile::Seek(s64 offset, Common::FS::SeekOrigin origin) {
    if (!m_file.IsOpen()) {
        return false;
    }
    return m_file.Seek(offset, origin);
}

u64 HostFile::Tell() const {
    if (!m_file.IsOpen()) {
        return 0;
    }
    return static_cast<u64>(m_file.Tell());
}

u64 HostFile::Size() const {
    if (!m_file.IsOpen()) {
        return 0;
    }
    return m_file.GetSize();
}

bool HostFile::Flush() {
    if (!m_file.IsOpen()) {
        return false;
    }
    if (m_read_only) {
        return true;
    }
    return m_file.Commit();
}

bool HostFile::IsOpen() const {
    return m_file.IsOpen();
}

void HostFile::Stat(FileStat& out) {
    m_file.Flush();
    const auto info = m_file.GetInfo();
    out = info ? MakeFileStat(*info, m_inode) : FileStat{};
}

HostDirectory::HostDirectory(const std::filesystem::path& root) : m_root(root) {
    std::error_code ec;
    m_it = std::filesystem::directory_iterator(m_root, ec);
    m_end = std::filesystem::directory_iterator{};
}

bool HostDirectory::Next(DirEntry& out) {
    if (m_it == m_end) {
        return false;
    }

    const auto& entry = *m_it;
    std::error_code ec;
    out.name = entry.path().filename().string();
    out.is_directory = entry.is_directory(ec);
    out.size = out.is_directory ? 0 : entry.file_size(ec);

    ++m_it;
    return true;
}

void HostDirectory::Rewind() {
    std::error_code ec;
    m_it = std::filesystem::directory_iterator(m_root, ec);
}

HostFsBackend::HostFsBackend(std::filesystem::path root, bool read_only)
    : m_root(std::move(root)), m_read_only(read_only) {
    if (Common::ToLower(m_root.filename().string()) == "image0") {
        const auto parent = m_root.parent_path();
        std::error_code ec;
        for (const auto& entry : std::filesystem::directory_iterator(parent, ec)) {
            if (Common::ToLower(entry.path().filename().string()) == "sc0" &&
                entry.is_directory(ec)) {
                m_metadata = std::make_unique<HostFsBackend>(entry.path(), true);
                break;
            }
        }
    }
}

std::filesystem::path HostFsBackend::Resolve(std::string_view rel_path) const {
    if (rel_path.empty()) {
        return m_root;
    }
    std::filesystem::path direct = m_root;
    direct /= rel_path;
    std::error_code ec;
    if (std::filesystem::exists(direct, ec))
        return direct;
    if constexpr (NeedsCaseInsensitiveSearch) {
        if (const auto resolved = ResolveCaseInsensitive(rel_path))
            return *resolved;
    }
    if (m_metadata && (rel_path == "sce_sys" || rel_path.starts_with("sce_sys/"))) {
        const auto fallback = m_metadata->Resolve(rel_path == "sce_sys" ? std::string_view{}
                                                                      : rel_path.substr(8));
        if (std::filesystem::exists(fallback, ec))
            return fallback;
    }
    return direct;
}

std::optional<std::filesystem::path> HostFsBackend::ResolveCaseInsensitive(
    std::string_view rel_path) const {
    std::scoped_lock lk{m_case_cache_mutex};

    std::filesystem::path current = m_root;
    std::string prefix;
    std::error_code ec;

    for (const auto& part : std::filesystem::path(rel_path)) {
        const std::string part_str = part.string();
        if (part_str.empty() || part_str == ".") {
            continue;
        }
        if (!prefix.empty()) {
            prefix += '/';
        }
        prefix += Common::ToLower(part_str);

        if (const auto it = m_case_cache.find(prefix); it != m_case_cache.end()) {
            current = it->second;
            continue;
        }

        if (std::filesystem::path candidate = current / part_str;
            std::filesystem::exists(candidate, ec)) {
            current = std::move(candidate);
            m_case_cache.emplace(prefix, current);
            continue;
        }

        const std::string part_low = Common::ToLower(part_str);
        bool found = false;
        for (const auto& entry : std::filesystem::directory_iterator(current, ec)) {
            const auto name = entry.path().filename();
            if (Common::ToLower(name.string()) != part_low) {
                continue;
            }
            current /= name;
            m_case_cache.emplace(prefix, current);
            found = true;
            break;
        }
        if (!found) {
            return std::nullopt;
        }
    }
    return current;
}

bool HostFsBackend::Exists(std::string_view rel_path) {
    std::error_code ec;
    return std::filesystem::exists(Resolve(rel_path), ec);
}

bool HostFsBackend::IsDirectory(std::string_view rel_path) {
    std::error_code ec;
    return std::filesystem::is_directory(Resolve(rel_path), ec);
}

bool HostFsBackend::Stat(std::string_view rel_path, FileStat& out) {
    const auto info = Common::FS::GetFileInfo(Resolve(rel_path));
    if (!info) {
        return false;
    }
    const auto inode = GetGuestInode(FileIdentity{.host = info->identity});
    if (!inode) {
        return false;
    }
    out = MakeFileStat(*info, *inode);
    return true;
}

std::unique_ptr<IFile> HostFsBackend::Open(std::string_view rel_path,
                                           Common::FS::FileAccessMode mode) {
    const bool writable = mode != Common::FS::FileAccessMode::Read;
    if (writable &&
        (m_read_only ||
         (m_metadata && (rel_path == "sce_sys" || rel_path.starts_with("sce_sys/"))))) {
        return nullptr;
    }
    const auto path = Resolve(rel_path);
    std::error_code ec;
    if (mode != Common::FS::FileAccessMode::Create && !std::filesystem::is_regular_file(path, ec)) {
        return nullptr;
    }
    auto handle = std::make_unique<HostFile>(path, mode, m_read_only || !writable);
    if (!handle->IsOpen()) {
        return nullptr;
    }
    return handle;
}

std::unique_ptr<IDirectory> HostFsBackend::OpenDir(std::string_view rel_path) {
    const auto path = Resolve(rel_path);
    std::error_code ec;
    if (!std::filesystem::is_directory(path, ec)) {
        return nullptr;
    }
    auto primary = std::make_unique<HostDirectory>(path);
    if (m_metadata && (rel_path == "sce_sys" || rel_path.starts_with("sce_sys/"))) {
        auto fallback = m_metadata->OpenDir(rel_path == "sce_sys" ? std::string_view{}
                                                               : rel_path.substr(8));
        if (fallback)
            return std::make_unique<MetadataDirectory>(std::move(primary), std::move(fallback));
    }
    return primary;
}

std::optional<std::vector<u8>> HostFsBackend::ReadFile(std::string_view rel_path) const {
    const std::filesystem::path full_path = Resolve(rel_path);
    std::error_code ec;
    if (!std::filesystem::is_regular_file(full_path, ec) || ec) {
        return std::nullopt;
    }

    std::ifstream in(full_path, std::ios::binary | std::ios::ate);
    if (!in.is_open()) {
        return std::nullopt;
    }
    const auto size = in.tellg();
    if (size < 0) {
        return std::nullopt;
    }
    std::vector<u8> data(static_cast<size_t>(size));
    in.seekg(0);
    in.read(reinterpret_cast<char*>(data.data()), size);
    if (!in.good() && !in.eof()) {
        return std::nullopt;
    }
    return data;
}

} // namespace Core::FileSys
