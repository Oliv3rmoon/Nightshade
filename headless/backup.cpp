// SPDX-FileCopyrightText: Copyright 2026 Oliv3rmoon
// SPDX-License-Identifier: GPL-3.0-or-later
#include "backup.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <set>
#include <sys/stat.h>
#include <system_error>
#include <unistd.h>
#include <utility>

#if defined(__PROSPERO__)
#include "native_directory.h"
#else
#include <sys/statvfs.h>
#endif

#include "diagnostics.h"
#include "update_check/self_update_sha256.h"

namespace Eden::Backup {
namespace {
// Folders deeper than this are not followed (the console's reader follows links).
constexpr int kMaxDepth = 64;
// What a file is copied through.
constexpr std::size_t kChunk = 1u << 20;
// backup.json is far smaller; Needed counts this much for it.
constexpr std::uint64_t kManifestRoom = 16384;

std::string Join(const std::string& folder, const std::string& name) {
    return folder.empty() ? name : folder + "/" + name;
}

std::string Why(int error) { return std::strerror(error); }

// A drive that refuses a write for want of room.
bool Full(int error) { return error == ENOSPC || error == EDQUOT; }

// Writes all of it; 0, or the errno that stopped it.
int WriteAll(const Platform& platform, int fd, const char* data, std::size_t size) {
    while (size > 0) {
        const ssize_t written = platform.write ? platform.write(fd, data, size) : ::write(fd, data, size);
        if (written < 0 && errno == EINTR) continue;
        if (written <= 0) return written < 0 && errno != 0 ? errno : EIO;
        data += written;
        size -= static_cast<std::size_t>(written);
    }
    return 0;
}

// fsync, where the filesystem has it: one without it (EINVAL and the like) is not a failure.
int Sync(int fd) {
    if (fsync(fd) == 0) return 0;
    const int error = errno;
    return error == EIO || Full(error) ? error : 0;
}

// A small file of the backup's own (SHA256SUMS, backup.json), never over one that is there.
int WriteNew(const Platform& platform, const std::string& path, const std::string& text) {
    const int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0666);
    if (fd < 0) return errno;
    int error = WriteAll(platform, fd, text.data(), text.size());
    if (error == 0) error = Sync(fd);
    if (close(fd) != 0 && error == 0) error = errno;
    return error;
}

enum class Copied : std::uint8_t { done, gone, stopped, full, failed };

// One file, hashed as it is read. A file that has gone since it was measured is skipped.
Copied CopyOne(const Platform& platform, const std::string& from, const std::string& to, std::vector<char>& buffer,
               Progress& progress, const std::atomic<bool>& stop, std::string* hex, std::uint64_t* size,
               std::string* error) {
    *size = 0;
    const int in = open(from.c_str(), O_RDONLY);
    if (in < 0) {
        if (errno == ENOENT) return Copied::gone;
        *error = "could not read " + from + ": " + Why(errno);
        return Copied::failed;
    }
    const int out = open(to.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0666);
    if (out < 0) {
        const int why = errno;
        close(in);
        *error = "could not write " + to + ": " + Why(why);
        return Full(why) ? Copied::full : Copied::failed;
    }
    self_update_sha256 hash;
    self_update_sha256_start(&hash);
    Copied outcome = Copied::done;
    for (;;) {
        if (stop.load()) {
            outcome = Copied::stopped;
            break;
        }
        const ssize_t count = read(in, buffer.data(), buffer.size());
        if (count == 0) break;
        if (count < 0) {
            if (errno == EINTR) continue;
            *error = "could not read " + from + ": " + Why(errno);
            outcome = Copied::failed;
            break;
        }
        self_update_sha256_add(&hash, buffer.data(), static_cast<std::size_t>(count));
        if (const int why = WriteAll(platform, out, buffer.data(), static_cast<std::size_t>(count)); why != 0) {
            *error = "could not write " + to + ": " + Why(why);
            outcome = Full(why) ? Copied::full : Copied::failed;
            break;
        }
        *size += static_cast<std::uint64_t>(count);
        progress.bytes += static_cast<std::uint64_t>(count);
    }
    close(in);
    if (outcome == Copied::done) {
        if (const int why = Sync(out); why != 0) {
            *error = "could not write " + to + ": " + Why(why);
            outcome = Full(why) ? Copied::full : Copied::failed;
        }
    }
    if (close(out) != 0 && outcome == Copied::done) {
        const int why = errno;
        *error = "could not write " + to + ": " + Why(why);
        outcome = Full(why) ? Copied::full : Copied::failed;
    }
    if (outcome != Copied::done) return outcome; // the part written goes with the backup's folder
    unsigned char digest[32];
    self_update_sha256_finish(&hash, digest);
    char text[65];
    self_update_sha256_hex(digest, text);
    *hex = text;
    return Copied::done;
}

// A folder and all it holds; false when anything stays.
bool RemoveTree(const Platform& platform, const std::string& path, int depth = 0) {
    std::vector<Entry> entries;
    bool ok = depth <= kMaxDepth;
    if (ok && platform.list(path, &entries)) {
        for (const Entry& entry : entries) {
            const std::string full = path + "/" + entry.name;
            if (entry.folder)
                ok = RemoveTree(platform, full, depth + 1) && ok;
            else if (unlink(full.c_str()) != 0 && errno != ENOENT)
                ok = false;
        }
    }
    return (rmdir(path.c_str()) == 0 || errno == ENOENT) && ok;
}

bool Exists(const std::string& path) {
    struct stat info {};
    return stat(path.c_str(), &info) == 0 || errno != ENOENT;
}

// A JSON string.
std::string Quoted(const std::string& text) {
    std::string out = "\"";
    for (const unsigned char c : text) {
        if (c == '"' || c == '\\') {
            out += '\\';
            out += static_cast<char>(c);
        } else if (c < 0x20) {
            char escaped[8];
            std::snprintf(escaped, sizeof(escaped), "\\u%04x", c);
            out += escaped;
        } else {
            out += static_cast<char>(c);
        }
    }
    return out + "\"";
}

std::string Manifest(const Info& info, std::uint64_t files, std::uint64_t bytes) {
    char created[32] = "";
    std::tm local{};
    if (localtime_r(&info.when, &local) != nullptr) std::strftime(created, sizeof(created), "%Y-%m-%dT%H:%M:%S", &local);
    std::string text = "{\n";
    text += "  \"app\": \"Nightshade\",\n";
    text += "  \"version\": " + Quoted(info.version) + ",\n";
    text += "  \"created\": " + Quoted(created) + ",\n";
    text += "  \"source\": " + Quoted(info.source) + ",\n";
    text += "  \"files\": " + std::to_string(files) + ",\n";
    text += "  \"bytes\": " + std::to_string(bytes) + ",\n";
    text += "  \"excluded\": [";
    for (std::size_t i = 0; i < info.excluded.size(); ++i)
        text += (i ? ", " : "") + Quoted(info.excluded[i]);
    text += "],\n";
    text += "  \"sums\": \"SHA256SUMS\",\n";
    text += "  \"restore\": " +
            Quoted("Close Nightshade, then copy everything in this folder but backup.json and SHA256SUMS into " +
                   info.source + " (FTP, or a file manager on the console), replacing what is there.") +
            "\n";
    return text + "}\n";
}

#if !defined(__PROSPERO__)
bool HostList(const std::string& folder, std::vector<Entry>* entries) {
    namespace fs = std::filesystem;
    entries->clear();
    std::error_code error;
    fs::directory_iterator it{folder, error};
    if (error) return false;
    for (fs::directory_iterator end; it != end; it.increment(error)) {
        if (error) return false;
        std::error_code type_error;
        const fs::file_status status = it->symlink_status(type_error);
        if (type_error || fs::is_symlink(status)) continue; // never followed (nor copied)
        entries->push_back({it->path().filename().string(), fs::is_directory(status)});
    }
    return !error;
}
#endif
} // namespace

