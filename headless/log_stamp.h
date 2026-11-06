// SPDX-License-Identifier: GPL-3.0-or-later
// Wall-clock marks in the logs. A copy of the logs that was resumed after it broke off can hold
// the end of one session's file run on into another's. The first line of each file
// (EDEN_LOG_START, main.cpp) says which start of the app wrote it, and the lines printed every
// few seconds (EDEN_PROGRESS, EDEN_MEMORY) carry the time, so the seam shows. In UTC: the
// console's own time zone is not known here.
#pragma once
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <random>
#include <time.h>

namespace Eden::LogStamp {
inline constexpr std::size_t kUtcSize = 24;

// Now, as 2026-11-06T08:15:42Z; "?" when the clock cannot be read.
inline void Utc(char (&text)[kUtcSize]) noexcept {
    const std::time_t now = std::time(nullptr);
    std::tm utc{};
    if (now == static_cast<std::time_t>(-1) || gmtime_r(&now, &utc) == nullptr ||
        std::strftime(text, sizeof(text), "%Y-%m-%dT%H:%M:%SZ", &utc) == 0)
        std::snprintf(text, sizeof(text), "?");
}

// 16 hex digits, drawn once for each start of the app.
inline const char* SessionId() {
    static const auto id = [] {
        // The clocks alone, should the system's random numbers not be there.
        std::uint64_t value =
            (static_cast<std::uint64_t>(std::chrono::system_clock::now().time_since_epoch().count()) *
             0x9E3779B97F4A7C15ull) ^
            static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
        try {
            std::random_device device;
            value ^= (std::uint64_t{device()} << 32) | device();
        } catch (...) {
        }
        std::array<char, 17> text{};
        std::snprintf(text.data(), text.size(), "%016llx", static_cast<unsigned long long>(value));
        return text;
    }();
    return id.data();
}
}  // namespace Eden::LogStamp
