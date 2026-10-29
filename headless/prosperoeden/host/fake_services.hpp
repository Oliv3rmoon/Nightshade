// ProsperoEden - Sample data for the launcher preview on a PC (no console, no game files).
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "pe/core/strings.hpp"
#include "pe/ui/services.hpp"

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

namespace pe::host
{

// A made-up library: invented titles with drawn covers, settings kept in memory.
class FakeServices : public ui::Services
{
  public:
    // covers_directory receives one drawn cover per sample game.
    explicit FakeServices(const std::string &covers_directory);

    // What the preview varies between pictures.
    bool setup_ready = true;
    std::string launch_error;
    std::string crash_report; // the previous run's crash report, when it left one
    bool has_history = true;
    bool import_available = true;
    ui::SaveSource import_source = ui::SaveSource::ryujinx;
    unsigned connected_controllers = 0b0011;
    Eden::Multiplayer::Snapshot room;
    Eden::Multiplayer::Connection last_room_connection;
    Eden::Multiplayer::Connection saved_room{"127.0.0.1", 24872, "PlayerPS5", "private-secret"};
    Eden::Multiplayer::RoomDirectory directory{{
        {"Community racing", "Kart Carnival Deluxe", "192.0.2.1", 24872, 3, 8, false, true},
        {"Friends' island", "Echoes of the Valley", "192.0.2.2", 24872, 2, 8, true, true},
        {"Another protocol", "Other game", "192.0.2.3", 24872, 0, 8, false, false}}, {}, 0};
    Eden::Multiplayer::RoomDirectory public_rooms(const std::atomic<bool>&) override { return directory; }
    Eden::Multiplayer::Snapshot room_status() override { return room; }
    Eden::Multiplayer::Connection room_connection() override { return saved_room; }
    void remember_room(const Eden::Multiplayer::Connection& connection, bool private_room) override {
        if (private_room) saved_room = connection;
        else saved_room.nickname = connection.nickname;
    }
    bool connect_public_room(const Eden::Multiplayer::Connection& connection) override {
        remember_room(connection, false);
        last_room_connection = connection;
        room = {Eden::Multiplayer::Phase::Connecting, {}, {}, {}};
        return true;
    }
    bool connect_room(const Eden::Multiplayer::Connection& connection) override {
        saved_room = connection;
        last_room_connection = connection;
        room = {Eden::Multiplayer::Phase::Connecting, {}, {}, {}};
        return true;
    }
    void leave_room() override { room = {}; }
    bool can_launch_game() override {
        return room.phase == Eden::Multiplayer::Phase::Idle || room.phase == Eden::Multiplayer::Phase::Connected;
    }

