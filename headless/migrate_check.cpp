// SPDX-FileCopyrightText: Copyright 2026 Oliv3rmoon
// SPDX-License-Identifier: GPL-3.0-or-later
// Host check of the move from ProsperoEden's data folder (migrate.h), with folders made in the
// temporary folder: when it asks, Move, Copy and Start fresh, what a copy leaves out and where the
// games stay, a drive without room (known before, or filling up), a cancel, a failure at each step
// and what is undone, the record, the EDEN_MIGRATE lines, and the worker. After every case
// ProsperoEden's folder is compared with what it held before: nothing in it may change, unless it
// was moved whole. tools/check-migrate.py builds and runs it.
#include "migrate.h"

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

namespace fs = std::filesystem;
using Eden::Migrate::Choice;
using Eden::Migrate::Folders;
using Eden::Migrate::Outcome;
using Eden::Migrate::Platform;
using Eden::Migrate::Result;
using Eden::Migrate::State;

namespace {
int checks = 0;
int failures = 0;

void expect(bool condition, const std::string& what) {
    ++checks;
    if (condition) return;
    ++failures;
    std::printf("FAIL %s\n", what.c_str());
}

void put(const fs::path& path, const std::string& text) {
    fs::create_directories(path.parent_path());
    std::ofstream(path, std::ios::binary) << text;
}

std::string read(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), {}};
}

// Every file and folder under a folder, with what each file holds ("/" for a folder).
std::map<std::string, std::string> tree(const fs::path& root) {
    std::map<std::string, std::string> out;
    if (!fs::exists(root)) return out;
    for (const auto& entry : fs::recursive_directory_iterator(root)) {
        const std::string name = fs::relative(entry.path(), root).generic_string();
        out[name] = entry.is_directory() ? std::string{"/"} : read(entry.path());
    }
    return out;
}

// ProsperoEden's data folder as a player's console has it.
void make_old(const fs::path& old, bool settings = true) {
    if (settings)
        put(old / "config/prosperoeden.json", R"({"version":1,"game_files":"/data/prosperoeden"})");
    put(old / "config/remote/sources.json", "{}");
    put(old / "user/nand/user/save/0000000000000000/1/main.sav", std::string(5000, 's'));
    put(old / "user/nand/system/save/8000000000000010/su/avators/profiles.dat", "profiles");
    put(old / "user/shader/0100000000010000/vulkan.bin", "pipelines");
    put(old / "user/cache/nsz/unpacked.nca", std::string(9000, 'n'));
    for (int i = 0; i < 40; ++i)
        put(old / ("cache/radv/" + std::to_string(i % 4) + "/entry-" + std::to_string(i)), std::string(100 + i, 'r'));
    put(old / "keys/prod.keys", "keys");
    put(old / "firmware/a.nca", "firmware");
    put(old / "covers/1a2b3c4d.tga", "cover");
    put(old / "wallpapers/sky.png", "png");
    put(old / "launchers/pack/manifest.json", "{}");
    put(old / "mods/0100000000010000/mod/romfs/a", "mod");
    put(old / "roms/Game [0100000000010000].nsp", std::string(20000, 'g'));
    put(old / "roms/Folder Game/Folder Game.xci", std::string(3000, 'x'));
    put(old / "updates/Game Update.nsp", std::string(4000, 'u'));
    put(old / ".remote-downloads/part.nsp", "part");
    put(old / "logs/stderr.log", "old log");
    put(old / "previous-version/PPSA99008-backup/eboot.bin", "old eboot");
}

// What this start made in Nightshade's own folder before the question: its logs, empty folders.
void make_fresh(const fs::path& data) {
    put(data / "logs/stderr.log", "this start");
    fs::create_directories(data / "config");
    fs::create_directories(data / "user/cache/nsz");
    fs::create_directories(data / "cache/radv");
    fs::create_directories(data / "keys");
}

struct Lab {
    fs::path root;
    Folders folders;
    std::vector<std::string> lines;
    std::vector<std::pair<std::string, Eden::Migrate::Rewrite>> rewrites;
    Platform platform;
    std::atomic<bool> stop{false};
    Eden::Migrate::Progress progress;

