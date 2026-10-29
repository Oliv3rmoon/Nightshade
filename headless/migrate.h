// SPDX-FileCopyrightText: Copyright 2026 Oliv3rmoon
// SPDX-License-Identifier: GPL-3.0-or-later
// Nightshade's own title ID (PPSA64448) and data folder (/data/nightshade). Up to 1.010 it ran
// under ProsperoEden's title ID (PPSA99008) and kept everything in ProsperoEden's data folder,
// /data/prosperoeden, which the official app keeps using. The first time Nightshade starts with
// no settings of its own while that folder is there, the launcher asks once what to do with it
// (pe/ui/migrate.cpp):
//
//   Move         /data/prosperoeden is renamed to /data/nightshade: everything goes, the games too,
//                at once and without needing room. The official app no longer sees it.
//   Copy         everything but the games and the logs is copied, so both apps keep working: the
//                settings, keys and firmware, Eden's NAND (user/: saves, profiles), covers,
//                wallpapers, launchers, mods and cache/ with the driver's shader cache. The games
//                stay where they are and Nightshade reads them there ("games_folder").
//   Start fresh  nothing is taken.
//
// Nothing in /data/prosperoeden is ever deleted or written: a move is one rename of the folder,
// a copy only reads it. The folder this start already made (/data/nightshade, with this start's
// logs) is set aside first and ends up in the new data folder as logs/first-start-<time>, so the
// switch itself is two renames, each undone if the next one fails. A copy is made beside the
// data folder, in /data/nightshade.incomplete, and takes its place only once it is whole; a
// cancel, a full drive or a failure removes it. So whatever fails, both folders are as they were
// before, or the result says where things are (Result::intact).
//
// Every step is logged (EDEN_MIGRATE lines), and config/migration.json records the answer, so the
// question is never asked again. A start that asked and was closed before an answer asks again.
// The move or the copy runs on a thread of its own (Worker), never the menu's. Checked on the
// host with temporary folders: migrate_check.cpp (tools/check-migrate.py).
#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <functional>
#include <mutex>
#include <string>
#include <sys/types.h>
#include <thread>
#include <vector>

#include "pe/ui/services.hpp"