std::vector<std::string> UsbDrives() {
    std::vector<std::string> drives;
    for (int i = 0; i < 8; ++i) drives.push_back("/mnt/usb" + std::to_string(i));
    return drives;
}

std::vector<std::string> Excluded(const std::string& data, const std::string& games) {
    std::set<std::string> out{"cache",           "covers",     "logs",       "previous-version",
                              "user/cache",      "user/shader", "user/log",  "user/crash_dumps",
                              "user/dump",       "roms",        "updates",   ".remote-downloads"};
    // The games folder, when it is inside the data folder somewhere else (a game files folder of
    // /data/prosperoeden/eden, say): its games and downloads too.
    std::string inside = games;
    while (inside.size() > 1 && inside.back() == '/') inside.pop_back();
    std::string top = data;
    while (top.size() > 1 && top.back() == '/') top.pop_back();
    if (inside.size() > top.size() + 1 && inside.compare(0, top.size(), top) == 0 && inside[top.size()] == '/') {
        const std::string relative = inside.substr(top.size() + 1);
        for (const char* folder : {"roms", "updates", ".remote-downloads"}) out.insert(relative + "/" + folder);
    }
    return {out.begin(), out.end()};
}

Platform Native() {
    Platform platform;
#if defined(__PROSPERO__)
    // The console's directory iterator needs this reader (native_directory.h).
    platform.list = [](const std::string& folder, std::vector<Entry>* entries) {
        entries->clear();
        std::error_code error;
        const auto found = Eden::ReadNativeDirectory(folder, error);
        if (error) return false;
        for (const auto& entry : found) {
            std::error_code type_error;
            const bool folder_entry = entry.is_directory(type_error);
            if (type_error) continue;
            entries->push_back({entry.path().filename().string(), folder_entry});
        }
        return true;
    };
    // statfs is only in libkernel_sys, which a title does not import, and the SDK's libc.a has it
    // as a raw system call, which ends the process (remote/remote.cpp): the room is not known.
    platform.space = [](const std::string&, Space*) { return false; };
#else
    platform.list = HostList;
    platform.space = [](const std::string& path, Space* space) {
        struct statvfs info {};
        if (statvfs(path.c_str(), &info) != 0) return false;
        const std::uint64_t unit = info.f_frsize ? info.f_frsize : info.f_bsize;
        space->free = static_cast<std::uint64_t>(info.f_bavail) * unit;
        space->unit = unit;
        return true;
    };
#endif
    // Mounted: another device than the folder it is in. An empty /mnt/usb0 with nothing mounted on
    // it, or a view of /mnt that doesn't reach the drive, takes no backup.
    platform.drive = [](const std::string& drive) {
        struct stat here {}, above {};
        if (stat(drive.c_str(), &here) != 0 || !S_ISDIR(here.st_mode)) return false;
        const std::size_t slash = drive.find_last_of('/');
        const std::string parent = slash == 0 || slash == std::string::npos ? "/" : drive.substr(0, slash);
        return stat(parent.c_str(), &above) != 0 || here.st_dev != above.st_dev;
    };
    platform.write = [](int fd, const void* data, std::size_t size) { return ::write(fd, data, size); };
    return platform;
}

