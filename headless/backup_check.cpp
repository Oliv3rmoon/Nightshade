// SPDX-FileCopyrightText: Copyright 2026 Oliv3rmoon
// SPDX-License-Identifier: GPL-3.0-or-later
// Host check of Back up to USB (backup.h), with a data folder made in the temporary folder and
// another temporary folder as the USB drive: what is left out, the timestamped folder and the one
// after it in the same second, backup.json and SHA256SUMS against the files, a cancel (between
// files and inside one) and a failure that leave nothing, or a folder still named .incomplete when
// it can't be removed, the refusal of a drive without room before anything is written, a drive
// that fills up, and the worker on its thread: looking, backing up, cancelling, going away while
// it backs up.
//
// usage: eden-backup-check [<folder>]
//        With a folder, a finished backup is left in it, for tools/check-backup.py to check with
//        tools of its own.
#include "backup.h"
#include "update_check/self_update_sha256.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace
{
namespace fs = std::filesystem;
int failures = 0;
int passed = 0;

void expect(bool ok, const std::string &what)
{
    if (ok)
        ++passed;
    else
    {
        std::printf("FAIL backup: %s\n", what.c_str());
        ++failures;
    }
}

std::string read(const fs::path &path)
{
    std::ifstream file(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), {}};
}

void write(const fs::path &path, const std::string &text)
{
    fs::create_directories(path.parent_path());
    std::ofstream(path, std::ios::binary) << text;
}

std::string sha256(const std::string &data)
{
    self_update_sha256 hash;
    self_update_sha256_start(&hash);
    self_update_sha256_add(&hash, data.data(), data.size());
    unsigned char digest[32];
    self_update_sha256_finish(&hash, digest);
    char text[65];
    self_update_sha256_hex(digest, text);
    return text;
}

// 2026-10-28 15:30:00 UTC (the check runs with TZ=UTC).
constexpr std::time_t kWhen = 1793201400;
const std::string kStamp = "20261028-153000";

// The host's platform, but a drive is any folder (the temporary ones are no mounts of their own).
Eden::Backup::Platform platform()
{
    Eden::Backup::Platform host = Eden::Backup::Native();
    host.drive = [](const std::string &drive)
    {
        std::error_code error;
        return fs::is_directory(drive, error) && !error;
    };
    host.space = {}; // the room is not known, as on the console, unless a case says
    return host;
}

// What a data folder holds: what is kept, and what a backup leaves out.
const std::map<std::string, std::string> kKept = {
    {"config/prosperoeden.json", "{\"launcher\": {\"wallpaper\": \"beach.png\"}}"},
    {"config/remote/save-sync.json", "{\"version\": 1}"},
    {"keys/prod.keys", "keys"},
    {"firmware/0100000000000809.nca", std::string(70000, 'f')},
    {"library.json", "[]"},
    {"backup/save-sync/1F1E/0100A00000001000/20261001-120000/save.bin", "an older save"},
    {"user/nand/system/save/8000000000000010/su/avators/profiles.dat", std::string(1200, 'p')},
    {"user/nand/user/save/0000000000000000/1F1E1D1C/0100A00000001000/a.bin", "a save"},
    {"user/nand/user/save/0000000000000000/1F1E1D1C/0100A00000001000/b.bin", "b save"},
    // A save over three of the copy's chunks, so that a cancel lands inside it.
    {"user/nand/user/save/0000000000000000/1F1E1D1C/0100B00000002000/big.bin", std::string(3 * 1048576 + 17, 'B')},
    {"user/post_presets/Mine.fxp", "preset"},
    {"wallpapers/beach.png", "png"},
    {"empty.txt", ""},
};
const std::vector<std::string> kLeftOut = {
    "cache/radv/index",       "covers/0a1b2c3d.tga",        "logs/stderr.log",
    "previous-version/PPSA99008-backup/eboot.bin",          "user/cache/nsz/game.nca",
    "user/cache/native-opengl/shader.bin",                  "user/shader/vulkan/0100A00000001000.bin",
    "user/log/eden_log.txt",  "user/crash_dumps/dump.dmp",  "user/dump/romfs.bin",
    "roms/Game/game.nsp",     "updates/game update.nsp",    ".remote-downloads/part.nsp",
};

