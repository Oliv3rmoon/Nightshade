// SPDX-FileCopyrightText: Copyright 2026 Oliv3rmoon
// SPDX-License-Identifier: GPL-3.0-or-later
// What the bridge's screen says (bridge.h): Nightshade has its own app now, what the bridge is doing,
// and what went wrong; drawn into a picture (picture.hpp) by the console (main.cpp) and on a PC
// (preview.cpp). Its text is the launcher's catalogs' (ui/lang/*.po, pe/core/strings.hpp).
#pragma once
#include <initializer_list>
#include <string>
#include <string_view>

#include "picture.hpp"
#include "update_check/github_release.h"
#include "update_check/self_update.h"

namespace Eden::Bridge {

enum class Step { opening, finding, installing, waiting, closing, failed };

struct View {
    Step step = Step::opening;
    std::string status;     // what is happening, or what went wrong
    std::string detail;     // a second line under it
    double progress = -1.0; // 0..1 of a download or an unpack; below 0: none
};

// The font the screen draws with: Say gives English for a translation it can't draw.
void UseFont(const pe::gfx::Font* font);
// The text in the player's language when the launcher's font can draw it, in English otherwise
// (the console's fonts are out of the bridge's reach: no_system_fonts.cpp).
std::string Say(const char* english);
std::string Say(const char* english, std::initializer_list<std::string_view> values);
// Why GitHub gave no release to install (the words of Settings > Apps, pe/ui/apps.cpp).
std::string Problem(github_release_reason reason, const std::string& zip);
// About how long a download has left (the update dialog's words, pe/ui/update.cpp).
std::string TimeLeft(const self_update_status& status);

// The whole screen; `time` in seconds moves what is waited for. Where its text ends (the buttons'
// hints are under kTextBottom).
float Draw(Picture& screen, const pe::gfx::Font& font, const View& view, double time);
inline constexpr float kTextBottom = 884.0f;

} // namespace Eden::Bridge
