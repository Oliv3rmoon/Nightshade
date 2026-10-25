// SPDX-FileCopyrightText: Copyright 2026 Oliv3rmoon
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// A start that shows no picture for a long time says what the game was doing. The loading
// screen's last step, starting the game, lasts until the game hands its first frame to the
// display; Ori and the Will of the Wisps sat there for minutes on one console, with nothing in the
// logs to tell slow from stuck. At 45, 90 and 180 s of that step, one line each: whether the GPU
// had game work waiting, how many service calls the game had made and how long ago the last one
// was, and at 45 and 180 s the last 64 calls (ipc_ring.h). A game still calling services is
// working or polling for something; one that stopped calling waits on something.
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <iterator>
#include <mutex>
#include "ipc_ring.h"

namespace Eden::StartWait {

inline constexpr int kSeconds[] = {45, 90, 180};

// The calls recorded so far and the steady-clock time of the last one, in nanoseconds (0: none).
inline std::uint32_t Calls(std::uint64_t* last_ns) {
    const std::lock_guard guard{IpcRing::Lock()};
    const std::uint32_t count = IpcRing::Count();
    *last_ns = count ? IpcRing::Entries()[(count - 1) % IpcRing::kEntries].when_ns : 0;
    return count;
}

// One start's reports. Check() is called with every loading-screen picture: starting is whether
// the start is at its last step, since_ms when that step began and now_ms the time now (both
// Loading::Now()), idle whether the GPU thread had no game commands waiting. line(text) is given
// each line, without its newline.
class Watch {
public:
    template <typename Line>
    void Check(bool starting, std::int64_t since_ms, std::int64_t now_ms, bool idle, Line&& line) {
        if (!starting) return;
        if (since_ms != since_) {  // another start, or this one at its last step again
            since_ = since_ms;
            reported_ = 0;
        }
        for (std::size_t i = 0; i < std::size(kSeconds); ++i) {
            if (reported_ & (1u << i) || now_ms - since_ms < kSeconds[i] * std::int64_t{1000}) continue;
            reported_ |= 1u << i;
            Report(kSeconds[i], idle, i != 1, line);
        }
    }

private:
    template <typename Line>
    static void Report(int seconds, bool idle, bool with_calls, Line& line) {
        std::uint64_t last_ns = 0;
        const std::uint32_t calls = Calls(&last_ns);
        const auto now_ns = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
        char text[256];
        if (calls)
            std::snprintf(text, sizeof(text),
                          "EDEN_START_WAIT %d s into starting the game and no picture yet: gpu=%s, %u service calls, "
                          "the last %.1f s ago",
                          seconds, idle ? "idle" : "busy", calls,
                          now_ns > last_ns ? static_cast<double>(now_ns - last_ns) / 1e9 : 0.0);
        else
            std::snprintf(text, sizeof(text),
                          "EDEN_START_WAIT %d s into starting the game and no picture yet: gpu=%s, no service calls",
                          seconds, idle ? "idle" : "busy");
        line(text);
        if (with_calls && calls) IpcRing::Dump(0, line);
    }

    std::int64_t since_ = -1;
    unsigned reported_ = 0;
};

} // namespace Eden::StartWait