fs::path make_data(const fs::path &base)
{
    const fs::path data = base / "data/prosperoeden";
    for (const auto &[path, text] : kKept)
        write(data / path, text);
    for (const std::string &path : kLeftOut)
        write(data / path, "regenerable");
    fs::create_directories(data / "user/sdmc");            // an empty folder is kept as one
    fs::create_symlink("/etc", data / "user/link-to-etc"); // a link is not followed
    return data;
}

Eden::Backup::Info info(const fs::path &data)
{
    Eden::Backup::Info made;
    made.version = "01.010.000";
    made.when = kWhen;
    made.source = data.string();
    made.excluded = Eden::Backup::Excluded(data.string(), data.string());
    return made;
}

Eden::Backup::Plan measure(const fs::path &data, const Eden::Backup::Platform &host)
{
    Eden::Backup::Plan plan;
    std::string error;
    const std::atomic<bool> stop{false};
    expect(Eden::Backup::Measure(data.string(), Eden::Backup::Excluded(data.string(), data.string()), host, stop,
                                 &plan, &error),
           "the data folder is measured: " + error);
    return plan;
}

// The drive's nightshade-backup/ entries.
std::set<std::string> backups(const fs::path &drive)
{
    std::set<std::string> names;
    std::error_code error;
    for (fs::directory_iterator it{drive / Eden::Backup::kFolder, error}, end; !error && it != end; it.increment(error))
        names.insert(it->path().filename().string());
    return names;
}

void excluded()
{
    using Eden::Backup::Excluded;
    const std::vector<std::string> base = {".remote-downloads", "cache",      "covers",           "logs",
                                           "previous-version",  "roms",       "updates",          "user/cache",
                                           "user/crash_dumps",  "user/dump",  "user/log",         "user/shader"};
    expect(Excluded("/data/prosperoeden", "/data/prosperoeden") == base,
           "caches, logs, covers, the previous version and the games are left out, sorted");
    expect(Excluded("/data/prosperoeden", "/mnt/usb0/games") == base, "a games folder on a drive adds nothing");
    std::vector<std::string> inside = base;
    for (const char *path : {"eden/.remote-downloads", "eden/roms", "eden/updates"})
        inside.push_back(path);
    std::sort(inside.begin(), inside.end());
    expect(Excluded("/data/prosperoeden/", "/data/prosperoeden/eden/") == inside,
           "a games folder inside the data folder has its games and downloads left out too");
    expect(Excluded("/data/prosperoeden", "/data/prosperoeden-games") == base,
           "a folder beside the data folder is not inside it");
}

void names()
{
    expect(Eden::Backup::Stamp(kWhen) == kStamp, "the backup's name is the local date and time: " + Eden::Backup::Stamp(kWhen));
    const std::string hex(64, 'a');
    expect(Eden::Backup::SumsLine(hex, "config/a b.json") == hex + "  config/a b.json\n", "a SHA256SUMS line");
    expect(Eden::Backup::SumsLine(hex, "odd\\name\nx") == "\\" + hex + "  odd\\\\name\\nx\n",
           "a path with a backslash or a line break is escaped as sha256sum does");
    Eden::Backup::Plan plan;
    plan.folders = {"a"};
    plan.files = {{"a/one", 1}, {"two", 5000}};
    const std::uint64_t sums = Eden::Backup::SumsLine(hex, "a/one").size() + Eden::Backup::SumsLine(hex, "two").size();
    const std::uint64_t expected = 4096 + 8192 + 2 * 4096 + (sums + 4095) / 4096 * 4096 + 16384;
    expect(Eden::Backup::Needed(plan, 4096) == expected,
           "the room needed counts every file and folder in whole clusters, and the backup's own files");
    expect(Eden::Backup::Needed(plan, 0) == Eden::Backup::Needed(plan, 512), "an unknown cluster counts as 512 bytes");
}

