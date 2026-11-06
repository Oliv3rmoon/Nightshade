// SPDX-License-Identifier: GPL-3.0-or-later
// Settings a few games start with, where the game has no value of its own.
//
// Asynchronous shaders. With them off, a draw whose pipeline is still being built waits for it: the
// frame is held until the driver has compiled it. Tears of the Kingdom builds new pipelines for
// minutes after a save is loaded and wherever it streams new scenery in, up to 96 in 30 seconds on
// a tester's console (v1.010.011, 800 and 1,961 in two sessions after its shader cache was built),
// and those were the frame drops testers saw. With them on, such a draw is skipped until its
// pipeline is ready (PipelineCache::BuiltPipeline, tools/prepare-vulkan-port.py), except where a
// skipped draw would be missed for longer than a frame. So a listed game has them on unless its
// own Performance settings (Library > Game settings > Performance) say otherwise: the game's own
// switch, on or off, is a player's choice and always wins. Settings > Performance's switch is not
// asked: it is written for every game whenever that page is saved, so it cannot tell a choice from
// the default. A safe start (Safe Launch) still turns them off.
#pragma once
#include <cstdint>

namespace Eden::TitleDefaults {
inline constexpr std::uint64_t kAsyncShaderTitles[] = {
    0x0100F2C0115B6000, // The Legend of Zelda: Tears of the Kingdom
};

inline bool AsyncShadersListed(std::uint64_t title_id) {
    for (const std::uint64_t title : kAsyncShaderTitles)
        if (title_id != 0 && title == title_id) return true;
    return false;
}

// What a session uses. `settings`: Settings' value with the game's own over it (LoadPerformance);
// `game_own`: the game's own switch alone, -1 when it has none (LoadGameSettings). `by_title`: the
// list decided it.
struct AsyncShaders {
    bool on = false;
    bool by_title = false;
};
inline AsyncShaders AsyncShadersFor(std::uint64_t title_id, bool settings, int game_own) {
    if (game_own >= 0) return {game_own == 1, false};
    if (AsyncShadersListed(title_id)) return {true, true};
    return {settings, false};
}
} // namespace Eden::TitleDefaults