    explicit Lab(const std::string& name) {
        root = fs::path(std::getenv("TMPDIR") ? std::getenv("TMPDIR") : "/tmp") / ("eden-migrate-" + name);
        fs::remove_all(root);
        fs::create_directories(root);
        folders.old_data = (root / "prosperoeden").string();
        folders.data = (root / "nightshade").string();
        folders.games = folders.old_data;
        platform = Eden::Migrate::Native();
        platform.log = [this](const std::string& line) { lines.push_back(line); };
        platform.settings = [this](const std::string& file, const Eden::Migrate::Rewrite& rewrite, std::string* changes,
                                   std::string*) {
            rewrites.push_back({file, rewrite});
            *changes = "game_files=" + rewrite.to + (rewrite.games.empty() ? "" : " games_folder=" + rewrite.games);
            return true;
        };
        platform.clock = [] { return std::time_t{1793280000}; }; // 2026-10-29
    }
    ~Lab() {
        // MIGRATE_CHECK_VERBOSE=1: the EDEN_MIGRATE lines of every case.
        if (std::getenv("MIGRATE_CHECK_VERBOSE"))
            for (const std::string& line : lines) std::printf("  %s\n", line.c_str());
        fs::remove_all(root);
    }
    fs::path old() const { return folders.old_data; }
    fs::path data() const { return folders.data; }
    bool logged(const std::string& text) const {
        for (const std::string& line : lines)
            if (line.find(text) != std::string::npos) return true;
        return false;
    }
    Result run(Choice choice) { return Eden::Migrate::Run(choice, folders, nullptr, platform, progress, stop); }
};

void check_asking() {
    {
        Lab lab("asking-none");
        expect(Eden::Migrate::Prepare(lab.folders, lab.platform) == State::none, "no ProsperoEden folder: nothing to ask");
        put(lab.old() / "logs/stderr.log", "log");
        expect(Eden::Migrate::Prepare(lab.folders, lab.platform) == State::none, "logs alone are nothing to ask about");
        expect(!fs::exists(lab.data() / Eden::Migrate::kRecord), "nothing asked: no record");
    }
    {
        Lab lab("asking");
        make_old(lab.old());
        make_fresh(lab.data());
        expect(Eden::Migrate::Prepare(lab.folders, lab.platform) == State::asking, "ProsperoEden's data: asked");
        expect(Eden::Migrate::Recorded(lab.folders.data) == State::asking, "the record says asking");
        expect(read(lab.data() / Eden::Migrate::kRecord).find("\"from\": \"" + lab.folders.old_data + "\"") != std::string::npos,
               "the record names the old folder");
        expect(lab.logged("EDEN_MIGRATE ask from="), "the question is logged");
        // Closed without an answer: the settings written meanwhile don't stop it being asked again.
        put(lab.data() / Eden::Migrate::kSettings, "{}");
        expect(Eden::Migrate::Prepare(lab.folders, lab.platform) == State::asking, "unanswered: asked again");
        expect(lab.logged("EDEN_MIGRATE ask again"), "asked again, logged");
    }
    {
        Lab lab("own-settings");
        make_old(lab.old());
        put(lab.data() / Eden::Migrate::kSettings, "{}");
        expect(Eden::Migrate::Prepare(lab.folders, lab.platform) == State::none, "Nightshade's own settings: nothing asked");
        // A copy an earlier start left unfinished goes, but not on the start's own thread.
        put(fs::path(lab.folders.data + Eden::Migrate::kIncomplete) / "user/a", "half");
        (void)Eden::Migrate::Prepare(lab.folders, lab.platform);
        expect(fs::exists(lab.folders.data + Eden::Migrate::kIncomplete), "Prepare leaves an unfinished copy alone");
        expect(Eden::Migrate::Tidy(lab.folders, lab.platform) && !fs::exists(lab.folders.data + Eden::Migrate::kIncomplete),
               "Tidy removes an unfinished copy");
        expect(lab.logged("EDEN_MIGRATE leftover path="), "and that is logged");
        expect(Eden::Migrate::Tidy(lab.folders, lab.platform), "Tidy with nothing to remove");
    }
    {
        Lab lab("answered");
        make_old(lab.old());
        for (const State state : {State::moved, State::copied, State::fresh}) {
            fs::remove_all(lab.data());
            expect(Eden::Migrate::Record(lab.folders.data, state, lab.folders, lab.platform), "a record written");
            expect(Eden::Migrate::Prepare(lab.folders, lab.platform) == state,
                   std::string{"answered ("} + Eden::Migrate::StateName(state) + "): never asked again");
        }
        put(lab.data() / Eden::Migrate::kRecord, "{\"state\": \"other\"}");
        expect(Eden::Migrate::Recorded(lab.folders.data) == State::none, "a record it doesn't know reads as none");
    }
}

