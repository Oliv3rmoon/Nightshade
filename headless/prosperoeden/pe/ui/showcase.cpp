// Nightshade - The launcher's own layout (Theme::showcase): the top bar with the mark, the wordmark
// and four tabs; the home screen over the selected game's art with a row of covers under its title;
// the Library's grid with a search and filters; and the hints at the bottom right. Settings' column
// and panel are in settings.cpp, beside the categories they show. docs/THEMES.md describes it.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "pe/ui/launch_commit.hpp"
#include "pe/ui/launcher.hpp"

#include "pe/core/log.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <exception>
#include <string>
#include <utility>
#include <vector>

namespace pe::ui
{

using audio::Cue;

namespace
{

constexpr Rect kScreen{0.0f, 0.0f, 1920.0f, 1080.0f};
const Color kWhite{1.0f, 1.0f, 1.0f, 1.0f};
const Color kBlack{0.0f, 0.0f, 0.0f, 1.0f};
constexpr float kLeft = 80.0f;    // where the screens' content starts
constexpr float kRight = 1840.0f; // ... and ends

// ---- the top bar ----
constexpr Rect kMark = kShowcaseMark;
constexpr float kBarTop = 20.0f;
constexpr float kBarHeight = 88.0f;
constexpr float kTabSize = 30.0f;
constexpr float kTabPad = 40.0f;       // either side of a tab's name
constexpr float kTabsCentre = 1000.0f; // the tabs are a group around here...
constexpr float kTabsRoom = 1040.0f;   // ... no wider than this (a long translation gets smaller)
constexpr float kUnderline = 100.0f;
constexpr const char *kTabNames[] = {TR("Home"), TR("Library"), TR("Multiplayer"), TR("Settings")};
constexpr float kClockRight = 1868.0f;

// ---- the hints ----
constexpr float kFooterLine = 1012.0f;
constexpr float kHintGlyph = 34.0f;
constexpr float kHintSize = 24.0f;
constexpr float kHintGap = 56.0f; // between two hints, with a hairline in its middle

// ---- home ----
constexpr float kTitleLine = 334.0f;
constexpr float kTitleWidth = 1080.0f;
constexpr float kChipsLine = 404.0f;
constexpr float kChipHeight = 46.0f;
constexpr float kChipText = 22.0f;
constexpr float kButtonsTop = 466.0f;
constexpr float kButtonHeight = 80.0f;
constexpr float kShelfTop = 664.0f;
constexpr float kShelfCover = 272.0f;
constexpr float kShelfPitch = 296.0f;
constexpr int kShelfShown = 6;
constexpr int kShelfMost = 24;
// The home screen's two buttons (home.cpp counts them so too).
constexpr int kPlay = 0;
constexpr int kGameSettings = 1;
// A notice the player must see (setup, a game that stopped), at the top right over the art.
constexpr Rect kStatusCard{1220.0f, 140.0f, 620.0f, 164.0f};

// ---- the Library ----
constexpr float kHeaderTop = 146.0f;
constexpr float kHeaderHeight = 62.0f;
constexpr Rect kSearch{kLeft, kHeaderTop, 640.0f, kHeaderHeight};
constexpr float kFiltersLeft = 752.0f;
constexpr float kFilterText = 26.0f;
constexpr int kColumns = 6;
constexpr float kTileWidth = 276.0f;
constexpr float kTileHeight = 220.0f;
constexpr float kTilePitchX = (kRight - kLeft - kTileWidth) / static_cast<float>(kColumns - 1);
constexpr float kTilePitchY = 246.0f;
constexpr float kGridTop = 248.0f;
constexpr int kGridRowsShown = 3;
constexpr Rect kGridWindow{40.0f, 228.0f, 1840.0f, 772.0f}; // down to the hints
constexpr const char *kFilterNames[] = {TR("All"), TR("Recently played"), TR("Favorites")};
constexpr int kFilters = 3;

std::string lower(std::string_view text)
{
    std::string out{text};
    for (char &ch : out)
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    return out;
}

std::string trimmed(const std::string &text)
{
    const std::size_t first = text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos)
        return {};
    const std::size_t last = text.find_last_not_of(" \t\r\n");
    return text.substr(first, last - first + 1);
}

// A pill: a face and a hairline, its ends round.
void pill(Canvas &c, const Rect &r, Color fill, Color edge)
{
    if (look().high_contrast)
    {
        c.list.bordered_rect(r, r.h * 0.5f, gfx::mix(th().scrim, th().row, 0.6f), 1.5f, th().copy);
        return;
    }
    c.list.bordered_rect(r, r.h * 0.5f, fill, std::max(1.0f, th().border), edge);
}

// A pill of the accent: the chosen filter, the main button. Its light reaches past it.
void accent_pill(Canvas &c, const Rect &r, float glow)
{
    const Theme &t = th();
    if (look().high_contrast)
    {
        c.list.bordered_rect(r, r.h * 0.5f, t.accent, 2.0f, t.title);
        return;
    }
    if (glow > 0.0f)
        c.list.shadow({r.x + 4.0f, r.y + 6.0f, r.w - 8.0f, r.h}, r.h * 0.5f, t.focus_glow, t.accent.with_alpha(0.34f * glow));
    c.list.hgradient_rect(r, r.h * 0.5f, gfx::mix(t.accent, kWhite, 0.08f), gfx::mix(t.accent, t.accent_deep, 0.28f),
                          1.5f, t.accent_pale.with_alpha(0.55f));
    c.list.gradient_rect({r.x + 2.0f, r.y + 2.0f, r.w - 4.0f, r.h * 0.5f}, r.h * 0.5f, kWhite.with_alpha(0.14f),
                         kWhite.with_alpha(0.0f));
}

// The magnifier of the search field.
void magnifier(Canvas &c, float cx, float cy, float size, Color ink)
{
    const float r = size * 0.30f;
    const float x = cx - size * 0.08f;
    const float y = cy - size * 0.08f;
    c.list.ring(x, y, r, size * 0.085f, ink);
    const float d = r * 0.74f;
    c.list.line(x + d, y + d, cx + size * 0.36f, cy + size * 0.36f, size * 0.10f, ink);
}

// A cover filling `r`, its middle cut to r's shape (covers are square); the app's mark on a dark
// tile while it loads or when there is none.
void cover_fill(Canvas &c, const std::string &path, const Rect &r, float radius)
{
    const Cover image = c.textures.cover(path, std::max(r.w, r.h));
    const float fade = image.texture != 0 ? tween::cubic_out(image.age / 0.22f) : 0.0f;
    if (fade < 1.0f)
    {
        c.list.gradient_rect(r, radius, th().glass, th().panel);
        if (image.missing && c.textures.brand() != 0)
        {
            const float side = std::min(r.w, r.h) * 0.62f;
            c.list.image(c.textures.brand(), {r.x + (r.w - side) * 0.5f, r.y + (r.h - side) * 0.5f, side, side},
                         {0.0f, 0.0f, 1.0f, 1.0f}, kWhite.with_alpha(0.9f));
        }
    }
    if (image.texture != 0)
    {
        const float aspect = r.w / r.h;
        const Rect uv = aspect >= 1.0f ? Rect{0.0f, (1.0f - 1.0f / aspect) * 0.5f, 1.0f, 1.0f / aspect} :
                                         Rect{(1.0f - aspect) * 0.5f, 0.0f, aspect, 1.0f};
        c.list.rounded_image(image.texture, r, uv, radius, kWhite.with_alpha(fade));
    }
    c.list.bordered_rect(r, radius, kWhite.with_alpha(0.0f), 1.0f, kWhite.with_alpha(0.14f));
}

// A focused cover's name on a shade at its foot, and under it a line about it (a download's state).
void caption(Canvas &c, const Rect &r, float radius, std::string_view name, std::string_view note, Color note_ink)
{
    const Theme &t = th();
    const float shade = note.empty() ? 96.0f : 124.0f;
    c.list.gradient_rect({r.x, r.y + r.h - shade, r.w, shade}, radius, kBlack.with_alpha(0.0f),
                         kBlack.with_alpha(0.88f));
    const float line = note.empty() ? r.y + r.h - 28.0f : r.y + r.h - 56.0f;
    {
        const Heavy heavy(c, 0.7f);
        text_fit(c, name, r.x + r.w * 0.5f, line, 24.0f, t.title, r.w - 32.0f, Align::center);
    }
    if (!note.empty())
        text_fit(c, note, r.x + r.w * 0.5f, r.y + r.h - 22.0f, 18.0f, note_ink, r.w - 32.0f, Align::center);
}

// The home screen's buttons: the main one a pill of the accent, the other an outlined pill. The
// main one carries its controller button as a white disc.
void play_pill(Canvas &c, const Rect &r, std::string_view label, Pad button, float focus, float press, bool enabled)
{
    const Theme &t = th();
    gfx::DrawList &list = c.list;
    list.push_opacity(enabled ? 1.0f : 0.45f);
    const float scale = 1.0f + (0.03f * focus - 0.035f * press * focus) * motion();
    list.push_transform(scale, r.x + r.w * 0.5f, r.y + r.h * 0.5f, 0.0f, 0.0f);
    focus_ring(c, r, r.h * 0.5f, focus);
    accent_pill(c, r, 0.7f + 0.3f * focus);
    const float glyph = 46.0f;
    const float size = 34.0f;
    const float words = std::min(r.w - 72.0f - glyph - 18.0f, text_width(c, label, size));
    const float left = r.x + (r.w - (glyph + 18.0f + words)) * 0.5f;
    const float cy = r.y + r.h * 0.5f;
    if (button == Pad::cross)
    {
        list.circle(left + glyph * 0.5f, cy, glyph * 0.5f, t.on_accent);
        const float d = glyph * 0.17f;
        list.line(left + glyph * 0.5f - d, cy - d, left + glyph * 0.5f + d, cy + d, 4.0f, t.accent);
        list.line(left + glyph * 0.5f - d, cy + d, left + glyph * 0.5f + d, cy - d, 4.0f, t.accent);
    }
    else
        draw_pad_ink(c, button, left, cy, glyph, t.on_accent);
    {
        const Heavy heavy(c);
        const bool contrast = look().high_contrast;
        look().high_contrast = false;
        text_shrink(c, label, left + glyph + 18.0f, baseline(r.y, r.h, size), size, t.on_accent, words + 1.0f);
        look().high_contrast = contrast;
    }
    list.pop_transform();
    list.pop_opacity();
}

void outline_pill(Canvas &c, const Rect &r, std::string_view label, Icon mark, float focus, float press, bool enabled)
{
    const Theme &t = th();
    gfx::DrawList &list = c.list;
    list.push_opacity(enabled ? 1.0f : 0.45f);
    const float scale = 1.0f + (0.03f * focus - 0.035f * press * focus) * motion();
    list.push_transform(scale, r.x + r.w * 0.5f, r.y + r.h * 0.5f, 0.0f, 0.0f);
    focus_ring(c, r, r.h * 0.5f, focus);
    pill(c, r, gfx::mix(t.panel, t.row_focus, focus).with_alpha(0.62f), kWhite.with_alpha(0.50f + 0.3f * focus));
    const float glyph = 44.0f;
    const float size = 30.0f;
    const float words = std::min(r.w - 72.0f - glyph - 16.0f, text_width(c, label, size));
    const float left = r.x + (r.w - (glyph + 16.0f + words)) * 0.5f;
    icon(c, mark, left + glyph * 0.5f, r.y + r.h * 0.5f, glyph, t.title);
    {
        const Heavy heavy(c, 0.6f);
        text_shrink(c, label, left + glyph + 16.0f, baseline(r.y, r.h, size), size, t.title, words + 1.0f);
    }
    list.pop_transform();
    list.pop_opacity();
}

// A fact about the game, in a pill.
float fact(Canvas &c, std::string_view value, float x, float cy, Tone tone, float room)
{
    const Theme &t = th();
    if (tone != Tone::plain)
        return chip(c, value, x, cy, tone, room);
    const float width = std::min(room, text_width(c, value, kChipText) + 48.0f);
    const Rect r{x, cy - kChipHeight * 0.5f, width, kChipHeight};
    pill(c, r, t.panel.with_alpha(0.58f), kWhite.with_alpha(0.34f));
    text_shrink(c, value, r.x + r.w * 0.5f, baseline(r.y, r.h, kChipText), kChipText, t.title, r.w - 40.0f,
                Align::center);
    return width;
}

} // namespace