    ui::Home home() override;
    std::string clock() override
    {
        return "21:47";
    }
    unsigned controllers() override
    {
        return connected_controllers;
    }
    void vibration_pulse(int percent) override
    {
        last_pulse = percent;
        ++pulses;
    }
    int last_pulse = 0; // the vibration the menu last asked for, and how many it asked for
    int pulses = 0;
    std::string version() override
    {
        return "v1.000.040";
    }
    // The people who play on the preview's console.
    std::vector<ui::Profile> people{{"Eden", true}};
    std::vector<ui::Profile> profiles() override
    {
        return people;
    }
    bool choose_profile(int index) override
    {
        if (index < 0 || index >= static_cast<int>(people.size()))
            return false;
        for (std::size_t i = 0; i < people.size(); ++i)
            people[i].playing = static_cast<int>(i) == index;
        return true;
    }
    int add_profile() override
    {
        if (people.size() >= 8)
            return -1;
        static constexpr const char *kNames[] = {"Marina", "Player 2", "Player 3", "Player 4",
                                                 "Player 5", "Player 6", "Player 7", "Player 8"};
        people.push_back({kNames[people.size() - 1], false});
        return static_cast<int>(people.size()) - 1;
    }
    bool rename_profile(int index, int step) override
    {
        if (index < 0 || index >= static_cast<int>(people.size()))
            return false;
        static constexpr const char *kNames[] = {"Eden", "Marina", "Player 1", "Player 2"};
        int at = 0;
        for (int i = 0; i < 4; ++i)
            if (people[static_cast<std::size_t>(index)].name == kNames[i])
                at = i;
        people[static_cast<std::size_t>(index)].name = kNames[((at + step) % 4 + 4) % 4];
        return true;
    }
    bool remove_profile(int index) override
    {
        if (people.size() < 2 || index < 0 || index >= static_cast<int>(people.size()) ||
            people[static_cast<std::size_t>(index)].playing)
            return false;
        people.erase(people.begin() + index);
        return true;
    }
    // A newer release for the preview to offer: handed over once. Installing it goes through its
    // phases a step per look (the launcher looks once a frame); update_fails ends it in a failure.
    std::string update_version;
    bool update_installable = true;
    // The sample release's notes, as the catalog gives them; empty: a release without notes.
    std::string update_notes =
        "Warning: This is a pre-release. Your jailbreak environment must provide a local ELF loader on TCP port "
        "9021.\n\nThis version makes the menu faster to open and adds a few things players asked for.\n\n"
        "New features\n- Release notes in the update dialog, so you can see what changes before you update.\n"
        "- A clock in the game menu.\n- Faster scrolling in long game lists, with a letter jump on L2 and R2.\n\n"
        "Bug fixes and improvements\n- The menu opens about a second sooner on a cold start.\n"
        "- Covers no longer flicker when a list scrolls quickly.\n- A crash when a controller was disconnected "
        "during the loading screen is fixed.\n- Saves are written to a temporary file first, so a power loss "
        "while saving no longer damages them.\n- Better text fitting in Greek, Hungarian and Finnish.\n\n"
        "Note: Your settings, saves and profiles stay where they are; nothing needs to be moved.\n\n"
        "Upgrading\nCopy the PPSA64448 folder from the ZIP over the one you have, or install it from the "
        "update dialog. See the README for every step: "
        "https://github.com/blackbearreloaded/ProsperoEden/blob/main/README.md#updating\n\n"
        "Thanks\nThank you to everyone who tested this release and reported what they found.";
    bool update_notes_truncated = false;
    bool update_fails = false;
    int update_steps = -1; // looks since start_update; -1: not begun
    bool update_cancelled = false;
    bool take_update(ui::UpdateOffer *offer) override
    {
        if (update_version.empty())
            return false;
        offer->version = update_version;
        offer->size = 38215192;
        offer->installable = update_installable;
        offer->notes = update_notes;
        offer->notes_truncated = update_notes_truncated;
        update_version.clear();
        return true;
    }
    bool start_update() override
    {
        update_steps = 0;
        update_cancelled = false;
        return true;
    }
    ui::UpdateStatus update_status() override
    {
        ui::UpdateStatus status;
        if (update_steps < 0)
            return status;
        const int step = update_steps++;
        constexpr std::uint64_t kSize = 38215192;
        if (update_cancelled)
            status.phase = step < 20 ? ui::UpdatePhase::starting : ui::UpdatePhase::cancelled;
        else if (step < 40)
            status.phase = ui::UpdatePhase::starting;
        else if (step < 400)
        {
            status.phase = ui::UpdatePhase::downloading;
            status.total = kSize;
            status.done = kSize * static_cast<std::uint64_t>(step - 40) / 360;
        }
        else if (update_fails)
        {
            status.phase = ui::UpdatePhase::failed;
            status.error = "Download failed: the connection was lost";
        }
        else if (step < 520)
            status.phase = ui::UpdatePhase::unpacking;
        else
            status.phase = ui::UpdatePhase::ready;
        return status;
    }
    void cancel_update() override
    {
        update_cancelled = true;
        update_steps = 0;
    }
    bool apply_update() override
    {
        return update_steps >= 0;
    }
    void finish_update() override
    {
        update_steps = -1;
    }
    // Settings > Apps: Nightshade TV installed and current, Nightshade Stream installed with a
    // newer release of the original project, Nightshade Station not installed with a release of
    // its own. An install goes through its phases a step per look (the launcher looks four times a
    // second); app_fails ends it in a failure. app_open says what opening one comes to.
    std::vector<ui::AppState> app_list = [] {
        std::vector<ui::AppState> list(3);
        for (ui::AppState &app : list)
            app.looked = app.checked = true;
        list[0].installed = "01.000.090";
        list[0].offer = ui::AppOffer::current;
        list[0].release = "v1.000.090";
        list[0].repository = "Oliv3rmoon/Nightshade-TV";
        list[0].size = 42353289;
        list[1].installed = "01.000.100";
        list[1].offer = ui::AppOffer::update;
        list[1].release = "01.000.110";
        list[1].repository = "blackbearreloaded/ProsperoLight";
        list[1].fallback = true;
        list[1].size = 36520955;
        list[2].offer = ui::AppOffer::get;
        list[2].release = "v1.005.009";
        list[2].repository = "Oliv3rmoon/Nightshade-Station";
        list[2].size = 18267896;
        return list;
    }();
    int apps_refreshed = 0;
    bool app_fails = false;
    ui::AppOpen app_open = ui::AppOpen::no_loader;
    int app_steps = -1; // looks since install_app; -1: none
    int app_installing = -1;
    bool app_cancelled = false;
    std::vector<ui::AppState> apps() override
    {
        return app_list;
    }
    void refresh_apps(bool releases) override
    {
        apps_refreshed += releases ? 1 : 0;
    }
    bool install_app(int app, std::string *reason) override
    {
        if (app < 0 || app >= static_cast<int>(app_list.size()) || app_steps >= 0)
        {
            *reason = "another app is being installed";
            return false;
        }
        app_installing = app;
        app_steps = 0;
        app_cancelled = false;
        return true;
    }
    ui::AppJob app_job() override
    {
        ui::AppJob job;
        if (app_steps < 0)
            return job;
        const int step = app_steps++;
        const ui::AppState &app = app_list[static_cast<std::size_t>(app_installing)];
        job.app = app_installing;
        job.fresh = app.installed.empty();
        if (app_cancelled)
            job.phase = ui::AppPhase::cancelled;
        else if (step < 4)
            job.phase = ui::AppPhase::starting;
        else if (step < 40)
        {
            job.phase = ui::AppPhase::downloading;
            job.total = app.size;
            job.done = app.size * static_cast<std::uint64_t>(step - 4) / 36;
        }
        else if (app_fails)
        {
            job.phase = ui::AppPhase::failed;
            job.error = "The download doesn't match the catalog's listing";
        }
        else if (step < 48)
            job.phase = ui::AppPhase::unpacking;
        else if (step < 52)
            job.phase = ui::AppPhase::installing;
        else
        {
            job.phase = ui::AppPhase::installed;
            job.version = app.release == "v1.005.009" ? "01.005.009" : app.release;
            ui::AppState &done = app_list[static_cast<std::size_t>(app_installing)];
            done.installed = job.version;
            done.offer = ui::AppOffer::current;
        }
        return job;
    }
    void cancel_app() override
    {
        app_cancelled = true;
    }
    void finish_app() override
    {
        app_steps = -1;
    }
    ui::AppOpen open_app(int, std::string *reason) override
    {
        if (app_open == ui::AppOpen::refused)
            *reason = "The request was refused";
        return app_open;
    }