void check_excluded() {
    Folders folders{"/data/prosperoeden", "/data/nightshade", "/data/prosperoeden"};
    const std::vector<std::string> plain = Eden::Migrate::Excluded(folders);
    expect(plain == std::vector<std::string>{".remote-downloads", "logs", "previous-version", "roms", "updates", "user/cache/nsz"},
           "a copy leaves out the games, logs, the previous version and unpacked NSZs");
    folders.games = "/data/prosperoeden/eden/";
    expect(Eden::Migrate::GamesInside(folders), "a games folder inside the old one");
    expect(Eden::Migrate::Excluded(folders) ==
               std::vector<std::string>{"eden/.remote-downloads", "eden/roms", "eden/updates", "logs", "previous-version",
                                        "user/cache/nsz"},
           "its games are left out, not the old folder's roms/");
    for (const char* elsewhere : {"/mnt/usb0/games", "/data/prosperoeden-games", "", "/data"}) {
        folders.games = elsewhere;
        expect(!Eden::Migrate::GamesInside(folders), std::string{"games elsewhere: "} + elsewhere);
        expect(Eden::Migrate::Excluded(folders) == std::vector<std::string>{"logs", "previous-version", "user/cache/nsz"},
               std::string{"games elsewhere leave nothing else out: "} + elsewhere);
    }
    Eden::Migrate::Plan plan;
    plan.folders = {"a", "a/b"};
    plan.files = {{"a/x", 1}, {"a/b/y", 4097}};
    expect(Eden::Migrate::Needed(plan, 4096) == (1u << 20) + 3 * 4096 + 4096 + 8192, "the room a copy needs");
    expect(Eden::Migrate::Needed(plan, 0) == Eden::Migrate::Needed(plan, 4096), "4096 when the unit is not known");
}