// ---------------------------------------------------------------- the top bar and the hints

void Launcher::draw_top_bar(Canvas &c)
{
    gfx::DrawList &list = c.list;
    const Theme &t = th();
    // A shade under it, so that it reads over a game's art.
    if (!look().high_contrast)
        list.gradient_rect({0.0f, 0.0f, 1920.0f, 190.0f}, 0.0f, t.base.with_alpha(0.78f), t.base.with_alpha(0.0f));

    // The mark (during the opening it is on its way here: draw_opening), a hairline, the wordmark.
    if (textures_.brand() != 0 && !(opening_on_ && !look().reduce_motion))
        list.image(textures_.brand(), kMark, {0.0f, 0.0f, 1.0f, 1.0f}, kWhite);
    list.rounded_rect({kMark.x + kMark.w + 26.0f, kBarTop + 18.0f, 1.0f, kBarHeight - 36.0f}, 0.0f,
                      t.panel_edge.with_alpha(0.9f));
    std::string wordmark{kBuildLabel};
    for (char &ch : wordmark)
        ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
    {
        const Heavy heavy(c, 0.35f);
        text(c, wordmark, kMark.x + kMark.w + 60.0f, baseline(kBarTop, kBarHeight, 28.0f), 28.0f, t.title,
             Align::left, 15.0f);
    }

    // The tabs: Home, Library, Multiplayer, Settings, a hairline between two, the shown one in
    // bold over a line of the accent.
    float widths[4];
    float total = 0.0f;
    for (int i = 0; i < 4; ++i)
    {
        widths[i] = text_width(c, tr(kTabNames[i]), kTabSize) + 2.0f * kTabPad;
        total += widths[i];
    }
    const float fit = std::min(1.0f, kTabsRoom / total);
    const float cy = kBarTop + kBarHeight * 0.5f;
    list.push_transform(fit, kTabsCentre, cy, 0.0f, 0.0f);
    float x = kTabsCentre - total * 0.5f;
    const bool connected = services_.room_status().phase == Eden::Multiplayer::Phase::Connected;
    for (int i = 0; i < 4; ++i)
    {
        const float active = tween::clamp01(rail_active_[static_cast<std::size_t>(i)].value);
        const float w = widths[i];
        if (i > 0)
            list.rounded_rect({x, cy - 18.0f, 1.0f, 36.0f}, 0.0f, t.panel_edge.with_alpha(0.9f));
        {
            const Heavy heavy(c, 0.9f * active);
            text(c, tr(kTabNames[i]), x + w * 0.5f, baseline(kBarTop, kBarHeight, kTabSize), kTabSize,
                 gfx::mix(t.body, t.title, active), Align::center);
        }
        if (active > 0.01f)
            list.rounded_rect({x + 10.0f, kUnderline, w - 20.0f, 3.0f}, 1.5f, t.accent.with_alpha(active));
        // A room joined: a dot of the accent by Multiplayer.
        if (i == 2 && connected)
            list.circle(x + w - kTabPad * 0.45f, cy - 16.0f, 5.0f, t.accent);
        x += w;
    }
    list.pop_transform();

    // The clock.
    {
        const Heavy heavy(c, 0.7f);
        text(c, clock_, kClockRight, baseline(kBarTop, kBarHeight, 40.0f), 40.0f, t.title, Align::right);
    }
}

