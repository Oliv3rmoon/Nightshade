// Nightshade - Launcher: the card that names the community once. At a start of the app, when the
// home screen is free (the opening over, no dialog open, no update offer waiting for its turn, no
// game that failed to say), a card at the middle names the community's Discord server: what it
// is, its name and address, and a QR code a phone opens it by (community.hpp). Any button closes
// it, and that is saved (Preferences::community_seen): it never shows again. About names the
// server always.
// SPDX-FileCopyrightText: Copyright 2026 Oliv3rmoon
// SPDX-License-Identifier: GPL-3.0-or-later

#include "pe/ui/community.hpp"
#include "pe/ui/launcher.hpp"

namespace pe::ui
{

namespace
{

// The card: the QR code at its left, under what it says; the server at the right.
constexpr Rect kCard{360.0f, 230.0f, 1200.0f, 620.0f};
constexpr Rect kCardQr{408.0f, 426.0f, 360.0f, 360.0f};
// The seconds the menu has shown before the card comes: it has arrived, and an update offer
// already known has gone first.
constexpr float kCardAfter = 1.6f;

} // namespace

void Launcher::offer_community()
{
    if (prefs_.community_seen || !first_start_ || opening_on_ || intro_ < kCardAfter)
        return;
    // Over the home screen at rest only, and never over what the start has to say first.
    if (screen_ != Screen::home || rail_focus_ || modal_ != Modal::none || modal_shown_ != Modal::none ||
        update_waiting_ || transition_.running || !selected_game_.empty() || !popup_.empty() ||
        home_.launch_failed || app_closing_ >= 0.0f)
        return;
    open_modal(Modal::community);
}

void Launcher::press_community(Key)
{
    // Any button closes it, for good. Saved quietly: should the settings file not take it now,
    // the card comes back at a later start, and nothing else changes.
    prefs_.community_seen = true;
    services_.set_preferences(prefs_);
    close_modal();
}

void Launcher::draw_community(Canvas &c, float open)
{
    gfx::DrawList &list = c.list;
    const Theme &t = th();
    list.push_opacity(open);
    list.push_transform(1.0f - 0.03f * (1.0f - open) * motion(), 960.0f, 540.0f, 0.0f,
                        (1.0f - open) * 26.0f * motion());
    glass(c, kCard, 28.0f, t.panel.with_alpha(0.98f), t.accent.with_alpha(0.38f), 1.8f);
    const float left = kCard.x + 48.0f;
    const float width = kCard.w - 96.0f;
    {
        const Heavy heavy(c);
        text_shrink(c, tr("Join the community"), left, baseline(kCard.y + 34.0f, 62.0f, t.size_display),
                    t.size_display, t.title, width);
    }
    text_block(c, tr("Get help, hear about new releases and meet other players on Discord."), left,
               baseline(kCard.y + 104.0f, 34.0f, t.size_text), t.size_text, 34.0f, t.copy, width, 2, kShrink);

    qr_code(c, community_qr(), kCardQr, 1);

    // The server at the right of the code: how to open it, its name and address, where it is
    // found again.
    const float x = kCardQr.x + kCardQr.w + 56.0f;
    const float room = kCard.x + kCard.w - 48.0f - x;
    text_block(c, tr("Scan the code with your phone, or open the address below."), x,
               baseline(kCardQr.y, 32.0f, t.size_text), t.size_text, 32.0f, t.copy, room, 2, kShrink);
    {
        const Heavy heavy(c);
        text_shrink(c, kCommunityName, x, baseline(kCardQr.y + 84.0f, 62.0f, t.size_display), t.size_display,
                    t.title, room);
    }
    text_shrink(c, kCommunityLink, x, baseline(kCardQr.y + 150.0f, 40.0f, t.size_heading), t.size_heading,
                t.accent_pale, room);
    text_block(c, tr("You can find it again in About."), x, baseline(kCardQr.y + 214.0f, 30.0f, t.size_small),
               t.size_small, 30.0f, t.meta, room, 2, kShrink);
    // Any button closes it; Cross is the one it names.
    static constexpr Hint kClose[] = {{Pad::cross, TR("Close")}};
    draw_menu_hints(c, kClose, 1, x, kCardQr.y + kCardQr.h - 20.0f, t.copy, room);
    list.pop_transform();
    list.pop_opacity();
}

} // namespace pe::ui
