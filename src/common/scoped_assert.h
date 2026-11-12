// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

namespace Common {
inline thread_local bool assertion_exceptions_enabled{};

class ScopedAssertExceptions {
public:
    ScopedAssertExceptions() : previous{assertion_exceptions_enabled} {
        assertion_exceptions_enabled = true;
    }
    ~ScopedAssertExceptions() {
        assertion_exceptions_enabled = previous;
    }
    ScopedAssertExceptions(const ScopedAssertExceptions&) = delete;
    ScopedAssertExceptions& operator=(const ScopedAssertExceptions&) = delete;

private:
    bool previous;
};
} // namespace Common