void Launcher::draw_showcase_footer(Canvas &c, const Hint *hints, int count, std::string_view status)
{
    gfx::DrawList &list = c.list;
    const Theme &t = th();
    const float cy = kFooterLine;
    // Laid out once to measure, then drawn from the right edge back.
    std::vector<Hint> shown(static_cast<std::size_t>(count));
    float total = 0.0f;
    for (int i = 0; i < count; ++i)
    {
        Hint &hint = shown[static_cast<std::size_t>(i)];
        hint = hints[i];
        hint.button = translate_hint(hint.button);
        hint.second = translate_hint(hint.second);
        total += pad_width(hint.button, kHintGlyph) + 12.0f + text_width(c, tr(hint.label), kHintSize);
        if (hint.second != Pad::none)
            total += 6.0f + pad_width(hint.second, kHintGlyph);
        if (i + 1 < count)
            total += kHintGap;
    }
    // A long translation gets smaller rather than reaching the left side.
    const float most = kClockRight - 640.0f;
    const float fit = std::min(1.0f, most / std::max(total, 1.0f));
    const float left = kClockRight - total * fit;

    // What the menu last said (a dialog says its own), or the status, at the left.
    const float room = left - kLeft - 48.0f;
    const float line = cy + t.size_small * 0.35f;
    if (!message_.empty() && modal_shown_ == Modal::none)
        notice(c, message_, kLeft, line, t.size_small, message_warning_ ? t.warning : t.accent_pale, room,
               message_warning_);
    else if (!status.empty())
        text_shrink(c, status, kLeft, line, t.size_small, t.meta, room);

    list.push_transform(fit, kClockRight, cy, 0.0f, 0.0f);
    float x = kClockRight - total;
    for (int i = 0; i < count; ++i)
    {
        const Hint &hint = shown[static_cast<std::size_t>(i)];
        draw_pad_ink(c, hint.button, x, cy, kHintGlyph, t.title);
        x += pad_width(hint.button, kHintGlyph);
        if (hint.second != Pad::none)
        {
            x += 6.0f;
            draw_pad_ink(c, hint.second, x, cy, kHintGlyph, t.title);
            x += pad_width(hint.second, kHintGlyph);
        }
        x += 12.0f;
        x += text(c, tr(hint.label), x, cy + kHintSize * 0.35f, kHintSize, t.text);
        if (i + 1 < count)
        {
            list.rounded_rect({x + kHintGap * 0.5f, cy - 18.0f, 1.0f, 36.0f}, 0.0f, t.panel_edge.with_alpha(0.9f));
            x += kHintGap;
        }
    }
    list.pop_transform();
}

// ---------------------------------------------------------------- Multiplayer

void Launcher::draw_showcase_intro(Canvas &c)
{
    const Theme &t = th();
    static constexpr const char *kStates[] = {TR("Offline"), TR("Connecting..."), TR("Connected"), TR("Leaving..."),
                                              TR("Connection failed")};
    const char *title = room_page_ == RoomPage::choice       ? TR("Multiplayer") :
                        room_page_ == RoomPage::public_rooms ? TR("Public rooms") :
                                                               TR("Private room");
    const float here = arrive(0);
    c.list.push_opacity(here);
    {
        const Heavy heavy(c);
        text_shrink(c, tr(title), kLeft, 300.0f, 72.0f, t.title, 760.0f, Align::left, 0.0f, 0.6f);
    }
    const auto phase = static_cast<std::size_t>(room_status_.phase);
    const bool connected = room_status_.phase == Eden::Multiplayer::Phase::Connected;
    if (phase < std::size(kStates))
        fact(c, tr(kStates[phase]), kLeft, 372.0f, connected ? Tone::accent : Tone::plain, 760.0f);
    text_block(c, tr("Join a room before launching a game, then choose Local Wireless in the game."), kLeft,
               baseline(430.0f, 38.0f, 26.0f), 26.0f, 38.0f, t.copy, 760.0f, 4, kShrink);
    c.list.pop_opacity();
}

