// SPDX-FileCopyrightText: Copyright 2026 Oliv3rmoon
// SPDX-License-Identifier: GPL-3.0-or-later
#include "migrate.h"

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

namespace Eden::Migrate {
namespace {
// Folders deeper than this are not followed (the console's reader follows links).
constexpr int kMaxDepth = 64;
// What a file is copied through.
constexpr std::size_t kChunk = 1u << 20;
// What Needed counts beyond the files and folders: the record and the settings rewritten.
constexpr std::uint64_t kSlack = 1u << 20;

std::string Join(const std::string& folder, const std::string& name) {
    return folder.empty() ? name : folder + "/" + name;
}

std::string Why(int error) { return std::strerror(error); }

// A drive that refuses a write for want of room.
bool Full(int error) { return error == ENOSPC || error == EDQUOT; }

std::string Trimmed(std::string path) {
    while (path.size() > 1 && path.back() == '/') path.pop_back();
    return path;
}

void Log(const Platform& platform, const std::string& line) {
    if (platform.log) platform.log("EDEN_MIGRATE " + line);
}

int Rename(const Platform& platform, const std::string& from, const std::string& to) {
    if (platform.rename) return platform.rename(from, to);
    return std::rename(from.c_str(), to.c_str()) == 0 ? 0 : (errno != 0 ? errno : EIO);
}

bool Exists(const std::string& path) {
    struct stat info {};
    return stat(path.c_str(), &info) == 0 || errno != ENOENT;
}
bool IsFolder(const std::string& path) {
    struct stat info {};
    return stat(path.c_str(), &info) == 0 && S_ISDIR(info.st_mode);
}
bool IsFile(const std::string& path) {
    struct stat info {};
    return stat(path.c_str(), &info) == 0 && S_ISREG(info.st_mode);
}

// The folder a path is in.
std::string Parent(const std::string& path) {
    const std::size_t slash = path.find_last_of('/');
    if (slash == std::string::npos) return ".";
    return slash == 0 ? std::string{"/"} : path.substr(0, slash);
}

std::time_t Now(const Platform& platform) { return platform.clock ? platform.clock() : std::time(nullptr); }

std::string Format(std::time_t when, const char* format) {
    char text[32] = "unknown";
    std::tm local{};
    if (localtime_r(&when, &local) != nullptr) std::strftime(text, sizeof(text), format, &local);
    return text;
}

// `base` when nothing has that name, else base-2, base-3...
std::string Unused(const std::string& base) {
    if (!Exists(base)) return base;
    for (int i = 2; i < 1000; ++i)
        if (const std::string candidate = base + "-" + std::to_string(i); !Exists(candidate)) return candidate;
    return base + "-" + std::to_string(std::time(nullptr));
}

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

// A small file of the migration's own, whole: written beside it, then renamed over it.
int WriteText(const Platform& platform, const std::string& path, const std::string& text) {
    const std::string staged = path + ".new";
    const int fd = open(staged.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0) return errno;
    int error = WriteAll(platform, fd, text.data(), text.size());
    if (error == 0 && fsync(fd) != 0 && (errno == EIO || Full(errno))) error = errno;
    if (close(fd) != 0 && error == 0) error = errno;
    if (error == 0 && std::rename(staged.c_str(), path.c_str()) != 0) error = errno;
    if (error != 0) (void)unlink(staged.c_str());
    return error;
}

bool ReadText(const std::string& path, std::string* text) {
    std::FILE* file = std::fopen(path.c_str(), "rb");
    if (!file) return false;
    text->clear();
    char buffer[4096];
    for (std::size_t count; (count = std::fread(buffer, 1, sizeof(buffer), file)) > 0;) {
        text->append(buffer, count);
        if (text->size() > 65536) break; // a record is far smaller
    }
    const bool ok = !std::ferror(file);
    std::fclose(file);
    return ok;
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

// A copy this file made and all it holds (its name ends in .incomplete: nothing else is ever
// removed); false when anything stays.
bool RemoveTree(const Platform& platform, const std::string& path, int depth = 0) {
    if (depth == 0 && !path.ends_with(kIncomplete)) return false;
    std::vector<Entry> entries;
    bool ok = depth <= kMaxDepth;
    if (ok && platform.list(path, &entries)) {
        for (const Entry& entry : entries) {
            if (entry.name.empty() || entry.name == "." || entry.name == ".." || entry.name.find('/') != std::string::npos)
                continue;
            const std::string full = path + "/" + entry.name;
            if (entry.folder)
                ok = RemoveTree(platform, full, depth + 1) && ok;
            else if (unlink(full.c_str()) != 0 && errno != ENOENT)
                ok = false;
        }
    }
    return (rmdir(path.c_str()) == 0 || errno == ENOENT) && ok;
}

enum class Copied : std::uint8_t { done, gone, stopped, full, failed };

// One file. A file that has gone since it was measured is skipped.
Copied CopyOne(const Platform& platform, const std::string& from, const std::string& to, std::vector<char>& buffer,
               Progress& progress, const std::atomic<bool>& stop, std::string* error) {
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
        if (const int why = WriteAll(platform, out, buffer.data(), static_cast<std::size_t>(count)); why != 0) {
            *error = "could not write " + to + ": " + Why(why);
            outcome = Full(why) ? Copied::full : Copied::failed;
            break;
        }
        progress.bytes += static_cast<std::uint64_t>(count);
    }
    close(in);
    if (close(out) != 0 && outcome == Copied::done) {
        const int why = errno;
        *error = "could not write " + to + ": " + Why(why);
        outcome = Full(why) ? Copied::full : Copied::failed;
    }
    return outcome; // what was written goes with the copy's folder
}

// The settings file of the moved or copied folder rewritten for its new place.
bool Settings(const Platform& platform, const std::string& file, const Rewrite& rewrite, std::string* error) {
    if (!platform.settings) return true;
    std::string changes;
    if (!platform.settings(file, rewrite, &changes, error)) {
        Log(platform, "failed step=settings file=" + file + " error=" + *error);
        return false;
    }
    Log(platform, "settings file=" + file + (changes.empty() ? std::string{" unchanged"} : " " + changes));
    return true;
}

// This start's own folder, set aside, goes into the new data folder's logs (it holds this start's
// logs, which are still being written). Where it is now.
std::string Place(const Platform& platform, const std::string& data, const std::string& aside) {
    if (aside.empty()) return {};
    (void)mkdir((data + "/logs").c_str(), 0777);
    const std::string target = Unused(data + "/logs/first-start-" + Format(Now(platform), "%Y%m%d-%H%M%S"));
    if (const int error = Rename(platform, aside, target); error != 0) {
        Log(platform, "aside kept path=" + aside + " error=" + Why(error));
        return aside;
    }
    Log(platform, "aside placed from=" + aside + " to=" + target);
    return target;
}

// Back to where it was after a failed step: the folder set aside takes its name again.
bool Restore(const Platform& platform, const std::string& aside, const std::string& data, Result* result) {
    if (aside.empty()) return true;
    if (const int error = Rename(platform, aside, data); error != 0) {
        result->intact = false;
        result->error += "; this start's own folder stays in " + aside;
        Log(platform, "undo failed from=" + aside + " to=" + data + " error=" + Why(error));
        return false;
    }
    Log(platform, "undone from=" + aside + " to=" + data);
    return true;
}

Result Fail(const Platform& platform, Result result, const std::string& step, std::string error) {
    result.outcome = Outcome::failed;
    result.error = std::move(error);
    Log(platform, "failed step=" + step + " error=" + result.error);
    return result;
}

Result MoveData(const Folders& folders, const Platform& platform) {
    Result result;
    const std::string old = Trimmed(folders.old_data), data = Trimmed(folders.data);
    Log(platform, "start choice=move from=" + old + " to=" + data);
    if (!IsFolder(old)) return Fail(platform, result, "check", old + " is not there");
    // The folder this start made steps aside, so that the old one can take its name whole.
    std::string aside;
    if (Exists(data)) {
        aside = Unused(data + kAside);
        if (const int error = Rename(platform, data, aside); error != 0)
            return Fail(platform, result, "aside", "could not rename " + data + " to " + aside + ": " + Why(error));
        Log(platform, "aside from=" + data + " to=" + aside);
    }
    if (const int error = Rename(platform, old, data); error != 0) {
        result = Fail(platform, result, "rename", "could not rename " + old + " to " + data + ": " + Why(error));
        Restore(platform, aside, data, &result);
        return result;
    }
    Log(platform, "moved from=" + old + " to=" + data);
    // The settings name the folder's new place; when they can't, everything goes back.
    std::string error;
    if (!Settings(platform, data + "/" + kSettings, {old, data, {}}, &error)) {
        result.outcome = Outcome::failed;
        result.error = "could not update the settings: " + error;
        if (const int back = Rename(platform, data, old); back != 0) {
            result.intact = false;
            result.error += "; ProsperoEden's data is now in " + data;
            Log(platform, "undo failed from=" + data + " to=" + old + " error=" + Why(back));
        } else {
            Log(platform, "undone from=" + data + " to=" + old);
            Restore(platform, aside, data, &result);
        }
        return result;
    }
    if (!Record(data, State::moved, folders, platform)) Log(platform, "record failed state=moved");
    result.aside = Place(platform, data, aside);
    result.outcome = Outcome::done;
    Log(platform, "done choice=move");
    return result;
}

Result CopyData(const Folders& folders, const Plan* given, const Platform& platform, Progress& progress,
                const std::atomic<bool>& stop) {
    Result result;
    const std::string old = Trimmed(folders.old_data), data = Trimmed(folders.data);
    const std::string staging = data + kIncomplete;
    const bool games_stay = GamesInside(folders);
    const std::string games = games_stay ? Trimmed(folders.games) : std::string{};
    Log(platform, "start choice=copy from=" + old + " to=" + data + " games=" + (games.empty() ? "-" : games));
    if (!IsFolder(old)) return Fail(platform, result, "check", old + " is not there");
    if (Exists(staging) && !RemoveTree(platform, staging))
        return Fail(platform, result, "leftover", "an unfinished copy in " + staging + " could not be removed");
    Plan measured;
    const Plan* plan = given;
    if (plan == nullptr) {
        std::string error;
        if (!Measure(old, Excluded(folders), platform, stop, &measured, &error)) {
            if (stop.load()) {
                result.outcome = Outcome::cancelled;
                Log(platform, "cancelled step=measure");
                return result;
            }
            return Fail(platform, result, "measure", error);
        }
        plan = &measured;
    }
    std::string excluded;
    for (const std::string& path : Excluded(folders)) excluded += (excluded.empty() ? "" : ",") + path;
    Log(platform, "plan files=" + std::to_string(plan->files.size()) + " folders=" + std::to_string(plan->folders.size()) +
                      " bytes=" + std::to_string(plan->bytes) + " excluded=" + excluded);
    // Room: refused before anything is written where the system says how much there is. A PS5
    // title can't ask (remote/remote.cpp): there the copy stops at the first write refused as full.
    Space space;
    if (platform.space && platform.space(Parent(data), &space)) {
        const std::uint64_t needed = Needed(*plan, space.unit);
        Log(platform, "room needed=" + std::to_string(needed) + " free=" + std::to_string(space.free));
        if (needed > space.free) {
            result.outcome = Outcome::no_room;
            result.room_known = true;
            result.needed = needed;
            result.free = space.free;
            result.error = "not enough free space";
            Log(platform, "no_room before writing");
            return result;
        }
    } else {
        Log(platform, "room unknown: the copy stops if the drive fills up");
    }
    // What the copy wrote goes when it does not finish.
    const auto abandon = [&](Outcome outcome, std::string error) {
        result.outcome = outcome;
        result.error = std::move(error);
        const bool removed = RemoveTree(platform, staging);
        Log(platform, std::string{outcome == Outcome::cancelled ? "cancelled" : outcome == Outcome::no_room ? "no_room" : "failed"} +
                          " step=copy" + (result.error.empty() ? "" : " error=" + result.error) +
                          " removed=" + (removed ? "1" : "0") + " path=" + staging);
        if (!removed) result.error += (result.error.empty() ? "" : "; ") + std::string{"the unfinished copy stays in "} + staging;
        return result;
    };
    if (mkdir(staging.c_str(), 0777) != 0)
        return Fail(platform, result, "copy", "could not make " + staging + ": " + Why(errno));
    for (const std::string& folder : plan->folders) {
        const std::string path = staging + "/" + folder;
        if (mkdir(path.c_str(), 0777) != 0 && errno != EEXIST) {
            const int why = errno;
            return abandon(Full(why) ? Outcome::no_room : Outcome::failed, "could not make " + path + ": " + Why(why));
        }
    }
    std::vector<char> buffer(kChunk);
    for (std::size_t i = 0; i < plan->files.size(); ++i) {
        const File& file = plan->files[i];
        if (platform.before_file) platform.before_file(i, file.path);
        if (stop.load()) return abandon(Outcome::cancelled, {});
        std::string error;
        switch (CopyOne(platform, old + "/" + file.path, staging + "/" + file.path, buffer, progress, stop, &error)) {
        case Copied::done:
            ++progress.files;
            result.files++;
            result.bytes += file.size;
            break;
        case Copied::gone:
            break;
        case Copied::stopped:
            return abandon(Outcome::cancelled, {});
        case Copied::full:
            result.needed = Needed(*plan, 0);
            return abandon(Outcome::no_room, error);
        case Copied::failed:
            return abandon(Outcome::failed, error);
        }
    }
    Log(platform, "copied files=" + std::to_string(result.files) + " bytes=" + std::to_string(result.bytes));
    // The copy's settings name the new place, and where the games stayed; its record says copied.
    std::string error;
    if (!Settings(platform, staging + "/" + kSettings, {old, data, games}, &error))
        return abandon(Outcome::failed, "could not update the settings: " + error);
    if (!games.empty()) Log(platform, "games_folder=" + games + " (the games stay there)");
    if (!Record(staging, State::copied, folders, platform, games))
        return abandon(Outcome::failed, "could not write " + staging + "/" + kRecord);
    // The switch: this start's folder steps aside, the copy takes its name.
    std::string aside;
    if (Exists(data)) {
        aside = Unused(data + kAside);
        if (const int why = Rename(platform, data, aside); why != 0)
            return abandon(Outcome::failed, "could not rename " + data + " to " + aside + ": " + Why(why));
        Log(platform, "aside from=" + data + " to=" + aside);
    }
    if (const int why = Rename(platform, staging, data); why != 0) {
        result.error = "could not rename " + staging + " to " + data + ": " + Why(why);
        Log(platform, "failed step=rename error=" + result.error);
        Restore(platform, aside, data, &result);
        const std::string kept = result.error;
        return abandon(Outcome::failed, kept);
    }
    Log(platform, "copied into=" + data);
    result.games_folder = games;
    result.aside = Place(platform, data, aside);
    result.outcome = Outcome::done;
    Log(platform, "done choice=copy");
    return result;
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

const char* StateName(State state) {
    switch (state) {
    case State::asking: return "asking";
    case State::moved: return "moved";
    case State::copied: return "copied";
    case State::fresh: return "fresh";
    case State::none: break;
    }
    return "none";
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
    // statfs is only in libkernel_sys, which a title does not import (remote/remote.cpp).
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
    platform.write = [](int fd, const void* data, std::size_t size) { return ::write(fd, data, size); };
    platform.rename = [](const std::string& from, const std::string& to) {
        return std::rename(from.c_str(), to.c_str()) == 0 ? 0 : (errno != 0 ? errno : EIO);
    };
    platform.log = [](const std::string& line) { Report("migration", line.c_str()); };
    return platform;
}

State Recorded(const std::string& data) {
    std::string text;
    if (!ReadText(Trimmed(data) + "/" + kRecord, &text)) return State::none;
    const std::size_t key = text.find("\"state\"");
    if (key == std::string::npos) return State::none;
    const std::size_t open = text.find('"', text.find(':', key));
    const std::size_t close = open == std::string::npos ? open : text.find('"', open + 1);
    if (close == std::string::npos) return State::none;
    const std::string value = text.substr(open + 1, close - open - 1);
    for (const State state : {State::asking, State::moved, State::copied, State::fresh})
        if (value == StateName(state)) return state;
    return State::none;
}

bool Record(const std::string& data, State state, const Folders& folders, const Platform& platform,
            const std::string& games_folder) {
    const std::string top = Trimmed(data);
    (void)mkdir(top.c_str(), 0777);
    (void)mkdir((top + "/config").c_str(), 0777);
    const std::time_t when = Now(platform);
    std::string text = "{\n";
    text += "  \"state\": " + Quoted(StateName(state)) + ",\n";
    text += "  \"from\": " + Quoted(Trimmed(folders.old_data)) + ",\n";
    text += "  \"to\": " + Quoted(Trimmed(folders.data)) + ",\n";
    if (!games_folder.empty()) text += "  \"games_folder\": " + Quoted(games_folder) + ",\n";
    text += "  \"when\": " + Quoted(Format(when, "%Y-%m-%dT%H:%M:%S")) + "\n";
    text += "}\n";
    const std::string file = top + "/" + kRecord;
    if (const int error = WriteText(platform, file, text); error != 0) {
        Log(platform, "record failed file=" + file + " error=" + Why(error));
        return false;
    }
    Log(platform, std::string{"record state="} + StateName(state) + " file=" + file);
    return true;
}

State Prepare(const Folders& folders, const Platform& platform) {
    const std::string data = Trimmed(folders.data), old = Trimmed(folders.old_data);
    // A copy an earlier start did not finish (it was closed while copying) is of no use.
    if (const std::string staging = data + kIncomplete; Exists(staging)) {
        const bool removed = RemoveTree(platform, staging);
        Log(platform, "leftover path=" + staging + " removed=" + (removed ? "1" : "0"));
    }
    const State recorded = Recorded(data);
    if (recorded != State::none && recorded != State::asking) return recorded;
    if (recorded == State::none && IsFile(data + "/" + kSettings)) return State::none; // Nightshade's own
    std::vector<Entry> entries;
    if (!IsFolder(old) || !platform.list(old, &entries)) return State::none;
    const bool something = std::any_of(entries.begin(), entries.end(), [](const Entry& entry) {
        return !entry.name.empty() && entry.name != "." && entry.name != ".." && entry.name != "logs";
    });
    if (!something) return State::none;
    Log(platform, std::string{recorded == State::asking ? "ask again" : "ask"} + " from=" + old + " to=" + data);
    if (recorded != State::asking) (void)Record(data, State::asking, folders, platform);
    return State::asking;
}

bool GamesInside(const Folders& folders) {
    const std::string old = Trimmed(folders.old_data), games = Trimmed(folders.games);
    return !games.empty() &&
           (games == old || (games.size() > old.size() + 1 && games.compare(0, old.size(), old) == 0 && games[old.size()] == '/'));
}

std::vector<std::string> Excluded(const Folders& folders) {
    std::set<std::string> out{"logs", "previous-version", "user/cache/nsz"};
    if (GamesInside(folders)) {
        const std::string old = Trimmed(folders.old_data), games = Trimmed(folders.games);
        const std::string relative = games.size() > old.size() ? games.substr(old.size() + 1) : std::string{};
        for (const char* folder : {"roms", "updates", ".remote-downloads"}) out.insert(Join(relative, folder));
    }
    return {out.begin(), out.end()};
}

bool Measure(const std::string& from, const std::vector<std::string>& excluded, const Platform& platform,
             const std::atomic<bool>& stop, Plan* plan, std::string* error) {
    *plan = {};
    const std::string top = Trimmed(from);
    const std::set<std::string> skip(excluded.begin(), excluded.end());
    const std::function<bool(const std::string&, int)> walk = [&](const std::string& relative, int depth) {
        if (stop.load()) {
            *error = "stopped";
            return false;
        }
        const std::string folder = relative.empty() ? top : top + "/" + relative;
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
            if (entry.name.empty() || entry.name == "." || entry.name == ".." || entry.name.find('/') != std::string::npos)
                continue;
            const std::string path = Join(relative, entry.name);
            if (skip.count(path)) continue;
            if (entry.folder) {
                plan->folders.push_back(path);
                if (!walk(path, depth + 1)) return false;
                continue;
            }
            struct stat info {};
            if (stat((top + "/" + path).c_str(), &info) != 0) {
                if (errno == ENOENT) continue; // gone since it was listed
                *error = "could not read " + top + "/" + path + ": " + Why(errno);
                return false;
            }
            if (!S_ISREG(info.st_mode)) continue;
            plan->files.push_back({path, static_cast<std::uint64_t>(info.st_size)});
            plan->bytes += static_cast<std::uint64_t>(info.st_size);
        }
        return true;
    };
    return walk("", 0);
}

std::uint64_t Needed(const Plan& plan, std::uint64_t unit) {
    if (unit == 0) unit = 4096;
    std::uint64_t total = kSlack + unit * (plan.folders.size() + 1);
    for (const File& file : plan.files) total += (file.size + unit - 1) / unit * unit;
    return total;
}

Result Run(Choice choice, const Folders& folders, const Plan* plan, const Platform& platform, Progress& progress,
           const std::atomic<bool>& stop) {
    switch (choice) {
    case Choice::move:
        return MoveData(folders, platform);
    case Choice::copy:
        return CopyData(folders, plan, platform, progress, stop);
    case Choice::fresh:
        break;
    }
    Result result;
    Log(platform, "start choice=fresh from=" + Trimmed(folders.old_data) + " to=" + Trimmed(folders.data));
    // Not recorded: it is asked again at the next start, which is all that is lost.
    if (!Record(folders.data, State::fresh, folders, platform)) Log(platform, "record failed state=fresh");
    result.outcome = Outcome::done;
    Log(platform, "done choice=fresh");
    return result;
}

// ---- Worker ----

Worker::Worker(Setup setup) : setup_(std::move(setup)) {
    status_.from = Trimmed(setup_.folders.old_data);
    status_.to = Trimmed(setup_.folders.data);
    status_.games = GamesInside(setup_.folders) ? Trimmed(setup_.folders.games) : std::string{};
    if (!setup_.access || Prepare(setup_.folders, setup_.platform) != State::asking) return;
    const std::lock_guard guard(lock_);
    status_.phase = pe::ui::MigrationPhase::asking;
    // What a copy would copy, measured while the question is read.
    Begin([this] { Look(); });
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

void Worker::Look() {
    Plan plan;
    std::string problem;
    const bool measured = Measure(setup_.folders.old_data, Excluded(setup_.folders), setup_.platform, stop_, &plan, &problem);
    Log(setup_.platform, measured ? "measured files=" + std::to_string(plan.files.size()) + " bytes=" + std::to_string(plan.bytes)
                                  : "measure ended: " + problem);
    pe::ui::MigrationChoice next{};
    bool answer = false;
    {
        const std::lock_guard guard(lock_);
        if (measured) {
            plan_ = std::move(plan);
            planned_ = true;
            status_.measured = true;
            status_.files = plan_.files.size();
            status_.bytes = plan_.bytes;
        }
        // An answer given while this measured goes on here.
        if (status_.phase == pe::ui::MigrationPhase::working) {
            next = status_.choice;
            answer = true;
            if (next == Choice::copy && stop_.load()) {
                status_.phase = pe::ui::MigrationPhase::cancelled; // cancelled before it began
                answer = false;
                Log(setup_.platform, "cancelled step=measure");
            } else {
                stop_ = false; // the measure's stop was for a move or a fresh start
            }
        }
        if (!answer) {
            busy_ = false;
            return;
        }
    }
    Answer(next);
}

void Worker::Answer(Choice choice) {
    const Result result = Run(choice, setup_.folders, choice == Choice::copy && planned_ ? &plan_ : nullptr,
                              setup_.platform, progress_, stop_);
    const std::lock_guard guard(lock_);
    status_.error = result.error;
    status_.intact = result.intact;
    status_.room_known = result.room_known;
    status_.needed = result.needed;
    status_.free = result.free;
    status_.done_files = progress_.files.load();
    status_.done_bytes = progress_.bytes.load();
    switch (result.outcome) {
    case Outcome::done:
        status_.phase = choice == Choice::fresh ? pe::ui::MigrationPhase::none : pe::ui::MigrationPhase::done;
        if (choice == Choice::copy) status_.games = result.games_folder;
        break;
    case Outcome::cancelled:
        status_.phase = pe::ui::MigrationPhase::cancelled;
        break;
    case Outcome::failed:
        status_.phase = pe::ui::MigrationPhase::failed;
        break;
    case Outcome::no_room:
        status_.phase = pe::ui::MigrationPhase::no_room;
        break;
    }
    busy_ = false; // with the result
}

bool Worker::Start(Choice choice, std::string* reason) {
    const std::lock_guard guard(lock_);
    if (status_.phase != pe::ui::MigrationPhase::asking) {
        *reason = "nothing is asked now";
        return false;
    }
    status_.phase = pe::ui::MigrationPhase::working;
    status_.choice = choice;
    status_.done_files = status_.done_bytes = 0;
    status_.error.clear();
    status_.intact = true;
    status_.room_known = false;
    status_.needed = status_.free = 0;
    progress_.files = 0;
    progress_.bytes = 0;
    if (busy_) {
        // Still measuring: a copy waits for it (it copies what it measured), a move or a fresh
        // start stops it. Look goes on with the answer.
        if (choice != Choice::copy) stop_ = true;
        return true;
    }
    Begin([this, choice] { Answer(choice); });
    return true;
}

void Worker::Cancel() {
    const std::lock_guard guard(lock_);
    // Only a copy stops part way; a move is two renames.
    if (status_.phase == pe::ui::MigrationPhase::working && status_.choice == Choice::copy) stop_ = true;
}

void Worker::Finish() {
    const std::lock_guard guard(lock_);
    switch (status_.phase) {
    case pe::ui::MigrationPhase::cancelled:
    case pe::ui::MigrationPhase::failed:
    case pe::ui::MigrationPhase::no_room:
        status_.phase = pe::ui::MigrationPhase::asking;
        break;
    default:
        break;
    }
}

pe::ui::MigrationStatus Worker::Status() {
    const std::lock_guard guard(lock_);
    pe::ui::MigrationStatus status = status_;
    if (status.phase == pe::ui::MigrationPhase::working) {
        status.done_files = progress_.files.load();
        status.done_bytes = progress_.bytes.load();
    }
    return status;
}

} // namespace Eden::Migrate
