// Nightshade - Video profiles: Minimum, Recommended, High and Ultra, for every game (Settings >
// Video > Profile) and for one game (its own Video > Profile, and Quick settings).
// SPDX-License-Identifier: GPL-3.0-or-later
//
// A profile is a set of the settings that already exist, written as the player would set them by
// hand: the internal resolution, the upscaling filter, how ASTC textures are held, the GPU
// accuracy, asynchronous shaders, frame generation and post-processing (both off), and for a game
// that needs it the memory its console is told it has. Nothing is stored as "the profile": which
// profile a game or Settings has is read from those values, so a value changed by hand afterwards
// shows as Custom, and settings saved before profiles existed are left exactly as they are (they
// read as whichever profile they happen to match, or Custom).
//
// Recommended is what a new install has. A few games have their own values for the four tiers
// (kTitleProfiles): what Nightshade learned about them, the same tiers otherwise.
#pragma once

#include "pe/core/strings.hpp"
#include "pe/ui/services.hpp"

#include <array>
#include <cstdint>

namespace pe::ui
{

constexpr int kVideoProfiles = 4;              // Minimum, Recommended, High, Ultra
constexpr int kRecommendedProfile = 1;
constexpr int kCustomProfile = kVideoProfiles; // the values match none of them: changed by hand
inline constexpr const char *kProfileNames[kVideoProfiles + 1] = {TR("Minimum"), TR("Recommended"), TR("High"),
                                                              TR("Ultra"), TR("Custom")};

// One tier's values. Resolution and filter are indexes into settings_store.h's kResolutionKeys
// ("0.5x", "0.75x", "1x", "1.5x", "2x", "3x", "4x") and kUpscalingFilterKeys (Bilinear, AMD FSR,
// Bicubic, Nearest); astc into kAstcTextureKeys (0 compressed as BC3, 1 full quality);
// memory_layout into kMemoryLayoutKeys (4, 6, 8 GiB), -1 where the profile leaves it alone.
struct VideoProfile
{
    int resolution = 2;
    int filter = 0;
    int astc = 0;
    bool fast_gpu = false;      // Performance > Faster GPU emulation (Eden's lowest GPU accuracy)
    bool async_shaders = false; // Performance > Asynchronous shaders (Vulkan)
    int memory_layout = -1;
};

// The general tiers. Minimum draws three quarters of the game's pixels and lets AMD FSR scale them
// up, with the fastest GPU accuracy and no pause for a new shader. Recommended is a new install's
// settings: the game's own resolution, Bilinear, exact. High and Ultra draw 1.5 and 2 times the
// game's pixels each way (2.25 and 4 times as many) with Bicubic, which keeps the picture sharp
// when the output scales it down or up. ASTC textures stay compressed in every tier: held decoded
// they take four times the graphics memory, and above 1x that is what a session runs out of
// (headless/main.cpp). Frame generation and post-processing are a player's own taste: every tier
// switches them off, and turning one on afterwards makes the profile Custom.
inline constexpr std::array<VideoProfile, kVideoProfiles> kGeneralProfiles{{
    {1, 1, 0, true, true, -1},   // Minimum: 0.75x, AMD FSR
    {2, 0, 0, false, false, -1}, // Recommended: 1x, Bilinear
    {3, 2, 0, false, false, -1}, // High: 1.5x, Bicubic
    {4, 2, 0, false, false, -1}, // Ultra: 2x, Bicubic
}};

// Games with values of their own for the four tiers.
struct TitleProfiles
{
    std::uint64_t title_id;
    const char *why; // shown under the tier's values (translated)
    std::array<VideoProfile, kVideoProfiles> tiers;
};
inline constexpr TitleProfiles kTitleProfiles[] = {
    // The Legend of Zelda: Tears of the Kingdom. With the 8 GiB layout and no mods it wrote gigabytes
    // of new memory 19 to 74 minutes in and stopped (v1.010.005, headless/memory_layout_guard.h); its
    // texture arrays take 816 MiB decoded and 204 MiB as BC3. Every tier keeps the console's 4 GiB
    // and BC3, whatever Settings > Performance > Memory says.
    {0x0100F2C0115B6000ULL,
     TR("Tears of the Kingdom: 4 GiB and compressed ASTC in every tier, what it needs to play for hours."),
     {{
         {1, 1, 0, true, true, 0},
         {2, 0, 0, false, false, 0},
         {3, 2, 0, false, false, 0},
         {4, 2, 0, false, false, 0},
     }}},
};

// The tiers a game has: its own, or the general ones.
inline const TitleProfiles *title_profiles(std::uint64_t title_id)
{
    for (const TitleProfiles &entry : kTitleProfiles)
        if (title_id != 0 && entry.title_id == title_id)
            return &entry;
    return nullptr;
}
inline VideoProfile profile_for(std::uint64_t title_id, int tier)
{
    const int at = tier < 0 ? kRecommendedProfile : tier >= kVideoProfiles ? kVideoProfiles - 1 : tier;
    const TitleProfiles *own = title_profiles(title_id);
    return own != nullptr ? own->tiers[static_cast<std::size_t>(at)] : kGeneralProfiles[static_cast<std::size_t>(at)];
}

// ---- Settings > Video > Profile: every game ----

inline void apply_profile(Preferences &prefs, int tier)
{
    const VideoProfile v = profile_for(0, tier);
    prefs.resolution = v.resolution;
    prefs.filter = v.filter;
    prefs.astc = v.astc;
    prefs.fast_gpu = v.fast_gpu;
    prefs.async_shaders = v.async_shaders;
    prefs.frame_gen = false;
    prefs.post_processing = 0;
}

inline bool profile_matches(const Preferences &prefs, const VideoProfile &v)
{
    return prefs.resolution == v.resolution && prefs.filter == v.filter && prefs.astc == v.astc &&
           prefs.fast_gpu == v.fast_gpu && prefs.async_shaders == v.async_shaders && !prefs.frame_gen &&
           prefs.post_processing == 0 && (v.memory_layout < 0 || prefs.memory_layout == v.memory_layout);
}

// The tier Settings has, or kCustomProfile.
inline int profile_of(const Preferences &prefs)
{
    for (int tier = 0; tier < kVideoProfiles; ++tier)
        if (profile_matches(prefs, profile_for(0, tier)))
            return tier;
    return kCustomProfile;
}

// ---- a game's own Video > Profile ----

// The Performance switches a profile sets, in GameSettings::performance's order (services.hpp).
constexpr std::size_t kAsyncShadersSwitch = 1;
constexpr std::size_t kFastGpuSwitch = 2;

// Whether the game has none of a profile's values of its own: it runs with Settings' (and its
// profile).
inline bool game_follows_profile(const GameSettings &game, std::uint64_t title_id)
{
    const TitleProfiles *own = title_profiles(title_id);
    const bool memory = own != nullptr && own->tiers[0].memory_layout >= 0;
    return game.resolution < 0 && game.filter < 0 && game.astc < 0 && game.frame_gen < 0 &&
           game.post_processing < 0 && game.performance[kAsyncShadersSwitch] < 0 &&
           game.performance[kFastGpuSwitch] < 0 && (!memory || game.memory_layout < 0);
}

// The game's settings with a tier's values of its own; tier -1: back to Settings' for all of them
// (the game's other settings stay as they are).
inline GameSettings with_profile(GameSettings game, std::uint64_t title_id, int tier)
{
    if (tier < 0 || tier >= kVideoProfiles)
    {
        game.resolution = game.filter = game.astc = game.frame_gen = game.post_processing = -1;
        game.performance[kAsyncShadersSwitch] = game.performance[kFastGpuSwitch] = -1;
        const TitleProfiles *own = title_profiles(title_id);
        if (own != nullptr && own->tiers[0].memory_layout >= 0)
            game.memory_layout = -1;
        return game;
    }
    const VideoProfile v = profile_for(title_id, tier);
    game.resolution = v.resolution;
    game.filter = v.filter;
    game.astc = v.astc;
    game.frame_gen = 0;
    game.post_processing = 0;
    game.performance[kAsyncShadersSwitch] = v.async_shaders ? 1 : 0;
    game.performance[kFastGpuSwitch] = v.fast_gpu ? 1 : 0;
    if (v.memory_layout >= 0)
        game.memory_layout = v.memory_layout;
    return game;
}

// What the game runs with: its own value where it has one, Settings' otherwise.
inline Preferences game_effective(const GameSettings &game, Preferences prefs)
{
    if (game.resolution >= 0)
        prefs.resolution = game.resolution;
    if (game.filter >= 0)
        prefs.filter = game.filter;
    if (game.astc >= 0)
        prefs.astc = game.astc;
    if (game.frame_gen >= 0)
        prefs.frame_gen = game.frame_gen == 1;
    if (game.post_processing >= 0)
        prefs.post_processing = game.post_processing;
    if (game.performance[kAsyncShadersSwitch] >= 0)
        prefs.async_shaders = game.performance[kAsyncShadersSwitch] == 1;
    if (game.performance[kFastGpuSwitch] >= 0)
        prefs.fast_gpu = game.performance[kFastGpuSwitch] == 1;
    if (game.memory_layout >= 0)
        prefs.memory_layout = game.memory_layout;
    return prefs;
}

// The tier the game has of its own; -1 while it follows Settings; kCustomProfile when its values
// match none of its tiers.
inline int game_profile_of(const GameSettings &game, const Preferences &prefs, std::uint64_t title_id)
{
    if (game_follows_profile(game, title_id))
        return -1;
    const Preferences shown = game_effective(game, prefs);
    for (int tier = 0; tier < kVideoProfiles; ++tier)
        if (profile_matches(shown, profile_for(title_id, tier)))
            return tier;
    return kCustomProfile;
}

} // namespace pe::ui
