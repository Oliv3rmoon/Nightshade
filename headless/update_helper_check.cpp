// SPDX-FileCopyrightText: Copyright 2026 Oliv3rmoon
// SPDX-License-Identifier: GPL-3.0-or-later
// Host check of the self-update helper's whole conversation (self_update_helper/updater.cpp, run):
// the app's own update as Nightshade has always asked for it, and another app's release installed
// beside it or over its own folder (Nightshade's Apps, "install"), with the app's side played by a
// stand-in that answers the helper as self_update.c does. Folders are made in the temporary folder.
//
// usage: eden-update-helper-check [--update-only]
//        --update-only runs the app's own update alone (it is what a helper from before "install"
//        can be checked with too).
#include "self_update_helper/updater.hpp"
#include "miniz/miniz.h"
#include "update_check/self_update_sha256.h"

#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unistd.h>
#include <utility>
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
        std::printf("FAIL update helper: %s\n", what.c_str());
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

std::string param(const std::string &title, const std::string &version)
{
    return "{\"titleId\": \"" + title + "\", \"contentVersion\": \"" + version + "\"}";
}

// A release ZIP as the apps' releases are made: <title>/ with eboot.bin and sce_sys/param.json.
std::string release_zip(const std::string &title, const std::string &version, const std::string &program)
{
    mz_zip_archive zip{};
    mz_zip_writer_init_heap(&zip, 0, 0);
    const std::vector<std::pair<std::string, std::string>> files = {
        {title + "/eboot.bin", program},
        {title + "/sce_sys/param.json", param(title, version)},
        {title + "/sce_module/libc.prx", "libc"}};
    for (const auto &[name, body] : files)
        mz_zip_writer_add_mem(&zip, name.c_str(), body.data(), body.size(), MZ_DEFAULT_COMPRESSION);
    void *buffer = nullptr;
    std::size_t size = 0;
    mz_zip_writer_finalize_heap_archive(&zip, &buffer, &size);
    std::string out(static_cast<const char *>(buffer), size);
    mz_free(buffer);
    mz_zip_writer_end(&zip);
    return out;
}

std::string sha256(const std::string &data)
{
    self_update_sha256 hash;
    unsigned char digest[32];
    char text[65];
    self_update_sha256_start(&hash);
    self_update_sha256_add(&hash, data.data(), data.size());
    self_update_sha256_finish(&hash, digest);
    self_update_sha256_hex(digest, text);
    return text;
}

// The app's side of the connection, as self_update.c speaks it: the request, the archive once the
// helper is ready, "apply" once it has staged; then it closes (its own update: the app ends) or
// waits for the install's last line (another app's).
struct App
{
    std::mutex lock;
    std::condition_variable wake;
    std::string inbox; // for the helper to read
    bool ended = false;
    std::string line;
    std::vector<std::string> heard;
    std::string archive;
    bool install = false;
    bool apply = true;

    void push(const std::string &bytes)
    {
        inbox += bytes;
        wake.notify_all();
    }
    void end()
    {
        ended = true;
        wake.notify_all();
    }
    void answer(const std::string &said)
    {
        heard.push_back(said);
        if (said == "ready")
        {
            for (std::size_t at = 0; at < archive.size(); at += 65536)
            {
                const std::size_t piece = std::min<std::size_t>(65536, archive.size() - at);
                const char header[4] = {static_cast<char>(piece), static_cast<char>(piece >> 8),
                                        static_cast<char>(piece >> 16), static_cast<char>(piece >> 24)};
                push(std::string(header, 4) + archive.substr(at, piece));
            }
            push(std::string(4, '\0'));
        }
        else if (said == "staged")
            push(apply ? "apply\n" : "cancel\n");
        else if ((said == "applying" && !install) || said.rfind("installed", 0) == 0 || said.rfind("fail ", 0) == 0)
            end();
    }
    long read(void *data, std::size_t size)
    {
        std::unique_lock guard(lock);
        wake.wait(guard, [&] { return !inbox.empty() || ended; });
        if (inbox.empty())
            return 0;
        const std::size_t count = std::min(size, inbox.size());
        std::memcpy(data, inbox.data(), count);
        inbox.erase(0, count);
        return static_cast<long>(count);
    }
    long write(const void *data, std::size_t size)
    {
        std::lock_guard guard(lock);
        for (std::size_t i = 0; i < size; ++i)
        {
            const char c = static_cast<const char *>(data)[i];
            if (c != '\n')
            {
                line.push_back(c);
                continue;
            }
            // Progress lines are many; the rest are what the check looks at.
            if (line.rfind("p ", 0) != 0)
                answer(line);
            line.clear();
        }
        return static_cast<long>(size);
    }
    bool said(const std::string &wanted)
    {
        std::lock_guard guard(lock);
        for (const std::string &one : heard)
            if (one == wanted)
                return true;
        return false;
    }
    std::string last()
    {
        std::lock_guard guard(lock);
        return heard.empty() ? std::string{} : heard.back();
    }
};