void check_move() {
    {
        Lab lab("move");
        make_old(lab.old());
        make_fresh(lab.data());
        const auto before = tree(lab.old());
        const Result result = lab.run(Choice::move);
        expect(result.outcome == Outcome::done && result.intact, "Move: done");
        expect(!fs::exists(lab.old()), "Move: the old folder's name is gone (it is renamed, nothing deleted)");
        auto after = tree(lab.data());
        bool all = true;
        for (const auto& [path, content] : before)
            if (!after.count(path) || after[path] != content) all = false;
        expect(all, "Move: everything the old folder held is in the new one, the games too");
        expect(fs::exists(lab.data() / "roms/Game [0100000000010000].nsp") && fs::exists(lab.data() / "cache/radv/0/entry-0"),
               "Move: the games and the driver's shader cache went along");
        expect(Eden::Migrate::Recorded(lab.folders.data) == State::moved, "Move: recorded");
        expect(result.aside == (lab.data() / "logs/first-start-20261029-000000").string() ||
                   result.aside.find("/logs/first-start-") != std::string::npos,
               "Move: this start's folder is in the new logs: " + result.aside);
        expect(read(fs::path(result.aside) / "logs/stderr.log") == "this start", "Move: this start's log kept");
        expect(read(lab.data() / "logs/stderr.log") == "old log", "Move: the old logs stay as they were");
        expect(!fs::exists(lab.folders.data + Eden::Migrate::kAside), "Move: nothing left beside the data folder");
        expect(lab.rewrites.size() == 1 && lab.rewrites[0].first == (lab.data() / Eden::Migrate::kSettings).string() &&
                   lab.rewrites[0].second.from == lab.folders.old_data && lab.rewrites[0].second.to == lab.folders.data &&
                   lab.rewrites[0].second.games.empty(),
               "Move: the settings name the new place, no games folder set");
        for (const char* line : {"EDEN_MIGRATE start choice=move", "EDEN_MIGRATE aside from=", "EDEN_MIGRATE moved from=",
                                 "EDEN_MIGRATE settings file=", "EDEN_MIGRATE record state=moved",
                                 "EDEN_MIGRATE aside placed", "EDEN_MIGRATE done choice=move"})
            expect(lab.logged(line), std::string{"Move: logged "} + line);
        expect(Eden::Migrate::Prepare(lab.folders, lab.platform) == State::moved, "Move: never asked again");
    }
    {
        // Nothing made yet for Nightshade: only the one rename.
        Lab lab("move-no-data");
        make_old(lab.old());
        const auto before = tree(lab.old());
        const Result result = lab.run(Choice::move);
        expect(result.outcome == Outcome::done && result.aside.empty(), "Move without a folder of this start");
        auto after = tree(lab.data());
        after.erase("config/migration.json");
        expect(after == before, "Move without a folder of this start: the same files");
    }
    // A failure at each step: both folders as they were.
    const auto failing = [](const std::string& name, int at) {
        Lab lab(name);
        make_old(lab.old());
        make_fresh(lab.data());
        const auto old_before = tree(lab.old()), data_before = tree(lab.data());
        int calls = 0;
        lab.platform.rename = [&calls, at](const std::string& from, const std::string& to) {
            if (++calls == at) return EACCES;
            return std::rename(from.c_str(), to.c_str()) == 0 ? 0 : errno;
        };
        const Result result = lab.run(Choice::move);
        expect(result.outcome == Outcome::failed && result.intact && !result.error.empty(),
               name + ": failed, both folders intact: " + result.error);
        expect(tree(lab.old()) == old_before && tree(lab.data()) == data_before, name + ": nothing changed");
        expect(lab.logged("EDEN_MIGRATE failed step="), name + ": the failure is logged");
        expect(Eden::Migrate::Recorded(lab.folders.data) == State::none, name + ": not recorded as answered");
    };
    failing("move-aside-fails", 1);
    failing("move-rename-fails", 2);
    {
        Lab lab("move-settings-fail");
        make_old(lab.old());
        make_fresh(lab.data());
        const auto old_before = tree(lab.old()), data_before = tree(lab.data());
        lab.platform.settings = [](const std::string&, const Eden::Migrate::Rewrite&, std::string*, std::string* error) {
            *error = "disk error";
            return false;
        };
        const Result result = lab.run(Choice::move);
        expect(result.outcome == Outcome::failed && result.intact, "Move: settings that can't be written undo the move");
        expect(tree(lab.old()) == old_before && tree(lab.data()) == data_before, "Move: settings failure: nothing changed");
        expect(lab.logged("EDEN_MIGRATE undone from="), "Move: the undo is logged");
    }
    {
        // The undo itself fails: the result says where things are.
        Lab lab("move-undo-fails");
        make_old(lab.old());
        make_fresh(lab.data());
        const auto old_before = tree(lab.old());
        int calls = 0;
        lab.platform.rename = [&calls](const std::string& from, const std::string& to) {
            if (++calls >= 2) return EIO;
            return std::rename(from.c_str(), to.c_str()) == 0 ? 0 : errno;
        };
        const Result result = lab.run(Choice::move);
        expect(result.outcome == Outcome::failed && !result.intact &&
                   result.error.find(lab.folders.data + Eden::Migrate::kAside) != std::string::npos,
               "Move: an undo that fails names where this start's folder is: " + result.error);
        expect(tree(lab.old()) == old_before, "Move: undo failed, ProsperoEden's folder still untouched");
        expect(lab.logged("EDEN_MIGRATE undo failed"), "Move: the failed undo is logged");
    }
}