void measuring(const fs::path &data)
{
    const Eden::Backup::Plan plan = measure(data, platform());
    std::set<std::string> files;
    std::uint64_t bytes = 0;
    for (const auto &file : plan.files)
        files.insert(file.path), bytes += file.size;
    std::set<std::string> kept;
    std::uint64_t kept_bytes = 0;
    for (const auto &[path, text] : kKept)
        kept.insert(path), kept_bytes += text.size();
    expect(files == kept, "the plan is the kept files, nothing left out and no link");
    expect(bytes == kept_bytes && plan.bytes == kept_bytes, "the plan's size is theirs");
    bool empty_folder = false, parent_first = true;
    std::set<std::string> seen;
    for (const std::string &folder : plan.folders)
    {
        empty_folder = empty_folder || folder == "user/sdmc";
        const std::size_t slash = folder.find_last_of('/');
        if (slash != std::string::npos && !seen.count(folder.substr(0, slash)))
            parent_first = false;
        seen.insert(folder);
        for (const std::string &out : Eden::Backup::Excluded(data.string(), data.string()))
            expect(folder != out, "a left-out folder is not in the plan: " + folder);
    }
    expect(empty_folder, "an empty folder is in the plan");
    expect(parent_first, "each folder comes after the one it is in");
    std::string error;
    const std::atomic<bool> stop{true};
    Eden::Backup::Plan stopped;
    expect(!Eden::Backup::Measure(data.string(), {}, platform(), stop, &stopped, &error) && stopped.files.empty(),
           "a measure that is stopped ends with nothing");
    expect(!Eden::Backup::Measure((data / "missing").string(), {}, platform(), std::atomic<bool>{false}, &stopped,
                                  &error),
           "a data folder that isn't there can't be measured");
}

void backing_up(const fs::path &base, const fs::path &data, const fs::path &keep)
{
    const fs::path drive = base / "usb0";
    fs::create_directories(drive);
    const Eden::Backup::Platform host = platform();
    const Eden::Backup::Plan plan = measure(data, host);
    Eden::Backup::Progress progress;
    const std::atomic<bool> stop{false};
    const auto result = Eden::Backup::Run(data.string(), plan, drive.string(), info(data), host, progress, stop);
    const fs::path folder = drive / Eden::Backup::kFolder / kStamp;
    expect(result.outcome == Eden::Backup::Outcome::done, "the backup is done: " + result.error);
    expect(result.folder == folder.string(), "it is in <drive>/nightshade-backup/<YYYYMMDD-HHMMSS>: " + result.folder);
    expect(backups(drive) == std::set<std::string>{kStamp}, "and nothing named .incomplete is left beside it");
    expect(result.files == kKept.size() && progress.files == kKept.size(), "every kept file is copied");
    bool same = true;
    for (const auto &[path, text] : kKept)
        same = same && read(folder / path) == text;
    expect(same, "each copy is the file");
    for (const std::string &path : kLeftOut)
        expect(!fs::exists(folder / path), "left out of the backup: " + path);
    expect(fs::is_directory(folder / "user/sdmc"), "the empty folder is in the backup");
    expect(!fs::exists(folder / "user/link-to-etc"), "the link is not in the backup");

    // SHA256SUMS: a line for each file, its hash the copy's.
    std::istringstream sums(read(folder / Eden::Backup::kSums));
    std::map<std::string, std::string> listed;
    for (std::string line; std::getline(sums, line);)
        if (line.size() > 66 && line.compare(64, 2, "  ") == 0)
            listed[line.substr(66)] = line.substr(0, 64);
    expect(listed.size() == kKept.size(), "SHA256SUMS has a line for each file");
    bool hashes = true;
    for (const auto &[path, text] : kKept)
        hashes = hashes && listed.count(path) && listed[path] == sha256(text) && sha256(read(folder / path)) == listed[path];
    expect(hashes, "each line is the SHA-256 of the file and of its copy");

    // backup.json: the app's version, the date, how many files and bytes, what was left out.
    std::uint64_t bytes = 0;
    for (const auto &[path, text] : kKept)
        bytes += text.size();
    const std::string manifest = read(folder / Eden::Backup::kManifest);
    for (const std::string &part :
         {std::string{"\"app\": \"Nightshade\""}, std::string{"\"version\": \"01.010.000\""},
          std::string{"\"created\": \"2026-10-28T15:30:00\""}, "\"files\": " + std::to_string(kKept.size()) + ",",
          "\"bytes\": " + std::to_string(bytes) + ",", "\"source\": \"" + data.string() + "\"",
          std::string{"\"user/shader\""}, std::string{"\"sums\": \"SHA256SUMS\""}, std::string{"\"restore\": "}})
        expect(manifest.find(part) != std::string::npos, "backup.json has " + part);
    expect(manifest.front() == '{' && manifest.substr(manifest.size() - 2) == "}\n", "backup.json is one JSON object");

    // A second backup in the same second: a name of its own, the first one untouched.
    const auto again = Eden::Backup::Run(data.string(), plan, drive.string(), info(data), host, progress, stop);
    expect(again.outcome == Eden::Backup::Outcome::done && again.folder == (folder.string() + "-2"),
           "a second backup in the same second is named -2: " + again.folder);
    expect(read(folder / "keys/prod.keys") == "keys", "and the first backup is as it was");
    if (!keep.empty())
    {
        std::error_code error;
        fs::copy(folder, keep / kStamp, fs::copy_options::recursive, error);
        expect(!error, "the finished backup is kept for check-backup.py: " + error.message());
    }
}

