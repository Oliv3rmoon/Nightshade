// Nightshade - Launcher: a game's Quick settings and Safe Launch. Options on a game (the Library,
// the home screen, the game's settings page) opens a panel with its cover at the left and two
// starts at the top of its rows: Play, and Start in safe mode, which runs the game once with
// settings that start nearly anything and its mods off (headless/settings_store.h, SafeLaunch),
// changing nothing that is saved. Under them the game's video profile (video_profiles.hpp) and the
// video settings players change most, each saved for the game the moment it changes, as its
// settings page saves them. Triangle opens that page for everything else.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "pe/ui/launch_commit.hpp"
#include "pe/ui/launcher.hpp"
#include "pe/ui/video_profiles.hpp"

#include <algorithm>
#include <string>
#include <vector>

namespace pe::ui
{

using audio::Cue;

namespace
{

enum QuickRow : int
{
    quick_play,
    quick_safe,
    quick_profile,
    quick_resolution,
    quick_filter,
    quick_renderer,
    quick_overlay,
    quick_frame_gen, // in a build with frame generation; last
    kQuickRows,
};

constexpr Rect kPanel{360.0f, 120.0f, 1200.0f, 820.0f};
constexpr Rect kArt{408.0f, 176.0f, 300.0f, 300.0f};
constexpr float kInfoWidth = 300.0f;
constexpr float kRowsLeft = 760.0f;
constexpr float kRowsWidth = 752.0f;
constexpr float kRowsTop = 220.0f;
constexpr float kRowPitch = 86.0f;
constexpr float kRowHeight = 78.0f;
constexpr int kRowsShown = 7;
constexpr float kHints = 898.0f;

std::string pick(const std::vector<std::string> &values, int index)
{
    return index >= 0 && index < static_cast<int>(values.size()) ? values[static_cast<std::size_t>(index)] :
                                                                   std::string{"-"};
}

// -1 (Settings' value), then each of `count` choices, a step at a time and round again.
int cycle(int value, int count, int step)
{
    return (value + 1 + step + count + 1) % (count + 1) - 1;
}

} // namespace

int Launcher::quick_rows() const
{
    return services_.frame_gen_state() == 0 && services_.mihawkgen_state() == 0 ? kQuickRows - 1 : kQuickRows;
}

void Launcher::open_quick(int index, bool after_failure)
{
    if (index < 0 || index >= static_cast<int>(games_.size()))
    {
        cue(Cue::error);
        return;
    }
    const Game &game = games_[static_cast<std::size_t>(index)];
    // As the game's settings page: a game that is not on the console has none yet.
    if (game.remote || game.title_id == 0)
    {
        say(game.remote ? tr("Download the game to change its settings.")
                        : tr("This game's settings cannot be saved (no title ID)."),
            true);
        cue(Cue::error);
        return;
    }
    quick_from_page_ = modal_ == Modal::game;
    quick_after_failure_ = after_failure;
    quick_game_ = index;
    if (!quick_from_page_)
    {
        game_settings_ = services_.game_settings(game.title_id);
        game_docked_ = services_.docked(game.title_id);
    }
    quick_rows_.visible = kRowsShown;
    quick_rows_.pitch = kRowPitch;
    quick_rows_.reset(quick_rows(), after_failure ? quick_safe : quick_play);
    modal_ = modal_shown_ = Modal::quick;
    message_.clear();
    popup_.clear();
    cue(quick_from_page_ ? Cue::open : Cue::modal_open);
}

void Launcher::launch_safe(const Game &game)
{
    if (game.title_id == 0 || play_file(game).empty())
    {
        cue(Cue::error);
        return;
    }
    launch(play_file(game), game.name, game.cover, game.title_id);
}

void Launcher::press_quick(Key key)
{
    if (quick_game_ < 0 || quick_game_ >= static_cast<int>(games_.size()))
    {
        close_modal();
        return;
    }
    const Game game = games_[static_cast<std::size_t>(quick_game_)];
    const auto back = [&]
    {
        if (quick_from_page_)
        {
            // Back to the game's settings page, on the row it was on.
            modal_ = modal_shown_ = Modal::game;
            option_ = game_rows_.selected;
            message_.clear();
            cue(Cue::back);
            return;
        }
        close_modal();
        // What the Library and the home screen say of the game follows what was changed here.
        refresh_selected_game();
        home_session_ = session_line(home_.last_title_id);
    };
    switch (key)
    {
    case Key::circle:
        back();
        return;
    case Key::up:
    case Key::down:
        if (quick_rows_.move(key == Key::down ? 1 : -1))
        {
            message_.clear();
            cue(Cue::focus);
        }
        return;
    case Key::triangle:
        // Every setting of the game: its settings page.
        if (quick_from_page_)
        {
            back();
            return;
        }
        modal_ = Modal::none;
        library_.reset(static_cast<int>(games_.size()), quick_game_);
        refresh_selected_game();
        sync_grid(true);
        if (screen_ == Screen::home && !th().minimal_home)
            open_game_settings_from_home(game.file);
        else
            press_library(Key::triangle);
        return;
    case Key::left:
    case Key::right:
    case Key::cross:
        break;
    default:
        return;
    }
    const int row = std::clamp(quick_rows_.selected, 0, quick_rows() - 1);
    if (row == quick_play || row == quick_safe)
    {
        if (key != Key::cross)
            return;
        if (!home_.setup_ready)
        {
            cue(Cue::error);
            return;
        }
        // The cover flies from the panel as the game starts; the page and the panel close.
        modal_ = Modal::none;
        launch_from_ = kArt;
        if (row == quick_safe)
            launch_safe(game);
        else
            launch(play_file(game), game.name, game.cover);
        // It did not start (the room, the file): the panel stays, saying why.
        if (selected_game_.empty() && modal_ == Modal::none && quick_game_ < static_cast<int>(games_.size()) &&
            games_[static_cast<std::size_t>(quick_game_)].file == game.file)
            modal_ = modal_shown_ = Modal::quick;
        return;
    }
    const int step = key == Key::left ? -1 : 1;
    GameSettings next = game_settings_;
    std::string said;
    switch (row)
    {
    case quick_profile:
    {
        const int now = game_profile_of(game_settings_, prefs_, game.title_id);
        const int tier = now == kCustomProfile ? (step < 0 ? kVideoProfiles - 1 : 0) : cycle(now, kVideoProfiles, step);
        next = with_profile(game_settings_, game.title_id, tier);
        if (tier >= 0)
            said = fill(tr("{0}: {1}. Applies on next launch."),
                        {profile_name(tier), profile_summary(game.title_id, tier)});
        break;
    }
    case quick_resolution:
        next.resolution = cycle(next.resolution, static_cast<int>(services_.resolution_labels().size()), step);
        break;
    case quick_filter:
        next.filter = cycle(next.filter, static_cast<int>(services_.filter_labels().size()), step);
        break;
    case quick_renderer:
        next.renderer = cycle(next.renderer, 2, step);
        break;
    case quick_overlay:
        next.hud = cycle(next.hud, 2, step);
        break;
    default:
    {
        // Frame generation: only with the method chosen ready (its file in place).
        const int method = next.frame_gen_method >= 0 ? next.frame_gen_method : prefs_.frame_gen_method;
        if ((method == 0 ? services_.frame_gen_state() : services_.mihawkgen_state()) != 2)
        {
            say(method == 0 ? tr("Frame generation needs Lossless.dll, from Lossless Scaling, in the lossless folder.")
                            : tr("Frame generation needs MihawkGen weights (network.fgw)."),
                true);
            cue(Cue::error);
            return;
        }
        next.frame_gen = cycle(next.frame_gen, 2, step);
        break;
    }
    }
    const bool saved = services_.set_game_settings(game.title_id, next);
    if (saved)
        game_settings_ = next;
    say(!saved ? std::string{tr("Could not save. Please try again.")} :
        !said.empty() ? said : std::string{tr("Saved for this game. Applies on next launch.")},
        !saved);
    cue(saved ? Cue::toggle : Cue::error);
}

void Launcher::draw_quick(Canvas &c, float open)
{
    if (quick_game_ < 0 || quick_game_ >= static_cast<int>(games_.size()))
        return;
    gfx::DrawList &list = c.list;
    const Theme &t = th();
    const Game &game = games_[static_cast<std::size_t>(quick_game_)];
    list.push_opacity(open);
    list.push_transform(1.0f - 0.03f * (1.0f - open) * motion(), 960.0f, 540.0f, 0.0f,
                        (1.0f - open) * 26.0f * motion());
    glass(c, kPanel, t.radius_panel, t.panel.with_alpha(0.97f), t.panel_edge.with_alpha(0.66f), 1.6f);

    // ---- the game, what it starts with, and what the focused row means ----
    if (!look().high_contrast)
        list.shadow({kArt.x - 16.0f, kArt.y, kArt.w + 32.0f, kArt.h + 30.0f}, 40.0f, 80.0f,
                    t.highlight.with_alpha(0.12f));
    cover(c, game.cover, kArt, t.radius_card, 1.4f);
    float y = kArt.y + kArt.h + 24.0f;
    {
        const Heavy heavy(c);
        const int lines = text_block(c, game.name, kArt.x, baseline(y, 40.0f, t.size_lead), t.size_lead, 40.0f,
                                     t.title, kInfoWidth, 2, 0.8f);
        y += 40.0f * static_cast<float>(std::max(1, lines)) + 10.0f;
    }
    const int rows = quick_rows();
    const int focused = std::clamp(quick_rows_.selected, 0, rows - 1);
    const bool docked = game_docked_;
    std::string about;
    bool warning = false;
    if (!message_.empty())
    {
        about = message_;
        warning = message_warning_;
    }
    else
    {
        switch (focused)
        {
        case quick_play:
            about = fill(tr("Starts with: {0}"), {session_line(game_settings_, docked)});
            break;
        case quick_safe:
            about = std::string{quick_after_failure_ ? tr("It stopped last time.") : ""} +
                    (quick_after_failure_ ? " " : "") +
                    tr("This start only: 1x, Bilinear, compressed ASTC, a 1080p output at 60 Hz, no frame "
                       "generation or post-processing, Performance at its defaults, 4 GiB, mods and cheats off. "
                       "Nothing you saved changes.");
            warning = quick_after_failure_;
            break;
        case quick_profile:
            about = profile_about(game.title_id, game_profile_of(game_settings_, prefs_, game.title_id));
            break;
        case quick_resolution:
            about = tr("How many pixels the game is drawn with: above 1x is sharper and needs more of the GPU and "
                       "its memory.");
            break;
        case quick_filter:
            about = tr("How the game's picture is scaled to the output.");
            break;
        case quick_renderer:
            about = tr("Vulkan is faster and recommended; OpenGL is there for a game that needs it.");
            break;
        case quick_overlay:
            about = tr("Frame rate, frame time, CPU, GPU and memory over the game. Touchpad + R1 shows or hides it "
                       "while playing.");
            break;
        default:
            about = tr("Frames made between the game's own, up to the output's rate (Vulkan).");
            break;
        }
    }
    notice_block(c, about, kArt.x, baseline(y, 30.0f, t.size_small), t.size_small, 30.0f,
                 warning ? t.warning : t.copy, kInfoWidth, 8, warning);

    // ---- its rows ----
    kicker(c, tr("Quick settings"), kRowsLeft, baseline(160.0f, 34.0f, t.size_small), t.accent, kRowsWidth);
    list.push_clip({kRowsLeft - 24.0f, kRowsTop - 6.0f, kRowsWidth + 48.0f,
                    kRowPitch * (kRowsShown - 1) + kRowHeight + 12.0f});
    const auto row_top = [&](int row) { return kRowsTop + kRowPitch * static_cast<float>(row) - quick_rows_.scroll(); };
    for (int row = quick_rows_.first_row(); row <= quick_rows_.last_row() && row < rows; ++row)
    {
        list.push_opacity(quick_rows_.row_alpha(row, kRowHeight));
        plate_rest(c, row_plate(), {kRowsLeft, row_top(row), kRowsWidth, kRowHeight});
        list.pop_opacity();
    }
    plate_focus(c, row_plate(), {kRowsLeft, kRowsTop + quick_rows_.cursor() - quick_rows_.scroll(), kRowsWidth,
                                 kRowHeight},
                1.0f);
    const std::vector<std::string> off_on = {tr("Off"), tr("On")};
    const std::vector<std::string> renderers = {"OpenGL", "Vulkan"};
    const int method = game_settings_.frame_gen_method >= 0 ? game_settings_.frame_gen_method : prefs_.frame_gen_method;
    const bool frame_gen_locked =
        (method == 0 ? services_.frame_gen_state() : services_.mihawkgen_state()) != 2;
    for (int row = quick_rows_.first_row(); row <= quick_rows_.last_row() && row < rows; ++row)
    {
        const float top = row_top(row);
        const bool on = row == focused;
        const float line = baseline(top, kRowHeight, t.size_text);
        list.push_opacity(quick_rows_.row_alpha(row, kRowHeight));
        if (row == quick_play || row == quick_safe)
        {
            // The two starts: their name, and the button that starts them.
            const Pad cross = translate_hint(Pad::cross);
            draw_pad(c, cross, kRowsLeft + kRowsWidth - 24.0f - pad_width(cross, 34.0f), top + kRowHeight * 0.5f,
                     34.0f, on ? 1.0f : 0.5f);
            // A controller for Play; a lock for the safe start, which keeps what is saved as it is.
            icon(c, row == quick_safe ? Icon::lock : Icon::controls, kRowsLeft + 44.0f, top + kRowHeight * 0.5f,
                 30.0f, row == quick_safe ? (quick_after_failure_ ? t.warning : t.label) : t.accent);
            const Heavy heavy(c, row == quick_play ? 0.6f : 0.0f);
            text_shrink(c, row == quick_play ? tr("Play") : tr("Start in safe mode"), kRowsLeft + 76.0f, line,
                        t.size_text, on ? t.title : t.value, kRowsWidth - 76.0f - 96.0f);
            list.pop_opacity();
            continue;
        }
        std::string name;
        std::string value;
        int own = -1;
        bool locked = false;
        switch (row)
        {
        case quick_profile:
        {
            name = tr("Video profile");
            own = game_profile_of(game_settings_, prefs_, game.title_id);
            value = own >= 0 ? profile_name(own) : fill(tr("Default ({0})"), {profile_name(profile_of(prefs_))});
            break;
        }
        case quick_resolution:
        {
            name = tr("Resolution");
            own = game_settings_.resolution;
            const std::string usual = pick(services_.resolution_labels(), prefs_.resolution);
            value = own >= 0 ? pick(services_.resolution_labels(), own)
                             : fill(tr("Default ({0})"), {usual.substr(0, usual.find(' '))});
            break;
        }
        case quick_filter:
            name = tr("Upscaling filter");
            own = game_settings_.filter;
            value = own >= 0 ? pick(services_.filter_labels(), own)
                             : fill(tr("Default ({0})"), {pick(services_.filter_labels(), prefs_.filter)});
            break;
        case quick_renderer:
            name = tr("Renderer");
            own = game_settings_.renderer;
            value = own >= 0 ? pick(renderers, own) : fill(tr("Default ({0})"), {pick(renderers, prefs_.renderer)});
            break;
        case quick_overlay:
            name = tr("FPS overlay");
            own = game_settings_.hud;
            value = own >= 0 ? pick(off_on, own) : fill(tr("Default ({0})"), {pick(off_on, prefs_.hud ? 1 : 0)});
            break;
        default:
            name = tr("Frame generation");
            own = game_settings_.frame_gen;
            locked = frame_gen_locked;
            value = locked ? std::string{tr("Unavailable")} :
                    own >= 0 ? pick(off_on, own) : fill(tr("Default ({0})"), {pick(off_on, prefs_.frame_gen ? 1 : 0)});
            break;
        }
        if (locked)
            list.push_opacity(0.38f);
        const float taken = chooser(c, value, kRowsLeft + kRowsWidth - 24.0f, line, on ? 1.0f : 0.0f,
                                    own >= 0 ? t.accent : t.meta);
        // A dot before the name of what the game has of its own, as on its settings page.
        const float lead = own >= 0 ? 22.0f : 0.0f;
        if (own >= 0)
            list.circle(kRowsLeft + 34.0f, top + kRowHeight * 0.5f, 4.0f, t.accent);
        text_shrink(c, name, kRowsLeft + 28.0f + lead, line, t.size_text, on ? t.title : t.value,
                    kRowsWidth - 28.0f - lead - 24.0f - taken - 28.0f);
        if (locked)
            list.pop_opacity();
        list.pop_opacity();
    }
    list.pop_clip();
    scrollbar(c, quick_rows_, kRowsLeft + kRowsWidth + 14.0f, kRowsTop, kRowPitch * (kRowsShown - 1) + kRowHeight);

    // ---- hints ----
    if (focused == quick_play || focused == quick_safe)
    {
        static constexpr Hint kStart[] = {{Pad::updown, TR("Select")},
                                          {Pad::cross, TR("Start")},
                                          {Pad::triangle, TR("All settings")},
                                          {Pad::circle, TR("Back")}};
        draw_menu_hints(c, kStart, 4, kRowsLeft, kHints, t.copy, kRowsWidth);
    }
    else
    {
        static constexpr Hint kChange[] = {{Pad::updown, TR("Select")},
                                           {Pad::leftright, TR("Change")},
                                           {Pad::triangle, TR("All settings")},
                                           {Pad::circle, TR("Back")}};
        draw_menu_hints(c, kChange, 4, kRowsLeft, kHints, t.copy, kRowsWidth);
    }
    list.pop_transform();
    list.pop_opacity();
}

} // namespace pe::ui
