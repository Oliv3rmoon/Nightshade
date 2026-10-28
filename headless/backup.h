// SPDX-FileCopyrightText: Copyright 2026 Oliv3rmoon
// SPDX-License-Identifier: GPL-3.0-or-later
// Settings > Diagnostics > Back up to USB (upstream request 157): the app's data folder,
// /data/prosperoeden (settings, profiles, every save, the keys and firmware when the game files
// folder is there), copied file by file to a USB drive as it is, without what the app makes again
// by itself (Excluded). It goes to <drive>/nightshade-backup/<YYYYMMDD-HHMMSS>/, with backup.json
// (the app's version, the date, how many files and bytes) and SHA256SUMS (sha256sum's format)
// beside the files. Nothing is restored from here: docs/INSTALL.md says how to copy a backup back
// by hand.
//
// The copy is made in <YYYYMMDD-HHMMSS>.incomplete and takes its name only once every file,
// SHA256SUMS and backup.json are written and synced to the drive, so a folder without
// ".incomplete" is a whole backup. A backup that is cancelled or fails is removed; when that fails
// too (the drive pulled out) it keeps the ".incomplete" name. Nothing is ever written over: a
// file that is there already stops the backup.
//
// Where the system says how much room a drive has (the host), a backup that doesn't fit is refused
// before anything is written. A PS5 title can't ask (statfs ends it: remote/remote.cpp), so there
// the backup goes ahead and stops at the first write the drive refuses as full, removing what it
// wrote. Everything runs on a thread of its own (Worker), never the menu's. Checked on the host
// with a temporary folder as the drive: backup_check.cpp.
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

