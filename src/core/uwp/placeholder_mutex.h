// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <mutex>

namespace Core::Uwp {
inline std::mutex& PlaceholderMutex() {
    static std::mutex mutex;
    return mutex;
}
} // namespace Core::Uwp
