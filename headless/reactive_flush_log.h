// SPDX-FileCopyrightText: Copyright 2026 Oliv3rmoon
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// With reactive flushing on, a guest read of memory the GPU wrote first copies what the GPU caches
// hold for it back into the guest's memory (GPU::Impl::OnCPURead, video_core/gpu.cpp). If the caches
// still count memory as the GPU's after the game has written something else there, the copy puts
// the old picture back over the game's data. eden-nx2 counted these copies once for the whole process
// and logged one in 64. Final Fantasy Resonance on v1.012.001 (Oliv3rmoon/Nightshade#1) made its first
// copy of a 16-minute session as a cutscene movie started, half a millisecond before the game first
// read through a null pointer; four minutes later the game jumped to address 0.
//
// Each session now logs its first 64 copies, then each power of two with its count (as
// unmapped_log.h), with the bytes the read sees before and after the copy and the area copied; the
// texture cache adds each image it copies back (headless/reactive_flush_image.inc). The first 16 of a
// session also go to stderr.log, which most reports include.
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>

namespace Eden::ReactiveFlushLog {

inline std::atomic<std::uint64_t>& Count() {
    static std::atomic<std::uint64_t> count{0};
    return count;
}

// Whether this copy is logged; *seen is its number in the session (from 1).
inline bool Wanted(std::uint64_t* seen) {
    const std::uint64_t n = Count().fetch_add(1, std::memory_order_relaxed) + 1;
    *seen = n;
    return n <= 64 || (n & (n - 1)) == 0;
}

// The first copies of a session also go to stderr.log.
inline bool ToStderr(std::uint64_t n) { return n <= 16; }

// A new session counts from the start (main.cpp).
inline void Reset() { Count().store(0, std::memory_order_relaxed); }

// The logged copy the GPU thread is making now (0: none), for the texture cache's image lines. Set
// and cleared on the GPU thread, around the flush it runs for the read.
inline std::uint64_t& Active() {
    thread_local std::uint64_t active = 0;
    return active;
}

// The bytes at the read, up to the end of its 4 KiB page.
inline constexpr std::size_t kPeekBytes = 32;
struct Peek {
    std::uint8_t bytes[kPeekBytes]{};
    std::size_t size = 0;
};
inline std::size_t PeekSize(std::uint64_t address) {
    const std::size_t to_page_end = 0x1000 - static_cast<std::size_t>(address & 0xfff);
    return to_page_end < kPeekBytes ? to_page_end : kPeekBytes;
}

inline std::string Hex(const Peek& peek) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string text;
    text.reserve(peek.size * 2);
    for (std::size_t i = 0; i < peek.size; ++i) {
        text += digits[peek.bytes[i] >> 4];
        text += digits[peek.bytes[i] & 15];
    }
    return text;
}

inline bool Same(const Peek& a, const Peek& b) {
    if (a.size != b.size) return false;
    for (std::size_t i = 0; i < a.size; ++i)
        if (a.bytes[i] != b.bytes[i]) return false;
    return true;
}

// Who read: Eden numbers the four guest cores' host threads 0-3.
inline std::string Reader(std::size_t host_thread) {
    return host_thread < 4 ? "guest core " + std::to_string(host_thread) : "a host thread";
}

// One copy, as eden_log.txt and stderr.log show it.
inline std::string Line(std::uint64_t n, std::uint64_t read_address, std::uint64_t read_size,
                        std::uint64_t start, std::uint64_t end, std::size_t host_thread,
                        const Peek& before, const Peek& after) {
    char head[256];
    std::snprintf(head, sizeof(head),
                  "EDEN_REACTIVE_FLUSH #%llu read %#llx+%llu on %s copied [%#llx, %#llx) (%llu KiB)",
                  static_cast<unsigned long long>(n), static_cast<unsigned long long>(read_address),
                  static_cast<unsigned long long>(read_size), Reader(host_thread).c_str(),
                  static_cast<unsigned long long>(start), static_cast<unsigned long long>(end),
                  static_cast<unsigned long long>((end - start) / 1024));
    return std::string{head} + ": at the read " + Hex(before) + " before, " + Hex(after) + " after" +
           (Same(before, after) ? " (unchanged)" : " (changed)");
}

} // namespace Eden::ReactiveFlushLog