// The helper's listener can outlive its conversation for as long as its read takes to end: the
// app's side stays until the check ends.
std::vector<std::unique_ptr<App>> apps;
App &new_app()
{
    apps.push_back(std::make_unique<App>());
    return *apps.back();
}

// A console in the temporary folder: /data with homebrew/ and prosperoeden/, the sandbox folder
// and the console's own copies of apps' sce_sys (none).
struct Console
{
    fs::path base, data, homebrew;
    std::vector<std::string> notices, log;
    self_update::Environment environment;

    explicit Console(const std::string &name)
    {
        base = fs::temp_directory_path() / ("eden-update-helper-" + std::to_string(getpid()) + "-" + name);
        fs::remove_all(base);
        data = base / "data";
        homebrew = data / "homebrew";
        fs::create_directories(homebrew);
        fs::create_directories(data / "prosperoeden");
        fs::create_directories(base / "sandbox");
        fs::create_directories(base / "user");
        environment.roots = {homebrew.string()};
        environment.drives = {data.string()};
        environment.sandboxes = (base / "sandbox").string();
        environment.registered = (base / "user").string();
        environment.sleep = [](unsigned) { usleep(1000); };
        environment.notify = [this](const std::string &message) { notices.push_back(message); };
        environment.log = [this](const std::string &line) { log.push_back(line); };
        environment.previous = (data / "prosperoeden/previous-version").string();
        environment.launcher_data = (data / "prosperoeden").string();
        environment.settle_ms = 0;
        // Nightshade is the app the console runs while it asks.
        environment.running_app = [](std::string &title)
        {
            title = "PPSA99008";
            return true;
        };
    }
    ~Console()
    {
        std::error_code ignored;
        fs::remove_all(base, ignored);
    }
    void app(const std::string &title, const std::string &version, const fs::path &folder)
    {
        write(folder / "eboot.bin", "old program of " + title);
        write(folder / "sce_sys/param.json", param(title, version));
        write(folder / "language.txt", "de-DE");
    }
    // One conversation: the request lines, then the app's side as above. The helper's result.
    int talk(App &app, const std::vector<std::string> &request)
    {
        for (const std::string &line : request)
            app.push(line + "\n");
        const self_update::Io io{[&app](void *data, std::size_t size) { return app.read(data, size); },
                                 [&app](const void *data, std::size_t size) { return app.write(data, size); }};
        const int result = self_update::run(environment, io);
        // A failed helper leaves its listener reading; the app's side ends as the connection would.
        {
            std::lock_guard guard(app.lock);
            app.end();
        }
        return result;
    }
    bool no_work_left() const
    {
        return !fs::exists(data / "self-update");
    }
};

std::vector<std::string> install_request(const std::string &title, const std::string &installed,
                                         const std::string &available, const std::string &archive,
                                         const std::string &digest, const std::string &name)
{
    return {"PSU1", "install", title, installed, available, std::to_string(archive.size()), digest, name,
            available == "any" ? "1.5.9" : available, "PPSA99008"};
}

