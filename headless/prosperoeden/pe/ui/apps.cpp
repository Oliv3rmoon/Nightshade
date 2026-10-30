// Nightshade - Launcher: Settings > Apps. Nightshade's own apps (apps_list.hpp), a row each: what
// it is, whether it is installed and at which version, its newest release and where that is (the
// app's Nightshade repository, or the project it is based on until that has one). Cross gets an
// app that isn't installed and opens one that is, Square updates it, Triangle asks GitHub again.
// An install shows in its row; opening an app closes Nightshade (the launch payload opens it once
// Nightshade has gone).
// SPDX-FileCopyrightText: Copyright 2026 Oliv3rmoon
// SPDX-License-Identifier: GPL-3.0-or-later

#include "pe/ui/apps_list.hpp"
#include "pe/ui/launcher.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <string>

namespace pe::ui
{

using audio::Cue;

namespace
{

// The dialog of Settings > Downloads (remote.cpp), with taller rows: a line more each.
constexpr Rect kDialog{550.0f, 180.0f, 820.0f, 720.0f};
constexpr float kLeft = 592.0f;
constexpr float kWidth = 736.0f;
constexpr float kRowsTop = 372.0f;
constexpr float kRowPitch = 132.0f;
constexpr float kRowHeight = 124.0f;
constexpr float kTextLeft = 684.0f;   // right of the app's icon
constexpr float kTextRight = 1292.0f; // a row's right edge, less its padding
constexpr float kMessageTop = 782.0f; // what was done, on up to two lines, above the hints
constexpr float kHints = 862.0f;
// How often the list and an install are looked at, and how long a result stands.
constexpr float kPollSeconds = 0.25f;
constexpr float kMessageSeconds = 8.0f;
// From the launch payload taking an app to Nightshade closing (the screen goes dark meanwhile).
constexpr float kClosingSeconds = 1.6f;
// The payload loader's port (self_update_protocol.h), as the messages name it.
constexpr const char *kLoaderPort = "9021";

Icon icon_of(AppIcon icon)
{
    switch (icon)
    {
    case AppIcon::tv:
        return Icon::tv;
    case AppIcon::stream:
        return Icon::stream;
    case AppIcon::disc:
        return Icon::disc;
    }
    return Icon::apps;
}

std::string megabytes(std::uint64_t bytes)
{
    char text[32];
    std::snprintf(text, sizeof(text), "%.1f MB", static_cast<double>(bytes) / (1024.0 * 1024.0));
    return text;
}

bool installed(const AppState &app)
{
    return !app.installed.empty() || app.image;
}

// Why nothing is offered, in the player's words.
std::string problem_text(const AppState &app, const AppListing &listing)
{
    switch (app.problem)
    {
    case AppProblem::none:
        return {};
    case AppProblem::no_release:
        return tr("No release yet.");
    case AppProblem::offline:
        return tr("GitHub could not be reached.");
    case AppProblem::rate_limited:
        return tr("GitHub is limiting requests. Try again in an hour.");
    case AppProblem::no_file:
        return fill(tr("The release has no {0}."), {listing.asset});
    case AppProblem::no_hash:
        return fill(tr("The release gives no SHA-256 for {0}, so it is not offered."), {listing.asset});
    case AppProblem::bad_hash:
        return fill(tr("The release's SHA-256 for {0} can't be trusted, so it is not offered."), {listing.asset});
    case AppProblem::moved:
        return fill(tr("{0} has moved to another account, so its release is not offered."), {app.repository});
    case AppProblem::unreadable:
        return tr("GitHub's answer could not be read.");
    }
    return {};
}

} // namespace

void Launcher::open_apps()
{
    open_modal(Modal::apps);
    app_rows_.visible = kAppCount;
    app_rows_.pitch = kRowPitch;
    app_rows_.reset(kAppCount, std::clamp(app_rows_.selected, 0, kAppCount - 1));
    // Their releases once a launch, when the dialog first opens; Triangle asks again.
    if (!apps_asked_)
    {
        services_.refresh_apps(true);
        apps_asked_ = apps_looked_ = true;
    }
    apps_ = services_.apps();
    apps_wait_ = 0.0f;
}

void Launcher::poll_apps(float dt)
{
    // The launch payload's answer, once it has come.
    if (app_open_.valid() && app_open_.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
    {
        const auto [result, reason] = app_open_.get();
        const std::string name = app_opening_ >= 0 ? kApps[app_opening_].name : "";
        app_opening_ = -1;
        switch (result)
        {
        case AppOpen::closing:
            app_closing_ = 0.0f;
            say(fill(tr("Opening {0}. Nightshade closes now."), {name}));
            cue(Cue::launch);
            break;
        case AppOpen::no_loader:
            say(fill(tr("No payload loader is listening on port {0}, so {1} can't be opened from here. Open it from "
                        "the PS5's home screen."),
                     {kLoaderPort, name}),
                true);
            cue(Cue::error);
            break;
        case AppOpen::not_listed:
            say(fill(tr("The PS5 doesn't list {0} yet. The homebrew loader lists it after its next scan or a "
                        "restart."),
                     {name}),
                true);
            cue(Cue::error);
            break;
        case AppOpen::refused:
            say(fill(tr("{0} could not be opened: {1}"), {name, reason}), true);
            cue(Cue::error);
            break;
        }
    }
    if (app_closing_ >= 0.0f)
    {
        app_closing_ += dt;
        if (app_closing_ >= kClosingSeconds)
            done_ = true;
        return;
    }
    // Where they are once Settings shows (its tile counts them), then a few times a second while
    // Settings or the dialog shows, or an app installs.
    if (screen_ == Screen::settings && !apps_looked_)
    {
        services_.refresh_apps(false);
        apps_looked_ = true;
    }
    if (modal_ != Modal::apps && screen_ != Screen::settings && !app_working_)
        return;
    apps_wait_ += dt;
    if (apps_wait_ < kPollSeconds)
        return;
    apps_wait_ = 0.0f;
    apps_ = services_.apps();
    if (!app_working_)
        return;
    app_job_ = services_.app_job();
    const std::string name = app_job_.app >= 0 && app_job_.app < kAppCount ? kApps[app_job_.app].name : "";
    switch (app_job_.phase)
    {
    case AppPhase::installed:
        say(app_job_.fresh ? fill(tr("{0} is installed. The homebrew loader lists it after its next scan or a restart."),
                                  {name})
                           : fill(tr("{0} is updated to {1}."), {name, app_job_.version}));
        cue(Cue::saved);
        break;
    case AppPhase::failed:
        if (app_job_.no_loader)
            say(fill(tr("No payload loader is listening on port {0}, so {1} can't be installed."), {kLoaderPort, name}),
                true);
        else
            say(fill(tr("{0} could not be installed: {1}"), {name, app_job_.error}), true);
        cue(Cue::error);
        break;
    case AppPhase::cancelled:
        message_.clear();
        cue(Cue::back);
        break;
    default:
        return;
    }
    services_.finish_app();
    app_working_ = false;
    apps_ = services_.apps();
}

void Launcher::press_apps(Key key)
{
    if (app_opening_ >= 0 || app_closing_ >= 0.0f)
        return; // the launch payload is being asked, or Nightshade closes
    if (app_working_)
    {
        // Until its files are put in place an install can stop, leaving everything as it was.
        if (key == Key::circle && app_job_.phase != AppPhase::installing)
        {
            services_.cancel_app();
            say(tr("Cancelling"));
            cue(Cue::back);
        }
        return;
    }
    switch (key)
    {
    case Key::circle:
        close_modal();
        return;
    case Key::up:
    case Key::down:
        if (app_rows_.move(key == Key::down ? 1 : -1))
        {
            message_.clear();
            cue(Cue::focus);
        }
        return;
    case Key::triangle:
        services_.refresh_apps(true);
        apps_ = services_.apps();
        message_.clear();
        cue(Cue::select);
        return;
    case Key::cross:
    case Key::square:
        break;
    default:
        return;
    }
    const int row = app_rows_.selected;
    if (row < 0 || row >= kAppCount || row >= static_cast<int>(apps_.size()))
        return;
    const AppState &app = apps_[static_cast<std::size_t>(row)];
    // Without the filesystem access nothing is known to be installed: Cross gets, Square opens.
    const bool open = app.looked ? key == Key::cross && installed(app) : key == Key::square;
    const bool get = app.looked ? (key == Key::cross && !installed(app) && app.offer == AppOffer::get) ||
                                      (key == Key::square && app.offer == AppOffer::update)
                                : key == Key::cross && app.offer == AppOffer::get;
    if (open)
    {
        app_opening_ = row;
        app_open_ = std::async(std::launch::async,
                               [this, row]
                               {
                                   std::string reason;
                                   const AppOpen result = services_.open_app(row, &reason);
                                   return std::make_pair(result, reason);
                               });
        say(fill(tr("Opening {0}..."), {kApps[row].name}));
        cue(Cue::select);
        return;
    }
    if (!get)
        return;
    std::string reason;
    if (!services_.install_app(row, &reason))
    {
        say(fill(tr("{0} could not be installed: {1}"), {kApps[row].name, reason}), true);
        cue(Cue::error);
        return;
    }
    app_working_ = true;
    app_job_ = {};
    app_job_.app = row;
    app_job_.phase = AppPhase::starting;
    message_.clear();
    cue(Cue::select);
}

void Launcher::draw_apps(Canvas &c, float open)
{
    gfx::DrawList &list = c.list;
    const Theme &t = th();
    list.push_opacity(open);
    list.push_transform(1.0f - 0.03f * (1.0f - open) * motion(), 960.0f, 540.0f, 0.0f, (1.0f - open) * 26.0f * motion());
    glass(c, kDialog, t.radius_panel, t.panel.with_alpha(0.97f), t.panel_edge.with_alpha(0.66f), 1.6f);
    text_shrink(c, tr("Apps"), kLeft, baseline(218.0f, 62.0f, t.size_display), t.size_display, t.title, kWidth);
    text_shrink(c, tr("Get, update and open Physalis, Datura and Mandrake."), kLeft,
                baseline(291.0f, 32.0f, t.size_small), t.size_small, t.copy, kWidth);
    if (services_.filesystem_access() != 0)
        notice(c, tr("Without filesystem access Nightshade can't tell which apps are installed."), kLeft,
               baseline(326.0f, 30.0f, t.size_small), t.size_small, t.warning, kWidth, true);
    else
        text_shrink(c, tr("An app without a Nightshade release yet is offered from the project it is based on."), kLeft,
                    baseline(326.0f, 30.0f, t.size_small), t.size_small, t.meta, kWidth);

    const auto row_top = [&](int row) { return kRowsTop + static_cast<float>(row) * kRowPitch - app_rows_.scroll(); };
    for (int row = 0; row < kAppCount; ++row)
        plate_rest(c, row_plate(), {kLeft, row_top(row), kWidth, kRowHeight});
    plate_focus(c, row_plate(), {kLeft, kRowsTop + app_rows_.cursor() - app_rows_.scroll(), kWidth, kRowHeight},
                app_working_ || app_opening_ >= 0 ? 0.6f : 1.0f);
    for (int row = 0; row < kAppCount; ++row)
    {
        const AppListing &listing = kApps[row];
        const AppState app = row < static_cast<int>(apps_.size()) ? apps_[static_cast<std::size_t>(row)] : AppState{};
        const float top = row_top(row);
        const bool focused = row == app_rows_.selected;
        icon(c, icon_of(listing.icon), kLeft + 46.0f, top + 40.0f, 40.0f, focused ? t.accent_pale : t.accent);

        // Its name, and at the right whether it is installed (and at which version).
        std::string state;
        Color tone = t.meta;
        if (app.looked && installed(app))
        {
            state = app.offer == AppOffer::update ? std::string{tr("Update available")}
                    : app.installed.empty()       ? std::string{}
                                                  : fill(tr("Version {0}"), {app.installed});
            tone = app.offer == AppOffer::update ? t.accent_pale : t.meta;
        }
        else if (app.looked)
            state = tr("Not installed");
        const float taken = text_shrink(c, state, kTextRight, baseline(top + 12.0f, 40.0f, t.size_small), t.size_small,
                                        tone, 300.0f, Align::right);
        {
            const Heavy heavy(c);
            text_fit(c, listing.name, kTextLeft, baseline(top + 12.0f, 40.0f, t.size_text), t.size_text,
                     focused ? t.title : t.value, kTextRight - kTextLeft - taken - 24.0f);
        }
        text_shrink(c, tr(listing.about), kTextLeft, baseline(top + 52.0f, 28.0f, t.size_small), t.size_small, t.copy,
                    kTextRight - kTextLeft);

        // Its install, or its release and where that is, or why there is none.
        const float third = baseline(top + 84.0f, 28.0f, t.size_small);
        if (app_working_ && app_job_.app == row)
        {
            const AppPhase phase = app_job_.phase;
            const bool measured = phase == AppPhase::downloading && app_job_.total > 0;
            const float share = measured ? static_cast<float>(static_cast<double>(app_job_.done) /
                                                              static_cast<double>(app_job_.total))
                                         : 0.0f;
            std::string line = tr(phase == AppPhase::downloading ? TR("Downloading")
                                  : phase == AppPhase::unpacking ? TR("Unpacking")
                                  : phase == AppPhase::installing ? TR("Installing")
                                                                  : TR("Preparing"));
            if (measured)
                line += "  ·  " + megabytes(app_job_.done) + " / " + megabytes(app_job_.total);
            text_shrink(c, line, kTextLeft, third, t.size_small, t.accent_pale, kTextRight - kTextLeft - 80.0f);
            if (measured)
                text(c, fill(tr("{0}%"), {std::to_string(std::min(100, static_cast<int>(share * 100.0f + 0.5f)))}),
                     kTextRight, third, t.size_small, t.accent_pale, Align::right);
            progress_bar(c, {kTextLeft, top + kRowHeight - 14.0f, kTextRight - kTextLeft, 6.0f},
                         measured ? share : phase == AppPhase::starting ? 0.0f : 1.0f);
        }
        else if (app.image)
            text_shrink(c, tr("Installed as an image: replace the image to update it."), kTextLeft, third, t.size_small,
                        t.meta, kTextRight - kTextLeft);
        else if (!app.checked)
            text_shrink(c, tr("Reading..."), kTextLeft, third, t.size_small, t.meta, kTextRight - kTextLeft);
        else if (app.problem != AppProblem::none)
            notice(c, problem_text(app, listing), kTextLeft, third, t.size_small, t.warning, kTextRight - kTextLeft, true);
        else
            text_shrink(c,
                        app.fallback ? fill(tr("Release {0} of the original project, {1}"), {app.release, app.repository})
                                     : fill(tr("Release {0}, from {1}"), {app.release, app.repository}),
                        kTextLeft, third, t.size_small, app.fallback ? t.meta : t.accent_pale, kTextRight - kTextLeft);
    }

    // What was done stands for a while (a warning is a card over everything: say()); the hints.
    if (!message_.empty() && (app_working_ || app_opening_ >= 0 || app_closing_ >= 0.0f ||
                              message_age_ < kMessageSeconds))
        text_block(c, message_, kLeft, baseline(kMessageTop, 30.0f, t.size_small), t.size_small, 30.0f,
                   t.accent_pale, kWidth, 2, kShrink);
    if (app_working_)
    {
        if (app_job_.phase != AppPhase::installing)
        {
            static constexpr Hint kWorking[] = {{Pad::circle, TR("Cancel")}};
            draw_menu_hints(c, kWorking, 1, kLeft, kHints, t.copy, kWidth);
        }
    }
    else if (app_opening_ < 0 && app_closing_ < 0.0f)
    {
        const AppState app = app_rows_.selected < static_cast<int>(apps_.size())
                                 ? apps_[static_cast<std::size_t>(app_rows_.selected)]
                                 : AppState{};
        Hint hints[4];
        int count = 0;
        if (!app.looked)
        {
            if (app.offer == AppOffer::get)
                hints[count++] = {Pad::cross, TR("Get")};
            hints[count++] = {Pad::square, TR("Open")};
        }
        else if (installed(app))
        {
            hints[count++] = {Pad::cross, TR("Open")};
            if (app.offer == AppOffer::update)
                hints[count++] = {Pad::square, TR("Update")};
        }
        else if (app.offer == AppOffer::get)
            hints[count++] = {Pad::cross, TR("Get")};
        hints[count++] = {Pad::triangle, TR("Refresh")};
        hints[count++] = {Pad::circle, TR("Back")};
        draw_menu_hints(c, hints, count, kLeft, kHints, t.copy, kWidth);
    }
    list.pop_transform();
    list.pop_opacity();
}

} // namespace pe::ui
