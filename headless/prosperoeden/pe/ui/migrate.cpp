// Nightshade - Launcher: ProsperoEden's data at the first start of Nightshade's own title ID
// (headless/migrate.h). Asked once, before anything else, in the update dialog's style: Move (the
// default, at once, the games too), Copy (both apps keep working: it needs room, and the games stay
// where they are) or Start fresh, each explained under the buttons as it is chosen. A copy fills a
// ring as it goes, and Circle stops it. When it is done Nightshade starts again, reading everything
// from its new place; a failure, a cancel or a drive without room says so, and that nothing was
// changed, and Back asks again.
// SPDX-FileCopyrightText: Copyright 2026 Oliv3rmoon
// SPDX-License-Identifier: GPL-3.0-or-later

#include "pe/ui/launcher.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

namespace pe::ui
{

using audio::Cue;

namespace
{

constexpr Rect kPanel{500.0f, 150.0f, 920.0f, 780.0f};
constexpr float kCenterX = 960.0f;
constexpr float kTextWidth = kPanel.w - 120.0f;
// The badge and the ring, under the top of the panel.
constexpr float kRingY = 288.0f;
constexpr float kRingRadius = 80.0f;
constexpr float kRingWidth = 9.0f;
// The three answers side by side; a single Back alone in the middle.
constexpr float kButtonsTop = 584.0f;
constexpr float kButtonHeight = 76.0f;
constexpr float kButtonsLeft = kPanel.x + 60.0f;
constexpr float kButtonsWidth = kPanel.w - 120.0f;
constexpr float kButtonGap = 16.0f;
constexpr float kBackWidth = 320.0f;
// What the answer chosen does, under the buttons.
constexpr float kAboutTop = 690.0f;
constexpr float kHints = kPanel.y + kPanel.h - 54.0f;
// How often the services are asked while the dialog shows, and how long "starts again" shows.
constexpr float kPollSeconds = 0.1f;
constexpr float kRestartSeconds = 3.0f;
constexpr float kPi = 3.14159265f;

std::string size_text(std::uint64_t bytes)
{
    char text[32];
    if (bytes >= (1ull << 30))
        std::snprintf(text, sizeof(text), "%.1f GB", static_cast<double>(bytes) / 1073741824.0);
    else
        std::snprintf(text, sizeof(text), "%.1f MB", static_cast<double>(bytes) / 1048576.0);
    return text;
}

Rect button_rect(int count, float index)
{
    const float width = (kButtonsWidth - kButtonGap * static_cast<float>(count - 1)) / static_cast<float>(count);
    return {kButtonsLeft + (width + kButtonGap) * index, kButtonsTop, width, kButtonHeight};
}

// Wrapped text, each line centred; at most `most` lines. A translation that needs more gets smaller
// letters first (down to kShrink), as text_block's do, then the last line ends in an ellipsis.
// Where the next line would be.
float centred_block(Canvas &c, std::string_view value, float top, float pitch, float size, Color color, int most)
{
    const gfx::Font &font = *c.fonts.font;
    float drawn = size;
    std::vector<std::string> lines = font.wrap(value, text_size(drawn), kTextWidth);
    while (static_cast<int>(lines.size()) > most && drawn > size * kShrink + 0.01f)
    {
        drawn = std::max(size * kShrink, drawn * 0.94f);
        lines = font.wrap(value, text_size(drawn), kTextWidth);
    }
    const int count = std::min(static_cast<int>(lines.size()), most);
    for (int i = 0; i < count; ++i)
    {
        const std::size_t at = static_cast<std::size_t>(i);
        const float baseline_y = baseline(top, pitch, size);
        if (i == count - 1 && static_cast<int>(lines.size()) > count)
        {
            // The last line that fits swallows the next one, so its ellipsis shows more follows.
            const std::string joined =
                lines[at] + (gfx::joins_without_space(lines[at], lines[at + 1]) ? "" : " ") + lines[at + 1];
            text_fit(c, joined, kCenterX, baseline_y, drawn, color, kTextWidth, Align::center);
        }
        else
        {
            text(c, lines[at], kCenterX, baseline_y, drawn, color, Align::center);
        }
        top += pitch;
    }
    return top;
}

} // namespace

void Launcher::update_migration(float dt)
{
    migrate_choice_x_.target = static_cast<float>(migrate_choice_);
    migrate_choice_x_.update(dt, th().cursor_spring);
    migrate_time_ += dt;
    migrate_spin_ += dt;
    // Whether there is a question, asked once per launcher (the services know at once). It comes
    // before anything else: the update offer waits for it.
    if (!migration_looked_)
    {
        migration_looked_ = true;
        migration_ = services_.migration();
        migrate_waiting_ = migration_.phase == MigrationPhase::asking;
    }
    if (migrate_waiting_ && modal_ == Modal::none && modal_shown_ == Modal::none && !transition_.running &&
        selected_game_.empty() && intro_ > 1.2f)
    {
        migrate_waiting_ = false;
        migrate_choice_ = 0; // Move
        migrate_choice_x_.snap(0.0f);
        migrate_fraction_.snap(0.0f);
        migrate_time_ = 0.0f;
        rail_focus_ = false;
        modal_ = modal_shown_ = Modal::migrate;
        message_.clear();
        cue(Cue::notify);
    }
    if (modal_ != Modal::migrate)
        return;
    migrate_poll_ += dt;
    if (migrate_poll_ >= kPollSeconds)
    {
        migrate_poll_ = 0.0f;
        const MigrationPhase before = migration_.phase;
        migration_ = services_.migration();
        if (migration_.phase != before)
        {
            migrate_time_ = 0.0f;
            switch (migration_.phase)
            {
            case MigrationPhase::none: // Start fresh: nothing more to ask
                modal_ = Modal::none;
                cue(Cue::modal_close);
                return;
            case MigrationPhase::done:
                migrate_fraction_.target = 1.0f;
                cue(Cue::saved);
                break;
            case MigrationPhase::failed:
            case MigrationPhase::no_room:
                cue(Cue::error);
                break;
            case MigrationPhase::cancelled:
                cue(Cue::back);
                break;
            default:
                break;
            }
        }
    }
    if (migration_.phase == MigrationPhase::working && migration_.choice == MigrationChoice::copy && migration_.bytes > 0)
        migrate_fraction_.target = std::min(1.0f, static_cast<float>(static_cast<double>(migration_.done_bytes) /
                                                                     static_cast<double>(migration_.bytes)));
    migrate_fraction_.update(dt, 9.0f);
    // Moved or copied: once that has been read, a fresh process reads everything from its new place.
    if (migration_.phase == MigrationPhase::done && migrate_time_ >= kRestartSeconds && !migrate_restarting_)
    {
        migrate_restarting_ = true;
        if (!services_.restart_app())
            done_ = true; // refused: the app closes, and the next start reads it all
    }
}

void Launcher::press_migrate(Key key)
{
    switch (migration_.phase)
    {
    case MigrationPhase::asking:
    {
        if (key == Key::left || key == Key::right)
        {
            const int choice = std::clamp(migrate_choice_ + (key == Key::right ? 1 : -1), 0, 2);
            if (choice != migrate_choice_)
            {
                migrate_choice_ = choice;
                cue(Cue::focus);
            }
            return;
        }
        if (key != Key::cross)
            return; // it is answered, not closed
        static constexpr MigrationChoice kChoices[] = {MigrationChoice::move, MigrationChoice::copy,
                                                       MigrationChoice::fresh};
        const MigrationChoice choice = kChoices[std::clamp(migrate_choice_, 0, 2)];
        std::string reason;
        migrate_time_ = 0.0f;
        migrate_fraction_.snap(0.0f);
        if (services_.start_migration(choice, &reason))
        {
            migration_.phase = MigrationPhase::working;
            migration_.choice = choice;
            migration_.done_bytes = 0;
            press_ = 1.0f;
            cue(Cue::select);
            return;
        }
        migration_.phase = MigrationPhase::failed;
        migration_.choice = choice;
        migration_.error = reason;
        migration_.intact = true;
        cue(Cue::error);
        return;
    }
    case MigrationPhase::working:
        if (key == Key::circle && migration_.choice == MigrationChoice::copy && !migrate_cancelling_)
        {
            services_.cancel_migration();
            migrate_cancelling_ = true;
            cue(Cue::back);
        }
        return;
    case MigrationPhase::cancelled:
    case MigrationPhase::failed:
    case MigrationPhase::no_room:
        if (key == Key::cross || key == Key::circle)
        {
            // Back to the question, on the answer that was chosen.
            services_.finish_migration();
            migration_ = services_.migration();
            migrate_cancelling_ = false;
            migrate_time_ = 0.0f;
            cue(Cue::back);
        }
        return;
    case MigrationPhase::done:
    case MigrationPhase::none:
        return;
    }
}

void Launcher::draw_migrate(Canvas &c, float open)
{
    gfx::DrawList &list = c.list;
    list.push_opacity(open);
    list.push_transform(1.0f - 0.04f * (1.0f - open) * motion(), kCenterX, 540.0f, 0.0f,
                        (1.0f - open) * 30.0f * motion());
    glass(c, kPanel, 28.0f, th().panel.with_alpha(0.97f), th().accent.with_alpha(0.38f), 1.8f);

    const MigrationStatus &status = migration_;
    const float t = migrate_time_;
    const float breathe = 0.5f + 0.5f * std::sin(c.time * 2.4f);
    const bool trouble = status.phase == MigrationPhase::failed || status.phase == MigrationPhase::no_room ||
                         status.phase == MigrationPhase::cancelled;
    const Color accent = trouble ? th().warning : th().accent;
    const bool copying = status.choice == MigrationChoice::copy;

    list.shadow({kCenterX - kRingRadius, kRingY - kRingRadius, kRingRadius * 2.0f, kRingRadius * 2.0f}, kRingRadius,
                60.0f, accent.with_alpha(0.10f + 0.08f * breathe));
    list.circle(kCenterX, kRingY, kRingRadius - kRingWidth, th().base.with_alpha(0.55f));
    list.ring(kCenterX, kRingY, kRingRadius, kRingWidth, th().panel_edge.with_alpha(0.22f));

    const auto centred = [&](std::string_view value, float top, float line, float size, Color color)
    { text_shrink(c, value, kCenterX, baseline(top, line, size), size, color, kTextWidth, Align::center); };
    const auto back_button = [&]()
    {
        const Rect r{kCenterX - kBackWidth * 0.5f, kButtonsTop + 60.0f, kBackWidth, kButtonHeight};
        plate_rest(c, row_plate(), r);
        plate_focus(c, row_plate(), r, 1.0f - 0.25f * press_);
        text_shrink(c, tr("Back"), r.x + r.w * 0.5f, baseline(r.y, r.h, th().size_text), th().size_text, th().title,
                    r.w - 32.0f, Align::center);
        static constexpr Hint kBackHints[] = {{Pad::cross, TR("Back")}};
        draw_menu_hints(c, kBackHints, 1, kPanel.x + 52.0f, kHints, th().copy, kPanel.w - 104.0f);
    };

    switch (status.phase)
    {
    case MigrationPhase::asking:
    case MigrationPhase::none:
    {
        // The ring closes once as the dialog rises, around the folders the data lives in.
        arc(list, kCenterX, kRingY, kRingRadius, kRingWidth, -kPi * 0.5f, 2.0f * kPi * tween::cubic_out(t / 0.9f),
            accent);
        const float settle = (1.0f - tween::back_out(t / 0.7f)) * -20.0f * motion();
        icon(c, Icon::files, kCenterX, kRingY + settle, 76.0f, accent);

        centred(tr("Your ProsperoEden data"), 388.0f, 56.0f, th().size_heading, th().title);
        centred_block(c, fill(tr("Nightshade now keeps its data in {0}. What you had before is in {1}."),
                              {status.to, status.from}),
                      452.0f, 34.0f, th().size_small, th().value, 3);

        static constexpr const char *kLabels[] = {TR("Move"), TR("Copy"), TR("Start fresh")};
        for (int i = 0; i < 3; ++i)
            plate_rest(c, row_plate(), button_rect(3, static_cast<float>(i)));
        plate_focus(c, row_plate(), button_rect(3, migrate_choice_x_.value), 1.0f - 0.25f * press_);
        for (int i = 0; i < 3; ++i)
        {
            const Rect r = button_rect(3, static_cast<float>(i));
            text_shrink(c, tr(kLabels[i]), r.x + r.w * 0.5f, baseline(r.y, r.h, th().size_text), th().size_text,
                        i == migrate_choice_ ? th().title : th().value, r.w - 32.0f, Align::center);
        }

        // What the answer chosen does.
        float y = kAboutTop;
        if (migrate_choice_ == 0)
        {
            centred_block(c, fill(tr("Instant, with your games: {0} becomes {1}. The official ProsperoEden app no "
                                     "longer sees it."),
                                  {status.from, status.to}),
                          y, 32.0f, th().size_small, th().copy, 3);
        }
        else if (migrate_choice_ == 1)
        {
            y = centred_block(c,
                              status.measured ?
                                  fill(tr("Both apps keep working: settings, keys, firmware, saves, mods and shader "
                                          "cache are copied. Needs {0} free."),
                                       {size_text(status.bytes)}) :
                                  std::string{tr("Both apps keep working: settings, keys, firmware, saves, mods and "
                                                 "shader cache are copied. Needs free space for them.")},
                              y, 32.0f, th().size_small, th().copy, 3);
            if (!status.games.empty())
                centred_block(c,
                              fill(tr("Games and new downloads stay in {0}. You can still choose another game files "
                                      "folder in Settings."),
                                   {status.games}),
                              y + 4.0f, 32.0f, th().size_small, th().accent_pale, 2);
        }
        else
        {
            centred_block(c, fill(tr("Nightshade starts empty. {0} stays as it is."), {status.from}), y, 32.0f,
                          th().size_small, th().copy, 3);
        }
        static constexpr Hint kAskHints[] = {{Pad::leftright, TR("Navigate")}, {Pad::cross, TR("Select")}};
        draw_menu_hints(c, kAskHints, 2, kPanel.x + 52.0f, kHints, th().copy, kPanel.w - 104.0f);
        break;
    }
    case MigrationPhase::working:
    {
        const bool measured = copying && status.bytes > 0 && !migrate_cancelling_;
        const float share = tween::clamp01(migrate_fraction_.value);
        if (measured)
        {
            arc(list, kCenterX, kRingY, kRingRadius, kRingWidth, -kPi * 0.5f, 2.0f * kPi * share, accent);
            const float a = -kPi * 0.5f + 2.0f * kPi * share;
            list.circle(kCenterX + kRingRadius * std::cos(a), kRingY + kRingRadius * std::sin(a), kRingWidth * 0.9f,
                        th().accent_pale.with_alpha(0.9f));
            const int percent = std::min(100, static_cast<int>(share * 100.0f + 0.5f));
            text(c, fill(tr("{0}%"), {std::to_string(percent)}), kCenterX,
                 baseline(kRingY - 30.0f, 60.0f, th().size_display), th().size_display, th().title, Align::center);
        }
        else
        {
            // Without a share (a move, a copy that is stopping): an arc that turns, and three dots.
            const float turn = migrate_spin_ * 4.2f;
            const float sweep = kPi * (0.55f + 0.45f * std::sin(migrate_spin_ * 2.1f));
            arc(list, kCenterX, kRingY, kRingRadius, kRingWidth, turn, sweep,
                migrate_cancelling_ ? th().warning : accent);
            for (int i = 0; i < 3; ++i)
            {
                const float wave = 0.5f + 0.5f * std::sin(migrate_spin_ * 6.0f - static_cast<float>(i) * 0.9f);
                list.circle(kCenterX - 26.0f + 26.0f * static_cast<float>(i), kRingY - wave * 8.0f * motion(), 7.0f,
                            th().accent_pale.with_alpha(0.35f + 0.6f * wave));
            }
        }
        centred(migrate_cancelling_ ? tr("Cancelling") : copying ? tr("Copying your data") : tr("Moving your data"),
                398.0f, 52.0f, th().size_heading, th().title);
        centred(status.from + "  >  " + status.to, 456.0f, 32.0f, th().size_small, th().copy);
        if (measured)
        {
            const Rect bar{kPanel.x + 120.0f, 516.0f, kPanel.w - 240.0f, 8.0f};
            list.rounded_rect(bar, 4.0f, th().panel_edge.with_alpha(0.22f));
            list.rounded_rect({bar.x, bar.y, std::max(bar.h, bar.w * share), bar.h}, 4.0f, accent);
            centred(size_text(status.done_bytes) + "  /  " + size_text(status.bytes), 540.0f, 32.0f,
                    th().size_small, th().accent_pale);
            static constexpr Hint kWorkingHints[] = {{Pad::circle, TR("Cancel")}};
            draw_menu_hints(c, kWorkingHints, 1, kPanel.x + 52.0f, kHints, th().copy, kPanel.w - 104.0f);
        }
        break;
    }
    case MigrationPhase::done:
    {
        // The ring closes, then a tick draws itself and the badge pops.
        arc(list, kCenterX, kRingY, kRingRadius, kRingWidth, -kPi * 0.5f, 2.0f * kPi, accent);
        const float pop = tween::back_out(t / 0.5f);
        list.push_transform(0.6f + 0.4f * pop, kCenterX, kRingY, 0.0f, 0.0f);
        list.circle(kCenterX, kRingY, kRingRadius - kRingWidth - 10.0f, accent.with_alpha(0.16f));
        const float stroke = tween::cubic_out((t - 0.15f) / 0.45f);
        const float x0 = kCenterX - 30.0f, y0 = kRingY + 2.0f;
        const float x1 = kCenterX - 8.0f, y1 = kRingY + 24.0f;
        const float x2 = kCenterX + 34.0f, y2 = kRingY - 24.0f;
        const float first = tween::clamp01(stroke / 0.4f);
        const float second = tween::clamp01((stroke - 0.4f) / 0.6f);
        if (first > 0.0f)
            list.line(x0, y0, x0 + (x1 - x0) * first, y0 + (y1 - y0) * first, 9.0f, accent);
        if (second > 0.0f)
            list.line(x1, y1, x1 + (x2 - x1) * second, y1 + (y2 - y1) * second, 9.0f, accent);
        list.pop_transform();

        centred(tr("Your data is ready"), 398.0f, 56.0f, th().size_heading, th().title);
        centred_block(c, fill(tr("It is now in {0}. Nightshade starts again."), {status.to}), 462.0f, 34.0f,
                      th().size_text, th().value, 2);
        if (copying && !status.games.empty())
            centred_block(c,
                          fill(tr("Games and new downloads stay in {0}. You can still choose another game files "
                                  "folder in Settings."),
                               {status.games}),
                          548.0f, 32.0f, th().size_small, th().copy, 2);
        // The time until it starts again.
        const float left = 1.0f - tween::clamp01(t / kRestartSeconds);
        list.rounded_rect({kPanel.x + 120.0f, 660.0f, (kPanel.w - 240.0f) * left, 4.0f}, 2.0f, accent.with_alpha(0.7f));
        break;
    }
    case MigrationPhase::failed:
    case MigrationPhase::cancelled:
    case MigrationPhase::no_room:
    {
        arc(list, kCenterX, kRingY, kRingRadius, kRingWidth, -kPi * 0.5f, 2.0f * kPi * tween::cubic_out(t / 0.6f),
            accent);
        // A gentle shake as it arrives, and an exclamation mark.
        const float shake = std::sin(t * 38.0f) * 10.0f * (1.0f - tween::clamp01(t / 0.45f)) * motion();
        list.line(kCenterX + shake, kRingY - 38.0f, kCenterX + shake, kRingY + 12.0f, 10.0f, accent);
        list.circle(kCenterX + shake, kRingY + 36.0f, 7.0f, accent);

        float y = 462.0f;
        if (status.phase == MigrationPhase::no_room)
        {
            centred(tr("Not enough free space"), 398.0f, 56.0f, th().size_heading, th().title);
            y = centred_block(c,
                              status.room_known ?
                                  fill(tr("Copying needs {0}; the console has {1} free. Nothing was changed."),
                                       {size_text(status.needed), size_text(status.free)}) :
                                  fill(tr("The console ran out of space (copying needs about {0}). What was copied "
                                          "was removed."),
                                       {size_text(std::max(status.needed, status.bytes))}),
                              y, 34.0f, th().size_small, th().value, 3);
            centred_block(c, tr("Moving needs no free space."), y, 34.0f, th().size_small, th().accent_pale, 1);
        }
        else if (status.phase == MigrationPhase::cancelled)
        {
            centred(tr("Copy cancelled"), 398.0f, 56.0f, th().size_heading, th().title);
            centred_block(c, tr("Nothing was changed: both folders are as they were."), y, 34.0f, th().size_small,
                          th().value, 2);
        }
        else
        {
            centred(copying ? tr("Your data could not be copied") : tr("Your data could not be moved"), 398.0f, 56.0f,
                    th().size_heading, th().title);
            y = centred_block(c,
                              status.intact ? std::string{tr("Nothing was changed: both folders are as they were.")} :
                                              std::string{tr("Not everything could be put back. The details below "
                                                             "say where it is.")},
                              y, 34.0f, th().size_small, th().value, 2);
            if (!status.error.empty())
                centred_block(c, status.error, y + 6.0f, 30.0f, th().size_small, th().copy.with_alpha(0.85f), 3);
        }
        back_button();
        break;
    }
    }
    list.pop_transform();
    list.pop_opacity();
}

} // namespace pe::ui