void check_copy() {
    {
        Lab lab("copy");
        make_old(lab.old());
        make_fresh(lab.data());
        const auto before = tree(lab.old());
        const Result result = lab.run(Choice::copy);
        expect(result.outcome == Outcome::done && result.intact, "Copy: done " + result.error);
        expect(tree(lab.old()) == before, "Copy: ProsperoEden's folder is exactly as it was");
        for (const char* kept : {"config/prosperoeden.json", "config/remote/sources.json", "user/nand/user/save/0000000000000000/1/main.sav",
                                 "user/nand/system/save/8000000000000010/su/avators/profiles.dat",
                                 "user/shader/0100000000010000/vulkan.bin", "cache/radv/3/entry-39", "keys/prod.keys",
                                 "firmware/a.nca", "covers/1a2b3c4d.tga", "wallpapers/sky.png", "launchers/pack/manifest.json",
                                 "mods/0100000000010000/mod/romfs/a"})
            expect(read(lab.data() / kept) == read(lab.old() / kept), std::string{"Copy: copied "} + kept);
        for (const char* left : {"roms", "updates", ".remote-downloads", "previous-version", "user/cache/nsz/unpacked.nca"})
            expect(!fs::exists(lab.data() / left), std::string{"Copy: left out "} + left);
        expect(read(lab.data() / "logs/stderr.log").empty() && fs::is_directory(lab.data() / "logs"),
               "Copy: the old logs are not copied; the logs folder holds this start's");
        expect(result.aside.find("/logs/first-start-") != std::string::npos &&
                   read(fs::path(result.aside) / "logs/stderr.log") == "this start",
               "Copy: this start's folder is in the new logs");
        expect(result.games_folder == lab.folders.old_data, "Copy: the games stay in the old folder");
        expect(lab.rewrites.size() == 1 &&
                   lab.rewrites[0].first == lab.folders.data + Eden::Migrate::kIncomplete + "/" + Eden::Migrate::kSettings &&
                   lab.rewrites[0].second.games == lab.folders.old_data,
               "Copy: the copy's settings set the games folder to where the games are");
        expect(Eden::Migrate::Recorded(lab.folders.data) == State::copied, "Copy: recorded");
        expect(read(lab.data() / Eden::Migrate::kRecord).find("\"games_folder\": \"" + lab.folders.old_data + "\"") != std::string::npos,
               "Copy: the record names the games folder");
        expect(!fs::exists(lab.folders.data + Eden::Migrate::kIncomplete) && !fs::exists(lab.folders.data + Eden::Migrate::kAside),
               "Copy: nothing left beside the data folder");
        expect(lab.progress.files.load() == 51 && result.files == 51, "Copy: every file counted: " + std::to_string(result.files));
        for (const char* line : {"EDEN_MIGRATE start choice=copy", "EDEN_MIGRATE plan files=51", "EDEN_MIGRATE room needed=",
                                 "EDEN_MIGRATE copied files=51", "EDEN_MIGRATE games_folder=" , "EDEN_MIGRATE record state=copied",
                                 "EDEN_MIGRATE aside from=", "EDEN_MIGRATE copied into=", "EDEN_MIGRATE done choice=copy"})
            expect(lab.logged(line), std::string{"Copy: logged "} + line);
    }
    {
        // The games in a folder of the old one (its game files folder there), and games elsewhere.
        Lab lab("copy-games-inside");
        make_old(lab.old());
        put(lab.old() / "eden/roms/Other.nsp", "other");
        put(lab.old() / "eden/keys/prod.keys", "keys");
        lab.folders.games = lab.folders.old_data + "/eden";
        const Result result = lab.run(Choice::copy);
        expect(result.outcome == Outcome::done && result.games_folder == lab.folders.old_data + "/eden",
               "Copy: games in a folder of the old one stay there");
        expect(!fs::exists(lab.data() / "eden/roms") && fs::exists(lab.data() / "eden/keys/prod.keys") &&
                   fs::exists(lab.data() / "roms/Game [0100000000010000].nsp"),
               "Copy: only that folder's games are left; the rest of it, and another roms/, are copied");
    }
    {
        Lab lab("copy-games-elsewhere");
        make_old(lab.old());
        lab.folders.games = (lab.root / "usb/games").string();
        const Result result = lab.run(Choice::copy);
        expect(result.outcome == Outcome::done && result.games_folder.empty() && lab.rewrites[0].second.games.empty(),
               "Copy: games elsewhere: no games folder set");
        expect(!lab.logged("EDEN_MIGRATE games_folder="), "Copy: games elsewhere: no games_folder line");
    }
    {
        // Room known and short: refused before anything is written.
        Lab lab("copy-no-room");
        make_old(lab.old());
        make_fresh(lab.data());
        const auto old_before = tree(lab.old()), data_before = tree(lab.data());
        lab.platform.space = [](const std::string&, Eden::Migrate::Space* space) {
            space->free = 40000;
            space->unit = 4096;
            return true;
        };
        const Result result = lab.run(Choice::copy);
        expect(result.outcome == Outcome::no_room && result.room_known && result.needed > result.free &&
                   result.free == 40000,
               "Copy without room: refused with what it needs and has");
        expect(tree(lab.old()) == old_before && tree(lab.data()) == data_before &&
                   !fs::exists(lab.folders.data + Eden::Migrate::kIncomplete),
               "Copy without room: nothing written");
        expect(lab.logged("EDEN_MIGRATE no_room before writing"), "Copy without room: logged");
    }
    {
        // Room not known (the console): the drive fills up part way.
        Lab lab("copy-fills-up");
        make_old(lab.old());
        make_fresh(lab.data());
        const auto old_before = tree(lab.old()), data_before = tree(lab.data());
        lab.platform.space = [](const std::string&, Eden::Migrate::Space*) { return false; };
        std::size_t written = 0;
        lab.platform.write = [&written](int fd, const void* data, std::size_t size) -> ssize_t {
            if (written + size > 6000) {
                errno = ENOSPC;
                return -1;
            }
            written += size;
            return ::write(fd, data, size);
        };
        const Result result = lab.run(Choice::copy);
        expect(result.outcome == Outcome::no_room && !result.room_known && result.needed > 0,
               "Copy on a drive that fills up: no room, with what it needs");
        expect(tree(lab.old()) == old_before && tree(lab.data()) == data_before &&
                   !fs::exists(lab.folders.data + Eden::Migrate::kIncomplete),
               "Copy on a drive that fills up: what it wrote is removed, the rest as it was");
        expect(lab.logged("EDEN_MIGRATE room unknown") && lab.logged("removed=1"), "Copy that fills up: logged");
    }
    {
        Lab lab("copy-cancel");
        make_old(lab.old());
        make_fresh(lab.data());
        const auto old_before = tree(lab.old()), data_before = tree(lab.data());
        lab.platform.before_file = [&lab](std::size_t file, const std::string&) {
            if (file == 5) lab.stop = true;
        };
        const Result result = lab.run(Choice::copy);
        expect(result.outcome == Outcome::cancelled, "Copy: cancelled");
        expect(tree(lab.old()) == old_before && tree(lab.data()) == data_before &&
                   !fs::exists(lab.folders.data + Eden::Migrate::kIncomplete),
               "Copy cancelled: what it wrote is removed");
        expect(Eden::Migrate::Recorded(lab.folders.data) == State::none, "Copy cancelled: not answered");
    }
    {
        Lab lab("copy-switch-fails");
        make_old(lab.old());
        make_fresh(lab.data());
        const auto old_before = tree(lab.old()), data_before = tree(lab.data());
        int calls = 0;
        lab.platform.rename = [&calls](const std::string& from, const std::string& to) {
            if (++calls == 2) return EBUSY; // the copy taking the data folder's name
            return std::rename(from.c_str(), to.c_str()) == 0 ? 0 : errno;
        };
        const Result result = lab.run(Choice::copy);
        expect(result.outcome == Outcome::failed && result.intact, "Copy: the switch fails: " + result.error);
        expect(tree(lab.old()) == old_before && tree(lab.data()) == data_before &&
                   !fs::exists(lab.folders.data + Eden::Migrate::kIncomplete),
               "Copy: the switch fails: this start's folder is back, the copy removed");
    }
    {
        Lab lab("copy-settings-fail");
        make_old(lab.old());
        make_fresh(lab.data());
        const auto data_before = tree(lab.data());
        lab.platform.settings = [](const std::string&, const Eden::Migrate::Rewrite&, std::string*, std::string* error) {
            *error = "disk error";
            return false;
        };
        const Result result = lab.run(Choice::copy);
        expect(result.outcome == Outcome::failed && tree(lab.data()) == data_before &&
                   !fs::exists(lab.folders.data + Eden::Migrate::kIncomplete),
               "Copy: settings that can't be written: the copy is removed");
    }
}