    // Settings > Diagnostics > Back up to USB: two USB drives, the second with its room known (on the
    // console it never is). A backup goes through its phases a step per look (the launcher looks four
    // times a second); backup_full ends it on a drive that fills up, a cancel removes what it wrote.
    ui::BackupStatus backup_state = [] {
        ui::BackupStatus status;
        status.drives = {{"/mnt/usb0", 0, false}, {"/mnt/usb1", 61ull << 30, true}};
        return status;
    }();
    int backups_refreshed = 0;
    int backups_made = 0;
    int backup_steps = -1; // looks since start_backup; -1: none
    bool backup_full = false;
    bool backup_cancelled = false;
    void refresh_backup() override
    {
        ++backups_refreshed;
        backup_state.measured = true;
        backup_state.files = 4182;
        backup_state.bytes = 1288490189; // 1.2 GB
    }
    ui::BackupStatus backup_status() override
    {
        if (backup_steps < 0)
            return backup_state;
        const int step = backup_steps++;
        ui::BackupStatus status = backup_state;
        if (backup_cancelled)
            status.phase = ui::BackupPhase::cancelled;
        else if (step < 2)
            status.phase = ui::BackupPhase::measuring;
        else if (step < 30 && !(backup_full && step >= 14))
        {
            status.phase = ui::BackupPhase::copying;
            status.done_bytes = status.bytes * static_cast<std::uint64_t>(step - 2) / 28;
            status.done_files = status.files * static_cast<std::uint64_t>(step - 2) / 28;
        }
        else if (backup_full)
            status.phase = ui::BackupPhase::no_room;
        else
        {
            status.phase = ui::BackupPhase::done;
            status.folder = status.drives[static_cast<std::size_t>(std::max(0, status.drive))].path +
                            "/nightshade-backup/20261028-153000";
        }
        return status;
    }
    bool start_backup(int drive, std::string *reason) override
    {
        if (backup_steps >= 0 || drive < 0 || drive >= static_cast<int>(backup_state.drives.size()))
        {
            *reason = "a backup runs";
            return false;
        }
        backup_state.drive = drive;
        backup_steps = 0;
        backup_cancelled = false;
        return true;
    }
    void cancel_backup() override
    {
        backup_cancelled = true;
    }
    void finish_backup() override
    {
        backups_made += backup_steps >= 30 && !backup_full && !backup_cancelled ? 1 : 0;
        backup_steps = -1;
        backup_state.drive = -1;
    }