bool Measure(const std::string& data, const std::vector<std::string>& excluded, const Platform& platform,
             const std::atomic<bool>& stop, Plan* plan, std::string* error) {
    *plan = {};
    const std::set<std::string> skip(excluded.begin(), excluded.end());
    const std::function<bool(const std::string&, int)> walk = [&](const std::string& relative, int depth) {
        if (stop.load()) {
            *error = "stopped";
            return false;
        }
        const std::string folder = relative.empty() ? data : data + "/" + relative;
        if (depth > kMaxDepth) {
            *error = folder + " is too deep";
            return false;
        }
        std::vector<Entry> entries;
        if (!platform.list(folder, &entries)) {
            *error = "could not read " + folder;
            return false;
        }
        std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) { return a.name < b.name; });
        for (const Entry& entry : entries) {
            if (entry.name.empty() || entry.name == "." || entry.name == ".." ||
                entry.name.find('/') != std::string::npos)
                continue;
            const std::string path = Join(relative, entry.name);
            if (skip.count(path)) continue;
            if (entry.folder) {
                plan->folders.push_back(path);
                if (!walk(path, depth + 1)) return false;
                continue;
            }
            struct stat info {};
            if (stat((data + "/" + path).c_str(), &info) != 0) {
                if (errno == ENOENT) continue; // gone since it was listed
                *error = "could not read " + data + "/" + path + ": " + Why(errno);
                return false;
            }
            if (!S_ISREG(info.st_mode)) continue;
            plan->files.push_back({path, static_cast<std::uint64_t>(info.st_size)});
            plan->bytes += static_cast<std::uint64_t>(info.st_size);
        }
        return true;
    };
    if (!walk("", 0)) {
        *plan = {};
        return false;
    }
    return true;
}