// Nightshade's own update, as every version has asked for it: staged, applied once the app is
// gone, the player's files kept, the version replaced kept.
void own_update()
{
    Console console("own");
    const fs::path folder = console.homebrew / "PPSA99008";
    console.app("PPSA99008", "01.005.100", folder);
    App &app = new_app();
    app.archive = release_zip("PPSA99008", "01.005.101", "new program");
    const int result = console.talk(app, {"PSU1", "update", "PPSA99008", "01.005.100", "01.005.101",
                                          std::to_string(app.archive.size()), sha256(app.archive), "Nightshade",
                                          "v1.005.101"});
    expect(result == 0, "the app's own update ends well");
    expect(app.said("ready") && app.said("staged") && app.said("applying"), "the app's own update: ready, staged, applying");
    expect(read(folder / "eboot.bin") == "new program", "the app's own update replaces its program");
    expect(read(folder / "sce_sys/param.json") == param("PPSA99008", "01.005.101"), "the app's own update: its new param.json");
    expect(read(folder / "language.txt") == "de-DE", "the app's own update keeps the player's files");
    expect(read(console.data / "prosperoeden/previous-version/PPSA99008-backup/eboot.bin") == "old program of PPSA99008",
           "the app's own update keeps the version it replaced");
    expect(console.notices.size() == 1 && console.notices[0] == "Nightshade was updated to v1.005.101. Open it again.",
           "the app's own update: its notification");
    expect(console.no_work_left(), "the app's own update leaves no work folder");

    // A wrong digest: nothing changes.
    Console wrong("own-wrong");
    wrong.app("PPSA99008", "01.005.100", wrong.homebrew / "PPSA99008");
    App &refused = new_app();
    refused.archive = app.archive;
    const int failed = wrong.talk(refused, {"PSU1", "update", "PPSA99008", "01.005.100", "01.005.101",
                                            std::to_string(refused.archive.size()), std::string(64, 'a'),
                                            "Nightshade", "v1.005.101"});
    expect(failed == 1 && refused.last() == "fail The download doesn't match the catalog's listing",
           "the app's own update refuses a download that isn't the one listed");
    expect(read(wrong.homebrew / "PPSA99008/eboot.bin") == "old program of PPSA99008" && wrong.no_work_left(),
           "the app's own update changes nothing then");
}

