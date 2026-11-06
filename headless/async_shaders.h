// SPDX-License-Identifier: GPL-3.0-or-later
// Eden PS5 port: what the Vulkan pipeline cache did with the draws whose pipeline was still being
// built (PipelineCache::BuiltPipeline, tools/prepare-vulkan-port.py), in stderr.log.
//
// A draw counts once each time it finds its pipeline unbuilt, so one pipeline that takes a few
// frames to build counts for every draw of it in those frames:
//   skipped             asynchronous shaders on: not drawn this time
//   skipped_depth       on, and the draw has a depth buffer (upstream Eden's rule): not drawn
//   waited_small_target on, but it draws into a target under 640x360 (Eden PR #4639's rule): waited
//   waited_small_draw   on, but it has 6 indices or vertices or fewer (a full-screen pass): waited
//   waited_sync         asynchronous shaders off: waited
// Every 5 s (the frame report, graphics.cpp), when a count changed since the last line:
//   EDEN_ASYNC_SHADERS on=1 skipped=N skipped_depth=N waited_small_target=N waited_small_draw=N waited_sync=N
// The counts are the session's so far. Reset when a session sets the switch (main.cpp).
#pragma once
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>

namespace Eden::AsyncShaders {
enum Decision : unsigned {
    skipped,
    skipped_depth,
    waited_small_target,
    waited_small_draw,
    waited_sync,
    kDecisions,
};
inline constexpr const char* kNames[kDecisions] = {"skipped", "skipped_depth", "waited_small_target",
                                                   "waited_small_draw", "waited_sync"};

struct State {
    std::array<std::atomic<std::uint64_t>, kDecisions> draws{};
    std::atomic<bool> on{false};
    std::array<std::uint64_t, kDecisions> reported{};  // the reporting thread's alone
};
inline State& Get() {
    static State state;
    return state;
}

// The GPU thread, for each draw that found its pipeline unbuilt.
inline void Note(Decision decision) {
    Get().draws[decision].fetch_add(1, std::memory_order_relaxed);
}

// The session's start: its switch, and no draws yet.
inline void Reset(bool on) {
    State& state = Get();
    for (auto& count : state.draws) count.store(0, std::memory_order_relaxed);
    state.reported = {};
    state.on.store(on, std::memory_order_relaxed);
}

// One line when anything changed since the last one; returns whether it wrote it.
inline bool Report(std::FILE* out) {
    State& state = Get();
    std::array<std::uint64_t, kDecisions> now{};
    for (unsigned i = 0; i < kDecisions; ++i) now[i] = state.draws[i].load(std::memory_order_relaxed);
    if (now == state.reported) return false;
    state.reported = now;
    char line[256];
    int at = std::snprintf(line, sizeof(line), "EDEN_ASYNC_SHADERS on=%d",
                           state.on.load(std::memory_order_relaxed) ? 1 : 0);
    for (unsigned i = 0; i < kDecisions && at > 0 && at < int(sizeof(line)); ++i)
        at += std::snprintf(line + at, sizeof(line) - std::size_t(at), " %s=%llu", kNames[i],
                            static_cast<unsigned long long>(now[i]));
    std::fprintf(out, "%s\n", line);
    return true;
}
} // namespace Eden::AsyncShaders
