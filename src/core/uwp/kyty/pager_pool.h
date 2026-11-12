// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdint>
#include <memory>

namespace Xbox {
struct PagerCounters;
class PagerPool {
public:
    explicit PagerPool(uint64_t bytes);
    ~PagerPool();
    void Put(uint32_t slot, const void* data, uint64_t size, PagerCounters& counters);
    bool Read(uint32_t slot, void* data, PagerCounters& counters);
    void Drop(uint32_t slot, PagerCounters& counters);

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
} // namespace Xbox
