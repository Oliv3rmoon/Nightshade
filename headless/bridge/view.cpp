// SPDX-FileCopyrightText: Copyright 2026 Oliv3rmoon
// SPDX-License-Identifier: GPL-3.0-or-later
#include "view.hpp"

#include <algorithm>
#include <cmath>

#include "pe/core/strings.hpp"

namespace Eden::Bridge {
namespace {

const pe::gfx::Font* shown_font = nullptr;

} // namespace

void UseFont(const pe::gfx::Font* font) { shown_font = font; }

// The text in the player's language when the launcher's font can draw it, in English otherwise
// (the console's fonts are out of the bridge's reach: no_system_fonts.cpp).
std::string Say(const char* english) {
    const char* translated = pe::tr(english);
    return shown_font != nullptr && !shown_font->can_draw(translated) ? std::string{english} : std::string{translated};
}
std::string Say(const char* english, std::initializer_list<std::string_view> values) {
    return pe::fill(Say(english), values);
}

// Why GitHub gave no release to install (the words of Settings > Apps, pe/ui/apps.cpp).
std::string Problem(github_release_reason reason, const std::string& zip) {
    switch (reason) {
    case GITHUB_RELEASE_NONE: return Say("No release yet.");
    case GITHUB_RELEASE_NETWORK:
    case GITHUB_RELEASE_HTTP_STATUS:
    case GITHUB_RELEASE_SUMS_UNREADABLE: return Say("GitHub could not be reached.");
    case GITHUB_RELEASE_RATE_LIMITED: return Say("GitHub is limiting requests. Try again in an hour.");
    case GITHUB_RELEASE_NO_ZIP: return Say("The release has no {0}.", {zip});
    case GITHUB_RELEASE_NO_HASH:
    case GITHUB_RELEASE_SUMS_NO_LINE: return Say("The release gives no SHA-256 for {0}, so it is not offered.", {zip});
    case GITHUB_RELEASE_HASHES_DIFFER:
    case GITHUB_RELEASE_SUMS_MALFORMED:
        return Say("The release's SHA-256 for {0} can't be trusted, so it is not offered.", {zip});
    default: return Say("GitHub's answer could not be read.");
    }
}

// About how long a download has left (the update dialog's words, pe/ui/update.cpp).
std::string TimeLeft(const self_update_status& status) {
    if (status.rate <= 0.0 || status.total <= status.done) return {};
    const double seconds = static_cast<double>(status.total - status.done) / status.rate;
    if (seconds < 60.0) return Say("About {0} s left", {std::to_string(std::max(1, static_cast<int>(std::ceil(seconds))))});
    return Say("About {0} min left", {std::to_string(static_cast<int>(std::ceil(seconds / 60.0)))});
}

namespace {

// The colours of Nightshade's own theme (pe/ui/theme.cpp, "nightly").
constexpr Rgb kBase{0x0c, 0x0a, 0x14};
constexpr Rgb kPanel{0x17, 0x13, 0x24};
constexpr Rgb kTrack{0x2b, 0x25, 0x3d};
constexpr Rgb kAccent{0xa7, 0x8b, 0xfa};
constexpr Rgb kTitle{0xf4, 0xf1, 0xfb};
constexpr Rgb kText{0xc9, 0xc3, 0xd9};
constexpr Rgb kDim{0x8d, 0x86, 0xa3};
constexpr Rgb kWarning{0xff, 0x8a, 0x80};

// Lines of wrapped text, centred; where the next line would be.
float Paragraph(Picture& screen, const pe::gfx::Font& font, const std::string& text, float top, float size,
                float width, float pitch, Rgb colour) {
    for (const std::string& line : font.wrap(text, size, width)) {
        screen.text(font, line, Picture::kWidth * 0.5f, top + font.ascent(size), size, colour, pe::gfx::Align::center);
        top += pitch;
    }
    return top;
}

} // namespace

float Draw(Picture& screen, const pe::gfx::Font& font, const View& view, double time) {
    constexpr float kCentre = Picture::kWidth * 0.5f;
    constexpr float kTextWidth = 1080.0f;
    screen.clear(kBase);
    screen.fill(360.0f, 120.0f, 1200.0f, 840.0f, kPanel);
    screen.fill(360.0f, 120.0f, 1200.0f, 4.0f, kAccent);
    screen.text(font, "NIGHTSHADE", kCentre, 196.0f, 26.0f, kAccent, pe::gfx::Align::center);
    float y = Paragraph(screen, font, Say("Nightshade has its own app now"), 232.0f, 58.0f, kTextWidth, 70.0f, kTitle);
    y = Paragraph(screen, font,
                  Say("From now on, open Nightshade from its own tile on the home screen. This tile only takes you there."),
                  y + 24.0f, 30.0f, kTextWidth, 42.0f, kText);
    y = Paragraph(screen, font,
                  Say("At its first start Nightshade asks what to do with your ProsperoEden data. Afterwards this tile "
                      "can be deleted, or used for the official ProsperoEden app again."),
                  y + 14.0f, 26.0f, kTextWidth, 36.0f, kDim);

    // What is happening.
    const bool failed = view.step == Step::failed;
    y = std::max(y + 40.0f, 610.0f);
    y = Paragraph(screen, font, view.status, y, 34.0f, kTextWidth, 46.0f, failed ? kWarning : kTitle);
    if (!view.detail.empty()) y = Paragraph(screen, font, view.detail, y + 4.0f, 26.0f, kTextWidth, 36.0f, kDim);
    const float bar_x = kCentre - 400.0f, bar_y = std::max(y + 22.0f, 800.0f), bar_w = 800.0f, bar_h = 8.0f;
    if (!failed && view.step != Step::closing) {
        screen.fill(bar_x, bar_y, bar_w, bar_h, kTrack);
        if (view.progress >= 0.0) {
            screen.fill(bar_x, bar_y, bar_w * static_cast<float>(std::min(1.0, view.progress)), bar_h, kAccent);
            screen.text(font, Say("{0}%", {std::to_string(static_cast<int>(view.progress * 100.0 + 0.5))}),
                        bar_x + bar_w + 18.0f, bar_y + 12.0f, 24.0f, kDim);
        } else {
            // Something is being waited for: a piece that goes back and forth.
            const double phase = time * 0.6 - static_cast<double>(static_cast<long>(time * 0.6));
            const float along = static_cast<float>(phase < 0.5 ? phase * 2.0 : 2.0 - phase * 2.0);
            screen.fill(bar_x + (bar_w - 160.0f) * along, bar_y, 160.0f, bar_h, kAccent);
        }
    }

    // The buttons, at the bottom right.
    float x = 1560.0f - 40.0f;
    const float hints = 912.0f;
    const auto hint = [&](const std::string& label, bool cross) {
        x -= font.measure(label, 26.0f);
        screen.text(font, label, x, hints + 9.0f, 26.0f, kText);
        x -= 34.0f;
        if (cross)
            screen.cross(x + 14.0f, hints, 10.0f, 3.5f, kText);
        else
            screen.ring(x + 14.0f, hints, 13.0f, 3.5f, kText);
        x -= 36.0f;
    };
    hint(Say("Close"), false);
    if (failed) hint(Say("Try again"), true);
    return y;
}

} // namespace Eden::Bridge
