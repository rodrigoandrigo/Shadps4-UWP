#pragma once

#include <cstring>
#include <limits>
#include <memory>
#include <span>
#include <vector>

#include "common/types.h"

namespace D3D12::GuestTransfer {

[[nodiscard]] constexpr bool IsValidFillRange(VAddr destination, u32 size) noexcept {
    if (!size)
        return true;
    return destination % 4 == 0 && size % 4 == 0 && size <= 16 * 1024 * 1024 &&
           destination <= std::numeric_limits<VAddr>::max() - size;
}

[[nodiscard]] constexpr bool IsValidGdsRange(VAddr address, u32 size) noexcept {
    return !size || (address <= 65536 && size <= 65536 - address);
}

template <typename AcquireAccess, typename Read, typename Write>
void Copy(VAddr destination, VAddr source, u32 size, bool destination_gds, bool source_gds,
          std::span<u8> gds, AcquireAccess&& acquire_access, Read&& read, Write&& write) {
    if (!size)
        return;

    std::shared_ptr<void> source_access;
    std::shared_ptr<void> destination_access;
    if (!source_gds)
        source_access = acquire_access(source, size, false);
    if (!destination_gds)
        destination_access = acquire_access(destination, size, true);

    // The temporary snapshot gives guest copies memmove semantics, including aliases and overlap.
    std::vector<u8> snapshot(size);
    if (source_gds)
        std::memcpy(snapshot.data(), gds.data() + source, size);
    else
        read(source, std::span<u8>{snapshot});

    if (destination_gds)
        std::memcpy(gds.data() + destination, snapshot.data(), size);
    else
        write(destination, std::span<const u8>{snapshot});
}

template <typename AcquireAccess, typename Write>
void Fill(VAddr destination, u32 size, u32 value, bool destination_gds, std::span<u8> gds,
          AcquireAccess&& acquire_access, Write&& write) {
    if (!size)
        return;

    std::shared_ptr<void> destination_access;
    if (!destination_gds)
        destination_access = acquire_access(destination, size, true);

    std::vector<u32> words(size / sizeof(u32), value);
    const auto bytes = std::span<const u8>{reinterpret_cast<const u8*>(words.data()), size};
    if (destination_gds)
        std::memcpy(gds.data() + destination, bytes.data(), bytes.size());
    else
        write(destination, bytes);
}

} // namespace D3D12::GuestTransfer