std::uint64_t Needed(const Plan& plan, std::uint64_t unit) {
    unit = std::max<std::uint64_t>(unit, 512);
    const auto round = [unit](std::uint64_t bytes) { return (bytes + unit - 1) / unit * unit; };
    std::uint64_t total = 0, sums = 0;
    for (const File& file : plan.files) {
        total += round(file.size);
        sums += SumsLine(std::string(64, '0'), file.path).size();
    }
    return total + (static_cast<std::uint64_t>(plan.folders.size()) + 1) * unit + round(sums) + round(kManifestRoom);
}

std::string Stamp(std::time_t when) {
    std::tm local{};
    char text[32] = "00000000-000000";
    if (localtime_r(&when, &local) != nullptr) std::strftime(text, sizeof(text), "%Y%m%d-%H%M%S", &local);
    return text;
}

std::string SumsLine(const std::string& hex, const std::string& path) {
    if (path.find_first_of("\\\n\r") == std::string::npos) return hex + "  " + path + "\n";
    std::string escaped;
    for (const char c : path)
        escaped += c == '\\' ? std::string{"\\\\"} : c == '\n' ? std::string{"\\n"} : c == '\r' ? std::string{"\\r"}
                                                                                                : std::string(1, c);
    return "\\" + hex + "  " + escaped + "\n";
}

Result Run(const std::string& data, const Plan& plan, const std::string& drive, const Info& info,
           const Platform& platform, Progress& progress, const std::atomic<bool>& stop) {
    Result result;
    progress.files = 0;
    progress.bytes = 0;
    if (stop.load()) {
        result.outcome = Outcome::cancelled;
        return result;
    }
    // The room first, where the system can tell: nothing is written for a backup that can't fit.
    Space space;
    if (platform.space && platform.space(drive, &space)) {
        result.needed = Needed(plan, space.unit);
        result.free = space.free;
        if (space.free < result.needed) {
            result.outcome = Outcome::no_room;
            result.room_known = true;
            result.error = "the backup needs " + std::to_string(result.needed) + " bytes, the drive has " +
                           std::to_string(space.free);
            return result;
        }
    }
    const std::string base = drive + "/" + kFolder;
    if (mkdir(base.c_str(), 0777) != 0 && errno != EEXIST) {
        const int why = errno;
        result.outcome = Full(why) ? Outcome::no_room : Outcome::failed;
        result.error = "could not make " + base + ": " + Why(why);
        return result;
    }
    // Never into a backup that is there: the next free name (two in the same second).
    const std::string stamp = Stamp(info.when);
    std::string name = stamp;
    for (int n = 2; Exists(base + "/" + name) || Exists(base + "/" + name + kIncomplete); ++n) {
        if (n > 99) {
            result.error = "no free name for a backup in " + base;
            return result;
        }
        name = stamp + "-" + std::to_string(n);
    }
    const std::string work = base + "/" + name + kIncomplete, done = base + "/" + name;
    if (mkdir(work.c_str(), 0777) != 0) {
        const int why = errno;
        (void)rmdir(base.c_str()); // only when it is empty: the backups in it stay
        result.outcome = Full(why) ? Outcome::no_room : Outcome::failed;
        result.error = "could not make " + work + ": " + Why(why);
        return result;
    }
    // Ends a backup that is not whole: its folder goes, or keeps its .incomplete name.
    const auto abandon = [&](Outcome outcome, std::string error) {
        result.outcome = outcome;
        result.error = std::move(error);
        if (RemoveTree(platform, work)) {
            (void)rmdir(base.c_str());
            result.folder.clear();
        } else {
            result.folder = work;
        }
        return result;
    };
    for (const std::string& folder : plan.folders)
        if (mkdir((work + "/" + folder).c_str(), 0777) != 0 && errno != EEXIST) {
            const int why = errno;
            return abandon(Full(why) ? Outcome::no_room : Outcome::failed,
                           "could not make " + work + "/" + folder + ": " + Why(why));
        }
    std::vector<char> buffer(kChunk);
    std::string sums;
    std::uint64_t files = 0, bytes = 0;
    for (std::size_t index = 0; index < plan.files.size(); ++index) {
        const File& file = plan.files[index];
        if (platform.before_file) platform.before_file(index, file.path);
        if (stop.load()) return abandon(Outcome::cancelled, {});
        std::string hex, error;
        std::uint64_t size = 0;
        switch (CopyOne(platform, data + "/" + file.path, work + "/" + file.path, buffer, progress, stop, &hex, &size,
                        &error)) {
        case Copied::done:
            sums += SumsLine(hex, file.path);
            ++files;
            bytes += size;
            ++progress.files;
            break;
        case Copied::gone:
            break;
        case Copied::stopped:
            return abandon(Outcome::cancelled, {});
        case Copied::full:
            return abandon(Outcome::no_room, error);
        case Copied::failed:
            return abandon(Outcome::failed, error);
        }
    }
    if (stop.load()) return abandon(Outcome::cancelled, {});
    // What makes it a backup, last: the sums, what it is, then its name.
    for (const auto& [name_of, text] : {std::pair<std::string, std::string>{kSums, sums},
                                        std::pair<std::string, std::string>{kManifest, Manifest(info, files, bytes)}})
        if (const int why = WriteNew(platform, work + "/" + name_of, text); why != 0)
            return abandon(Full(why) ? Outcome::no_room : Outcome::failed,
                           "could not write " + work + "/" + name_of + ": " + Why(why));
    if (stop.load()) return abandon(Outcome::cancelled, {});
    if (Exists(done))
        return abandon(Outcome::failed, "could not name the backup " + done + ": a folder of that name is there");
    if (std::rename(work.c_str(), done.c_str()) != 0)
        return abandon(Outcome::failed, "could not name the backup " + done + ": " + Why(errno));
    result.outcome = Outcome::done;
    result.folder = done;
    result.files = files;
    result.bytes = bytes;
    return result;
}

