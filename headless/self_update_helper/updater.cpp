// ps5-native-app-boilerplate - The self-update helper's work, apart from the console.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The helper runs outside the app's sandbox, started by the console's payload
// loader, because that is where an app's folder can be written and where the
// console does not slow file writes down. It trusts nothing it is sent: the
// title ID decides every path, the archive must match the digest it was
// announced with, and the unpacked app must be the title and version asked for.
#include "updater.hpp"
#include "update_check.h"
#include "self_update_protocol.h"
#include "self_update_sha256.h"
#include "archive.hpp"
#include "files.hpp"
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <dirent.h>
#include <fcntl.h>
#include <memory>
#include <mutex>
#include <pthread.h>
#include <sys/stat.h>
#include <unistd.h>

namespace self_update
{
namespace
{
constexpr std::size_t kChildren = 4096;   // apps looked at in one folder
constexpr std::size_t kParam = 64 * 1024; // largest param.json read
constexpr std::uint64_t kDownloadMargin = 16ull << 20;
constexpr std::uint64_t kUnpackMargin = 64ull << 20;

bool title_id(const std::string &text)
{
    if (text.size() != 9)
        return false;
    for (std::size_t i = 0; i < 9; ++i)
        if (i < 4 ? text[i] < 'A' || text[i] > 'Z' : text[i] < '0' || text[i] > '9')
            return false;
    return true;
}

bool content_version(const std::string &text)
{
    unsigned parts[3];
    return update_check_version_parse(text.c_str(), parts) == 1;
}

bool newer(const std::string &available, const std::string &installed)
{
    int comparable = 0;
    return update_check_version_compare(available.c_str(), installed.c_str(), &comparable) > 0 &&
           comparable;
}

bool hex_digest(const std::string &text)
{
    if (text.size() != 64)
        return false;
    for (const char c : text)
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')))
            return false;
    return true;
}

// Text shown in a notification: no control characters, not too long.
std::string shown(const std::string &text, std::size_t limit, const std::string &fallback)
{
    std::string out;
    for (const char c : text)
    {
        const auto byte = static_cast<unsigned char>(c);
        if (byte < 0x20 || byte == 0x7f)
            continue;
        if (out.size() >= limit)
            break;
        out.push_back(c);
    }
    return out.empty() ? fallback : out;
}

bool list_entries(const std::string &folder, std::size_t limit, std::vector<std::string> &names)
{
    std::unique_ptr<DIR, decltype(&closedir)> directory(opendir(folder.c_str()), closedir);
    if (!directory)
        return false;
    for (;;)
    {
        errno = 0;
        const auto *entry = readdir(directory.get());
        if (!entry)
            return errno == 0;
        const std::string name = entry->d_name;
        if (name == "." || name == "..")
            continue;
        if (names.size() >= limit)
            return false;
        names.push_back(name);
    }
}

// The title ID and content version an app folder declares.
bool read_param(const std::string &folder, std::string &title, std::string &version)
{
    std::string body;
    if (!read_small(folder + "/sce_sys/param.json", kParam, body))
        return false;
    char id[10];
    char content[12];
    if (update_check_json_string(body.data(), body.size(), "titleId", id, sizeof(id)) != 1 ||
        update_check_json_string(body.data(), body.size(), "contentVersion", content,
                                 sizeof(content)) != 1)
        return false;
    title = id;
    version = content;
    return true;
}

struct Copy
{
    std::string path, version;
};

// Every installed folder of the title under the roots. False: a root could not be read fully.
bool find_copies(const Environment &environment, const std::string &title, std::vector<Copy> &out)
{
    for (const auto &root : environment.roots)
    {
        if (kind(root) != Kind::directory)
            continue;
        std::vector<std::string> names;
        if (!list_entries(root, kChildren, names))
            return false;
        for (const auto &name : names)
        {
            const std::string folder = root + "/" + name;
            std::string id, version;
            if (kind(folder) != Kind::directory || !read_param(folder, id, version) || id != title)
                continue;
            bool known = false;
            for (const auto &copy : out)
                known = known || copy.path == folder;
            if (!known)
                out.push_back({folder, version});
        }
    }
    return true;
}

// ProsperoEden: ShadowMountPlus records where it mounted an app from in
// <registered>/app/<TITLEID>/mount.lnk (a folder) or mount_img.lnk (an image). 1: path is
// that folder; 0: no record; -1: the app is an image (its files can't be replaced one by one).
int mounted_source(const Environment &environment, const std::string &title, std::string &path)
{
    if (environment.registered.empty())
        return 0;
    const std::string app = environment.registered + "/app/" + title;
    std::FILE *file = std::fopen((app + "/mount.lnk").c_str(), "rb");
    if (!file)
        return kind(app + "/mount_img.lnk") == Kind::file ? -1 : 0;
    char text[512] = {};
    const std::size_t length = std::fread(text, 1, sizeof(text) - 1, file);
    std::fclose(file);
    path.assign(text, length);
    while (!path.empty() && (path.back() == '\n' || path.back() == '\r' || path.back() == ' ' ||
                             path.back() == '\0' || path.back() == '/'))
        path.pop_back();
    return path.size() > 1 && path[0] == '/' ? 1 : 0;
}

bool same_filesystem(const std::string &first, const std::string &second)
{
    struct stat a
    {
    };
    struct stat b
    {
    };
    return stat(first.c_str(), &a) == 0 && stat(second.c_str(), &b) == 0 && a.st_dev == b.st_dev;
}

// The console starts an app only if its files can be read and run by everyone, which is how an
// app copied to the console arrives (mode 0777, owner and group 0). Unpacked files are made
// the same; with any other mode the console answers "Can't start the game or app" (found on a
// console: process creation fails with EACCES).
bool open_permissions(const std::string &path, unsigned depth = 0)
{
    const Kind what = kind(path);
    if ((what != Kind::directory && what != Kind::file) || depth > 32)
        return false;
    if (chmod(path.c_str(), 0777) != 0)
        return false;
    (void)chown(path.c_str(), 0, 0); // best effort: the mode is what decides
    if (what == Kind::file)
        return true;
    std::vector<std::string> names;
    if (!list_entries(path, kArchiveEntries, names))
        return false;
    for (const auto &name : names)
        if (!open_permissions(path + "/" + name, depth + 1))
            return false;
    return true;
}

// ShadowMountPlus copies an app's sce_sys once, when it first registers the
// title. After an update those copies are brought up to date here. Best effort.
void refresh_registered(const Environment &environment, const std::string &title,
                        const std::string &target)
{
    if (environment.registered.empty())
        return;
    const std::string source = target + "/sce_sys";
    std::vector<std::string> names;
    if (!list_files(source, 64, names))
        return;
    const std::string app = environment.registered + "/app/" + title;
    for (const auto &folder : {environment.registered + "/appmeta/" + title, app + "/sce_sys"})
        if (kind(folder) == Kind::directory)
            for (const auto &name : names)
                copy_file(source + "/" + name, folder + "/" + name);
    if (kind(app + "/icon0.png") == Kind::file)
        copy_file(source + "/icon0.png", app + "/icon0.png");
}

// The conversation with the app. Lines out are serialised; after the archive
// has arrived a thread of its own reads what the app says.
struct Session
{
    // Copies: the listening thread can outlive the call that started it.
    const Environment environment;
    const Io io;
    Session(const Environment &environment_, const Io &io_) : environment(environment_), io(io_)
    {
    }
    std::mutex output;
    std::atomic<bool> cancelled{false}, apply{false}, closed{false}, unpacking{false};
    std::atomic<std::uint64_t> written{0};
    std::uint64_t to_unpack = 0;