// ---------------------------------------------------------------- the game's art

void Launcher::draw_showcase_art(Canvas &c, float strength)
{
    gfx::DrawList &list = c.list;
    const Theme &t = th();
    const std::uint32_t now = textures_.game_art();
    const std::uint32_t before = textures_.game_art_before();
    if (now == 0 && before == 0)
        return; // the theme's own backdrop shows
    const float fade = look().reduce_motion ? 1.0f : tween::cubic_out(textures_.game_art_age() / 0.45f);
    list.rounded_rect(kScreen, 0.0f, t.base);
    // The art at the right, as wide as it is tall times 1.43: its middle part, darkened.
    constexpr Rect kArt{380.0f, 0.0f, 1540.0f, 1080.0f};
    constexpr float kTall = 1080.0f / 1540.0f;
    const Rect uv{0.0f, (1.0f - kTall) * 0.5f, 1.0f, kTall};
    const float light = look().high_contrast ? 0.30f : 0.30f + 0.42f * strength;
    const Color tint{light, light, light, 1.0f};
    if (before != 0 && fade < 1.0f)
        list.image(before, kArt, uv, tint.with_alpha(now != 0 ? 1.0f : 1.0f - fade));
    if (now != 0)
        list.image(now, kArt, uv, tint.with_alpha(fade));
    // Dark at the left where the words are, opening toward the right; dark under the covers and
    // the hints.
    list.hgradient_rect({kArt.x - 1.0f, 0.0f, 820.0f, 1080.0f}, 0.0f, t.base, t.base.with_alpha(0.0f));
    list.gradient_rect({0.0f, 540.0f, 1920.0f, 540.0f}, 0.0f, t.base.with_alpha(0.0f), t.base.with_alpha(0.94f));
    // The theme's crimson, low in the dark: a vignette from black to red.
    if (!look().high_contrast)
    {
        list.shadow({-260.0f, 720.0f, 1200.0f, 560.0f}, 280.0f, 280.0f, t.accent_deep.with_alpha(0.50f * strength));
        list.shadow({1560.0f, -240.0f, 760.0f, 900.0f}, 320.0f, 320.0f, t.accent_deep.with_alpha(0.32f * strength));
    }
    if (strength < 1.0f)
        list.rounded_rect(kScreen, 0.0f, t.base.with_alpha(0.45f * (1.0f - strength)));
}

// ---------------------------------------------------------------- home

void Launcher::collect_shelf()
{
    const int count = static_cast<int>(games_.size());
    std::string keep;
    if (shelf_.selected >= 0 && shelf_.selected < static_cast<int>(shelf_games_.size()))
    {
        const int index = shelf_games_[static_cast<std::size_t>(shelf_.selected)];
        if (index >= 0 && index < count)
            keep = games_[static_cast<std::size_t>(index)].file;
    }
    shelf_games_.clear();
    const auto add = [&](int index)
    {
        if (static_cast<int>(shelf_games_.size()) < kShelfMost &&
            std::find(shelf_games_.begin(), shelf_games_.end(), index) == shelf_games_.end())
            shelf_games_.push_back(index);
    };
    // The recent games first, newest first; then the rest of the Library in its order.
    for (const Recent &recent : home_.recents)
        for (int i = 0; i < count; ++i)
            if (games_[static_cast<std::size_t>(i)].file == recent.file)
            {
                add(i);
                break;
            }
    for (int i = 0; i < count; ++i)
        add(i);

    int at = 0;
    if (shelf_moved_)
        for (int i = 0; i < static_cast<int>(shelf_games_.size()); ++i)
            if (games_[static_cast<std::size_t>(shelf_games_[static_cast<std::size_t>(i)])].file == keep)
                at = i;
    shelf_.wrap = false;
    shelf_.visible = kShelfShown;
    shelf_.pitch = kShelfPitch;
    shelf_.reset(static_cast<int>(shelf_games_.size()), at);
    // Until the player moves, the focus is on the row's first cover: the game played last.
    if (shelf_games_.empty())
        home_on_grid_ = false;
    else if (!shelf_moved_)
        home_on_grid_ = true;
    if (screen_ == Screen::home && modal_ == Modal::none)
        select_shelf();
}

void Launcher::select_shelf()
{
    if (shelf_games_.empty())
        return;
    const int at = std::clamp(shelf_.selected, 0, static_cast<int>(shelf_games_.size()) - 1);
    const int index = shelf_games_[static_cast<std::size_t>(at)];
    if (index < 0 || index >= static_cast<int>(games_.size()) || index == library_.selected)
        return;
    library_.selected = index;
    refresh_selected_game();
}