namespace Eden::Migrate {

inline constexpr const char* kOldDataDir = "/data/prosperoeden"; // ProsperoEden's (PPSA99008)
inline constexpr const char* kRecord = "config/migration.json";  // in the data folder
inline constexpr const char* kSettings = "config/prosperoeden.json"; // the settings, in either
inline constexpr const char* kIncomplete = ".incomplete";        // a copy while it is made
inline constexpr const char* kAside = ".first-start";            // this start's own folder, set aside

// What config/migration.json says.
enum class State : std::uint8_t {
    none,   // no record: nothing was asked
    asking, // asked, not answered yet
    moved,
    copied,
    fresh,  // Start fresh
};
const char* StateName(State state);

struct Folders {
    std::string old_data; // /data/prosperoeden
    std::string data;     // /data/nightshade
    // Where ProsperoEden's roms/ and updates/ are (its "games_folder", else its game files folder,
    // else old_data). A copy leaves them there when they are inside old_data.
    std::string games;
};

// ---- what the migration asks of the system (Native(); the check gives its own) ----
struct Entry {
    std::string name;
    bool folder = false;
};
struct Space {
    std::uint64_t free = 0; // bytes the drive can still take
    std::uint64_t unit = 0; // what it allocates at a time; 0: not known
};
// The settings of the moved or copied folder: a game files folder or games folder saved inside
// `from` is named inside `to`; with `games` set, games_folder is set to it (the games stayed in
// `from`). What changed, for the log, in *changes; false (and *error) when the file can't be written.
struct Rewrite {
    std::string from;
    std::string to;
    std::string games;
};
struct Platform {
    // A folder's files and folders (not "." or ".."); false when it can't be read.
    std::function<bool(const std::string& folder, std::vector<Entry>* entries)> list;
    // The room on the drive that holds a path; false when the system can't tell (a PS5 title).
    std::function<bool(const std::string& path, Space* space)> space;
    // ::write and ::rename (0, or the errno).
    std::function<ssize_t(int fd, const void* data, std::size_t size)> write;
    std::function<int(const std::string& from, const std::string& to)> rename;
    // Rewrites the settings file `file` (settings_store.h, MoveFolders); may be empty: nothing to do.
    std::function<bool(const std::string& file, const Rewrite& rewrite, std::string* changes, std::string* error)>
        settings;
    // An EDEN_MIGRATE line.
    std::function<void(const std::string& line)> log;
    // Before each file is copied, with its place in the plan (the check cancels or fills the drive
    // there); may be empty.
    std::function<void(std::size_t file, const std::string& path)> before_file;
    std::function<std::time_t()> clock; // empty: the system's
};
// The console's (or the host's): its directory reader, ::write, ::rename, the room where it can be
// had, and the log on stderr and the kernel log. settings stays empty.
Platform Native();

// ---- whether to ask ----
// What the record in `data` says.
State Recorded(const std::string& data);
// At the start, before anything writes the settings file: the record's state, or asking when the
// data folder has no settings and no record while old_data holds something (logs alone are
// nothing); the record then says asking. A copy left unfinished by an earlier start is removed.
State Prepare(const Folders& folders, const Platform& platform);
// Writes the record (true when written).
bool Record(const std::string& data, State state, const Folders& folders, const Platform& platform,
            const std::string& games_folder = {});

// ---- what a copy copies ----
struct File {
    std::string path; // inside old_data, '/' between folders
    std::uint64_t size = 0;
};
struct Plan {
    std::vector<std::string> folders; // inside old_data, each before what it holds
    std::vector<File> files;
    std::uint64_t bytes = 0;
};
// What a copy leaves out, as paths inside old_data: the logs, the version ProsperoEden's last
// self-update replaced (previous-version), the NSZ games Eden unpacked (user/cache/nsz, made again
// when needed) and the games: roms/, updates/ and .remote-downloads/ of the games folder when it is
// inside old_data. Sorted.
std::vector<std::string> Excluded(const Folders& folders);
// Whether the games stay behind in a copy: the games folder is old_data or inside it.
bool GamesInside(const Folders& folders);
// The folders and files under old_data but the excluded ones, sorted; false (error says why, in
// English) when a folder can't be read or stop is set.
bool Measure(const std::string& from, const std::vector<std::string>& excluded, const Platform& platform,
             const std::atomic<bool>& stop, Plan* plan, std::string* error);
// The room a plan takes on a drive that allocates `unit` bytes at a time (4096 when not known).
std::uint64_t Needed(const Plan& plan, std::uint64_t unit);

// ---- the move or the copy ----
using Choice = pe::ui::MigrationChoice;
struct Progress {
    std::atomic<std::uint64_t> files{0};
    std::atomic<std::uint64_t> bytes{0};
};
enum class Outcome : std::uint8_t { done, cancelled, failed, no_room };
struct Result {
    Outcome outcome = Outcome::failed;
    std::string error;         // failed, no_room: why (English)
    // failed: both folders are as they were. False only when a step could not be undone: error
    // then says where each folder is.
    bool intact = true;
    std::string aside;         // done: where this start's own folder went (empty: there was none)
    std::string games_folder;  // done (a copy): the games folder set, where the games stayed
    std::uint64_t files = 0;   // done (a copy): copied
    std::uint64_t bytes = 0;
    bool room_known = false;   // no_room before anything was written: needed and free
    std::uint64_t needed = 0;
    std::uint64_t free = 0;
};
// Moves or copies (a copy measures first when plan is null), as the top of this file says, and
// records the answer. Start fresh only records it.
Result Run(Choice choice, const Folders& folders, const Plan* plan, const Platform& platform, Progress& progress,
           const std::atomic<bool>& stop);

// ---- the launcher's dialog (EdenServices) ----
struct Setup {
    Folders folders;
    bool access = true; // the filesystem access (storage_paths.h): without it, nothing is asked
    Platform platform;
};
// Asks (Prepare) when made; measures what a copy copies, and moves or copies, on a thread of its
// own: one thing at a time.
class Worker {
public:
    explicit Worker(Setup setup);
    // A copy that runs stops (and what it wrote is removed) before the worker goes.
    ~Worker();
    Worker(const Worker&) = delete;
    Worker& operator=(const Worker&) = delete;

    pe::ui::MigrationStatus Status();
    // Begins the answer (false: it could not begin, and reason says why, in English); stops a
    // copy; and takes a cancel's or a failure's end, back to the question.
    bool Start(Choice choice, std::string* reason);
    void Cancel();
    void Finish();

private:
    // Called with lock_ held: runs work on the thread.
    void Begin(std::function<void()> work);
    void Look();
    void Answer(Choice choice);

    Setup setup_;
    std::mutex lock_;
    std::thread thread_;
    bool busy_ = false;
    std::atomic<bool> stop_{false};
    Progress progress_;
    Plan plan_;
    bool planned_ = false;
    pe::ui::MigrationStatus status_;
};

} // namespace Eden::Migrate