    void log(const std::string &line) const
    {
        if (environment.log)
            environment.log(line);
    }
    bool say(const std::string &line)
    {
        // ProsperoEden: a refusal is in the log too, not only on the app's screen.
        if (line.rfind("fail ", 0) == 0)
            log(line.substr(0, line.size() - (line.empty() || line.back() != '\n' ? 0 : 1)));
        std::lock_guard lock(output);
        std::size_t done = 0;
        while (done < line.size())
        {
            const long count = io.write(line.data() + done, line.size() - done);
            if (count <= 0)
                return false;
            done += static_cast<std::size_t>(count);
        }
        return true;
    }
    bool read_exact(void *data, std::size_t size) const
    {
        auto *bytes = static_cast<char *>(data);
        while (size)
        {
            const long count = io.read(bytes, size);
            if (count <= 0)
                return false;
            bytes += count;
            size -= static_cast<std::size_t>(count);
        }
        return true;
    }
    bool read_line(std::string &line) const
    {
        line.clear();
        for (;;)
        {
            char byte = 0;
            if (io.read(&byte, 1) <= 0)
                return false;
            if (byte == '\n')
                return true;
            if (line.size() >= SELF_UPDATE_LINE)
                return false;
            line.push_back(byte);
        }
    }
    void sleep(unsigned milliseconds) const
    {
        if (environment.sleep)
            environment.sleep(milliseconds);
        else
            usleep(milliseconds * 1000u);
    }
};

void *listen(void *argument)
{
    // Its own reference keeps the session alive for as long as it reads.
    const std::unique_ptr<std::shared_ptr<Session>> holder(
        static_cast<std::shared_ptr<Session> *>(argument));
    auto &session = **holder;
    std::string line;
    while (session.read_line(line))
    {
        if (line == "apply")
            session.apply = true;
        else if (!session.apply.load())
            session.cancelled = true; // "cancel", or anything unexpected
    }
    // The app closing the connection before "apply" is a cancel; after it, it is the app leaving.
    if (!session.apply.load())
        session.cancelled = true;
    session.closed = true;
    return nullptr;
}

void *report(void *argument)
{
    auto &session = *static_cast<Session *>(argument);
    while (session.unpacking.load())
    {
        session.say("p " + std::to_string(session.written.load()) + " " +
                    std::to_string(session.to_unpack) + "\n");
        session.sleep(200);
    }
    return nullptr;
}

// Saves the archive as it arrives, hashing it. The file is kept only when the
// end mark arrived, its size is the one announced and its digest matches.
bool receive_archive(Session &session, const std::string &path, std::uint64_t size,
                     const std::string &digest, std::string &error)
{
    error = "The download could not be saved";
    const int descriptor = open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
    if (descriptor < 0)
        return false;
    const std::uint64_t limit = size ? size : SELF_UPDATE_MAX_ARCHIVE;
    std::vector<char> piece(SELF_UPDATE_PIECE);
    self_update_sha256 hash;
    self_update_sha256_start(&hash);
    std::uint64_t received = 0;
    bool complete = false, saved = true, connected = true;
    for (;;)
    {
        unsigned char header[4];
        if (!session.read_exact(header, sizeof(header)))
        {
            connected = false;
            break;
        }
        const std::size_t length = static_cast<std::size_t>(header[0]) |
                                   (static_cast<std::size_t>(header[1]) << 8) |
                                   (static_cast<std::size_t>(header[2]) << 16) |
                                   (static_cast<std::size_t>(header[3]) << 24);
        if (length == 0)
        {
            complete = true;
            break;
        }
        if (length > piece.size() || length > limit - received)
        {
            error = "The download is larger than the catalog lists";
            break;
        }
        if (!session.read_exact(piece.data(), length))
        {
            connected = false;
            break;
        }
        self_update_sha256_add(&hash, piece.data(), length);
        received += length;
        for (std::size_t done = 0; done < length && saved;)
        {
            const auto count = write(descriptor, piece.data() + done, length - done);
            if (count < 0 && errno == EINTR)
                continue;
            if (count <= 0)
                saved = false;
            else
                done += static_cast<std::size_t>(count);
        }
        if (!saved)
            break;
    }
    saved = saved && fsync(descriptor) == 0;
    saved = close(descriptor) == 0 && saved;
    if (!connected)
    {
        session.cancelled = true;
        error = "Cancelled";
        return false;
    }
    if (!complete || !saved)
        return false;
    unsigned char bytes[32];
    self_update_sha256_finish(&hash, bytes);
    if ((size && received != size) || !self_update_sha256_matches(bytes, digest.c_str()))
    {
        error = "The download doesn't match the catalog's listing";
        return false;
    }
    error.clear();
    return true;
}

// The release, from the app into <work>/staged: saved as it arrives and checked against its
// digest, a thread listening for what the app says next, the app's folder unpacked from it and
// opened to everyone. 0: staged (info says how much was unpacked). 1: not; the work folder is gone
// (clean) and the app was told why, unless it cancelled.
int stage_release(const std::shared_ptr<Session> &shared, const std::string &title, std::uint64_t size,
                  const std::string &digest, const std::string &work, const std::function<void()> &clean,
                  ArchiveInfo &info)
{
    Session &session = *shared;
    const Environment &environment = session.environment;
    const std::string archive = work + "/update.zip", staged = work + "/staged";
    std::uint64_t free_bytes = 0;
    std::string error;
    if (!receive_archive(session, archive, size, digest, error))
    {
        clean();
        if (!session.cancelled.load())
            session.say("fail " + error + "\n");
        return 1;
    }

    pthread_t listener{};
    auto *reference = new std::shared_ptr<Session>(shared);
    if (pthread_create(&listener, nullptr, listen, reference) != 0)
    {
        delete reference;
        clean();
        session.say("fail The helper couldn't start\n");
        return 1;
    }
    // The listener stays in its read until the app goes away, which a failed
    // update doesn't wait for: nothing it touches is used after this function.
    pthread_detach(listener);
    const auto fail = [&](const std::string &reason)
    {
        clean();
        if (!session.cancelled.load())
            session.say("fail " + reason + "\n");
        return 1;
    };

    if (!inspect_archive(archive, title, info, error))
        return fail(error);
    if (environment.space && environment.space(work, free_bytes) &&
        free_bytes < info.unpacked + kUnpackMargin)
        return fail("There isn't enough free space to unpack the update");
    session.to_unpack = info.unpacked;
    session.unpacking = true;
    pthread_t reporter{};
    const bool reporting = pthread_create(&reporter, nullptr, report, &session) == 0;
    const bool unpacked =
        extract_archive(archive, title, staged, session.cancelled, session.written, error);
    session.unpacking = false;
    if (reporting)
        pthread_join(reporter, nullptr);
    if (!unpacked)
        return fail(error.empty() ? "The update could not be unpacked" : error);
    (void)unlink(archive.c_str());
    if (!open_permissions(staged))
        return fail("The update's files could not be prepared");
    return 0;
}

// ProsperoEden: a title's one installed folder, found as run() finds the app's own: the folder
// ShadowMountPlus mounted it from when it recorded one, otherwise the one copy under the roots.
// 1: found; 0: there is none; -1: it is an image; -2: more than one copy; -3: a root could not be
// read whole, so there may be one.
int locate(const Session &session, const std::string &title, Copy &out)
{
    const Environment &environment = session.environment;
    std::string source, id, version;
    const int mounted = mounted_source(environment, title, source);
    if (mounted < 0)
        return -1;
    if (mounted > 0 && kind(source) == Kind::directory && read_param(source, id, version) && id == title)
    {
        out = {source, version};
        session.log("installed folder of " + title + " (ShadowMountPlus): " + source);
        return 1;
    }
    std::vector<Copy> copies;
    if (!find_copies(environment, title, copies))
        return -3;
    if (copies.size() > 1)
        return -2;
    if (copies.empty())
        return 0;
    out = copies[0];
    return 1;
}
} // namespace

std::vector<std::string> default_drives()
{
    std::vector<std::string> drives{"/data", "/mnt/ext0", "/mnt/ext1"};
    for (int i = 0; i < 8; ++i)
        drives.push_back("/mnt/usb" + std::to_string(i));
    return drives;
}

std::vector<std::string> default_roots()
{
    std::vector<std::string> roots;
    for (const auto &drive : default_drives())
    {
        roots.push_back(drive + "/homebrew");
        roots.push_back(drive + "/etaHEN/games");
        if (drive != "/data")
            roots.push_back(drive);
    }
    return roots;
}

std::string drive_root(const std::vector<std::string> &drives, const std::string &path)
{
    for (const auto &drive : drives)
        if (path.size() > drive.size() && path.compare(0, drive.size(), drive) == 0 &&
            path[drive.size()] == '/')
            return drive;
    return {};
}

std::vector<std::string> title_sandboxes(const std::string &sandboxes, const std::string &title)
{
    std::vector<std::string> names, found;
    if (title_id(title) && list_entries(sandboxes, 1024, names))
        for (const auto &name : names)
            if (name.size() > 10 && name[9] == '_' && name.compare(0, 9, title) == 0)
                found.push_back(name);
    return found;
}

CloseWait wait_for_close(const Environment &environment, const std::string &title,
                         const std::function<bool()> &connection_closed,
                         const std::function<bool()> &cancelled)
{
    const auto pause = [&](unsigned milliseconds)
    {
        if (environment.sleep)
            environment.sleep(milliseconds);
        else
            usleep(milliseconds * 1000u);
    };
    unsigned waited = 0, gone_for = 0;
    for (;;)
    {
        if (cancelled && cancelled())
            return CloseWait::cancelled;
        if (title_running(environment.sandboxes, title) == 0)
            return CloseWait::closed;
        if (connection_closed && connection_closed())
        {
            if (gone_for >= environment.stale_sandbox_ms)
                return CloseWait::left_behind;
            gone_for += 250;
        }
        else if (waited >= environment.exit_wait_ms)
            return CloseWait::timed_out;
        pause(250);
        waited += 250;
    }
}

int title_running(const std::string &sandboxes, const std::string &title)
{
    if (!title_id(title))
        return -1;
    std::vector<std::string> names;
    if (list_entries(sandboxes, 1024, names))
    {
        for (const auto &name : names)
            if (name.size() > 10 && name[9] == '_' && name.compare(0, 9, title) == 0)
                return 1;
        return 0;
    }
    // The folder can't be listed: ask for the usual sandbox by name.
    return kind(sandboxes + "/" + title + "_000") == Kind::absent ? -1 : 1;
}

bool launcher_update_compatible(const std::string& data_root, const std::string& staged, std::string& reason)
{
    if(data_root.empty()) return true;
    std::string selection;
    if(!read_small(data_root+"/config/launcher.json",65536,selection)) return true;
    char id[64]{};
    if(update_check_json_string(selection.data(),selection.size(),"selected",id,sizeof(id))!=1 || std::string{id}=="builtin") return true;
    const std::string selected=id;
    if(selected.empty() || selected.size()>48 || !std::all_of(selected.begin(),selected.end(),[](char c){return (c>='a' && c<='z') || (c>='0' && c<='9') || c=='-';})) { reason="Invalid selected launcher";return false; }
    std::string manifest,descriptor;
    char contract[80]{},supports[512]{};
    if(!read_small(data_root+"/launchers/"+selected+"/manifest.json",65536,manifest) ||
       update_check_json_string(manifest.data(),manifest.size(),"host_contract",contract,sizeof(contract))!=1 ||
       !read_small(staged+"/launcher-runtime.json",65536,descriptor) ||
       update_check_json_string(descriptor.data(),descriptor.size(),"supports",supports,sizeof(supports))!=1) {
        reason="This release cannot preserve the selected launcher. Select the built-in launcher before updating.";return false;
    }
    // Sharing a contract name does not imply support for every additive API/capability.
    const auto fail_api=[&] { reason="The selected launcher needs APIs or capabilities missing from this release. Select the built-in launcher before updating.";return false; };
    const char* api=nullptr; std::size_t api_size=0;
    const int has_api=update_check_json_value(manifest.data(),manifest.size(),"api",&api,&api_size);
    if(has_api<0) return fail_api();
    if(has_api==1) {
        unsigned major=0,minor=0,host_major=0,host_minor=0;
        const int has_minor=update_check_json_uint(api,api_size,"min_minor",&minor);
        if(update_check_json_uint(api,api_size,"major",&major)!=1 || has_minor<0 ||
           update_check_json_uint(descriptor.data(),descriptor.size(),"api_major",&host_major)!=1 ||
           update_check_json_uint(descriptor.data(),descriptor.size(),"api_minor",&host_minor)!=1 ||
           major!=host_major || minor>host_minor) return fail_api();
    }
    const char* required=nullptr; std::size_t required_size=0;
    const int has_required=update_check_json_value(manifest.data(),manifest.size(),"required_capabilities",&required,&required_size);
    if(has_required<0) return fail_api();
    if(has_required==1) {
        const char* available=nullptr; std::size_t available_size=0;
        const int has_available=update_check_json_value(descriptor.data(),descriptor.size(),"capabilities",&available,&available_size);
        for(std::size_t index=0;index<=64;++index) {
            char capability[128]{};
            const int found=update_check_json_array_string(required,required_size,index,capability,sizeof(capability));
            if(found==0) break;
            if(found<0 || index==64 || has_available!=1) return fail_api();
            bool supported=false;
            for(std::size_t item=0;item<=64;++item) {
                char candidate[128]{};
                const int present=update_check_json_array_string(available,available_size,item,candidate,sizeof(candidate));
                if(present==0) break;
                if(present<0 || item==64) return fail_api();
                if(std::string{candidate}==capability) supported=true;
            }
            if(!supported) return fail_api();
        }
    }
    const std::string supported=supports;
    for(std::size_t at=0;at<=supported.size();) {
        const auto end=supported.find(',',at);
        if(supported.substr(at,end==supported.npos?supported.size()-at:end-at)==contract) return true;
        if(end==supported.npos) break;at=end+1;
    }
    reason="The selected launcher needs a different runtime contract. Select the built-in launcher before updating.";return false;
}

bool swap_entries(const std::string &target, const std::string &staged, const std::string &backup)
{
    std::vector<std::string> present, new_names;
    if (!list_entries(target, kChildren, present) ||
        !list_entries(staged, kChildren, new_names) || new_names.empty() ||
        mkdir(backup.c_str(), 0755) != 0)
        return false;
    // ProsperoEden: only the entries the release replaces are moved aside (and deleted with the
    // work folder). Whatever else the folder holds is the player's (language.txt, the earlier
    // versions' assets/ with keys, firmware and games) and stays where it is.
    std::vector<std::string> old_names;
    for (const auto &name : present)
        if (std::find(new_names.begin(), new_names.end(), name) != new_names.end())
            old_names.push_back(name);
    std::size_t moved = 0, placed = 0;
    bool ok = true;
    for (; ok && moved < old_names.size(); ++moved)
        ok = rename((target + "/" + old_names[moved]).c_str(),
                    (backup + "/" + old_names[moved]).c_str()) == 0;
    if (!ok)
        --moved; // the one that failed was not moved
    for (; ok && placed < new_names.size(); ++placed)
        ok = rename((staged + "/" + new_names[placed]).c_str(),
                    (target + "/" + new_names[placed]).c_str()) == 0;
    if (!ok && placed)
        --placed;
    if (ok)
    {
        (void)sync_directory(target);
        return true;
    }
    // Back to how it was: the new entries return to staged, the old ones to target.
    for (std::size_t i = 0; i < placed; ++i)
        (void)rename((target + "/" + new_names[i]).c_str(), (staged + "/" + new_names[i]).c_str());
    for (std::size_t i = 0; i < moved; ++i)
        (void)rename((backup + "/" + old_names[i]).c_str(), (target + "/" + old_names[i]).c_str());
    (void)rmdir(backup.c_str());
    (void)sync_directory(target);
    return false;
}

namespace
{
// ProsperoEden: another app's release (Nightshade's Apps, self_update_protocol.h): put in a new
// folder beside the asking app's, or in place of its own folder's entries when it has one. The
// asking app stays open: the console runs one app at a time, so the title is not running and
// nothing waits for it to close. The app's own update (run) has more to check and is not this.
int install(const std::shared_ptr<Session> &shared, const std::string &title, const std::string &installed,
            const std::string &available, const std::string &size_text, const std::string &digest,
            std::string name, std::string label)
{
    Session &session = *shared;
    const Environment &environment = session.environment;
    std::string beside;
    if (!session.read_line(beside))
        return 1;
    // "none": not installed yet. "any": the release names no version; its param.json says it.
    const bool fresh = installed == "none", any = available == "any";
    char *end = nullptr;
    errno = 0;
    const std::uint64_t size = std::strtoull(size_text.c_str(), &end, 10);
    if (!title_id(title) || !title_id(beside) || title == beside || (!fresh && !content_version(installed)) ||
        (!any && !content_version(available)) || !hex_digest(digest) || size_text.empty() || errno ||
        *end != '\0' || size > SELF_UPDATE_MAX_ARCHIVE)
    {
        session.say("fail The request was refused\n");
        return 1;
    }
    name = shown(name, 64, title);
    label = shown(label, 40, title);
    const auto refuse = [&](const std::string &reason)
    {
        session.say("fail " + reason + "\n");
        return 1;
    };
    // Never the files of the app the console runs: the asking one (refused above, it is the one
    // that runs while it asks) or any other. Asked of the console, not read from /mnt/sandbox, which
    // keeps a closed app's sandbox until a restart (Swordpdf/ProsperoEden#232). When the console
    // can't say, nothing is installed.
    std::string running;
    if (!environment.running_app || !environment.running_app(running) || running.empty())
        return refuse("The app the console runs couldn't be told, so nothing was installed");
    session.log("install " + title + " while " + running + " runs");
    if (running == title)
        return refuse("The app is running; close it and try again");

    // Where it goes: its one folder, at the version that was asked about; a new one beside the
    // asking app's when it has none, where nothing may be yet.
    Copy copy;
    const int found = locate(session, title, copy);
    if (found == -1)
        return refuse("The app is installed as an image; update it by replacing the image");
    if (found == -2)
        return refuse("More than one copy of the app is installed");
    if (found == -3)
        return refuse("The installed apps couldn't all be looked at");
    std::string target;
    if (found == 1)
    {
        if (fresh)
            return refuse("The app is already installed in " + copy.path);
        if (copy.version != installed)
            return refuse("The installed app isn't the version that was asked about");
        target = copy.path;
    }
    else
    {
        if (!fresh)
            return refuse("The app's folder wasn't found");
        Copy asking;
        if (locate(session, beside, asking) != 1)
            return refuse("The folder of the app asking wasn't found");
        target = asking.path.substr(0, asking.path.rfind('/')) + "/" + title;
        if (kind(target) != Kind::absent)
            return refuse("A folder that isn't the app is in the way: " + target);
    }
    const std::string parent = target.substr(0, target.rfind('/'));
    const std::string drive = drive_root(environment.drives, target);
    if (drive.empty() || parent.empty())
        return refuse("The app can't be installed in " + target);

    const std::string base = drive + "/self-update", work = base + "/" + title;
    const std::string staged = work + "/staged";
    const auto clean = [&]
    {
        (void)remove_tree(work);
        (void)rmdir(base.c_str());
    };
    std::uint64_t free_bytes = 0;
    if (!remove_tree(work) || !make_directory(base) || !make_directory(work) ||
        !same_filesystem(fresh ? parent : target, work))
    {
        clean();
        return refuse("The install's work folder couldn't be made");
    }
    if (environment.space && environment.space(work, free_bytes) && free_bytes < size + kDownloadMargin)
    {
        clean();
        return refuse("There isn't enough free space for the download");
    }
    session.log("install " + title + " " + installed + " -> " + available + " in " + target + " (asked by " +
                beside + ")");
    if (!session.say("ready\n"))
    {
        clean();
        return 1;
    }

    ArchiveInfo info;
    if (stage_release(shared, title, size, digest, work, clean, info) != 0)
        return 1;
    const auto fail = [&](const std::string &reason)
    {
        clean();
        if (!session.cancelled.load())
            session.say("fail " + reason + "\n");
        return 1;
    };
    std::string staged_title, version;
    if (!read_param(staged, staged_title, version) || staged_title != title || !content_version(version) ||
        (!any && version != available))
        return fail("The download isn't the app and version the release names");
    session.say("p " + std::to_string(info.unpacked) + " " + std::to_string(info.unpacked) + "\n");
    if (!session.say("staged\n"))
    {
        clean();
        return 1;
    }
    while (!session.apply.load())
    {
        if (session.cancelled.load())
        {
            clean();
            return 1;
        }
        session.sleep(50);
    }
    session.say("applying\n");

    if (fresh)
    {
        if (kind(target) != Kind::absent || rename(staged.c_str(), target.c_str()) != 0)
            return fail("The app's folder couldn't be made: " + target);
        (void)sync_directory(parent);
    }
    else
    {
        if (!swap_entries(target, staged, work + "/backup"))
            return fail("The app's files couldn't be replaced; it is as it was");
        refresh_registered(environment, title, target);
    }
    session.log("installed " + title + " " + version + " in " + target);
    // Said as soon as the files are in place, with where they are when the line has room for it:
    // keeping the replaced version below can be a copy to another drive that outlasts the app's
    // wait for this line.
    const std::string done = "installed " + version + " " + target;
    session.say((done.size() < SELF_UPDATE_LINE ? done : "installed " + version) + "\n");
    if (environment.notify)
        environment.notify(fresh ? name + " " + label +
                                       " was installed. The homebrew loader lists it after its next scan or a restart."
                                 : name + " was updated to " + label + ".");
    if (!fresh && !environment.previous.empty())
    {
        const std::string kept = environment.previous + "/" + title + "-backup";
        if (make_directory(environment.previous) && remove_tree(kept) && move_tree(work + "/backup", kept))
            session.log("version " + installed + " is kept in " + kept);
        else
            session.log("version " + installed + " could not be kept in " + kept);
    }
    clean();
    return 0;
}
} // namespace

int run(const Environment &environment, const Io &io)
{
    const auto shared = std::make_shared<Session>(environment, io);
    Session &session = *shared;
    std::string magic, verb, title, installed, available, size_text, digest, name, label;
    if (!session.read_line(magic) || magic != SELF_UPDATE_MAGIC || !session.read_line(verb) ||
        !session.read_line(title) || !session.read_line(installed) ||
        !session.read_line(available) || !session.read_line(size_text) ||
        !session.read_line(digest) || !session.read_line(name) || !session.read_line(label))
        return 1;
    // ProsperoEden: another app's release, with one line more (install above).
    if (verb == "install")
        return install(shared, title, installed, available, size_text, digest, name, label);
    char *end = nullptr;
    errno = 0;
    const std::uint64_t size = std::strtoull(size_text.c_str(), &end, 10);
    if (verb != "update" || !title_id(title) || !content_version(installed) ||
        !content_version(available) || !newer(available, installed) || !hex_digest(digest) ||
        size_text.empty() || errno || *end != '\0' || size > SELF_UPDATE_MAX_ARCHIVE)
    {
        session.say("fail The request was refused\n");
        return 1;
    }
    name = shown(name, 64, title);
    label = shown(label, 40, available);

    // The one installed folder of the title, at the version that asked: the folder
    // ShadowMountPlus mounted it from, when it recorded one; otherwise the one copy under the roots.
    std::vector<Copy> copies;
    std::string source;
    const int mounted = mounted_source(environment, title, source);
    if (mounted < 0)
    {
        session.say("fail The app is installed as an image; update it by replacing the image\n");
        return 1;
    }
    if (mounted > 0)
    {
        std::string id, version;
        if (kind(source) == Kind::directory && read_param(source, id, version) && id == title)
        {
            copies.push_back({source, version});
            session.log("installed folder (ShadowMountPlus): " + source);
        }
    }
    if (copies.empty() && (!find_copies(environment, title, copies) || copies.empty()))
    {
        session.say("fail The app's folder wasn't found. Apps installed as an image can't "
                    "update themselves\n");
        return 1;
    }
    if (copies.size() != 1)
    {
        session.say("fail More than one copy of the app is installed\n");
        return 1;
    }
    const std::string target = copies[0].path;
    if (copies[0].version != installed)
    {
        session.say("fail The installed files aren't the version that is running\n");
        return 1;
    }
    const std::string drive = drive_root(environment.drives, target);
    if (drive.empty())
    {
        session.say("fail The app is installed where it can't be updated\n");
        return 1;
    }
    const std::string base = drive + "/self-update", work = base + "/" + title;
    const std::string staged = work + "/staged";
    const auto clean = [&]
    {
        (void)remove_tree(work);
        (void)rmdir(base.c_str());
    };
    std::uint64_t free_bytes = 0;
    if (!remove_tree(work) || !make_directory(base) || !make_directory(work) ||
        !same_filesystem(target, work))
    {
        clean();
        session.say("fail The update's work folder couldn't be made\n");
        return 1;
    }
    if (environment.space && environment.space(work, free_bytes) &&
        free_bytes < size + kDownloadMargin)
    {
        clean();
        session.say("fail There isn't enough free space for the download\n");
        return 1;
    }
    session.log("update " + title + " " + installed + " -> " + available + " in " + target);
    if (!session.say("ready\n"))
    {
        clean();
        return 1;
    }

    ArchiveInfo info;
    if (stage_release(shared, title, size, digest, work, clean, info) != 0)
        return 1;
    const auto fail = [&](const std::string &reason)
    {
        clean();
        if (!session.cancelled.load())
            session.say("fail " + reason + "\n");
        return 1;
    };

    std::string error, staged_title, staged_version;
    if (!read_param(staged, staged_title, staged_version) || staged_title != title ||
        staged_version != available)
        return fail("The download isn't the version the catalog lists");
    if(!launcher_update_compatible(environment.launcher_data,staged,error)) return fail(error);
    session.say("p " + std::to_string(info.unpacked) + " " + std::to_string(info.unpacked) + "\n");
    if (!session.say("staged\n"))
    {
        clean();
        return 1;
    }

    while (!session.apply.load())
    {
        if (session.cancelled.load())
        {
            clean();
            return 1;
        }
        session.sleep(50);
    }
    if(!launcher_update_compatible(environment.launcher_data,staged,error)) return fail(error);
    session.say("applying\n");

    // Only a closed app is touched. Unknown counts as running, until the app's connection has
    // ended: then its process is gone, and a sandbox that stays is the system's leftover
    // (Swordpdf/ProsperoEden#232, wait_for_close).
    const auto sandboxes = [&]
    {
        std::string left;
        for (const auto &entry : title_sandboxes(environment.sandboxes, title))
            left += (left.empty() ? "" : ", ") + entry;
        return left.empty() ? std::string{"(could not be listed)"} : left;
    };
    const CloseWait close = wait_for_close(
        environment, title, [&] { return session.closed.load(); }, [&] { return session.cancelled.load(); });
    if (close == CloseWait::cancelled)
    {
        clean();
        return 1;
    }
    if (close == CloseWait::timed_out)
    {
        clean();
        session.log("the app did not close; nothing was changed; in " + environment.sandboxes + ": " + sandboxes());
        if (environment.notify)
            environment.notify(name + " wasn't updated: it didn't close. Nothing was changed. "
                               "Restart the console and update again.");
        return 1;
    }
    if (close == CloseWait::left_behind)
        session.log("the app has closed (its connection ended); its sandbox stayed, which the system "
                    "leaves until a restart; in " + environment.sandboxes + ": " + sandboxes());
    session.sleep(environment.settle_ms);

    if (!swap_entries(target, staged, work + "/backup"))
    {
        clean();
        session.log("the files could not be replaced; the app is as it was");
        if (environment.notify)
            environment.notify(name + " wasn't updated: its files couldn't be replaced.");
        return 1;
    }
    refresh_registered(environment, title, target);
    // The files the update replaced are kept as the previous version, so that it can be put
    // back by hand if the new one misbehaves. Only the latest one is kept.
    if (!environment.previous.empty())
    {
        const std::string kept = environment.previous + "/" + title + "-backup";
        if (make_directory(environment.previous) && remove_tree(kept) &&
            move_tree(work + "/backup", kept))
            session.log("version " + installed + " is kept in " + kept);
        else
            session.log("version " + installed + " could not be kept in " + kept);
    }
    clean();
    session.log("updated " + title + " to " + available);
    if (environment.notify)
        environment.notify(name + " was updated to " + label + ". Open it again.");
    return 0;
}
} // namespace self_update