// ---- Worker ----

Worker::Worker(Setup setup) : setup_(std::move(setup)) {
    if (!setup_.clock) setup_.clock = [] { return std::time(nullptr); };
    status_.access = setup_.access;
}

Worker::~Worker() {
    stop_ = true;
    if (thread_.joinable()) thread_.join();
}

void Worker::Begin(std::function<void()> work) {
    // busy_ is false: the last thread has published its end (and busy_ with it, under lock_) and
    // only has to return.
    if (thread_.joinable()) thread_.join();
    busy_ = true;
    stop_ = false;
    thread_ = std::thread(std::move(work));
}

void Worker::Refresh() {
    const std::lock_guard guard(lock_);
    if (busy_ || status_.phase != pe::ui::BackupPhase::idle) return;
    status_.phase = pe::ui::BackupPhase::looking;
    Begin([this] { Look(); });
}

void Worker::Look() {
    std::vector<pe::ui::BackupDrive> drives;
    Plan plan;
    std::string problem;
    bool measured = false;
    if (setup_.access) {
        for (const std::string& drive : setup_.drives) {
            if (setup_.platform.drive && !setup_.platform.drive(drive)) continue;
            std::vector<Entry> entries;
            if (!setup_.platform.list(drive, &entries)) continue; // there, but it can't be read
            pe::ui::BackupDrive found;
            found.path = drive;
            Space space;
            if (setup_.platform.space && setup_.platform.space(drive, &space)) {
                found.free = space.free;
                found.free_known = true;
            }
            drives.push_back(std::move(found));
        }
        measured = Measure(setup_.data, Excluded(setup_.data, setup_.games), setup_.platform, stop_, &plan, &problem);
        if (!measured && stop_.load()) problem.clear();
    }
    std::string line = std::to_string(drives.size()) + " drives";
    for (const auto& drive : drives) line += " " + drive.path;
    line += measured ? ", " + std::to_string(plan.files.size()) + " files, " + std::to_string(plan.bytes) + " bytes to back up"
                     : ", " + (problem.empty() ? std::string{"nothing measured"} : problem);
    Report("backup", line.c_str());
    const std::lock_guard guard(lock_);
    status_.drives = std::move(drives);
    status_.measured = measured;
    status_.files = measured ? plan.files.size() : 0;
    status_.bytes = measured ? plan.bytes : 0;
    status_.problem = problem;
    status_.phase = pe::ui::BackupPhase::idle;
    busy_ = false; // with the result, so that a Refresh or Start that sees it can begin
}