// Another app: installed beside Nightshade, updated in its own folder, refused when the download
// is not the one announced or the folder is not as asked.
void other_apps()
{
    {
        Console console("fresh");
        console.app("PPSA99008", "01.010.000", console.homebrew / "PPSA99008");
        App &app = new_app();
        app.install = true;
        app.archive = release_zip("PPSA99003", "01.000.090", "tv program");
        const int result = console.talk(app, install_request("PPSA99003", "none", "01.000.090", app.archive,
                                                             sha256(app.archive), "Physalis"));
        const fs::path folder = console.homebrew / "PPSA99003";
        expect(result == 0, "a new app is installed");
        expect(app.last() == "installed 01.000.090 " + folder.string(), "a new app: the helper says where");
        expect(read(folder / "eboot.bin") == "tv program" &&
                   read(folder / "sce_sys/param.json") == param("PPSA99003", "01.000.090"),
               "a new app's folder is beside Nightshade's");
        expect((fs::status(folder / "eboot.bin").permissions() & fs::perms::all) == fs::perms::all,
               "a new app's files can be run by everyone");
        expect(read(console.homebrew / "PPSA99008/eboot.bin") == "old program of PPSA99008",
               "a new app leaves Nightshade as it was");
        expect(console.notices.size() == 1 &&
                   console.notices[0] == "Physalis 01.000.090 was installed. The homebrew loader lists it "
                                         "after its next scan or a restart.",
               "a new app: its notification");
        expect(console.no_work_left(), "a new app leaves no work folder");

        // Again with "none": it is there now, and is not installed over.
        App &again = new_app();
        again.install = true;
        again.archive = app.archive;
        expect(console.talk(again, install_request("PPSA99003", "none", "01.000.090", again.archive,
                                                   sha256(again.archive), "Physalis")) == 1 &&
                   again.last() == "fail The app is already installed in " + folder.string(),
               "an app already installed is not installed again as a new one");
    }
    {
        Console console("update");
        console.app("PPSA99008", "01.010.000", console.homebrew / "PPSA99008");
        const fs::path folder = console.homebrew / "PPSA99002";
        console.app("PPSA99002", "01.000.100", folder);
        App &app = new_app();
        app.install = true;
        app.archive = release_zip("PPSA99002", "01.000.110", "stream program");
        const int result = console.talk(app, install_request("PPSA99002", "01.000.100", "01.000.110", app.archive,
                                                             sha256(app.archive), "Datura"));
        expect(result == 0 && app.last() == "installed 01.000.110 " + folder.string(), "an installed app is updated");
        expect(read(folder / "eboot.bin") == "stream program" && read(folder / "language.txt") == "de-DE",
               "an updated app: its files replaced, the player's kept");
        expect(read(console.data / "prosperoeden/previous-version/PPSA99002-backup/eboot.bin") ==
                   "old program of PPSA99002",
               "an updated app: the version it replaced is kept");
        expect(console.notices.size() == 1 && console.notices[0] == "Datura was updated to 01.000.110.",
               "an updated app: its notification");

        // A release that names no version ("1.5.9"): the app's own param.json says it.
        App &any = new_app();
        any.install = true;
        any.archive = release_zip("PPSA99002", "01.000.111", "stream program 2");
        expect(console.talk(any, install_request("PPSA99002", "01.000.110", "any", any.archive, sha256(any.archive),
                                                 "Datura")) == 0 &&
                   any.last() == "installed 01.000.111 " + folder.string(),
               "a release that names no version installs the version its ZIP holds");

        // The installed files are not the version asked about.
        App &stale = new_app();
        stale.install = true;
        stale.archive = release_zip("PPSA99002", "01.000.120", "stream program 3");
        expect(console.talk(stale, install_request("PPSA99002", "01.000.100", "01.000.120", stale.archive,
                                                   sha256(stale.archive), "Datura")) == 1 &&
                   stale.last() == "fail The installed app isn't the version that was asked about" &&
                   read(folder / "eboot.bin") == "stream program 2",
               "an app whose installed version changed is not updated");
    }
    {
        Console console("wrong");
        console.app("PPSA99008", "01.010.000", console.homebrew / "PPSA99008");
        App &app = new_app();
        app.install = true;
        app.archive = release_zip("PPSA98510", "01.005.009", "station program");
        const int result = console.talk(app, install_request("PPSA98510", "none", "any", app.archive,
                                                             std::string(64, '0'), "Mandrake"));
        expect(result == 1 && app.last() == "fail The download doesn't match the catalog's listing",
               "a download whose SHA-256 is not the one announced is refused");
        expect(!fs::exists(console.homebrew / "PPSA98510") && console.no_work_left() && console.notices.empty(),
               "a refused download leaves nothing behind");

        // A ZIP of another title than the one asked for.
        App &other = new_app();
        other.install = true;
        other.archive = release_zip("PPSA99003", "01.000.090", "tv program");
        expect(console.talk(other, install_request("PPSA98510", "none", "any", other.archive, sha256(other.archive),
                                                   "Mandrake")) == 1 &&
                   !fs::exists(console.homebrew / "PPSA98510"),
               "a ZIP of another title is not installed");

        // A folder of that name in the way, which is not the app.
        write(console.homebrew / "PPSA98510/notes.txt", "mine");
        App &blocked = new_app();
        blocked.install = true;
        blocked.archive = app.archive;
        expect(console.talk(blocked, install_request("PPSA98510", "none", "any", blocked.archive,
                                                     sha256(blocked.archive), "Mandrake")) == 1 &&
                   blocked.last() ==
                       "fail A folder that isn't the app is in the way: " + (console.homebrew / "PPSA98510").string() &&
                   read(console.homebrew / "PPSA98510/notes.txt") == "mine",
               "a folder in the way is left as it is");

        // Cancelled once staged: nothing is installed.
        fs::remove_all(console.homebrew / "PPSA98510");
        App &cancelled = new_app();
        cancelled.install = true;
        cancelled.apply = false;
        cancelled.archive = app.archive;
        expect(console.talk(cancelled, install_request("PPSA98510", "none", "any", cancelled.archive,
                                                       sha256(cancelled.archive), "Mandrake")) == 1 &&
                   !fs::exists(console.homebrew / "PPSA98510") && console.no_work_left(),
               "a cancelled install leaves nothing behind");

        // A ZIP whose param.json names no content version, from a release that names none.
        fs::remove_all(console.homebrew / "PPSA98510");
        App &oddly = new_app();
        oddly.install = true;
        oddly.archive = release_zip("PPSA98510", "1.5.9", "station program");
        expect(console.talk(oddly, install_request("PPSA98510", "none", "any", oddly.archive, sha256(oddly.archive),
                                                   "Mandrake")) == 1 &&
                   oddly.last() == "fail The download isn't the app and version the release names" &&
                   !fs::exists(console.homebrew / "PPSA98510"),
               "a ZIP whose version isn't a content version is not installed");

        // The app the console runs is never installed over; nor anything when the console can't
        // say which runs.
        const auto refused = [&](const std::function<bool(std::string &)> &answer, const std::string &why,
                                 const std::string &what)
        {
            console.environment.running_app = answer;
            App &asked = new_app();
            asked.install = true;
            asked.archive = app.archive;
            expect(console.talk(asked, install_request("PPSA98510", "none", "any", asked.archive,
                                                       sha256(asked.archive), "Mandrake")) == 1 &&
                       asked.last() == "fail " + why && !fs::exists(console.homebrew / "PPSA98510") &&
                       console.no_work_left(),
                   what);
        };
        refused([](std::string &title) { title = "PPSA98510"; return true; },
                "The app is running; close it and try again", "the app the console runs is not installed over");
        refused([](std::string &) { return false; },
                "The app the console runs couldn't be told, so nothing was installed",
                "nothing is installed when the console can't say which app runs");
        refused(nullptr, "The app the console runs couldn't be told, so nothing was installed",
                "nothing is installed with no way to ask which app runs");
        console.environment.running_app = [](std::string &title)
        {
            title = "PPSA99008";
            return true;
        };
        // A closed app's sandbox the system kept until a restart (Swordpdf/ProsperoEden#232) is no
        // reason to refuse.
        fs::create_directories(console.base / "sandbox/PPSA98510_000");
        App &leftover = new_app();
        leftover.install = true;
        leftover.archive = app.archive;
        expect(console.talk(leftover, install_request("PPSA98510", "none", "any", leftover.archive,
                                                      sha256(leftover.archive), "Mandrake")) == 0 &&
                   read(console.homebrew / "PPSA98510/eboot.bin") == "station program",
               "an app closed with its sandbox left behind is installed");
        fs::remove_all(console.base / "sandbox/PPSA98510_000");
        fs::remove_all(console.homebrew / "PPSA98510");

        // Nightshade's own title is never installed this way (its own update checks more).
        App &own = new_app();
        own.install = true;
        own.archive = release_zip("PPSA99008", "01.010.001", "new program");
        expect(console.talk(own, install_request("PPSA99008", "01.010.000", "01.010.001", own.archive,
                                                 sha256(own.archive), "Nightshade")) == 1 &&
                   own.last() == "fail The request was refused",
               "the asking app's own title is refused");
    }
}
} // namespace

int main(int argc, char **argv)
{
    const bool update_only = argc == 2 && std::strcmp(argv[1], "--update-only") == 0;
    own_update();
    if (!update_only)
        other_apps();
    std::printf("%s update helper: %d checks of the app's own update%s, %d failed\n", failures ? "FAIL" : "PASS",
                passed + failures, update_only ? "" : " and of installs of other apps", failures);
    return failures ? 1 : 0;
}