    // ProsperoEden's data at the first start (migrate.h): asked when migration_state.phase is asking
    // (none otherwise, as on a console that has answered). A move or a copy goes a step per look (the
    // launcher looks ten times a second); migration_fails fails it with both folders as they were,
    // migration_full fills the drive part way through a copy, and a cancel removes what it wrote.
    ui::MigrationStatus migration_state = [] {
        ui::MigrationStatus status;
        status.from = "/data/prosperoeden";
        status.to = "/data/nightshade";
        status.games = "/data/prosperoeden";
        status.measured = true;
        status.files = 48211;
        status.bytes = 6871947674; // 6.4 GB: a large game's shader cache among them
        return status;
    }();
    int migration_steps = -1; // looks since start_migration; -1: none
    bool migration_fails = false;
    bool migration_full = false;
    bool migration_cancelled = false;
    int restarts = 0;
    ui::MigrationStatus migration() override
    {
        if (migration_steps < 0)
            return migration_state;
        const int step = migration_steps++;
        ui::MigrationStatus status = migration_state;
        const bool copy = status.choice == ui::MigrationChoice::copy;
        const int steps = copy ? 40 : 4;
        if (migration_cancelled)
            status.phase = ui::MigrationPhase::cancelled;
        else if (copy && migration_full && step >= 16)
        {
            status.phase = ui::MigrationPhase::no_room;
            status.needed = status.bytes + (64ull << 20);
        }
        else if (step < steps)
        {
            status.done_bytes = copy ? status.bytes * static_cast<std::uint64_t>(step) / steps : 0;
            status.done_files = copy ? status.files * static_cast<std::uint64_t>(step) / steps : 0;
        }
        else if (migration_fails)
        {
            status.phase = ui::MigrationPhase::failed;
            status.error = "could not rename /data/prosperoeden to /data/nightshade: Device busy";
        }
        else
            status.phase = status.choice == ui::MigrationChoice::fresh ? ui::MigrationPhase::none
                                                                         : ui::MigrationPhase::done;
        if (status.phase != ui::MigrationPhase::working)
        {
            migration_state = status;
            migration_steps = -1;
        }
        return status;
    }
    bool start_migration(ui::MigrationChoice choice, std::string *reason) override
    {
        if (migration_state.phase != ui::MigrationPhase::asking)
        {
            *reason = "nothing is asked now";
            return false;
        }
        migration_state.phase = ui::MigrationPhase::working;
        migration_state.choice = choice;
        migration_steps = 0;
        migration_cancelled = false;
        return true;
    }
    void cancel_migration() override
    {
        migration_cancelled = true;
    }
    void finish_migration() override
    {
        if (migration_state.phase == ui::MigrationPhase::failed || migration_state.phase == ui::MigrationPhase::no_room ||
            migration_state.phase == ui::MigrationPhase::cancelled)
            migration_state.phase = ui::MigrationPhase::asking;
        migration_state.error.clear();
    }
    bool restart_app() override
    {
        ++restarts;
        return true;
    }