void check_fresh() {
    Lab lab("fresh");
    make_old(lab.old());
    make_fresh(lab.data());
    const auto old_before = tree(lab.old());
    const Result result = lab.run(Choice::fresh);
    expect(result.outcome == Outcome::done && tree(lab.old()) == old_before, "Start fresh: nothing taken");
    expect(Eden::Migrate::Recorded(lab.folders.data) == State::fresh &&
               Eden::Migrate::Prepare(lab.folders, lab.platform) == State::fresh,
           "Start fresh: recorded, never asked again");
    expect(lab.logged("EDEN_MIGRATE done choice=fresh"), "Start fresh: logged");
}

template <typename Ready>
bool wait_for(Ready ready) {
    for (int i = 0; i < 2000; ++i) {
        if (ready()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return false;
}

void check_worker() {
    using Phase = pe::ui::MigrationPhase;
    {
        Lab lab("worker-none");
        Eden::Migrate::Worker worker({lab.folders, true, lab.platform});
        expect(worker.Status().phase == Phase::none, "worker: nothing to ask");
        std::string reason;
        expect(!worker.Start(Choice::move, &reason) && !reason.empty(), "worker: no answer without a question");
    }
    {
        Lab lab("worker-no-access");
        make_old(lab.old());
        Eden::Migrate::Worker worker({lab.folders, false, lab.platform});
        expect(worker.Status().phase == Phase::none, "worker: without filesystem access nothing is asked");
    }
    {
        Lab lab("worker-copy");
        make_old(lab.old());
        make_fresh(lab.data());
        put(fs::path(lab.folders.data + Eden::Migrate::kIncomplete) / "user/a", "half");
        Eden::Migrate::Worker worker({lab.folders, true, lab.platform});
        auto status = worker.Status();
        expect(status.phase == Phase::asking && status.from == lab.folders.old_data && status.to == lab.folders.data &&
                   status.games == lab.folders.old_data,
               "worker: asks, naming both folders and where a copy leaves the games");
        expect(wait_for([&] { return worker.Status().measured; }), "worker: measures what a copy copies");
        expect(!fs::exists(lab.folders.data + Eden::Migrate::kIncomplete), "worker: an unfinished copy removed on its thread");
        status = worker.Status();
        expect(status.files == 51 && status.bytes > 0, "worker: 51 files to copy");
        std::string reason;
        expect(worker.Start(Choice::copy, &reason), "worker: Copy begins");
        expect(wait_for([&] { return worker.Status().phase != Phase::working; }), "worker: Copy ends");
        status = worker.Status();
        expect(status.phase == Phase::done && status.choice == Choice::copy && status.done_files == 51,
               "worker: Copy done, every file counted");
        expect(!worker.Start(Choice::copy, &reason), "worker: answered once");
    }
    {
        // Move while the measure runs (a slow folder): the measure stops, the move goes on.
        Lab lab("worker-move");
        make_old(lab.old());
        make_fresh(lab.data());
        auto list = lab.platform.list;
        lab.platform.list = [list](const std::string& folder, std::vector<Eden::Migrate::Entry>* entries) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            return list(folder, entries);
        };
        Eden::Migrate::Worker worker({lab.folders, true, lab.platform});
        std::string reason;
        expect(worker.Start(Choice::move, &reason), "worker: Move begins while measuring");
        expect(wait_for([&] { return worker.Status().phase != Phase::working; }), "worker: Move ends");
        expect(worker.Status().phase == Phase::done && !fs::exists(lab.old()), "worker: moved");
    }
    {
        // A copy cancelled, then back to the question; Start fresh closes it.
        Lab lab("worker-cancel");
        make_old(lab.old());
        make_fresh(lab.data());
        std::atomic<bool> go{false};
        lab.platform.before_file = [&go](std::size_t file, const std::string&) {
            if (file == 3)
                while (!go.load()) std::this_thread::sleep_for(std::chrono::milliseconds(2));
        };
        Eden::Migrate::Worker worker({lab.folders, true, lab.platform});
        expect(wait_for([&] { return worker.Status().measured; }), "worker: measured before the cancel");
        std::string reason;
        expect(worker.Start(Choice::copy, &reason), "worker: Copy begins");
        expect(wait_for([&] { return worker.Status().done_files >= 3; }), "worker: Copy under way");
        worker.Cancel();
        go = true;
        expect(wait_for([&] { return worker.Status().phase != Phase::working; }), "worker: the cancel ends it");
        expect(worker.Status().phase == Phase::cancelled && !fs::exists(lab.folders.data + Eden::Migrate::kIncomplete),
               "worker: cancelled, what it wrote removed");
        worker.Finish();
        expect(worker.Status().phase == Phase::asking, "worker: back to the question");
        expect(worker.Start(Choice::fresh, &reason), "worker: Start fresh");
        expect(wait_for([&] { return worker.Status().phase != Phase::working; }), "worker: Start fresh ends");
        expect(worker.Status().phase == Phase::none && Eden::Migrate::Recorded(lab.folders.data) == State::fresh,
               "worker: Start fresh closes the question, recorded");
    }
}
} // namespace

int main() {
    check_asking();
    check_excluded();
    check_move();
    check_copy();
    check_fresh();
    check_worker();
    std::printf("%s migrate: %d checks of when it asks, Move, Copy, Start fresh, rooms, cancels, failures, undoing and the "
                "worker, %d failed\n",
                failures ? "FAIL" : "PASS", checks, failures);
    return failures ? 1 : 0;
}
