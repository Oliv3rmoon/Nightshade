// Nightshade - The apps Settings > Apps offers: Nightshade's own release lines of the projects
// they are based on, each with that project's title ID.
// SPDX-FileCopyrightText: Copyright 2026 Oliv3rmoon
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The list is data: pe/ui/apps.cpp shows it, headless/apps.h finds each app on the console, asks
// GitHub for its newest release, installs it with the self-update helper and opens it with the
// launch payload. An app's release is the ZIP <title ID>.zip, whose top folder <title ID>/ holds
// eboot.bin and sce_sys/param.json, as the projects' own releases are. Until an app's Nightshade
// repository has a release, the project it is based on is asked instead (fallback), and the list
// says so.

#pragma once

#include "pe/core/strings.hpp"

#include <cstdint>

namespace pe::ui
{

enum class AppIcon : std::uint8_t
{
    tv,
    stream,
    disc,
};

struct AppListing
{
    const char *name;       // as the list shows it (a name: not translated)
    const char *title;      // its title ID, the project's own
    const char *repository; // its Nightshade release line on GitHub
    const char *fallback;   // the project it is based on: asked while the line has no release
    const char *asset;      // the release's file
    const char *about;      // one line about it (English, marked for translation)
    AppIcon icon;
};

inline constexpr AppListing kApps[] = {
    {"Nightshade TV", "PPSA99003", "Oliv3rmoon/Nightshade-TV", "blackbearreloaded/ProsperoTV", "PPSA99003.zip",
     TR("Live TV, films and series from your IPTV provider."), AppIcon::tv},
    {"Nightshade Stream", "PPSA99002", "Oliv3rmoon/Nightshade-Stream", "blackbearreloaded/ProsperoLight",
     "PPSA99002.zip", TR("Play games streamed from your PC with Moonlight."), AppIcon::stream},
    {"Nightshade Station", "PPSA98510", "Oliv3rmoon/Nightshade-Station", "darkxex/SwanStationPS5", "PPSA98510.zip",
     TR("Play PS1 games with the SwanStation emulator."), AppIcon::disc},
};
inline constexpr int kAppCount = static_cast<int>(sizeof(kApps) / sizeof(kApps[0]));

} // namespace pe::ui
