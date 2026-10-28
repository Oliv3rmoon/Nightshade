// Nightshade - Launcher: Settings > Diagnostics > Back up to USB (headless/backup.h). The app's data
// (settings, profiles and every save) copied to a USB drive: how much there is to copy, then a row
// for each USB drive with the room it has where that is known. Cross backs up to the drive chosen,
// which shows how far it is in its row; Circle stops it, and what it wrote is removed. Triangle
// looks for the drives again. Nothing is restored here (docs/INSTALL.md says how, by hand).
// SPDX-FileCopyrightText: Copyright 2026 Oliv3rmoon
// SPDX-License-Identifier: GPL-3.0-or-later

#include "pe/ui/launcher.hpp"

#include <algorithm>
#include <cstdio>
#include <string>

namespace pe::ui
{

using audio::Cue;

namespace
{

// The dialog of Settings > Apps (apps.cpp), its rows a line shorter.
constexpr Rect kDialog{550.0f, 180.0f, 820.0f, 720.0f};
constexpr float kLeft = 592.0f;
constexpr float kWidth = 736.0f;
constexpr float kRowsTop = 372.0f;
constexpr float kRowPitch = 112.0f;
constexpr float kRowHeight = 104.0f;
constexpr int kRowsShown = 3;
constexpr float kTextLeft = 684.0f;   // right of the drive's icon
constexpr float kTextRight = 1292.0f; // a row's right edge, less its padding
constexpr float kMessageTop = 782.0f; // what was done, on up to two lines, above the hints
constexpr float kHints = 862.0f;
// How often the drives and a backup are looked at, and how long a result stands.
constexpr float kPollSeconds = 0.25f;
constexpr float kMessageSeconds = 8.0f;
// Where a backup goes on the drive (headless/backup.h, kFolder).
constexpr const char *kBackupFolder = "/nightshade-backup";
// The Diagnostics row that opens this dialog (settings.cpp, diagnostics_backup).
constexpr int kDiagnosticsRow = 3;

// A size as the other dialogs write it: MB, or GB from one on.
std::string size_text(std::uint64_t bytes)
{
    char text[32];
    if (bytes >= (1ull << 30))
        std::snprintf(text, sizeof(text), "%.1f GB", static_cast<double>(bytes) / 1073741824.0);
    else
        std::snprintf(text, sizeof(text), "%.1f MB", static_cast<double>(bytes) / 1048576.0);
    return text;
}

// What is left on the drive after a backup that is not whole.
std::string left_text(const BackupStatus &status)
{
    return status.folder.empty() ? std::string{tr("Nothing was left on the drive.")}
                                 : fill(tr("{0} is left on the drive, marked incomplete: delete it."), {status.folder});
}

} // namespace

void Launcher::open_backup()
{
    modal_ = modal_shown_ = Modal::backup;
    backup_rows_.visible = kRowsShown;
    backup_rows_.pitch = kRowPitch;
    backup_ = services_.backup_status();
    backup_rows_.reset(std::max(1, static_cast<int>(backup_.drives.size())), 0);
    // The drives and what there is to copy, each time it opens: a drive may have been plugged in.
    if (!backup_working_)
    {
        services_.refresh_backup();
        backup_ = services_.backup_status();
    }
    backup_wait_ = 0.0f;
    message_.clear();
    cue(Cue::open);
}

void Launcher::poll_backup(float dt)
{
    if (modal_ != Modal::backup && !backup_working_)
        return;
    backup_wait_ += dt;
    if (backup_wait_ < kPollSeconds)
        return;
    backup_wait_ = 0.0f;
    const std::size_t drives = backup_.drives.size();
    backup_ = services_.backup_status();
    if (backup_.drives.size() != drives)
        backup_rows_.reset(std::max(1, static_cast<int>(backup_.drives.size())),
                           std::clamp(backup_rows_.selected, 0, std::max(0, static_cast<int>(backup_.drives.size()) - 1)));
    if (!backup_working_)
        return;
    switch (backup_.phase)
    {
    case BackupPhase::done:
        say(fill(tr("Backed up to {0}."), {backup_.folder}));
        cue(Cue::saved);
        break;
    case BackupPhase::cancelled:
        say(std::string{tr("Backup cancelled.")} + " " + left_text(backup_), !backup_.folder.empty());
        cue(Cue::back);
        break;
    case BackupPhase::failed:
        say(fill(tr("The backup failed: {0}"), {backup_.error}) + " " + left_text(backup_), true);
        cue(Cue::error);
        break;
    case BackupPhase::no_room:
        say((backup_.room_known ? fill(tr("Not enough room on the drive: the backup needs {0}, {1} is free."),
                                       {size_text(backup_.needed), size_text(backup_.free)})
                                : std::string{tr("The drive is full.")}) +
                " " + left_text(backup_),
            true);
        cue(Cue::error);
        break;
    default:
        return;
    }
    services_.finish_backup();
    backup_working_ = false;
    backup_cancelling_ = false;
    // The room on the drive has changed, and what there is to copy may have.
    services_.refresh_backup();
    backup_ = services_.backup_status();
}

void Launcher::press_backup(Key key)
{
    if (backup_working_)
    {
        // Until it ends a backup can stop: what it wrote is removed.
        if (key == Key::circle && !backup_cancelling_)
        {
            services_.cancel_backup();
            backup_cancelling_ = true;
            say(tr("Cancelling"));
            cue(Cue::back);
        }
        return;
    }
    switch (key)
    {
    case Key::circle:
        // Back to Diagnostics, on the row that opened this dialog (diagnostics_rows_ never left it).
        modal_ = modal_shown_ = Modal::diagnostics;
        option_ = kDiagnosticsRow;
        option_cursor_.snap(dialog_row_top(Modal::diagnostics, option_));
        message_.clear();
        cue(Cue::back);
        return;
    case Key::up:
    case Key::down:
        if (backup_.drives.size() > 1 && backup_rows_.move(key == Key::down ? 1 : -1))
        {
            message_.clear();
            cue(Cue::focus);
        }
        return;
    case Key::triangle:
        services_.refresh_backup();
        backup_ = services_.backup_status();
        message_.clear();
        cue(Cue::select);
        return;
    case Key::cross:
        break;
    default:
        return;
    }
    const int row = backup_rows_.selected;
    if (!backup_.access || !backup_.measured || backup_.phase != BackupPhase::idle || row < 0 ||
        row >= static_cast<int>(backup_.drives.size()))
        return;
    std::string reason;
    if (!services_.start_backup(row, &reason))
    {
        say(fill(tr("The backup failed: {0}"), {reason}), true);
        cue(Cue::error);
        return;
    }
    backup_working_ = true;
    backup_cancelling_ = false;
    backup_.phase = BackupPhase::measuring;
    backup_.drive = row;
    backup_.done_files = backup_.done_bytes = 0;
    message_.clear();
    cue(Cue::select);
}

void Launcher::draw_backup(Canvas &c, float open)
{
    gfx::DrawList &list = c.list;
    const Theme &t = th();
    list.push_opacity(open);
    list.push_transform(1.0f - 0.03f * (1.0f - open) * motion(), 960.0f, 540.0f, 0.0f, (1.0f - open) * 26.0f * motion());
    glass(c, kDialog, t.radius_panel, t.panel.with_alpha(0.97f), t.panel_edge.with_alpha(0.66f), 1.6f);
    text_shrink(c, tr("Back up to USB"), kLeft, baseline(218.0f, 62.0f, t.size_display), t.size_display, t.title, kWidth);
    text_shrink(c, tr("Copies settings, profiles and save data to a USB drive."), kLeft,
                baseline(291.0f, 32.0f, t.size_small), t.size_small, t.copy, kWidth);

    // What there is to copy, or why there is nothing.
    const float line = baseline(326.0f, 30.0f, t.size_small);
    if (!backup_.access)
        notice(c, tr("Without filesystem access there is nothing to back up."), kLeft, line, t.size_small, t.warning,
               kWidth, true);
    else if (!backup_.problem.empty() && !backup_.measured)
        notice(c, fill(tr("Nightshade's data could not be read: {0}"), {backup_.problem}), kLeft, line, t.size_small,
               t.warning, kWidth, true);
    else if (!backup_.measured)
        text_shrink(c, tr("Reading..."), kLeft, line, t.size_small, t.meta, kWidth);
    else
        text_shrink(c,
                    fill(backup_.files == 1 ? tr("To back up: {0} in {1} file") : tr("To back up: {0} in {1} files"),
                         {size_text(backup_.bytes), std::to_string(backup_.files)}),
                    kLeft, line, t.size_small, t.body, kWidth);

    // A row for each USB drive.
    const int drives = static_cast<int>(backup_.drives.size());
    if (backup_.access && drives == 0 && backup_.phase != BackupPhase::looking)
        text_block(c, tr("No USB drive is connected. Connect one formatted as exFAT or FAT32."), kLeft,
                   baseline(kRowsTop + 8.0f, 32.0f, t.size_text), t.size_text, 38.0f, t.body, kWidth, 2, kShrink);
    const auto row_top = [&](int row) { return kRowsTop + static_cast<float>(row) * kRowPitch - backup_rows_.scroll(); };
    if (drives > 0)
    {
        const bool scrolls = drives > kRowsShown;
        if (scrolls)
            list.push_clip({kLeft - 24.0f, kRowsTop - 6.0f, kWidth + 48.0f,
                            kRowPitch * (kRowsShown - 1) + kRowHeight + 12.0f});
        const int first = scrolls ? backup_rows_.first_row() : 0;
        const int last = scrolls ? std::min(drives - 1, backup_rows_.last_row()) : drives - 1;
        for (int row = first; row <= last; ++row)
        {
            list.push_opacity(scrolls ? backup_rows_.row_alpha(row, kRowHeight) : 1.0f);
            plate_rest(c, row_plate(), {kLeft, row_top(row), kWidth, kRowHeight});
            list.pop_opacity();
        }
        plate_focus(c, row_plate(), {kLeft, kRowsTop + backup_rows_.cursor() - backup_rows_.scroll(), kWidth, kRowHeight},
                    backup_working_ ? 0.6f : 1.0f);
        for (int row = first; row <= last; ++row)
        {
            const BackupDrive &drive = backup_.drives[static_cast<std::size_t>(row)];
            const float top = row_top(row);
            const bool focused = row == backup_rows_.selected;
            list.push_opacity(scrolls ? backup_rows_.row_alpha(row, kRowHeight) : 1.0f);
            icon(c, Icon::files, kLeft + 46.0f, top + 40.0f, 40.0f, focused ? t.accent_pale : t.accent);
            // Its name and, at the right, the room it has.
            const float taken =
                text_shrink(c,
                            drive.free_known ? fill(tr("{0} free"), {size_text(drive.free)})
                                             : std::string{tr("Free space unknown")},
                            kTextRight, baseline(top + 12.0f, 40.0f, t.size_small), t.size_small, t.meta, 300.0f,
                            Align::right);
            // /mnt/usb0 is USB drive 1, as the PS5 counts its USB ports' drives.
            const char last = drive.path.empty() ? '\0' : drive.path.back();
            const std::string number = last >= '0' && last <= '9' ? std::to_string(last - '0' + 1)
                                                                  : std::to_string(row + 1);
            {
                const Heavy heavy(c);
                text_fit(c, fill(tr("USB drive {0}"), {number}), kTextLeft, baseline(top + 12.0f, 40.0f, t.size_text),
                         t.size_text, focused ? t.title : t.value, kTextRight - kTextLeft - taken - 24.0f);
            }
            // Where a backup goes, or how far the one going there is.
            const float second = baseline(top + 56.0f, 28.0f, t.size_small);
            if (backup_working_ && backup_.drive == row)
            {
                const bool copying = backup_.phase == BackupPhase::copying && backup_.bytes > 0;
                const float share =
                    copying ? std::min(1.0f, static_cast<float>(static_cast<double>(backup_.done_bytes) /
                                                                static_cast<double>(backup_.bytes)))
                            : 0.0f;
                std::string state = tr(backup_.phase == BackupPhase::copying ? TR("Copying") : TR("Preparing"));
                if (copying)
                    state += "  ·  " + size_text(backup_.done_bytes) + " / " + size_text(backup_.bytes);
                text_shrink(c, state, kTextLeft, second, t.size_small, t.accent_pale, kTextRight - kTextLeft - 80.0f);
                if (copying)
                    text(c, fill(tr("{0}%"), {std::to_string(std::min(100, static_cast<int>(share * 100.0f + 0.5f)))}),
                         kTextRight, second, t.size_small, t.accent_pale, Align::right);
                progress_bar(c, {kTextLeft, top + kRowHeight - 18.0f, kTextRight - kTextLeft, 6.0f}, share);
            }
            else
                text_shrink(c, fill(tr("Goes to {0}"), {drive.path + kBackupFolder}), kTextLeft, second, t.size_small,
                            t.copy, kTextRight - kTextLeft);
            list.pop_opacity();
        }
        if (scrolls)
        {
            list.pop_clip();
            scrollbar(c, backup_rows_, kLeft + kWidth + 12.0f, kRowsTop, kRowPitch * (kRowsShown - 1) + kRowHeight);
        }
    }

    // What was done stands for a while (a warning is a card over everything: say()); the hints.
    if (!message_.empty() && (backup_working_ || message_age_ < kMessageSeconds))
        text_block(c, message_, kLeft, baseline(kMessageTop, 30.0f, t.size_small), t.size_small, 30.0f, t.accent_pale,
                   kWidth, 2, kShrink);
    if (backup_working_)
    {
        if (!backup_cancelling_)
        {
            static constexpr Hint kWorking[] = {{Pad::circle, TR("Cancel")}};
            draw_menu_hints(c, kWorking, 1, kLeft, kHints, t.copy, kWidth);
        }
    }
    else
    {
        Hint hints[4];
        int count = 0;
        if (drives > 1)
            hints[count++] = {Pad::updown, TR("Select")};
        if (drives > 0 && backup_.measured && backup_.phase == BackupPhase::idle)
            hints[count++] = {Pad::cross, TR("Back up")};
        if (backup_.access)
            hints[count++] = {Pad::triangle, TR("Refresh")};
        hints[count++] = {Pad::circle, TR("Back")};
        draw_menu_hints(c, hints, count, kLeft, kHints, t.copy, kWidth);
    }
    list.pop_transform();
    list.pop_opacity();
}

} // namespace pe::ui