void Launcher::press_showcase_home(Key key)
{
    // Quick settings need the game in the list: Options pressed before it is read waits for it.
    if (key == Key::options && !games_loaded_)
        finish_scan(true);
    shelf_moved_ = true;
    const bool ready = home_.setup_ready;
    const int count = static_cast<int>(games_.size());
    // The game the failure notice is about: Options offers it the safe start, wherever the focus is.
    if (key == Key::options && home_.launch_failed && home_.last_exists && home_.last_title_id != 0)
    {
        for (int i = 0; i < count; ++i)
            if (games_[static_cast<std::size_t>(i)].file == home_.last_file)
                return open_quick(i, true);
    }
    const bool has_game = library_.selected >= 0 && library_.selected < count;
    const Game *game = has_game ? &games_[static_cast<std::size_t>(library_.selected)] : nullptr;
    const bool settable = game != nullptr && !game->remote && game->title_id != 0;
    switch (key)
    {
    case Key::left:
    case Key::right:
    {
        if (home_on_grid_)
        {
            if (shelf_.move(key == Key::right ? 1 : -1))
            {
                select_shelf();
                message_.clear();
                cue(Cue::focus);
            }
            return;
        }
        const int next = key == Key::right ? kGameSettings : kPlay;
        if (next == home_focus_ || (next == kGameSettings && !settable))
            return;
        home_focus_ = next;
        cue(Cue::focus);
        return;
    }
    case Key::down:
        if (!home_on_grid_ && !shelf_games_.empty())
        {
            home_on_grid_ = true;
            select_shelf();
            cue(Cue::focus);
        }
        return;
    case Key::up:
        if (home_on_grid_)
        {
            home_on_grid_ = false;
            home_focus_ = kPlay;
            cue(Cue::focus);
        }
        return;
    case Key::cross:
        press_ = 1.0f;
        if (!home_on_grid_ && home_focus_ == kGameSettings)
        {
            if (settable)
                open_game_settings_from_home(game->file);
            else
                cue(Cue::error);
            return;
        }
        if (!ready)
        {
            go_section(3); // what the setup needs is in Settings
            return;
        }
        if (game == nullptr)
        {
            go_section(1);
            return;
        }
        // One on its sources only is downloaded and then played, unless a source sends it to be
        // played where it lies (play_file): then it starts at once.
        if (game->remote && play_file(*game).empty())
        {
            play_remote(*game);
            return;
        }
        if (home_on_grid_)
            launch_from_ = {kLeft + kShelfPitch * static_cast<float>(shelf_.selected) - shelf_.scroll(), kShelfTop,
                            kShelfCover, kShelfCover};
        else
            launch_from_ = {kLeft, kShelfTop, kShelfCover, kShelfCover};
        launch(play_file(*game), game->name, game->cover);
        return;
    case Key::triangle:
        if (game == nullptr)
            return;
        if (!settable)
        {
            say(game->remote ? tr("Download the game to change its settings.")
                             : tr("This game's settings cannot be saved (no title ID)."),
                true);
            cue(Cue::error);
            return;
        }
        open_game_settings_from_home(game->file);
        return;
    case Key::square:
    {
        if (game == nullptr)
            return;
        if (game->remote)
        {
            queue_remote(*game);
            return;
        }
        if (game->title_id == 0 || game->mods == 0)
        {
            cue(Cue::error);
            return;
        }
        Game &chosen = games_[static_cast<std::size_t>(library_.selected)];
        const bool saved = services_.set_mods_enabled(chosen.title_id, !chosen.mods_enabled);
        if (saved)
            count_mods(chosen, game_mods(chosen.title_id));
        say(saved ? tr("Saved for this game. Applies on next launch.") : tr("Could not save. Please try again."),
            !saved);
        cue(saved ? Cue::toggle : Cue::error);
        return;
    }
    case Key::options:
        if (game != nullptr && !game->remote)
            open_quick(library_.selected, false);
        else
            cue(Cue::error);
        return;
    default:
        return;
    }
}

void Launcher::draw_showcase_home(Canvas &c)
{
    gfx::DrawList &list = c.list;
    const Theme &t = th();
    const auto arrive_part = [&](int part)
    { return tween::cubic_out((intro_ - 0.10f - t.stagger * static_cast<float>(part)) / 0.55f); };
    const auto begin_part = [&](int part, float rise)
    {
        const float e = arrive_part(part);
        list.push_opacity(e);
        list.push_transform(1.0f, 0.0f, 0.0f, 0.0f, (1.0f - e) * rise * motion());
    };
    const auto end_part = [&]
    {
        list.pop_transform();
        list.pop_opacity();
    };
    const auto focus = [&](int index) { return home_springs_[static_cast<std::size_t>(index)].value; };

    draw_showcase_art(c, 1.0f);

    const bool ready = home_.setup_ready;
    const int count = static_cast<int>(games_.size());
    const Game *game = library_.selected >= 0 && library_.selected < count ?
                           &games_[static_cast<std::size_t>(library_.selected)] : nullptr;
    const bool playable = game != nullptr && !play_file(*game).empty();

    // ---- the game's name ----
    begin_part(1, 26.0f);
    {
        const std::string title = game != nullptr ? game->name :
                                  !home_.last_file.empty() ? home_.last_title :
                                                             std::string{tr("Your next adventure")};
        list.push_opacity(tween::clamp01(0.4f + detail_.value));
        const Heavy heavy(c);
        text_shrink(c, title, kLeft, kTitleLine, t.size_hero, t.title, kTitleWidth, Align::left, 0.0f, 0.56f);
        list.pop_opacity();
    }
    end_part();

    // ---- what it is: its format, size, mode, the picture's size and rate ----
    begin_part(2, 22.0f);
    {
        std::vector<std::pair<std::string, Tone>> facts;
        if (game != nullptr && !game->remote)
        {
            facts.emplace_back(game->format, Tone::plain);
            facts.emplace_back(game->size, Tone::plain);
            if (game->title_id != 0)
                facts.emplace_back(selected_docked_ ? tr("Docked") : tr("Handheld"), Tone::plain);
            facts.emplace_back(output_name(prefs_.output), Tone::plain);
            facts.emplace_back(hertz(selected_refresh_ >= 0 ? selected_refresh_ : prefs_.refresh), Tone::plain);
            if (!game->language_note.empty())
                facts.emplace_back(game->language, Tone::warning);
        }
        else if (game != nullptr)
        {
            Color ink = t.copy;
            facts.emplace_back(remote_state(*game, &ink), Tone::accent);
            if (!game->size.empty())
                facts.emplace_back(game->size, Tone::plain);
        }
        else
            facts.emplace_back(ready ? tr("Choose a game from your library.") : tr("Needs attention"), Tone::plain);
        float x = kLeft;
        for (const auto &[words, tone] : facts)
        {
            if (words.empty())
                continue;
            const float room = kLeft + kTitleWidth - x;
            if (room < 120.0f)
                break;
            x += fact(c, words, x, kChipsLine, tone, room) + 16.0f;
        }
    }
    end_part();

    // ---- Play and Game settings ----
    begin_part(3, 22.0f);
    {
        const char *first = !ready ? tr("Settings") : game == nullptr ? tr("Open library") : tr("Play");
        const char *second = tr("Game settings");
        const float first_width = std::clamp(text_width(c, first, 34.0f) + 172.0f, 300.0f, 560.0f);
        const float second_width = std::clamp(text_width(c, second, 30.0f) + 156.0f, 300.0f, 560.0f);
        const bool on_buttons = !home_on_grid_;
        play_pill(c, {kLeft, kButtonsTop, first_width, kButtonHeight}, first, translate_hint(Pad::cross),
                  on_buttons ? focus(kPlay) : 0.0f, press_, !ready || game == nullptr || playable || game->remote);
        outline_pill(c, {kLeft + first_width + 28.0f, kButtonsTop, second_width, kButtonHeight}, second,
                     Icon::settings, on_buttons ? focus(kGameSettings) : 0.0f, press_,
                     game != nullptr && !game->remote && game->title_id != 0);
    }
    end_part();

    // ---- what needs the player: the setup, a game that stopped ----
    if (!home_.status.empty())
    {
        begin_part(2, 18.0f);
        const bool offer_safe = home_.launch_failed && home_.last_exists && home_.last_title_id != 0;
        const Rect card{kStatusCard.x, kStatusCard.y, kStatusCard.w, offer_safe ? 204.0f : kStatusCard.h};
        const Color edge = home_.launch_failed ? t.warning : t.accent;
        glass(c, card, t.radius_card, t.panel.with_alpha(0.86f), edge.with_alpha(0.55f), 0.8f);
        notice_block(c, home_.status, card.x + 32.0f, baseline(card.y + 26.0f, 36.0f, t.size_text), t.size_text,
                     36.0f, t.text, card.w - 64.0f, 3, home_.launch_failed);
        if (offer_safe)
        {
            static constexpr Hint kSafe[] = {{Pad::options, TR("Start it in safe mode")}};
            draw_menu_hints(c, kSafe, 1, card.x + 32.0f, card.y + card.h - 34.0f, t.accent_pale, card.w - 64.0f);
        }
        end_part();
    }

    // ---- the row of covers: the recent games, then the rest ----
    if (!shelf_games_.empty())
    {
        begin_part(4, 30.0f);
        list.push_clip({0.0f, kShelfTop - 48.0f, 1920.0f, kShelfCover + 96.0f});
        const float scroll = shelf_.scroll();
        const float ring = shelf_.cursor();
        for (int i = 0; i < static_cast<int>(shelf_games_.size()); ++i)
        {
            const int index = shelf_games_[static_cast<std::size_t>(i)];
            if (index < 0 || index >= count)
                continue;
            const Game &shown = games_[static_cast<std::size_t>(index)];
            const float at = kShelfPitch * static_cast<float>(i);
            const Rect r{kLeft + at - scroll, kShelfTop, kShelfCover, kShelfCover};
            if (r.x > 1920.0f || r.x + r.w < 0.0f)
                continue;
            const float near = tween::clamp01(1.0f - std::fabs(at - ring) / kShelfPitch);
            const float f = home_on_grid_ ? near : 0.0f;
            const float sc = 1.0f + (0.06f * f - 0.035f * press_ * f) * motion();
            list.push_transform(sc, r.x + r.w * 0.5f, r.y + r.h * 0.5f, 0.0f, -12.0f * f * motion());
            focus_ring(c, r, t.radius_card, f);
            if (shown.remote)
                remote_cover(c, shown.cover, r, t.radius_card, 0.4f + 0.5f * f);
            else
                cover(c, shown.cover, r, t.radius_card, 0.4f + 0.5f * f);
            // The game the screen shows, while the focus is on the buttons.
            if (!home_on_grid_ && i == shelf_.selected)
                list.bordered_rect(r, t.radius_card, kWhite.with_alpha(0.0f), 2.0f, t.accent_pale.with_alpha(0.6f));
            if (f > 0.01f)
            {
                list.push_opacity(f);
                Color note_ink = t.copy;
                const std::string note = shown.remote ? remote_state(shown, &note_ink) : std::string{};
                caption(c, r, t.radius_card, shown.name, note, note_ink);
                list.pop_opacity();
            }
            list.pop_transform();
        }
        list.pop_clip();
        end_part();
    }

    // ---- hints ----
    if (screen_ == Screen::home)
    {
        begin_part(6, 0.0f);
        if (home_on_grid_)
        {
            static constexpr Hint kShelfHints[] = {{Pad::cross, TR("Play")},
                                                   {Pad::triangle, TR("Game settings")},
                                                   {Pad::options, TR("Quick settings")},
                                                   {Pad::l1, TR("Sections"), Pad::r1}};
            draw_showcase_footer(c, kShelfHints, 4, home_.system_status);
        }
        else
        {
            static constexpr Hint kHints[] = {{Pad::cross, TR("Select")},
                                              {Pad::triangle, TR("Game settings")},
                                              {Pad::options, TR("Quick settings")},
                                              {Pad::l1, TR("Sections"), Pad::r1}};
            draw_showcase_footer(c, kHints, 4, home_.system_status);
        }
        end_part();
    }
}