    // Game files the preview takes away from the folder, as a player would by deleting them.
    std::vector<std::string> removed;
    bool game_exists(const std::string &file) override
    {
        return std::find(removed.begin(), removed.end(), file) == removed.end();
    }
    std::vector<ui::Game> games() override
    {
        std::vector<ui::Game> present;
        for (const ui::Game &game : games_)
            if (game_exists(game.file))
                present.push_back(game);
        if (has_server)
            for (ui::Game game : remote_)
            {
                // With Office away, only Home's games are there.
                if (server_fails)
                    game.sources.erase(std::remove(game.sources.begin(), game.sources.end(), "Office"),
                                       game.sources.end());
                if (!game.sources.empty())
                    present.push_back(game);
            }
        std::sort(present.begin(), present.end(),
                  [](const ui::Game &a, const ui::Game &b) { return a.name < b.name; });
        return present;
    }
    std::string game_path(const std::string &file) override
    {
        return "/games/roms/" + file;
    }
    bool docked(std::uint64_t title_id) override;
    bool set_docked(std::uint64_t title_id, bool docked) override;
    ui::GameSettings game_settings(std::uint64_t) override
    {
        return game_settings_;
    }
    bool set_game_settings(std::uint64_t, const ui::GameSettings &settings) override
    {
        game_settings_ = settings;
        return true;
    }
    // What the keyboard gives the next time a game's RNG seed is asked for: a seed, -1 for a
    // text that is no seed, nothing for a cancelled keyboard.
    std::optional<std::int64_t> typed_seed;
    std::optional<std::int64_t> ask_seed(std::int64_t, const std::atomic<bool> &) override
    {
        return typed_seed;
    }
    ui::Preferences preferences() override
    {
        return preferences_;
    }
    bool set_preferences(const ui::Preferences &preferences) override
    {
        preferences_ = preferences;
        return true;
    }
    const std::vector<std::string> &resolution_labels() override;
    const std::vector<std::string> &resolution_keys() override;
    const std::vector<std::string> &filter_labels() override;
    const std::vector<std::string> &astc_labels() override;
    const std::vector<std::string> &post_processing_labels() override;
    const std::vector<std::string> &language_labels() override;
    std::string language_region(int language) override;
    std::string setup_details() override;
    std::string crash_reports_folder() override { return "/data/nightshade/logs"; }
    std::vector<std::string> wallpapers() override;
    std::vector<std::string> wallpaper_folders() override
    {
        return {wallpapers_directory_};
    }
    bool folders(const std::string &directory, std::vector<std::string> *names) override;
    ui::FolderInfo folder_info(const std::string &directory) override;
    std::string files_folder() override
    {
        return "/mnt/ext1/eden";
    }
    std::string saved_files_folder() override
    {
        return saved_folder_;
    }
    std::string default_files_folder() override
    {
        return "/data/nightshade";
    }
    bool set_files_folder(const std::string &directory) override
    {
        saved_folder_ = directory;
        return true;
    }
    int filesystem_access() override
    {
        return 0;
    }
    bool save_transfer_available() override
    {
        return import_available;
    }
    ui::SaveSource save_import_source(std::uint64_t) override
    {
        return import_source;
    }
    bool save_import(std::uint64_t, std::string *message) override
    {
        *message = tr("Imported. The save it replaced was backed up.");
        return true;
    }
    bool save_export(std::uint64_t, std::string *message) override
    {
        *message = fill(tr("Exported to {0}."), {"save-export/0100A00B00003000-20261001-213000"});
        return true;
    }
    // Sample mods, for three of the games and their copies: three, the second switched off, the third with one cheat
    // beside its files; and two of cheats alone, the cheats folder with cheats chosen one by one
    // (its two frame rates replace each other) and a loose file of one cheat. None when has_mods
    // is cleared.
    bool has_mods = true;
    std::vector<ui::Mod> mods(std::uint64_t) override;
    bool set_mod_enabled(std::uint64_t, const std::string &name, bool enabled) override;
    bool set_cheat_enabled(std::uint64_t, const std::string &mod, const std::string &cheat,
                           bool enabled) override;
    std::vector<ui::GameCheat> cheats(std::uint64_t title_id) override;
    // One Mods switch and one Cheats switch for every sample game.
    bool mods_enabled(std::uint64_t) override
    {
        return mods_enabled_;
    }
    bool set_mods_enabled(std::uint64_t, bool enabled) override
    {
        mods_enabled_ = enabled;
        return true;
    }
    bool cheats_enabled(std::uint64_t) override
    {
        return cheats_enabled_;
    }
    bool set_cheats_enabled(std::uint64_t, bool enabled) override
    {
        cheats_enabled_ = enabled;
        return true;
    }
    // Every sample game is in a folder of its own in roms/, named after it.
    std::string mods_folder(std::uint64_t title_id) override
    {
        return "roms/" + game_folder(title_id) + "/mods/";
    }
    std::string cheats_folder(std::uint64_t title_id) override
    {
        return "roms/" + game_folder(title_id) + "/cheats/";
    }
    bool make_mods_folder(std::uint64_t) override
    {
        return true;
    }
    bool load_image(const std::string &path, gfx::Image *image) override
    {
        return gfx::load_tga(path, image);
    }

