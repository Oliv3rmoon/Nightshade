// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// The last service calls games made, kept so that a game that stops itself (svcBreak) says what it
// had just asked for. A game aborts on a reply it cannot use (2010-0212 is a CMIF reply without a
// valid header): each entry records the reply as it is in the game's memory right after Eden
// wrote it, so a bad reply Eden wrote can be told apart from a good one that changed afterwards.
// Recorded in every build (headless/CMakeLists.txt, "The last service calls"); printed with
// EDEN_IPC_RING lines in Eden's log when a game breaks.
#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string_view>

namespace Eden::IpcRing {

struct Entry {
    std::uint64_t when_ns;    // steady clock
    std::uint64_t thread;     // the guest thread that called
    std::uint32_t sequence;   // calls recorded before this one
    std::uint32_t command;
    std::uint32_t type;       // IPC::CommandType
    std::int32_t magic_at;    // word of the CMIF reply header ("SFCO") in the reply; -1: none
    std::uint32_t result;     // the result after that header
    std::uint32_t header[2];  // the reply's first two words (HIPC header)
    char service[32];
};

inline constexpr std::size_t kEntries = 64;
inline constexpr std::uint32_t kReplyMagic = 0x4F434653; // "SFCO"

// Function-local statics: inline variables of one header do not initialize in order on the PS5.
inline std::mutex& Lock() {
    static std::mutex lock;
    return lock;
}
inline std::array<Entry, kEntries>& Entries() {
    static std::array<Entry, kEntries> entries{};
    return entries;
}
inline std::uint32_t& Count() {
    static std::uint32_t count = 0;
    return count;
}

// reply: the command buffer as read back from the calling thread's local storage.
inline void Record(std::string_view service, std::uint64_t thread, std::uint32_t command, std::uint32_t type,
                   const std::uint32_t* reply, std::size_t words) {
    Entry entry{};
    entry.when_ns = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
    entry.thread = thread;
    entry.command = command;
    entry.type = type;
    entry.magic_at = -1;
    if (words > 0) entry.header[0] = reply[0];
    if (words > 1) entry.header[1] = reply[1];
    for (std::size_t i = 0; i + 2 < words; ++i) {
        if (reply[i] == kReplyMagic) {
            entry.magic_at = static_cast<std::int32_t>(i);
            entry.result = reply[i + 2];
            break;
        }
    }
    const std::size_t length = std::min(service.size(), sizeof(entry.service) - 1);
    std::memcpy(entry.service, service.data(), length);
    const std::lock_guard guard{Lock()};
    entry.sequence = Count();
    Entries()[Count() % kEntries] = entry;
    ++Count();
}

// The recorded calls, oldest first, one line each; those of breaking_thread are marked.
// ms_before_break is each call's age when the dump began: a call recorded after the game had
// already moved on shows a smaller age than the calls it made after it.
template <typename Line>
inline void Dump(std::uint64_t breaking_thread, Line&& line) {
    std::array<Entry, kEntries> entries;
    std::uint32_t count;
    const auto dump_ns = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
    {
        const std::lock_guard guard{Lock()};
        entries = Entries();
        count = Count();
    }
    const std::uint32_t first = count > kEntries ? count - static_cast<std::uint32_t>(kEntries) : 0;
    const std::uint64_t last_ns = count ? entries[(count - 1) % kEntries].when_ns : 0;
    for (std::uint32_t n = first; n < count; ++n) {
        const Entry& entry = entries[n % kEntries];
        char text[256];
        std::snprintf(text, sizeof(text),
                      "EDEN_IPC_RING #%u %s thread=%llu ms_before_last=%.1f service=%s command=%u type=%u "
                      "sfco_at=%d result=0x%x header=%08x:%08x ms_before_break=%.1f",
                      entry.sequence, entry.thread == breaking_thread ? "BREAKING" : "other",
                      static_cast<unsigned long long>(entry.thread),
                      static_cast<double>(last_ns - entry.when_ns) / 1e6, entry.service, entry.command,
                      entry.type, entry.magic_at, entry.result, entry.header[0], entry.header[1],
                      static_cast<double>(static_cast<std::int64_t>(dump_ns - entry.when_ns)) / 1e6);
        line(text);
    }
}

} // namespace Eden::IpcRing
