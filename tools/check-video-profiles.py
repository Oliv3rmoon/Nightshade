#!/usr/bin/env python3
# Nightshade - Video profiles (headless/prosperoeden/pe/ui/video_profiles.hpp): the tiers are the
# settings that already exist, Recommended is a new install's, a game's own tiers and Custom read
# back from the values, and choosing Settings' again takes back only what a profile sets.
# SPDX-License-Identifier: GPL-3.0-or-later
import subprocess
import tempfile
from pathlib import Path

root = Path(__file__).resolve().parents[1]
launcher = root / 'headless/prosperoeden'
store = (root / 'headless/settings_store.h').read_text()
profiles = (launcher / 'pe/ui/video_profiles.hpp').read_text()
# The indexes the tiers use are settings_store.h's keys in that order.
assert 'kResolutionKeys[] = {"0.5x", "0.75x", "1x", "1.5x", "2x", "3x", "4x"}' in store
assert 'kUpscalingFilterKeys[] = {"bilinear", "fsr", "bicubic", "nearest"}' in store
assert 'kMemoryLayoutKeys[] = {"4Gb", "6Gb", "8Gb"}' in store
assert '0x0100F2C0115B6000' in (root / 'headless/memory_layout_guard.h').read_text()
assert '0x0100F2C0115B6000ULL' in profiles

with tempfile.TemporaryDirectory() as directory:
    source = Path(directory) / 'profiles.cpp'
    binary = Path(directory) / 'profiles'
    source.write_text(r'''#include "pe/ui/video_profiles.hpp"
#include <cassert>
#include <cstdio>
using namespace pe::ui;
int main() {
    constexpr std::uint64_t totk = 0x0100F2C0115B6000ULL, other = 0x0100000000010000ULL;
    // A new install's settings are Recommended; every tier reads back as itself.
    Preferences prefs;
    assert(profile_of(prefs) == kRecommendedProfile);
    for (int tier = 0; tier < kVideoProfiles; ++tier) {
        Preferences p = prefs;
        p.volume = 20; p.renderer = 0; p.refresh = 1; p.hud = false;  // not a profile's: kept
        apply_profile(p, tier);
        assert(profile_of(p) == tier && p.volume == 20 && p.renderer == 0 && p.refresh == 1 && !p.hud);
    }
    // A value of the profile's changed by hand: Custom. Frame generation or post-processing too.
    Preferences high = prefs;
    apply_profile(high, 2);
    assert(high.resolution == 3 && high.filter == 2 && high.astc == 0 && !high.fast_gpu);
    high.resolution = 5;
    assert(profile_of(high) == kCustomProfile);
    apply_profile(high, 2);
    high.frame_gen = true;
    assert(profile_of(high) == kCustomProfile);
    apply_profile(high, 2);
    high.post_processing = 1;
    assert(profile_of(high) == kCustomProfile);
    Preferences minimum = prefs;
    apply_profile(minimum, 0);
    assert(minimum.resolution == 1 && minimum.filter == 1 && minimum.fast_gpu && minimum.async_shaders);
    // Every tier keeps ASTC compressed: decoded it takes four times the graphics memory.
    for (const VideoProfile &tier : kGeneralProfiles) assert(tier.astc == 0 && tier.memory_layout < 0);

    // A game: it follows Settings until it has a profile's values of its own.
    GameSettings game;
    game.volume = 50; game.renderer = 0; game.speed_limit = 2;
    assert(game_follows_profile(game, other) && game_profile_of(game, prefs, other) == -1);
    GameSettings ultra = with_profile(game, other, 3);
    assert(ultra.resolution == 4 && ultra.filter == 2 && ultra.astc == 0 && ultra.frame_gen == 0 &&
           ultra.post_processing == 0 && ultra.performance[kFastGpuSwitch] == 0 &&
           ultra.performance[kAsyncShadersSwitch] == 0 && ultra.memory_layout == -1);
    assert(ultra.volume == 50 && ultra.renderer == 0 && ultra.speed_limit == 2);  // the rest stays
    assert(game_profile_of(ultra, prefs, other) == 3);
    // Whatever Settings has, the game's own tier is read from its own values.
    Preferences custom = prefs;
    custom.resolution = 6;
    assert(game_profile_of(ultra, custom, other) == 3);
    ultra.filter = 3;
    assert(game_profile_of(ultra, prefs, other) == kCustomProfile);
    // Settings' again: only what a profile sets is taken back.
    const GameSettings back = with_profile(ultra, other, -1);
    assert(game_follows_profile(back, other) && back.volume == 50 && back.renderer == 0 && back.speed_limit == 2);
    // A game with one value of the profile's of its own and the rest from Settings: its tier is what
    // it runs with.
    GameSettings one;
    one.resolution = 3;
    one.filter = 2;
    assert(game_profile_of(one, prefs, other) == 2);

    // Tears of the Kingdom: its own tiers keep the console's 4 GiB in every one.
    const TitleProfiles *own = title_profiles(totk);
    assert(own != nullptr && title_profiles(other) == nullptr && title_profiles(0) == nullptr);
    for (const VideoProfile &tier : own->tiers) assert(tier.memory_layout == 0 && tier.astc == 0);
    GameSettings zelda;
    const GameSettings recommended = with_profile(zelda, totk, kRecommendedProfile);
    assert(recommended.memory_layout == 0 && recommended.resolution == 2 && recommended.astc == 0);
    assert(game_profile_of(recommended, prefs, totk) == kRecommendedProfile);
    // Its memory changed by hand (a mod that needs 8 GiB): Custom.
    GameSettings modded = recommended;
    modded.memory_layout = 2;
    assert(game_profile_of(modded, prefs, totk) == kCustomProfile);
    // Settings' again takes its memory back too.
    assert(with_profile(recommended, totk, -1).memory_layout == -1);
    assert(game_follows_profile(with_profile(recommended, totk, -1), totk));
    std::puts("video profiles PASS");
}
''')
    subprocess.run(['c++', '-std=c++20', '-I' + str(launcher), str(source), str(launcher / 'pe/core/strings.cpp'),
                    '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
