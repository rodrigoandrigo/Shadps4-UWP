// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <array>
#include "common/types.h"
#include "core/libraries/kernel/sync/semaphore_diagnostics.h"

namespace Libraries::Kernel {

template <s64 Maximum>
class SemaphorePool {
public:
    static HANDLE Acquire(s32 initial) {
        if (initial < 0 || initial > Maximum) {
            SetLastError(ERROR_INVALID_PARAMETER);
            return nullptr;
        }
        auto& pool = Instance();
        HANDLE handle{};
        uint64_t reused{};
        AcquireSRWLockExclusive(&pool.lock);
        if (pool.size) {
            handle = pool.handles[--pool.size];
            --SemaphoreDiagnostics::counters.cached;
            reused = ++SemaphoreDiagnostics::counters.reused;
        }
        ReleaseSRWLockExclusive(&pool.lock);
        if (reused == 1 || (reused && reused % 65536 == 0)) {
            SemaphoreDiagnostics::Report("reused", nullptr, handle, Maximum, initial, 0);
        }
        if (!handle) {
            handle = CreateSemaphoreW(nullptr, 0, Maximum, nullptr);
            SemaphoreDiagnostics::Created(nullptr, handle, Maximum, 0);
        }
        if (handle && initial && !ReleaseSemaphore(handle, initial, nullptr)) {
            const auto error = GetLastError();
            ++SemaphoreDiagnostics::counters.release_failures;
            SemaphoreDiagnostics::Report("initialize.failed", nullptr, handle, Maximum, initial,
                                         error);
            Close(handle);
            SetLastError(error);
            return nullptr;
        }
        return handle;
    }

    static void Recycle(HANDLE handle) {
        if constexpr (Maximum > 2) {
            Close(handle);
            return;
        }
        for (;;) {
            const auto result = WaitForSingleObjectEx(handle, 0, FALSE);
            if (result == WAIT_TIMEOUT) {
                break;
            }
            if (result != WAIT_OBJECT_0) {
                const auto error = GetLastError();
                ++SemaphoreDiagnostics::counters.wait_failures;
                SemaphoreDiagnostics::Report("reset.failed", nullptr, handle, Maximum, 0, error,
                                             result);
                Close(handle);
                return;
            }
        }
        auto& pool = Instance();
        AcquireSRWLockExclusive(&pool.lock);
        if (pool.size < pool.handles.size()) {
            pool.handles[pool.size++] = handle;
            ++SemaphoreDiagnostics::counters.cached;
            handle = nullptr;
        }
        ReleaseSRWLockExclusive(&pool.lock);
        if (handle) {
            Close(handle);
        }
    }

private:
    static void Close(HANDLE handle) {
        if (CloseHandle(handle)) {
            ++SemaphoreDiagnostics::counters.closed;
            --SemaphoreDiagnostics::counters.live;
        } else {
            const auto error = GetLastError();
            ++SemaphoreDiagnostics::counters.close_failures;
            SemaphoreDiagnostics::Report("CloseHandle.failed", nullptr, handle, Maximum, 0, error);
        }
    }

    SemaphorePool() {
        if constexpr (Maximum == 1) {
            for (auto& handle : handles) {
                handle = CreateSemaphoreW(nullptr, 0, Maximum, nullptr);
                SemaphoreDiagnostics::Created(nullptr, handle, Maximum, 0);
                if (!handle) {
                    break;
                }
                ++size;
                ++SemaphoreDiagnostics::counters.cached;
            }
        }
    }

    ~SemaphorePool() {
        for (size_t i = 0; i < size; ++i) {
            Close(handles[i]);
            --SemaphoreDiagnostics::counters.cached;
        }
    }

    static SemaphorePool& Instance() {
        static SemaphorePool pool;
        return pool;
    }

    SRWLOCK lock = SRWLOCK_INIT;
    std::array<HANDLE, Maximum <= 2 ? 256 : 0> handles{};
    size_t size{};
};

} // namespace Libraries::Kernel