// ---------------------------------------------------------------- the Library

bool Launcher::is_favorite(const std::string &file) const
{
    return std::find(favorites_.begin(), favorites_.end(), file) != favorites_.end();
}

void Launcher::set_favorite(const std::string &file, bool favorite)
{
    if (!services_.set_favorite(file, favorite))
    {
        say(tr("Could not save. Please try again."), true);
        cue(Cue::error);
        return;
    }
    favorites_ = services_.favorites();
    cue(Cue::toggle);
    if (library_filter_ == 2)
        filter_library();
}

void Launcher::filter_library()
{
    const int count = static_cast<int>(games_.size());
    std::string keep;
    if (library_pos_ >= 0 && library_pos_ < static_cast<int>(library_view_.size()))
    {
        const int index = library_view_[static_cast<std::size_t>(library_pos_)];
        if (index >= 0 && index < count)
            keep = games_[static_cast<std::size_t>(index)].file;
    }
    const std::string wanted = lower(library_search_);
    const auto passes = [&](int index)
    {
        const Game &game = games_[static_cast<std::size_t>(index)];
        if (library_filter_ == 2 && !is_favorite(game.file))
            return false;
        return wanted.empty() || lower(game.name).find(wanted) != std::string::npos ||
               lower(game.file).find(wanted) != std::string::npos;
    };
    library_view_.clear();
    if (library_filter_ == 1)
    {
        // Recently played: the home screen's recent games, newest first.
        for (const Recent &recent : home_.recents)
            for (int i = 0; i < count; ++i)
                if (games_[static_cast<std::size_t>(i)].file == recent.file)
                {
                    if (passes(i))
                        library_view_.push_back(i);
                    break;
                }
    }
    else
    {
        for (int i = 0; i < count; ++i)
            if (passes(i))
                library_view_.push_back(i);
    }
    const int shown = static_cast<int>(library_view_.size());
    library_pos_ = 0;
    for (int p = 0; p < shown; ++p)
        if (games_[static_cast<std::size_t>(library_view_[static_cast<std::size_t>(p)])].file == keep)
            library_pos_ = p;
    if (shown == 0 && library_header_ < 0)
        library_header_ = library_filter_ + 1; // nothing to focus in the grid: the filters
    sync_showcase_grid(true);
    if (screen_ == Screen::library && shown > 0)
    {
        const int index = library_view_[static_cast<std::size_t>(library_pos_)];
        if (index != library_.selected)
        {
            library_.selected = index;
            refresh_selected_game();
        }
    }
}

