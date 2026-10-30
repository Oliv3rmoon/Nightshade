// SPDX-License-Identifier: GPL-3.0-or-later
// Games that stop when they are given more memory than their console has.
//
// Settings > Performance > Memory tells a game it has 6 or 8 GiB, the developer units' memory, for
// mods that need more than the console's 4 GiB. A game told so is handed most of it: Eden's 8 GiB
// layout gives the game 6.4 GiB (the console 3.2 GiB, a developer unit 4.8 GiB), and a game that sizes
// itself by what it is given uses it.
//
// Tears of the Kingdom does, and does not survive it. On a tester's console, with 8 GiB and no mods,
// the four sessions that ended (v1.010.002 and .003, 19 to 48 minutes in) all ended the same way:
// after holding about 2.7 GB of its memory for most of the session, the game wrote 0.4 to 3.4 GB of
// memory it had never written before within seconds, mostly content that repeats, from its own
// code (in the three where the console's memory ran out first, the write that found none came from
// the same place, main+0x6E1404, every time), until either the console's memory ran out or, with
// 6.1 GB of its 6.4 GiB written, one of the game's threads jumped to an address where nothing is
// mapped. The console's 4 GiB is what the game was made for: a game here that runs without mods is
// given that, whatever Memory says, and the launch log says so. With a mod on, the mod may need the
// memory, and the layout chosen stands.
#pragma once
#include <cstdint>

namespace Eden::MemoryLayoutGuard {
inline constexpr std::uint64_t kTitles[] = {
    0x0100F2C0115B6000, // The Legend of Zelda: Tears of the Kingdom
};

inline bool Listed(std::uint64_t title_id) {
    for (const std::uint64_t title : kTitles)
        if (title == title_id) return true;
    return false;
}

// The layout a session gets (an index into kMemoryLayoutKeys, 0 being the console's own 4 GiB):
// `chosen`, except for a listed game without a mod on, which gets 0. `guard_off`: dev-settings
// layout_guard=off, to try such a game with the larger memory anyway.
inline int Layout(std::uint64_t title_id, int chosen, bool mods_on, bool guard_off = false) {
    if (chosen <= 0 || guard_off || mods_on || !Listed(title_id)) return chosen;
    return 0;
}
} // namespace Eden::MemoryLayoutGuard