bool Worker::Start(int drive, std::string* reason) {
    const std::lock_guard guard(lock_);
    if (busy_ || status_.phase != pe::ui::BackupPhase::idle) {
        *reason = "a backup or a look for the drives runs";
        return false;
    }
    if (!setup_.access) {
        *reason = "no filesystem access";
        return false;
    }
    if (drive < 0 || drive >= static_cast<int>(status_.drives.size())) {
        *reason = "no such drive";
        return false;
    }
    status_.phase = pe::ui::BackupPhase::measuring;
    status_.drive = drive;
    status_.done_files = status_.done_bytes = 0;
    status_.folder.clear();
    status_.error.clear();
    status_.room_known = false;
    status_.needed = status_.free = 0;
    progress_.files = 0;
    progress_.bytes = 0;
    Begin([this, path = status_.drives[static_cast<std::size_t>(drive)].path] { Back(path); });
    return true;
}

void Worker::Back(std::string drive) {
    Info info;
    info.version = setup_.version;
    info.when = setup_.clock();
    info.source = setup_.data;
    info.excluded = Excluded(setup_.data, setup_.games);
    Report("backup", ("To " + drive + "/" + kFolder + "/" + Stamp(info.when)).c_str());
    Plan plan;
    std::string problem;
    Result result;
    if (!Measure(setup_.data, info.excluded, setup_.platform, stop_, &plan, &problem)) {
        result.outcome = stop_.load() ? Outcome::cancelled : Outcome::failed;
        result.error = stop_.load() ? std::string{} : problem;
    } else {
        {
            const std::lock_guard guard(lock_);
            status_.files = plan.files.size();
            status_.bytes = plan.bytes;
            status_.measured = true;
            status_.phase = pe::ui::BackupPhase::copying;
        }
        result = Run(setup_.data, plan, drive, info, setup_.platform, progress_, stop_);
    }
    const char* ends[] = {"done", "cancelled", "failed", "no room"};
    std::string line = std::string{ends[static_cast<int>(result.outcome)]} + ": " + std::to_string(result.files) +
                       " files, " + std::to_string(result.bytes) + " bytes";
    if (!result.error.empty()) line += ", " + result.error;
    if (!result.folder.empty()) line += (result.outcome == Outcome::done ? ", in " : ", left in ") + result.folder;
    Report("backup", line.c_str());
    const std::lock_guard guard(lock_);
    status_.folder = result.folder;
    status_.error = result.error;
    status_.room_known = result.room_known;
    status_.needed = result.needed;
    status_.free = result.free;
    switch (result.outcome) {
    case Outcome::done:
        status_.phase = pe::ui::BackupPhase::done;
        break;
    case Outcome::cancelled:
        status_.phase = pe::ui::BackupPhase::cancelled;
        break;
    case Outcome::failed:
        status_.phase = pe::ui::BackupPhase::failed;
        break;
    case Outcome::no_room:
        status_.phase = pe::ui::BackupPhase::no_room;
        break;
    }
    busy_ = false; // with the result, so that the Refresh after Finish begins
}

void Worker::Cancel() { stop_ = true; }

void Worker::Finish() {
    const std::lock_guard guard(lock_);
    switch (status_.phase) {
    case pe::ui::BackupPhase::done:
    case pe::ui::BackupPhase::cancelled:
    case pe::ui::BackupPhase::failed:
    case pe::ui::BackupPhase::no_room:
        status_.phase = pe::ui::BackupPhase::idle;
        status_.drive = -1;
        break;
    default:
        break;
    }
}

pe::ui::BackupStatus Worker::Status() {
    const std::lock_guard guard(lock_);
    pe::ui::BackupStatus status = status_;
    if (status.phase == pe::ui::BackupPhase::copying || status.phase == pe::ui::BackupPhase::measuring) {
        status.done_files = progress_.files.load();
        status.done_bytes = progress_.bytes.load();
    }
    return status;
}

} // namespace Eden::Backup