namespace Eden::Backup {

inline constexpr const char* kFolder = "nightshade-backup"; // on the drive, holding the backups
inline constexpr const char* kIncomplete = ".incomplete";   // a backup's folder while it is made
inline constexpr const char* kManifest = "backup.json";
inline constexpr const char* kSums = "SHA256SUMS";

// The drives a backup can go to: the USB drives, /mnt/usb0 to /mnt/usb7, as the self-update
// helper names them (self_update_helper/updater.cpp, default_drives). A PC reads them, which the
// console's extended storage (/mnt/ext*) it can't.
std::vector<std::string> UsbDrives();

// What a backup leaves out of the data folder, as paths inside it: the driver's shader cache
// (cache/), the covers (read again from the games), the logs, the version the last update
// replaced (previous-version/), Eden's caches, pipeline caches, log and dumps (user/cache/, which
// also holds the unpacked NSZ games, user/shader/, user/log/, user/crash_dumps/, user/dump/), and
// the games themselves with the downloads not finished yet (roms/, updates/, .remote-downloads/,
// where the data folder is the game files folder, and those of the games folder when it is inside
// the data folder). Sorted.
std::vector<std::string> Excluded(const std::string& data, const std::string& games);

// ---- what the backup asks of the system (Native(); the check gives its own) ----
struct Entry {
    std::string name;
    bool folder = false;
};
struct Space {
    std::uint64_t free = 0; // bytes the drive can still take
    std::uint64_t unit = 0; // what it allocates at a time (its cluster); 0: not known
};
struct Platform {
    // A folder's files and folders (not "." or ".."); false when it can't be read.
    std::function<bool(const std::string& folder, std::vector<Entry>* entries)> list;
    // A drive that is there: its folder is a mount of its own, not the empty folder under /mnt.
    std::function<bool(const std::string& drive)> drive;
    // The room on the drive that holds a path; false when the system can't tell.
    std::function<bool(const std::string& path, Space* space)> space;
    // ::write.
    std::function<ssize_t(int fd, const void* data, std::size_t size)> write;
    // Before each file is copied, with its place in the plan (the check cancels, fills the drive
    // or takes it away there); may be empty.
    std::function<void(std::size_t file, const std::string& path)> before_file;
};
// The console's (or the host's): its directory reader, ::write, and the room where it can be had.
Platform Native();

// ---- the backup itself ----
struct File {
    std::string path; // inside the data folder, '/' between folders
    std::uint64_t size = 0;
};
struct Plan {
    std::vector<std::string> folders; // inside the data folder, each before what it holds
    std::vector<File> files;
    std::uint64_t bytes = 0;
};
// The folders and files under `data` but the excluded paths, sorted; false (error says why, in
// English) when a folder can't be read or stop is set.
bool Measure(const std::string& data, const std::vector<std::string>& excluded, const Platform& platform,
             const std::atomic<bool>& stop, Plan* plan, std::string* error);
// The room a plan takes on a drive that allocates `unit` bytes at a time (512 when not known):
// each file and folder rounded up to it, with SHA256SUMS, backup.json and the backup's own folder.
std::uint64_t Needed(const Plan& plan, std::uint64_t unit);
// The backup's name: YYYYMMDD-HHMMSS, the console's local time.
std::string Stamp(std::time_t when);
// A line of SHA256SUMS as sha256sum writes it: the hash, two spaces, the path; a path with a
// backslash or a line break escaped, and the line marked with a backslash first.
std::string SumsLine(const std::string& hex, const std::string& path);

struct Info {
    std::string version; // the app's (version.h)
    std::time_t when = 0;
    std::string source;  // the data folder
    std::vector<std::string> excluded;
};
struct Progress {
    std::atomic<std::uint64_t> files{0};
    std::atomic<std::uint64_t> bytes{0};
};
enum class Outcome : std::uint8_t { done, cancelled, failed, no_room };
struct Result {
    Outcome outcome = Outcome::failed;
    // done: the backup's folder. Otherwise what is left on the drive, named .incomplete (empty:
    // nothing was left).
    std::string folder;
    std::string error;         // failed, no_room: why (English)
    std::uint64_t files = 0;   // done: copied
    std::uint64_t bytes = 0;
    bool room_known = false;   // no_room before anything was written: needed and free
    std::uint64_t needed = 0;
    std::uint64_t free = 0;
};
// Copies the plan from `data` to <drive>/nightshade-backup/<stamp>/, as the top of this file says.
Result Run(const std::string& data, const Plan& plan, const std::string& drive, const Info& info,
           const Platform& platform, Progress& progress, const std::atomic<bool>& stop);

// ---- Settings > Diagnostics > Back up to USB (EdenServices) ----
struct Setup {
    std::string data;                // /data/prosperoeden
    std::string games;               // the games folder (assets_dir.h, GamesDir)
    std::vector<std::string> drives; // UsbDrives()
    std::string version;
    bool access = true;              // the filesystem access (storage_paths.h): without it, nothing
    Platform platform;
    std::function<std::time_t()> clock; // empty: the system's
};
// Looks for the drives and measures, or backs up, on a thread of its own: one thing at a time.
class Worker {
public:
    explicit Worker(Setup setup);
    // A backup that runs stops (and its folder is removed) before the worker goes.
    ~Worker();
    Worker(const Worker&) = delete;
    Worker& operator=(const Worker&) = delete;

    // Looks for the drives and measures what a backup copies (nothing when something runs).
    void Refresh();
    // Backs up to Status().drives[drive] (false: it could not begin, and reason says why, in
    // English); stops it; and takes its end, before the next one.
    bool Start(int drive, std::string* reason);
    void Cancel();
    void Finish();
    pe::ui::BackupStatus Status();

private:
    // Called with lock_ held: runs work on the thread, which ends by setting busy_ false (under
    // lock_) with its result.
    void Begin(std::function<void()> work);
    void Look();
    void Back(std::string drive);

    Setup setup_;
    std::mutex lock_;
    std::thread thread_;
    bool busy_ = false; // the thread runs (until it has published its result)
    std::atomic<bool> stop_{false};
    Progress progress_;
    pe::ui::BackupStatus status_;
};

} // namespace Eden::Backup