    // Two download sources, "Home" with three games and "Office" with two (one of them Home's
    // too); a download moves a step each time the queue is looked at (the launcher looks four
    // times a second) and the game then joins the library. server_fails: Office cannot be
    // reached; download_fails: a download stops part way.
    bool has_server = false;
    bool server_fails = false;
    bool download_fails = false;
    ui::Sources sources() override;
    void refresh_sources() override
    {
        ++generation_;
    }
    bool download(const ui::Game &game, int source, bool first) override;
    bool cancel_download(const std::string &key) override;
    std::vector<ui::Download> downloads() override;
    // The queue as it is, without moving it on (for the preview to look at).
    const std::vector<ui::Download> &downloads_now() const
    {
        return queue_;
    }
    bool delete_game(const ui::Game &game, std::string *message) override;

    // The profile syncs its save data (save_sync_on); a sync takes a few looks of the launcher.
    // save_sync_conflict: both sides changed; save_sync_fails: the server cannot be reached.
    // played(): a game just ended, its save data goes up once the menu is back.
    bool save_sync_on = false;
    bool save_sync_conflict = false;
    bool save_sync_fails = false;
    bool save_sync_too_old = false; // the server runs RomM 4.9.2
    bool save_sync_wanted(const std::string &) override
    {
        return save_sync_on;
    }
    void start_save_sync(const std::string &file, bool before) override;
    ui::SaveSync save_sync() override;
    void choose_save_data(ui::SaveChoice choice) override;
    void end_save_sync() override
    {
        if (sync_.stage == ui::SaveSyncStage::done || sync_.stage == ui::SaveSyncStage::failed)
            sync_ = {};
    }
    void stop_save_sync() override
    {
        sync_ = {};
    }
    void played(const std::string &name);

    // Settings > Save sync: two profiles (the second paired with Home), two servers to pair with;
    // a pairing is approved after a few looks.
    ui::SaveSyncSetup save_sync_setup() override;
    std::vector<ui::PairServer> pair_servers() override
    {
        return {{"Home", "http://192.168.1.20:3000"}, {"Office", "https://games.example.org"}};
    }
    bool start_pairing(int profile, int server) override;
    ui::PairingStatus pairing() override;
    void cancel_pairing() override
    {
        pair_ = {};
    }
    bool unlink_profile(int profile) override;

  private:
    ui::PairingStatus pair_;
    int pair_looks_ = 0;
    int pair_profile_ = 0;
    std::vector<std::string> linked_{"", "kids @ http://192.168.1.20:3000"};
    ui::SaveSync sync_;
    int sync_looks_ = 0;
    std::vector<ui::Game> remote_; // the sources' games not on the console
    std::vector<ui::Download> queue_;
    // Where a download that goes on was (verifying reads up to there first), by its key.
    std::vector<std::pair<std::string, std::uint64_t>> going_on_;
    std::uint64_t generation_ = 1;
    std::vector<ui::Game> games_;
    std::vector<std::uint64_t> handheld_;
    ui::GameSettings game_settings_;
    ui::Preferences preferences_;
    std::string saved_folder_;
    std::string wallpapers_directory_; // <covers_directory>/wallpapers, read like the console's
    std::vector<std::uint64_t> modded_; // the games that have the sample mods
    std::vector<std::string> mods_off_{"Sharper textures"};
    std::vector<std::string> cheats_on_{"60 FPS"};
    bool bonus_cheat_ = true; // the one cheat beside the third mod's files
    // The name of a sample game, as its folder in roms/ would have it.
    std::string game_folder(std::uint64_t title_id) const
    {
        for (const ui::Game &game : games_)
            if (game.title_id == title_id)
                return game.name;
        return "Game";
    }
    bool mods_enabled_ = true;
    bool cheats_enabled_ = true;
};

} // namespace pe::host