void cancelling(const fs::path &base, const fs::path &data)
{
    const Eden::Backup::Plan plan = measure(data, platform());
    // Between files: at the third one.
    {
        const fs::path drive = base / "usb1";
        fs::create_directories(drive);
        std::atomic<bool> stop{false};
        Eden::Backup::Platform host = platform();
        host.before_file = [&](std::size_t file, const std::string &)
        {
            if (file == 2)
                stop = true;
        };
        Eden::Backup::Progress progress;
        const auto result = Eden::Backup::Run(data.string(), plan, drive.string(), info(data), host, progress, stop);
        expect(result.outcome == Eden::Backup::Outcome::cancelled, "a cancel between files cancels the backup");
        expect(result.folder.empty() && !fs::exists(drive / Eden::Backup::kFolder),
               "a cancelled backup leaves nothing on the drive, not even nightshade-backup/");
    }
    // Inside a file, after its first chunk; a drive with an earlier backup on it.
    {
        const fs::path drive = base / "usb2";
        write(drive / Eden::Backup::kFolder / "20261001-090000/keys/prod.keys", "old");
        std::atomic<bool> stop{false};
        Eden::Backup::Platform host = platform();
        std::uint64_t written = 0;
        host.write = [&](int fd, const void *bytes, std::size_t size)
        {
            written += size;
            if (written > 1048576 + 1024)
                stop = true;
            return ::write(fd, bytes, size);
        };
        Eden::Backup::Progress progress;
        const auto result = Eden::Backup::Run(data.string(), plan, drive.string(), info(data), host, progress, stop);
        expect(result.outcome == Eden::Backup::Outcome::cancelled && progress.bytes > 1048576 &&
                   progress.bytes < plan.bytes,
               "a cancel inside a large file stops it at the next chunk");
        expect(result.folder.empty() && backups(drive) == std::set<std::string>{"20261001-090000"},
               "and only the earlier backup is left on the drive");
        expect(read(drive / Eden::Backup::kFolder / "20261001-090000/keys/prod.keys") == "old",
               "which is untouched");
    }
}

void lacking_room(const fs::path &base, const fs::path &data)
{
    const Eden::Backup::Plan plan = measure(data, platform());
    // Known before anything is written: refused.
    {
        const fs::path drive = base / "usb3";
        fs::create_directories(drive);
        Eden::Backup::Platform host = platform();
        host.space = [](const std::string &, Eden::Backup::Space *space)
        {
            space->free = 1048576; // 1 MiB: the plan is over 3
            space->unit = 32768;
            return true;
        };
        Eden::Backup::Progress progress;
        const auto result = Eden::Backup::Run(data.string(), plan, drive.string(), info(data), host, progress,
                                              std::atomic<bool>{false});
        expect(result.outcome == Eden::Backup::Outcome::no_room && result.room_known && result.free == 1048576 &&
                   result.needed == Eden::Backup::Needed(plan, 32768) && result.needed > plan.bytes,
               "a drive without room is refused, with what the backup needs and the drive has");
        expect(fs::is_empty(drive), "and nothing is written to it");
        // With room for it, the same drive takes it.
        host.space = [](const std::string &, Eden::Backup::Space *space)
        {
            space->free = 1ull << 34;
            space->unit = 32768;
            return true;
        };
        const auto fits = Eden::Backup::Run(data.string(), plan, drive.string(), info(data), host, progress,
                                            std::atomic<bool>{false});
        expect(fits.outcome == Eden::Backup::Outcome::done, "a drive with room takes it: " + fits.error);
    }
    // Not known (the console): the drive fills up during the copy.
    {
        const fs::path drive = base / "usb4";
        fs::create_directories(drive);
        Eden::Backup::Platform host = platform();
        std::uint64_t written = 0;
        host.write = [&](int fd, const void *bytes, std::size_t size) -> ssize_t
        {
            if (written + size > 2 * 1048576)
            {
                errno = ENOSPC;
                return -1;
            }
            written += size;
            return ::write(fd, bytes, size);
        };
        Eden::Backup::Progress progress;
        const auto result = Eden::Backup::Run(data.string(), plan, drive.string(), info(data), host, progress,
                                              std::atomic<bool>{false});
        expect(result.outcome == Eden::Backup::Outcome::no_room && !result.room_known,
               "a drive that fills up during the copy stops it as without room: " + result.error);
        expect(result.folder.empty() && !fs::exists(drive / Eden::Backup::kFolder),
               "and what it wrote is removed");
    }
}

