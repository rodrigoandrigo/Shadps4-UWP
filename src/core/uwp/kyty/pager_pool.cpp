// SPDX-License-Identifier: GPL-2.0-or-later
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <stdexcept>
#include <vector>
#include "pagerStore.h"
#include "pager_pool.h"
#ifdef TRACY_ENABLE
#include "../../../../externals/tracy/public/common/tracy_lz4.hpp"
#else
#include "../../../../externals/tracy/public/common/tracy_lz4.cpp"
#endif
namespace Xbox {
struct PagerPool::Impl {
    static constexpr uint64_t Window = 65536;
    static constexpr uint32_t Granule = 4096;
    static constexpr uint32_t ChunkSize = 1024;
    static constexpr uint32_t MaxGranules = 12;
    static constexpr uint32_t MaxSlots = 262144;
    static constexpr uint32_t None = UINT32_MAX;

    struct Entry {
        uint32_t bytes = 0;
        uint32_t newer = None;
        uint32_t older = None;
        uint32_t count = 0;
        uint32_t granules[MaxGranules]{};
    };

    struct Pool {
        std::mutex mutex;
        uint32_t max_chunks = 0;
        std::vector<char*> chunks;
        std::vector<uint32_t> free;
        std::vector<Entry> entries;
        uint32_t newest = None;
        uint32_t oldest = None;
        char gather[MaxGranules * Granule];
    };

    Pool pool;
    std::mutex compress_mutex;
    tracy::LZ4_stream_t state{};
    char packed[LZ4_COMPRESSBOUND(Window)];
    char* GranuleAt(uint32_t index) {
        return pool.chunks[index / ChunkSize] + static_cast<uint64_t>(index % ChunkSize) * Granule;
    }

    void Remove(uint32_t slot, PagerCounters& counters) {
        auto& entry = pool.entries[slot];
        if (entry.bytes == 0) {
            return;
        }
        (entry.newer != None ? pool.entries[entry.newer].older : pool.newest) = entry.older;
        (entry.older != None ? pool.entries[entry.older].newer : pool.oldest) = entry.newer;
        for (uint32_t i = 0; i < entry.count; i++) {
            pool.free.push_back(entry.granules[i]);
        }
        counters.pool_bytes -= uint64_t{entry.count} * Granule;
        counters.pool_raw -= Window;
        entry.bytes = 0;
        entry.count = 0;
        entry.newer = None;
        entry.older = None;
    }

    bool Reserve(uint32_t need, PagerCounters& counters) {
        while (pool.free.size() < need) {
            if (pool.chunks.size() < pool.max_chunks) {
                if (auto* chunk = static_cast<char*>(std::malloc(uint64_t{ChunkSize} * Granule));
                    chunk != nullptr) {
                    const auto first = static_cast<uint32_t>(pool.chunks.size()) * ChunkSize;
                    pool.chunks.push_back(chunk);
                    for (uint32_t i = 0; i < ChunkSize; i++) {
                        pool.free.push_back(first + i);
                    }
                    continue;
                }
            }
            if (pool.oldest == None) {
                return false;
            }
            Remove(pool.oldest, counters);
            counters.pool_dropped++;
        }
        return true;
    }

    void Put(uint32_t slot, const char* packed, uint32_t bytes, PagerCounters& counters) {
        std::lock_guard lock(pool.mutex);
        if (pool.max_chunks == 0) {
            return;
        }
        const uint32_t need = (bytes + Granule - 1) / Granule;
        if (slot >= MaxSlots || need > MaxGranules) {
            counters.pool_skipped++;
            return;
        }
        Remove(slot, counters);
        if (!Reserve(need, counters)) {
            counters.pool_skipped++;
            return;
        }
        auto& entry = pool.entries[slot];
        for (uint32_t i = 0; i < need; i++) {
            const uint32_t granule = pool.free.back();
            pool.free.pop_back();
            entry.granules[i] = granule;
            std::memcpy(GranuleAt(granule), packed + uint64_t{i} * Granule,
                        std::min<uint64_t>(Granule, bytes - uint64_t{i} * Granule));
        }
        entry.bytes = bytes;
        entry.count = need;
        entry.older = pool.newest;
        entry.newer = None;
        (pool.newest != None ? pool.entries[pool.newest].newer : pool.oldest) = slot;
        pool.newest = slot;
        counters.pool_bytes += uint64_t{need} * Granule;
        counters.pool_raw += Window;
    }

    void PutRaw(uint32_t slot, const void* data, uint64_t size, PagerCounters& counters) {

        std::unique_lock lock(compress_mutex, std::try_to_lock);
        if (!lock.owns_lock() || size != Window) {
            counters.pool_skipped++;
            return;
        }

        const int bytes = tracy::LZ4_compress_fast_extState(&state, static_cast<const char*>(data),
                                                            packed, static_cast<int>(size),
                                                            static_cast<int>(sizeof(packed)), 1);
        if (bytes > 0) {
            Put(slot, packed, static_cast<uint32_t>(bytes), counters);
        }
    }

    bool Read(uint32_t slot, void* data, PagerCounters& counters) {
        std::lock_guard lock(pool.mutex);
        if (slot >= pool.entries.size() || pool.entries[slot].bytes == 0) {
            return false;
        }
        const auto& entry = pool.entries[slot];
        for (uint32_t i = 0; i < entry.count; i++) {
            std::memcpy(pool.gather + uint64_t{i} * Granule, GranuleAt(entry.granules[i]), Granule);
        }
        if (tracy::LZ4_decompress_safe(pool.gather, static_cast<char*>(data),
                                       static_cast<int>(entry.bytes),
                                       static_cast<int>(Window)) != static_cast<int>(Window)) {
            counters.pool_bad++;
            Remove(slot, counters);
            return false;
        }
        counters.pool_hits++;
        return true;
    }

    void Drop(uint32_t slot, PagerCounters& counters) {
        std::lock_guard lock(pool.mutex);
        if (slot < pool.entries.size()) {
            Remove(slot, counters);
        }
    }

    explicit Impl(uint64_t bytes) {
        const auto chunks = static_cast<uint32_t>(bytes / (uint64_t{ChunkSize} * Granule));
        pool.entries.resize(MaxSlots);
        pool.chunks.reserve(chunks);
        pool.free.reserve(uint64_t{chunks} * ChunkSize);
        pool.max_chunks = chunks;
    }
    ~Impl() {
        for (auto* chunk : pool.chunks)
            std::free(chunk);
    }
};
PagerPool::PagerPool(uint64_t bytes) : impl{std::make_unique<Impl>(bytes)} {}
PagerPool::~PagerPool() = default;
void PagerPool::Put(uint32_t slot, const void* data, uint64_t size, PagerCounters& counters) {
    impl->PutRaw(slot, data, size, counters);
}
bool PagerPool::Read(uint32_t slot, void* data, PagerCounters& counters) {
    return impl->Read(slot, data, counters);
}
void PagerPool::Drop(uint32_t slot, PagerCounters& counters) {
    impl->Drop(slot, counters);
}
} // namespace Xbox