void Launcher::sync_showcase_grid(bool snap)
{
    const int shown = static_cast<int>(library_view_.size());
    for (int p = 0; p < shown; ++p)
        if (library_view_[static_cast<std::size_t>(p)] == library_.selected)
            library_pos_ = p;
    library_pos_ = std::clamp(library_pos_, 0, std::max(0, shown - 1));
    const int rows = (shown + kColumns - 1) / kColumns;
    grid_rows_.visible = kGridRowsShown;
    grid_rows_.pitch = kTilePitchY;
    grid_rows_.wrap = false;
    if (snap || grid_rows_.count != rows)
        grid_rows_.reset(rows, library_pos_ / kColumns);
    else
        grid_rows_.select(library_pos_ / kColumns);
    grid_x_.target = static_cast<float>(library_pos_ % kColumns) * kTilePitchX;
    grid_y_.target = static_cast<float>(library_pos_ / kColumns) * kTilePitchY;
    // The highlight glides only where it can be seen doing it.
    if (snap || screen_ != Screen::library || transition_.running)
    {
        grid_x_.snap(grid_x_.target);
        grid_y_.snap(grid_y_.target);
    }
}

void Launcher::poll_search()
{
    if (!search_edit_.valid() || search_edit_.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
        return;
    try
    {
        const std::optional<std::string> typed = search_edit_.get();
        if (!typed)
            return; // cancelled: the search stays as it was
        library_search_ = trimmed(*typed);
        filter_library();
        library_header_ = library_view_.empty() ? 0 : -1;
        cue(Cue::saved);
    }
    catch (const std::exception &error)
    {
        sys::log("search: %s", error.what());
        say(tr("The keyboard could not be opened."), true);
        cue(Cue::error);
    }
}

void Launcher::press_showcase_library(Key key)
{
    const int shown = static_cast<int>(library_view_.size());
    // The selection may have been put on a game from elsewhere (the home screen's game settings):
    // the grid finds it.
    sync_showcase_grid(false);
    const auto go = [&](int pos)
    {
        if (pos == library_pos_ || pos < 0 || pos >= shown)
            return;
        library_pos_ = pos;
        library_.selected = library_view_[static_cast<std::size_t>(pos)];
        message_.clear();
        refresh_selected_game(); // the grid follows (sync_showcase_grid)
        mode_.snap(selected_docked_ ? 0.0f : 1.0f);
        cue(Cue::focus);
    };

    if (library_header_ >= 0)
    {
        switch (key)
        {
        case Key::left:
        case Key::right:
        {
            const int next = std::clamp(library_header_ + (key == Key::right ? 1 : -1), 0, kFilters);
            if (next != library_header_)
            {
                library_header_ = next;
                cue(Cue::focus);
            }
            return;
        }
        case Key::down:
            if (shown > 0)
            {
                library_header_ = -1;
                cue(Cue::focus);
            }
            return;
        case Key::cross:
            press_ = 1.0f;
            if (library_header_ == 0)
            {
                // What to look for, on the system keyboard (it takes a moment; the menu goes on).
                if (!search_edit_.valid())
                {
                    search_edit_stop_ = false;
                    const std::string current = library_search_;
                    search_edit_ = std::async(std::launch::async, [this, current]
                                              { return services_.ask_search(current, search_edit_stop_); });
                    cue(Cue::open);
                }
                return;
            }
            if (library_header_ - 1 != library_filter_)
            {
                library_filter_ = library_header_ - 1;
                library_pos_ = 0;
                library_view_.clear(); // the first game of the new list takes the focus
                filter_library();
                cue(Cue::toggle);
            }
            return;
        case Key::circle:
            if (!library_search_.empty())
            {
                library_search_.clear();
                filter_library();
                cue(Cue::back);
                return;
            }
            go_section(0);
            return;
        default:
            return; // Triangle, Square, Options are about a game, and none has the focus
        }
    }

    switch (key)
    {
    case Key::up:
        if (library_pos_ < kColumns)
        {
            library_header_ = library_filter_ + 1;
            cue(Cue::focus);
        }
        else
            go(library_pos_ - kColumns);
        return;
    case Key::down:
    {
        int next = library_pos_ + kColumns;
        // Down onto a place the last row does not have: its last game.
        if (next >= shown && library_pos_ / kColumns < (shown - 1) / kColumns)
            next = shown - 1;
        go(next);
        return;
    }
    case Key::left:
        if (library_pos_ % kColumns != 0)
            go(library_pos_ - 1);
        return;
    case Key::right:
        if (library_pos_ % kColumns != kColumns - 1 && library_pos_ + 1 < shown)
            go(library_pos_ + 1);
        return;
    case Key::circle:
        // Back: a search first goes, then the Library.
        if (!library_search_.empty())
        {
            library_search_.clear();
            filter_library();
            cue(Cue::back);
            return;
        }
        go_section(0);
        return;
    case Key::cross:
    case Key::triangle:
    case Key::square:
    case Key::options:
        if (shown == 0)
        {
            if (key == Key::cross)
                cue(Cue::error);
            return;
        }
        // What the Library does to the game focused (library.cpp).
        press_library(key);
        if (key == Key::cross)
            launch_from_ = {kLeft + static_cast<float>(library_pos_ % kColumns) * kTilePitchX,
                            kGridTop + static_cast<float>(library_pos_ / kColumns) * kTilePitchY - grid_rows_.scroll(),
                            kTileWidth, kTileHeight};
        return;
    default:
        return;
    }
}

void Launcher::draw_showcase_library(Canvas &c)
{
    gfx::DrawList &list = c.list;
    const Theme &t = th();
    const int count = static_cast<int>(games_.size());
    const int shown = static_cast<int>(library_view_.size());
    const bool active = screen_ == Screen::library && modal_ == Modal::none;

    draw_showcase_art(c, 0.38f);

    // ---- the search, the filters, how many games ----
    {
        const float f = active && library_header_ == 0 ? 1.0f : 0.0f;
        const float here = arrive(0);
        list.push_opacity(here);
        pill(c, kSearch, gfx::mix(t.panel, t.row_focus, f).with_alpha(0.66f), kWhite.with_alpha(0.24f + 0.3f * f));
        focus_ring(c, kSearch, kSearch.h * 0.5f, f);
        const float cy = kSearch.y + kSearch.h * 0.5f;
        magnifier(c, kSearch.x + 42.0f, cy, 34.0f, t.title);
        const bool empty = library_search_.empty();
        text_fit(c, search_edit_.valid() ? std::string{"..."} : empty ? std::string{tr("Search games")} : library_search_,
                 kSearch.x + 80.0f, baseline(kSearch.y, kSearch.h, kFilterText), kFilterText,
                 empty ? t.meta : t.title, kSearch.w - 80.0f - 32.0f);
        float x = kFiltersLeft;
        for (int i = 0; i < kFilters; ++i)
        {
            const char *name = tr(kFilterNames[i]);
            const float w = text_width(c, name, kFilterText) + 72.0f;
            const Rect r{x, kHeaderTop, w, kHeaderHeight};
            const bool chosen = library_filter_ == i;
            const float focused = active && library_header_ == i + 1 ? 1.0f : 0.0f;
            if (chosen)
                accent_pill(c, r, 1.0f);
            else
                pill(c, r, gfx::mix(t.panel, t.row_focus, focused).with_alpha(0.55f),
                     kWhite.with_alpha(0.30f + 0.3f * focused));
            focus_ring(c, r, r.h * 0.5f, focused);
            {
                const Heavy heavy(c, chosen ? 0.6f : 0.0f);
                text(c, name, r.x + r.w * 0.5f, baseline(r.y, r.h, kFilterText), kFilterText,
                     chosen ? t.on_accent : t.title, Align::center);
            }
            x += w + 22.0f;
        }
        const std::string many = fill(shown == 1 ? tr("{0} game") : tr("{0} games"), {std::to_string(shown)});
        {
            const Heavy heavy(c, 0.5f);
            text_shrink(c, many, kRight, baseline(kHeaderTop, kHeaderHeight, 32.0f), 32.0f, t.title,
                        std::max(120.0f, kRight - x - 24.0f), Align::right);
        }
        list.pop_opacity();
    }

    // ---- the grid ----
    if (shown == 0)
    {
        const char *why = count == 0 ? (games_loaded_ || !home_.setup_ready ? tr("No ROM files found.") : tr("Reading...")) :
                          library_filter_ == 2 && library_search_.empty() ?
                                           tr("No favorites yet. Add one from Quick settings.") :
                                           tr("No games match your search.");
        text_shrink(c, why, 960.0f, baseline(520.0f, 40.0f, t.size_text), t.size_text, t.copy, 1400.0f,
                    Align::center);
    }
    else
    {
        list.push_clip(kGridWindow);
        const float scroll = grid_rows_.scroll();
        const int first = std::max(0, grid_rows_.first_row()) * kColumns;
        const int last = std::min(shown - 1, (grid_rows_.last_row() + 2) * kColumns - 1);
        const bool grid_active = active && library_header_ < 0;
        for (int p = first; p <= last; ++p)
        {
            const int index = library_view_[static_cast<std::size_t>(p)];
            if (index < 0 || index >= count)
                continue;
            const Game &game = games_[static_cast<std::size_t>(index)];
            const Rect r{kLeft + static_cast<float>(p % kColumns) * kTilePitchX,
                         kGridTop + static_cast<float>(p / kColumns) * kTilePitchY - scroll, kTileWidth, kTileHeight};
            if (r.y > kGridWindow.y + kGridWindow.h || r.y + r.h < kGridWindow.y)
                continue;
            const float near = tween::clamp01(
                1.0f - (std::fabs(r.x - kLeft - grid_x_.value) + std::fabs(r.y + scroll - kGridTop - grid_y_.value)) /
                           150.0f);
            const float f = grid_active ? near : 0.0f;
            const float here = arrive(p - first, 0.016f);
            list.push_opacity(here);
            const float sc = 1.0f + (0.05f * f - 0.035f * press_ * f) * motion();
            list.push_transform(sc, r.x + r.w * 0.5f, r.y + r.h * 0.5f, 0.0f, (1.0f - here) * 14.0f * motion());
            focus_ring(c, r, t.radius_card, f);
            if (f > 0.01f && !look().high_contrast)
                list.shadow({r.x + 4.0f, r.y + 14.0f, r.w - 8.0f, r.h - 4.0f}, t.radius_card, 26.0f,
                            kBlack.with_alpha(0.5f * f));
            if (game.remote)
                remote_cover(c, game.cover, r, t.radius_card, 0.3f);
            else
                cover_fill(c, game.cover, r, t.radius_card);
            if (f > 0.01f)
            {
                list.push_opacity(f);
                Color note_ink = t.copy;
                const std::string note = game.remote ? remote_state(game, &note_ink) : std::string{};
                caption(c, r, t.radius_card, game.name, note, note_ink);
                list.pop_opacity();
            }
            list.pop_transform();
            list.pop_opacity();
        }
        list.pop_clip();
        // The row under the window fades toward the hints.
        if (!look().high_contrast)
            list.gradient_rect({0.0f, 930.0f, 1920.0f, 72.0f}, 0.0f, t.base.with_alpha(0.0f), t.base.with_alpha(0.9f));
        if (grid_rows_.count > grid_rows_.visible)
            scrollbar(c, grid_rows_, 1876.0f, kGridTop, kTilePitchY * (kGridRowsShown - 1) + kTileHeight);
    }

    // ---- hints, and what the focused game is at the left ----
    if (screen_ == Screen::library)
    {
        std::string about;
        if (shown > 0 && library_header_ < 0 && library_.selected >= 0 && library_.selected < count)
        {
            const Game &game = games_[static_cast<std::size_t>(library_.selected)];
            if (game.remote)
            {
                Color ink = t.copy;
                about = remote_state(game, &ink);
            }
            else
            {
                about = game.format + "  /  " + game.size;
                const std::string addons = game.addons_short.empty() ? game.addons : game.addons_short;
                if (!addons.empty() || game.mods > 0)
                    about += "  /  " + addons_line(addons, game.mods, game.mods_on, true);
                if (selected_changes_ > 0)
                    about += "  /  " + fill(tr("{0} changed"), {std::to_string(selected_changes_)});
            }
        }
        if (library_header_ >= 0)
        {
            static constexpr Hint kHeaderHints[] = {{Pad::cross, TR("Select")},
                                                    {Pad::circle, TR("Back")},
                                                    {Pad::l1, TR("Sections"), Pad::r1}};
            draw_showcase_footer(c, kHeaderHints, 3, about);
        }
        else
        {
            static constexpr Hint kGridHints[] = {{Pad::cross, TR("Play")},
                                                  {Pad::triangle, TR("Game settings")},
                                                  {Pad::options, TR("Quick settings")},
                                                  {Pad::square, TR("Mods")},
                                                  {Pad::l1, TR("Sections"), Pad::r1}};
            draw_showcase_footer(c, kGridHints, 5, about);
        }
    }
}

} // namespace pe::ui
