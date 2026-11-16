// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <stop_token>
#include <string>
#include <thread>

#include "core/net/host_net.h"

namespace Libraries::Net {

class DiscoveryResponder {
public:
    static DiscoveryResponder& Instance();

    void Start();
    void Stop();

private:
    DiscoveryResponder() = default;
    ~DiscoveryResponder();
    DiscoveryResponder(const DiscoveryResponder&) = delete;
    DiscoveryResponder& operator=(const DiscoveryResponder&) = delete;

    void Run(std::stop_token stop, const std::string& response);
    void StopLocked();
    std::string BuildResponse() const;

    std::mutex m_mutex;
    std::atomic<bool> m_started{false};
    std::unique_ptr<Core::Net::Host::WakeHandle> m_wake;
    std::jthread m_thread;
};

} // namespace Libraries::Net
