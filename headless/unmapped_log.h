// SPDX-FileCopyrightText: Copyright 2026 Oliv3rmoon
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// Eden logs every guest access to memory that is not mapped. A game that walks a null pointer
// over and over (Ori and the Will of the Wisps 1.0.0, started without its update) made a line of
// eden_log.txt for each: 958 000 lines in 9 s, the 100 MiB log full and its start gone. Each
// session logs its first 64, then the 128th, 256th and every power of two after, with their count.
#include <atomic>
#include <cstdint>

namespace Eden::UnmappedLog {

inline std::atomic<std::uint64_t>& Count() {
    static std::atomic<std::uint64_t> count{0};
    return count;
}

// Whether this access is logged; *seen is its number in the session (from 1).
inline bool Wanted(std::uint64_t* seen) {
    const std::uint64_t n = Count().fetch_add(1, std::memory_order_relaxed) + 1;
    *seen = n;
    return n <= 64 || (n & (n - 1)) == 0;
}

// A new session counts from the start (main.cpp).
inline void Reset() { Count().store(0, std::memory_order_relaxed); }

} // namespace Eden::UnmappedLog
