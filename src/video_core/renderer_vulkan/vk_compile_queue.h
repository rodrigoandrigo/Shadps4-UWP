// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <condition_variable>
#include <deque>
#include <future>
#include <mutex>
#include <thread>
#include <type_traits>
#include <vector>

#include "common/thread.h"

namespace Vulkan {

class CompileQueue {
public:
    explicit CompileQueue(size_t count, const char* name) {
        workers.reserve(count);
        try {
            for (size_t i = 0; i < count; ++i) {
                workers.emplace_back([this, name] {
                    Common::SetCurrentThreadName(name);
                    for (;;) {
                        std::packaged_task<void()> task;
                        {
                            std::unique_lock lock{mutex};
                            available.wait(lock, [this] { return stopping || !tasks.empty(); });
                            if (tasks.empty()) {
                                return;
                            }
                            task = std::move(tasks.front());
                            tasks.pop_front();
                        }
                        task();
                    }
                });
            }
        } catch (...) {
            Stop();
            throw;
        }
    }

    ~CompileQueue() {
        Stop();
    }

private:
    void Stop() {
        {
            std::scoped_lock lock{mutex};
            stopping = true;
        }
        available.notify_all();
        for (auto& worker : workers) {
            worker.join();
        }
    }

public:
    template <typename F>
    auto Submit(F&& function) {
        std::packaged_task<std::invoke_result_t<F>()> task{std::forward<F>(function)};
        auto future = task.get_future();
        {
            std::scoped_lock lock{mutex};
            tasks.emplace_back([task = std::move(task)]() mutable { task(); });
        }
        available.notify_one();
        return future;
    }

private:
    std::mutex mutex;
    std::condition_variable available;
    std::deque<std::packaged_task<void()>> tasks;
    bool stopping{};
    std::vector<std::thread> workers;
};

} // namespace Vulkan