void failing(const fs::path &base, const fs::path &data)
{
    if (geteuid() == 0)
    {
        std::printf("note backup: run as root, the folder that can't be removed is not checked\n");
        return;
    }
    const Eden::Backup::Plan plan = measure(data, platform());
    const fs::path drive = base / "usb5";
    fs::create_directories(drive);
    const fs::path work = drive / Eden::Backup::kFolder / (kStamp + Eden::Backup::kIncomplete);
    const fs::path saves = work / "user/nand/user/save/0000000000000000/1F1E1D1C/0100A00000001000";
    Eden::Backup::Platform host = platform();
    // When b.bin is next, a.bin is in its folder: the folder is made read-only, so b.bin can't be
    // written and a.bin can't be removed (as from a drive pulled out half way).
    host.before_file = [&](std::size_t, const std::string &path)
    {
        if (path.size() > 6 && path.compare(path.size() - 6, 6, "/b.bin") == 0)
            chmod(saves.c_str(), 0555);
    };
    Eden::Backup::Progress progress;
    const auto result = Eden::Backup::Run(data.string(), plan, drive.string(), info(data), host, progress,
                                          std::atomic<bool>{false});
    chmod(saves.c_str(), 0755);
    expect(result.outcome == Eden::Backup::Outcome::failed && result.error.find("b.bin") != std::string::npos,
           "a file that can't be written fails the backup and says which: " + result.error);
    expect(result.folder == work.string() && fs::is_directory(work),
           "a backup that can't be removed is left as <stamp>.incomplete");
    expect(backups(drive) == std::set<std::string>{kStamp + Eden::Backup::kIncomplete},
           "and nothing named as a whole backup is there");
    expect(!fs::exists(work / Eden::Backup::kManifest) && !fs::exists(work / Eden::Backup::kSums),
           "nor backup.json or SHA256SUMS in it");
}

// Polls the worker until its phase is one of `wanted`, for up to 20 s.
pe::ui::BackupStatus wait_for(Eden::Backup::Worker &worker, std::initializer_list<pe::ui::BackupPhase> wanted)
{
    pe::ui::BackupStatus status;
    for (int i = 0; i < 2000; ++i)
    {
        status = worker.Status();
        for (const auto phase : wanted)
            if (status.phase == phase)
                return status;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return status;
}

void working(const fs::path &base, const fs::path &data)
{
    const fs::path drive = base / "usb6";
    fs::create_directories(drive);
    const auto setup = [&](Eden::Backup::Platform host)
    {
        Eden::Backup::Setup made;
        made.data = data.string();
        made.games = data.string();
        made.drives = {drive.string(), (base / "usb7").string()}; // the second is not plugged in
        made.version = "01.010.000";
        made.platform = std::move(host);
        made.clock = [] { return kWhen; };
        return made;
    };
    const Eden::Backup::Plan plan = measure(data, platform());
    std::atomic<int> looks{0}; // how often the worker asked whether a drive is there
    {
        Eden::Backup::Platform counted = platform();
        counted.drive = [&looks, check = counted.drive](const std::string &drive)
        {
            ++looks;
            return check(drive);
        };
        Eden::Backup::Worker worker(setup(counted));
        expect(worker.Status().phase == pe::ui::BackupPhase::idle && !worker.Status().measured,
               "a new worker has looked at nothing");
        worker.Refresh();
        auto status = wait_for(worker, {pe::ui::BackupPhase::idle});
        expect(status.drives.size() == 1 && status.drives[0].path == drive.string() && !status.drives[0].free_known,
               "the drive plugged in is found, the other is not");
        expect(status.measured && status.files == plan.files.size() && status.bytes == plan.bytes,
               "what a backup copies is measured");
        std::string reason;
        expect(!worker.Start(1, &reason), "a drive that isn't listed can't be backed up to");
        expect(worker.Start(0, &reason), "a backup begins: " + reason);
        status = wait_for(worker, {pe::ui::BackupPhase::done, pe::ui::BackupPhase::failed});
        expect(status.phase == pe::ui::BackupPhase::done && status.folder == (drive / Eden::Backup::kFolder / kStamp).string(),
               "the worker backs up: " + status.error);
        expect(!worker.Start(0, &reason), "the next waits until the end of this one is taken");
        worker.Finish();
        expect(worker.Status().phase == pe::ui::BackupPhase::idle, "taken, the worker is idle");
        // A look right after the end is taken (as the launcher does) begins: the thread was done.
        const int before = looks.load();
        worker.Refresh();
        status = wait_for(worker, {pe::ui::BackupPhase::idle});
        expect(looks.load() > before && status.drives.size() == 1, "a look right after a backup's end looks again");
    }
    // Cancelled while it copies, and gone while it copies.
    for (const bool destroy : {false, true})
    {
        const fs::path left = drive / Eden::Backup::kFolder / (kStamp + "-2" + Eden::Backup::kIncomplete);
        Eden::Backup::Platform slow = platform();
        slow.before_file = [](std::size_t, const std::string &)
        { std::this_thread::sleep_for(std::chrono::milliseconds(20)); };
        auto worker = std::make_unique<Eden::Backup::Worker>(setup(slow));
        worker->Refresh();
        wait_for(*worker, {pe::ui::BackupPhase::idle});
        std::string reason;
        expect(worker->Start(0, &reason), "another backup begins: " + reason);
        pe::ui::BackupStatus status;
        for (int i = 0; i < 2000; ++i)
        {
            status = worker->Status();
            if (status.phase == pe::ui::BackupPhase::copying && status.done_files >= 2)
                break;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        expect(status.phase == pe::ui::BackupPhase::copying && status.done_files >= 2 &&
                   status.done_files < plan.files.size() && fs::is_directory(left),
               "it shows how far it is while it copies, in its .incomplete folder");
        if (destroy)
        {
            worker.reset();
            expect(!fs::exists(left), "a worker that goes while it backs up removes what it wrote");
            continue;
        }
        worker->Cancel();
        status = wait_for(*worker, {pe::ui::BackupPhase::cancelled, pe::ui::BackupPhase::done});
        expect(status.phase == pe::ui::BackupPhase::cancelled && status.folder.empty() && !fs::exists(left),
               "a cancel stops it and removes what it wrote");
        worker->Finish();
    }
    expect(backups(drive) == std::set<std::string>{kStamp}, "only the finished backup is on the drive");
    // Without filesystem access: nothing is looked at or backed up.
    Eden::Backup::Setup none = setup(platform());
    none.access = false;
    Eden::Backup::Worker blind(std::move(none));
    blind.Refresh();
    const auto status = wait_for(blind, {pe::ui::BackupPhase::idle});
    std::string reason;
    expect(!status.access && status.drives.empty() && !status.measured && !blind.Start(0, &reason),
           "without filesystem access there is nothing to back up");
}
} // namespace

int main(int argc, char **argv)
{
    if (argc > 2)
    {
        std::fprintf(stderr, "usage: eden-backup-check [<folder to keep a backup in>]\n");
        return 2;
    }
    setenv("TZ", "UTC", 1);
    tzset();
    std::error_code error;
    const fs::path base = fs::temp_directory_path(error) / ("eden-backup-check-" + std::to_string(getpid()));
    if (error)
    {
        std::fprintf(stderr, "no temporary folder: %s\n", error.message().c_str());
        return 2;
    }
    fs::remove_all(base, error);
    const fs::path data = make_data(base);
    excluded();
    names();
    measuring(data);
    backing_up(base, data, argc == 2 ? fs::path{argv[1]} : fs::path{});
    cancelling(base, data);
    lacking_room(base, data);
    failing(base, data);
    working(base, data);
    fs::remove_all(base, error);
    std::printf("%s backup: %d checks of what is left out, the backup's folder, manifest and sums, cancels, failures, "
                "drives without room and the worker, %d failed\n",
                failures ? "FAIL" : "PASS", passed + failures, failures);
    return failures ? 1 : 0;
}
