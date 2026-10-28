// SPDX-FileCopyrightText: Copyright 2026 Oliv3rmoon
// SPDX-License-Identifier: GPL-3.0-or-later
// Host check of Nightshade's Apps (apps_release.h): an app's release found on GitHub (its own
// repository, or the project it is based on while that has none), its SHA-256 from SHA256SUMS or
// GitHub's digest, and the install through the self-update kit's job (self_update.c) talking to
// the self-update helper itself (self_update_helper/updater.cpp) over a socket pair, in a console
// made in the temporary folder: a good ZIP, a wrong hash, a missing hash, a missing release. Then
// the originals' own releases/latest answers (fixtures/apps, as GitHub gave them), where an app is
// found, what a release offers, and the launch payload's conversation.
//
// usage: eden-apps-check <fixtures/apps> [<folder with PPSA99003.zip, PPSA99002.zip, PPSA98510.zip>]
//        With the originals' ZIPs (downloaded from their releases), each is installed as well.
#include "apps_release.h"
#include "miniz/miniz.h"
#include "self_update_helper/updater.hpp"
#include "update_check/self_update_sha256.h"

#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <sys/socket.h>
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
        std::printf("FAIL apps: %s\n", what.c_str());
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

std::string release_zip(const std::string &title, const std::string &version, const std::string &program)
{
    mz_zip_archive zip{};
    mz_zip_writer_init_heap(&zip, 0, 0);
    const std::string metadata = param(title, version);
    mz_zip_writer_add_mem(&zip, (title + "/eboot.bin").c_str(), program.data(), program.size(), MZ_DEFAULT_COMPRESSION);
    mz_zip_writer_add_mem(&zip, (title + "/sce_sys/param.json").c_str(), metadata.data(), metadata.size(),
                          MZ_DEFAULT_COMPRESSION);
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

// releases/latest as GitHub answers it, with the files given: name, size, digest ("" for null).
struct File
{
    std::string name;
    std::size_t size;
    std::string digest;
};
// `repository`: where GitHub says the release is ("url"), as after a renamed repository's redirect.
std::string latest(const std::string &tag, const std::vector<File> &files, const std::string &repository = "")
{
    std::string assets;
    for (const File &file : files)
        assets += std::string(assets.empty() ? "" : ",") + "{\"url\":\"https://api.github.com/x\",\"name\":\"" +
                  file.name + "\",\"label\":null,\"uploader\":{\"login\":\"someone\",\"name\":\"not this\"}," +
                  "\"state\":\"uploaded\",\"size\":" + std::to_string(file.size) + ",\"digest\":" +
                  (file.digest.empty() ? std::string{"null"} : "\"sha256:" + file.digest + "\"") + "}";
    const std::string url =
        repository.empty() ? "" : "\"url\":\"https://api.github.com/repos/" + repository + "/releases/1\",";
    return "{" + url + "\"tag_name\":\"" + tag + "\",\"name\":\"" + tag + "\",\"draft\":false,\"prerelease\":false,"
           "\"assets\":[" + assets + "],\"body\":\"Notes\"}";
}

// A console in the temporary folder, as update_helper_check.cpp makes one.
struct Console
{
    fs::path base, data, homebrew, user;
    self_update::Environment environment;
    std::vector<std::string> notices;
    explicit Console(const std::string &name)
    {
        base = fs::temp_directory_path() / ("eden-apps-" + std::to_string(getpid()) + "-" + name);
        fs::remove_all(base);
        data = base / "data";
        homebrew = data / "homebrew";
        user = base / "user";
        fs::create_directories(homebrew);
        fs::create_directories(data / "prosperoeden");
        fs::create_directories(base / "sandbox");
        fs::create_directories(user);
        write(homebrew / "PPSA99008/eboot.bin", "nightshade");
        write(homebrew / "PPSA99008/sce_sys/param.json", param("PPSA99008", "01.010.000"));
        environment.roots = {homebrew.string()};
        environment.drives = {data.string()};
        environment.sandboxes = (base / "sandbox").string();
        environment.registered = user.string();
        environment.sleep = [](unsigned) { usleep(1000); };
        environment.notify = [this](const std::string &message) { notices.push_back(message); };
        environment.previous = (data / "prosperoeden/previous-version").string();
        environment.launcher_data = (data / "prosperoeden").string();
        environment.settle_ms = 0;
    }
    ~Console()
    {
        std::error_code ignored;
        fs::remove_all(base, ignored);
    }
};

// GitHub and the payload loader as the kit's platform sees them: answers by address, files by
// address, and a loader that runs the helper on the other end of a socket pair.
struct Network
{
    std::map<std::string, std::pair<int, std::string>> api;
    std::map<std::string, std::string> files;
    std::vector<std::string> asked;
    Console *console = nullptr;
    std::vector<std::thread> helpers;
    // The helpers' conversations have ended (a helper cleans up once the app has hung up).
    void settle()
    {
        for (std::thread &helper : helpers)
            helper.join();
        helpers.clear();
    }
    ~Network()
    {
        settle();
    }
};

int fetch(void *user, const char *url, char *body, std::size_t capacity, std::size_t *length, int *status)
{
    Network &network = *static_cast<Network *>(user);
    network.asked.push_back(url);
    const auto found = network.api.find(url);
    *length = 0;
    *status = 0;
    if (found == network.api.end())
        return -(10000 + 6); // as a name that can't be looked up
    *status = found->second.first;
    if (found->second.second.size() > capacity)
        return SELF_UPDATE_FETCH_TOO_LARGE;
    std::memcpy(body, found->second.second.data(), found->second.second.size());
    *length = found->second.second.size();
    return 0;
}

int download(void *user, const char *url, std::uint64_t limit, self_update_sink sink, void *sink_user)
{
    Network &network = *static_cast<Network *>(user);
    network.asked.push_back(url);
    const auto found = network.files.find(url);
    if (found == network.files.end())
        return -404;
    if (found->second.size() > limit)
        return -1;
    for (std::size_t at = 0; at < found->second.size(); at += 65536)
        if (sink(sink_user, found->second.data() + at, std::min<std::size_t>(65536, found->second.size() - at)) != 1)
            return -1;
    return 0;
}

int verify(void *, const unsigned char *, const unsigned char *, const void *, std::size_t)
{
    return 0;
}

long channel_send(void *user, const void *data, std::size_t size)
{
    return static_cast<long>(::write(static_cast<int>(reinterpret_cast<std::intptr_t>(user)), data, size));
}
long channel_receive(void *user, void *data, std::size_t size)
{
    return static_cast<long>(::read(static_cast<int>(reinterpret_cast<std::intptr_t>(user)), data, size));
}
void channel_close(void *user)
{
    ::close(static_cast<int>(reinterpret_cast<std::intptr_t>(user)));
}

int open_helper(void *user, self_update_channel *channel)
{
    Network &network = *static_cast<Network *>(user);
    int ends[2];
    if (network.console == nullptr || socketpair(AF_UNIX, SOCK_STREAM, 0, ends) != 0)
        return 0;
    const int helper = ends[1];
    const self_update::Environment environment = network.console->environment;
    // The helper's end stays open: its listener may still be reading it when run() returns.
    network.helpers.emplace_back(
        [environment, helper]
        {
            const self_update::Io io{[helper](void *data, std::size_t size)
                                     { return static_cast<long>(::read(helper, data, size)); },
                                     [helper](const void *data, std::size_t size)
                                     { return static_cast<long>(::write(helper, data, size)); }};
            (void)self_update::run(environment, io);
        });
    channel->send = channel_send;
    channel->receive = channel_receive;
    channel->close = channel_close;
    channel->user = reinterpret_cast<void *>(static_cast<std::intptr_t>(ends[0]));
    return 1;
}

std::uint64_t now_ms(void *)
{
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

self_update_platform platform_of(Network &network)
{
    return {fetch, download, verify, open_helper, nullptr, nullptr, now_ms, &network};
}

// The kit's job from start to end, as the launcher follows it.
self_update_status install(const self_update_platform &platform, const self_update_offer &offer)
{
    static self_update_job job;
    self_update_status status{};
    std::memset(&job, 0, sizeof(job));
    if (self_update_start(&job, &platform, &offer) != 1)
        return status;
    for (int wait = 0; wait < 60000; ++wait)
    {
        self_update_poll(&job, &status);
        if (status.phase == SELF_UPDATE_INSTALLED || status.phase == SELF_UPDATE_FAILED ||
            status.phase == SELF_UPDATE_CANCELLED)
            break;
        usleep(1000);
    }
    self_update_finish(&job);
    return status;
}

const pe::ui::AppListing &app(const char *title)
{
    for (const pe::ui::AppListing &listing : pe::ui::kApps)
        if (std::strcmp(listing.title, title) == 0)
            return listing;
    std::abort();
}

const std::string kApi = "https://api.github.com/repos/";
std::string latest_of(const std::string &repository)
{
    return kApi + repository + "/releases/latest";
}
std::string file_of(const std::string &repository, const std::string &tag, const std::string &file)
{
    return "https://github.com/" + repository + "/releases/download/" + tag + "/" + file;
}

// ---- downloads, checks and installs -------------------------------------------------------------

void good_zip()
{
    Console console("good");
    Network network;
    network.console = &console;
    const pe::ui::AppListing &tv = app("PPSA99003");
    const std::string zip = release_zip("PPSA99003", "01.000.090", "tv");
    // Nightshade TV has no release yet (404); ProsperoTV's lists PPSA99003.zip with SHA256SUMS and
    // a digest that agree.
    network.api[latest_of(tv.repository)] = {404, "{\"message\":\"Not Found\"}"};
    network.api[latest_of(tv.fallback)] = {
        200, latest("01.000.090", {{"PPSA99003.zip", zip.size(), sha256(zip)}, {"SHA256SUMS", 80, ""}})};
    network.files[file_of(tv.fallback, "01.000.090", "SHA256SUMS")] = sha256(zip) + "  PPSA99003.zip\n";
    network.files[file_of(tv.fallback, "01.000.090", "PPSA99003.zip")] = zip;
    const self_update_platform platform = platform_of(network);

    const Eden::Apps::Release release = Eden::Apps::Check(&platform, tv);
    expect(release.reason == GITHUB_RELEASE_NEWER && release.fallback && release.repository == tv.fallback,
           "with no release of its own, the project it is based on is offered");
    expect(release.report.redirects == 0 && std::string{release.asset.tag} == "01.000.090" &&
               release.asset.from_sums == 1 && release.asset.sha256 == sha256(zip) && release.asset.size == zip.size(),
           "the original project's release, with its SHA256SUMS");
    expect(std::string{release.asset.url} == file_of(tv.fallback, "01.000.090", "PPSA99003.zip"),
           "the download's address is made from the repository, the tag and the file");

    const Eden::Apps::Installed none = Eden::Apps::Find("PPSA99003", console.user.string(),
                                                        (console.homebrew / "PPSA99008").string(),
                                                        console.homebrew.string());
    expect(none.folder.empty() && Eden::Apps::Decide(none, true, "01.000.090") == Eden::Apps::Offer::get,
           "an app not installed is offered");
    self_update_offer offer{};
    expect(Eden::Apps::MakeOffer(tv, release, none, "PPSA99008", &offer) &&
               std::string{offer.installed} == "none" && std::string{offer.available} == "01.000.090" &&
               std::string{offer.beside} == "PPSA99008",
           "a new app's offer");
    const self_update_status status = install(platform, offer);
    network.settle();
    const fs::path folder = console.homebrew / "PPSA99003";
    expect(status.phase == SELF_UPDATE_INSTALLED && std::string{status.version} == "01.000.090" &&
               status.folder == folder.string(),
           "a good ZIP is installed (" + std::string{status.error} + ")");
    expect(read(folder / "eboot.bin") == "tv" && !fs::exists(console.data / "self-update"),
           "a good ZIP's app is beside Nightshade, and no work folder is left");
    const Eden::Apps::Installed found = Eden::Apps::Find("PPSA99003", console.user.string(),
                                                         (console.homebrew / "PPSA99008").string(),
                                                         console.homebrew.string());
    expect(found.folder == folder.string() && found.version == "01.000.090" &&
               Eden::Apps::Decide(found, true, "01.000.090") == Eden::Apps::Offer::current,
           "the installed app is found, and its release is not offered again");

    // Its update: a newer release, installed over its folder.
    const std::string newer = release_zip("PPSA99003", "01.000.100", "tv 2");
    network.api[latest_of(tv.fallback)] = {
        200, latest("01.000.100", {{"PPSA99003.zip", newer.size(), sha256(newer)}, {"SHA256SUMS", 80, ""}})};
    network.files[file_of(tv.fallback, "01.000.100", "SHA256SUMS")] = sha256(newer) + " *PPSA99003.zip\n";
    network.files[file_of(tv.fallback, "01.000.100", "PPSA99003.zip")] = newer;
    const Eden::Apps::Release update = Eden::Apps::Check(&platform, tv);
    expect(Eden::Apps::Decide(found, update.reason == GITHUB_RELEASE_NEWER, update.asset.tag) ==
               Eden::Apps::Offer::update,
           "a newer release is offered as an update");
    expect(Eden::Apps::MakeOffer(tv, update, found, "PPSA99008", &offer) &&
               std::string{offer.installed} == "01.000.090",
           "an update's offer names the version installed");
    const self_update_status updated = install(platform, offer);
    expect(updated.phase == SELF_UPDATE_INSTALLED && read(folder / "eboot.bin") == "tv 2" &&
               read(console.data / "prosperoeden/previous-version/PPSA99003-backup/eboot.bin") == "tv",
           "an update is put in its folder, the version it replaced kept");
}

void wrong_hash()
{
    Console console("wrong");
    Network network;
    network.console = &console;
    const pe::ui::AppListing &stream = app("PPSA99002");
    const std::string zip = release_zip("PPSA99002", "01.000.110", "stream");
    const std::string other = std::string(64, 'b');
    // The release says another SHA-256 than its ZIP has (both its SHA256SUMS and its digest).
    network.api[latest_of(stream.repository)] = {
        200, latest("v1.000.110", {{"PPSA99002.zip", zip.size(), other}, {"SHA256SUMS", 80, ""}})};
    network.files[file_of(stream.repository, "v1.000.110", "SHA256SUMS")] = other + "  PPSA99002.zip\n";
    network.files[file_of(stream.repository, "v1.000.110", "PPSA99002.zip")] = zip;
    const self_update_platform platform = platform_of(network);
    const Eden::Apps::Release release = Eden::Apps::Check(&platform, stream);
    expect(release.reason == GITHUB_RELEASE_NEWER && !release.fallback && release.asset.sha256 == other,
           "its own repository's release is offered");
    self_update_offer offer{};
    expect(Eden::Apps::MakeOffer(stream, release, {}, "PPSA99008", &offer) &&
               std::string{offer.available} == "01.000.110",
           "a tag as make-dist.py writes it names its version");
    const self_update_status status = install(platform, offer);
    network.settle();
    expect(status.phase == SELF_UPDATE_FAILED &&
               std::string{status.error} == "The download doesn't match the catalog's listing",
           "a ZIP whose SHA-256 is not the release's is refused (" + std::string{status.error} + ")");
    expect(!fs::exists(console.homebrew / "PPSA99002") && !fs::exists(console.data / "self-update"),
           "a refused ZIP leaves nothing behind");

    // SHA256SUMS and GitHub's digest disagree: not offered.
    network.api[latest_of(stream.repository)] = {
        200, latest("v1.000.110", {{"PPSA99002.zip", zip.size(), sha256(zip)}, {"SHA256SUMS", 80, ""}})};
    expect(Eden::Apps::Check(&platform, stream).reason == GITHUB_RELEASE_HASHES_DIFFER,
           "SHA256SUMS and the digest differ: nothing is offered");
    // SHA256SUMS that names other files only.
    network.files[file_of(stream.repository, "v1.000.110", "SHA256SUMS")] = sha256(zip) + "  PPSA99002.zip.sig\n";
    expect(Eden::Apps::Check(&platform, stream).reason == GITHUB_RELEASE_SUMS_NO_LINE,
           "SHA256SUMS without a line for the ZIP: nothing is offered");
}

void missing_hash()
{
    Console console("unhashed");
    Network network;
    network.console = &console;
    const pe::ui::AppListing &station = app("PPSA98510");
    const std::string zip = release_zip("PPSA98510", "01.005.009", "station");
    network.api[latest_of(station.repository)] = {404, "{}"};
    // No SHA256SUMS and no digest (a file uploaded before GitHub kept them).
    network.api[latest_of(station.fallback)] = {200, latest("1.5.9", {{"PPSA98510.zip", zip.size(), ""}})};
    network.files[file_of(station.fallback, "1.5.9", "PPSA98510.zip")] = zip;
    const self_update_platform platform = platform_of(network);
    const Eden::Apps::Release release = Eden::Apps::Check(&platform, station);
    self_update_offer offer{};
    expect(release.reason == GITHUB_RELEASE_NO_HASH && !Eden::Apps::MakeOffer(station, release, {}, "PPSA99008", &offer),
           "a release with neither SHA256SUMS nor a digest is not offered");
    expect(Eden::Apps::Decide({}, release.reason == GITHUB_RELEASE_NEWER, release.asset.tag) == Eden::Apps::Offer::none,
           "nothing is offered for it");

    // With GitHub's digest alone it is, at the version its ZIP holds (the tag 1.5.9 is a likely one).
    network.api[latest_of(station.fallback)] = {200, latest("1.5.9", {{"PPSA98510.zip", zip.size(), sha256(zip)}})};
    const Eden::Apps::Release digest = Eden::Apps::Check(&platform, station);
    expect(digest.reason == GITHUB_RELEASE_NEWER && digest.asset.from_sums == 0 && digest.asset.sha256 == sha256(zip),
           "GitHub's digest stands in for a missing SHA256SUMS");
    expect(Eden::Apps::MakeOffer(station, digest, {}, "PPSA99008", &offer) && std::string{offer.available} == "any",
           "a likely version is not one the helper is held to");
    const self_update_status status = install(platform, offer);
    expect(status.phase == SELF_UPDATE_INSTALLED && std::string{status.version} == "01.005.009" &&
               read(console.homebrew / "PPSA98510/eboot.bin") == "station",
           "a ZIP checked against GitHub's digest is installed at its own version");
}

void missing_release()
{
    Network network;
    const pe::ui::AppListing &tv = app("PPSA99003");
    network.api[latest_of(tv.repository)] = {404, "{}"};
    network.api[latest_of(tv.fallback)] = {404, "{}"};
    const self_update_platform platform = platform_of(network);
    const Eden::Apps::Release release = Eden::Apps::Check(&platform, tv);
    self_update_offer offer{};
    expect(release.reason == GITHUB_RELEASE_NONE && release.fallback &&
               network.asked == std::vector<std::string>{latest_of(tv.repository), latest_of(tv.fallback)},
           "with no release anywhere both repositories are asked, nothing more");
    expect(!Eden::Apps::MakeOffer(tv, release, {}, "PPSA99008", &offer), "a missing release is nothing to install");

    // A release of its own without the ZIP: the project it is based on is not asked instead.
    network.asked.clear();
    network.api[latest_of(tv.repository)] = {200, latest("v1.000.100", {{"PPSA99003.exfat", 100, ""}})};
    expect(Eden::Apps::Check(&platform, tv).reason == GITHUB_RELEASE_NO_ZIP && network.asked.size() == 1,
           "a release of its own without the ZIP is not offered, and nothing else is asked");
    network.api[latest_of(tv.repository)] = {403, "{}"};
    expect(Eden::Apps::Check(&platform, tv).reason == GITHUB_RELEASE_RATE_LIMITED, "GitHub's rate limit is told");
    network.api.erase(latest_of(tv.repository));
    expect(Eden::Apps::Check(&platform, tv).reason == GITHUB_RELEASE_NETWORK, "no answer is told");
    network.api[latest_of(tv.repository)] = {200, latest("release/2", {{"PPSA99003.zip", 100, std::string(64, 'a')}})};
    expect(Eden::Apps::Check(&platform, tv).reason == GITHUB_RELEASE_BAD_TAG,
           "a tag that can't be in an address is refused");
}

// A renamed repository: followed to its new name, never to another account; and SHA256SUMS that
// can't be downloaded, or can't be one.
void redirects(const fs::path &fixtures)
{
    Network network;
    const pe::ui::AppListing &tv = app("PPSA99003");
    const std::string moved = "https://api.github.com/repositories/42/releases/latest";
    const std::string zip = release_zip("PPSA99003", "01.001.000", "tv");
    network.api[latest_of(tv.repository)] = {301, "{\"message\":\"Moved Permanently\",\"url\":\"" + moved + "\"}"};
    network.api[moved] = {200, latest("01.001.000", {{"PPSA99003.zip", zip.size(), sha256(zip)}},
                                      "oliv3rmoon/Nightshade-Television")};
    const self_update_platform platform = platform_of(network);
    const Eden::Apps::Release renamed = Eden::Apps::Check(&platform, tv);
    expect(renamed.reason == GITHUB_RELEASE_NEWER && !renamed.fallback && renamed.report.redirects == 1,
           "a repository renamed by its owner is followed");
    network.api[moved] = {200, latest("01.001.000", {{"PPSA99003.zip", zip.size(), sha256(zip)}},
                                      "someone-else/Nightshade-TV")};
    network.asked.clear();
    const Eden::Apps::Release elsewhere = Eden::Apps::Check(&platform, tv);
    self_update_offer offer{};
    expect(elsewhere.reason == GITHUB_RELEASE_OWNER_CHANGED && !elsewhere.fallback &&
               !Eden::Apps::MakeOffer(tv, elsewhere, {}, "PPSA99008", &offer) &&
               network.asked == std::vector<std::string>{latest_of(tv.repository), moved},
           "a redirect to another account is refused, and nothing more is asked");
    network.api[moved] = {200, latest("01.001.000", {{"PPSA99003.zip", zip.size(), sha256(zip)}})};
    expect(Eden::Apps::Check(&platform, tv).reason == GITHUB_RELEASE_OWNER_CHANGED,
           "a redirect whose answer doesn't say where the release is is refused");

    // As GitHub answers today for a list that names blackbearreloaded/ProsperoTV, which moved to
    // ThatGuyKalvin's account.
    const pe::ui::AppListing old{"Nightshade TV", "PPSA99003", "Oliv3rmoon/Nightshade-TV", "blackbearreloaded/ProsperoTV",
                                 "PPSA99003.zip", "", pe::ui::AppIcon::tv};
    Network github;
    github.api[latest_of(old.repository)] = {404, "{\"message\":\"Not Found\"}"};
    github.api[latest_of(old.fallback)] = {301, read(fixtures / "ProsperoTV-moved.json")};
    github.api["https://api.github.com/repositories/1351488589/releases/latest"] = {200,
                                                                                    read(fixtures / "ProsperoTV-latest.json")};
    const self_update_platform real = platform_of(github);
    const Eden::Apps::Release moved_away = Eden::Apps::Check(&real, old);
    expect(moved_away.reason == GITHUB_RELEASE_OWNER_CHANGED && moved_away.fallback &&
               moved_away.repository == "blackbearreloaded/ProsperoTV",
           "blackbearreloaded/ProsperoTV, now ThatGuyKalvin's, is not installed from");

    // SHA256SUMS listed but not downloaded (told as GitHub not answering), or of no size.
    Network sums;
    sums.api[latest_of(tv.repository)] = {
        200, latest("01.001.000", {{"PPSA99003.zip", zip.size(), sha256(zip)}, {"SHA256SUMS", 80, ""}})};
    const self_update_platform none = platform_of(sums);
    expect(Eden::Apps::Check(&none, tv).reason == GITHUB_RELEASE_SUMS_UNREADABLE,
           "SHA256SUMS that can't be downloaded: nothing is offered");
    sums.api[latest_of(tv.repository)] = {
        200, latest("01.001.000", {{"PPSA99003.zip", zip.size(), sha256(zip)}, {"SHA256SUMS", 0, ""}})};
    expect(Eden::Apps::Check(&none, tv).reason == GITHUB_RELEASE_SUMS_MALFORMED,
           "SHA256SUMS of no size: nothing is offered");

    // Nightshade TV's own first release, as GitHub gave it.
    Network own;
    own.api[latest_of(tv.repository)] = {200, read(fixtures / "Nightshade-TV-latest.json")};
    own.files[file_of(tv.repository, "01.001.000", "SHA256SUMS")] = read(fixtures / "Nightshade-TV-SHA256SUMS");
    const self_update_platform ours = platform_of(own);
    const Eden::Apps::Release first = Eden::Apps::Check(&ours, tv);
    expect(first.reason == GITHUB_RELEASE_NEWER && !first.fallback && std::string{first.asset.tag} == "01.001.000" &&
               first.asset.from_sums == 1 &&
               std::string{first.asset.sha256} == "dd810b47475d604faf01917f0631712c04607ebeffc0adfc539c2f905f00acd9" &&
               std::string{first.asset.url} == file_of(tv.repository, "01.001.000", "PPSA99003.zip"),
           "Nightshade TV's own release 01.001.000 is offered from its repository");
}

// ---- the originals' releases, as GitHub answered for them ---------------------------------------

void originals(const fs::path &fixtures, const fs::path &zips)
{
    struct Original
    {
        const char *title, *project, *tag, *sha256;
        bool sums;
    };
    const Original all[] = {
        {"PPSA99003", "ProsperoTV", "01.000.090", "efbe68d8723bd2cbab3225e5945ea86aa7c28dab827412848f285cc7d9ef40b3", true},
        {"PPSA99002", "ProsperoLight", "01.000.110", "f29935f3bd9d61926c1ea63f8d34019fff19ee993e8d030ee373c004efd974ba",
         true},
        {"PPSA98510", "SwanStationPS5", "1.5.9", "4def0fd289a935cb37b5df04bf19dbc3523c8a2de8c7b1f92020ea0d63c77bca", false},
    };
    for (const Original &original : all)
    {
        const pe::ui::AppListing &listing = app(original.title);
        const std::string name = original.project;
        Network network;
        network.api[latest_of(listing.repository)] = {404, "{\"message\":\"Not Found\"}"};
        // The list names each project where it is now: GitHub answers it at once.
        network.api[latest_of(listing.fallback)] = {200, read(fixtures / (name + "-latest.json"))};
        if (original.sums)
            network.files[file_of(listing.fallback, original.tag, "SHA256SUMS")] =
                read(fixtures / (name + "-SHA256SUMS"));
        const self_update_platform platform = platform_of(network);
        const Eden::Apps::Release release = Eden::Apps::Check(&platform, listing);
        expect(release.reason == GITHUB_RELEASE_NEWER && release.fallback &&
                   std::string{release.asset.tag} == original.tag && release.asset.sha256 == std::string{original.sha256} &&
                   release.asset.from_sums == (original.sums ? 1 : 0),
               name + "'s release as GitHub gave it: " + github_release_reason_text(release.reason));

        const fs::path zip = zips / (std::string{original.title} + ".zip");
        if (zips.empty() || !fs::exists(zip))
            continue;
        // Its own ZIP, installed beside Nightshade.
        Console console(name);
        network.console = &console;
        network.files[file_of(listing.fallback, original.tag, std::string{original.title} + ".zip")] = read(zip);
        self_update_offer offer{};
        const bool offered = Eden::Apps::MakeOffer(listing, release, {}, "PPSA99008", &offer);
        const self_update_status status = offered ? install(platform, offer) : self_update_status{};
        std::string title, version;
        expect(status.phase == SELF_UPDATE_INSTALLED &&
                   Eden::Apps::ReadParam((console.homebrew / original.title / "sce_sys/param.json").string(), &title,
                                         &version) &&
                   title == original.title && version == status.version &&
                   fs::exists(console.homebrew / original.title / "eboot.bin"),
               name + "'s own ZIP is installed (" + std::string{status.error} + ")");
        std::printf("apps: %s %s installed from its own ZIP as %s %s\n", name.c_str(), original.tag, title.c_str(),
                    version.c_str());
    }
}

// ---- where apps are, what releases offer, opening one -------------------------------------------

void finding()
{
    Console console("find");
    const std::string user = console.user.string(), homebrew = console.homebrew.string();
    // Nightshade's own folder: ShadowMountPlus's record of it, else the usual one.
    expect(Eden::Apps::OwnFolder("PPSA99008", user, (console.homebrew / "PPSA99008").string()) ==
               (console.homebrew / "PPSA99008").string(),
           "Nightshade's folder, with no record of ShadowMountPlus's");
    const fs::path usb = console.base / "usb0/homebrew";
    write(usb / "PPSA99008/sce_sys/param.json", param("PPSA99008", "01.010.000"));
    write(console.user / "app/PPSA99008/mount.lnk", (usb / "PPSA99008").string() + "\n");
    const std::string own = Eden::Apps::OwnFolder("PPSA99008", user, (console.homebrew / "PPSA99008").string());
    expect(own == (usb / "PPSA99008").string(), "Nightshade's folder, where ShadowMountPlus mounted it from");
    // An app beside it under another name; one in /data/homebrew; one ShadowMountPlus recorded.
    write(usb / "ProsperoLight/sce_sys/param.json", param("PPSA99002", "01.000.100"));
    write(console.homebrew / "PPSA99003/sce_sys/param.json", param("PPSA99003", "01.000.080"));
    write(console.base / "elsewhere/PPSA98510/sce_sys/param.json", param("PPSA98510", "01.005.008"));
    write(console.user / "app/PPSA98510/mount.lnk", (console.base / "elsewhere/PPSA98510").string());
    const Eden::Apps::Installed stream = Eden::Apps::Find("PPSA99002", user, own, homebrew);
    const Eden::Apps::Installed tv = Eden::Apps::Find("PPSA99003", user, own, homebrew);
    const Eden::Apps::Installed station = Eden::Apps::Find("PPSA98510", user, own, homebrew);
    expect(stream.folder == (usb / "ProsperoLight").string() && stream.version == "01.000.100",
           "an app beside Nightshade's folder, under another name");
    expect(tv.folder == (console.homebrew / "PPSA99003").string() && tv.version == "01.000.080",
           "an app in /data/homebrew");
    expect(station.folder == (console.base / "elsewhere/PPSA98510").string() && station.version == "01.005.008",
           "an app where ShadowMountPlus mounted it from");
    // An image: found, opened, not updated.
    fs::remove(console.user / "app/PPSA98510/mount.lnk");
    write(console.user / "app/PPSA98510/mount_img.lnk", "/mnt/usb0/PPSA98510.exfat");
    write(console.user / "app/PPSA98510/sce_sys/param.json", param("PPSA98510", "01.005.008"));
    const Eden::Apps::Installed image = Eden::Apps::Find("PPSA98510", user, own, homebrew);
    expect(image.image && image.version == "01.005.008" &&
               Eden::Apps::Decide(image, true, "1.5.9") == Eden::Apps::Offer::none,
           "an app installed as an image is found and not updated");
}

void tags()
{
    std::string version;
    const auto tag = [&](const char *text, Eden::Apps::TagKind kind, const char *wanted)
    {
        version.clear();
        expect(Eden::Apps::TagVersion(text, &version) == kind && version == wanted, std::string{"the tag "} + text);
    };
    using Eden::Apps::TagKind;
    tag("01.000.090", TagKind::exact, "01.000.090");
    tag("v1.005.100", TagKind::exact, "01.005.100");
    tag("v10.000.000", TagKind::exact, "10.000.000");
    tag("1.5.9", TagKind::likely, "01.005.009");
    tag("v1.5.9", TagKind::likely, "01.005.009");
    tag("1.005.100", TagKind::likely, "01.005.100");
    tag("1.5", TagKind::none, "");
    tag("1.5.9-beta", TagKind::none, "");
    tag("100.0.0", TagKind::none, "");
    tag("nightly", TagKind::none, "");
    const Eden::Apps::Installed installed{"/data/homebrew/PPSA98510", "01.005.009", false};
    using Eden::Apps::Offer;
    expect(Eden::Apps::Decide(installed, true, "1.5.9") == Offer::current, "1.5.9 over 01.005.009 is not newer");
    expect(Eden::Apps::Decide(installed, true, "1.5.10") == Offer::update, "1.5.10 over 01.005.009 is newer");
    expect(Eden::Apps::Decide(installed, true, "1.4.0") == Offer::current, "an older release is not offered");
    expect(Eden::Apps::Decide(installed, true, "nightly") == Offer::update, "a tag that names no version is offered");
    expect(Eden::Apps::Decide(installed, false, "") == Offer::none, "no release, nothing to do");
}

// The launch payload's side of its conversation, on the other end of a socket pair.
void launching()
{
    const auto conversation = [](const char *answer, std::string *heard)
    {
        int ends[2];
        if (socketpair(AF_UNIX, SOCK_STREAM, 0, ends) != 0)
            return std::string{"no socket pair"};
        std::thread payload(
            [&, answer]
            {
                char buffer[64];
                std::string got;
                while (std::count(got.begin(), got.end(), '\n') < 3)
                {
                    const ssize_t count = ::read(ends[1], buffer, sizeof(buffer));
                    if (count <= 0)
                        break;
                    got.append(buffer, static_cast<std::size_t>(count));
                }
                *heard = got;
                (void)!::write(ends[1], answer, std::strlen(answer));
                ::close(ends[1]);
            });
        const self_update_channel channel{channel_send, channel_receive, channel_close,
                                          reinterpret_cast<void *>(static_cast<std::intptr_t>(ends[0]))};
        const std::string result = Eden::Apps::AskLaunch(channel, "PPSA99003", "PPSA99008");
        payload.join();
        ::close(ends[0]);
        return result;
    };
    std::string heard;
    expect(conversation("waiting\n", &heard).empty() && heard == "PSL1\nPPSA99003\nPPSA99008\n",
           "the launch payload is asked for the title and told who asks");
    expect(conversation("fail The console doesn't list the app yet\n", &heard) == Eden::Apps::kNotListed,
           "the launch payload's refusal is told");
    expect(conversation("", &heard) == "The launch payload didn't answer", "a launch payload that doesn't answer");
}
} // namespace

int main(int argc, char **argv)
{
    if (argc < 2 || argc > 3)
    {
        std::fprintf(stderr, "usage: eden-apps-check <fixtures/apps> [<folder with the originals' ZIPs>]\n");
        return 2;
    }
    std::signal(SIGPIPE, SIG_IGN);
    good_zip();
    wrong_hash();
    missing_hash();
    missing_release();
    redirects(argv[1]);
    originals(argv[1], argc == 3 ? fs::path{argv[2]} : fs::path{});
    finding();
    tags();
    launching();
    std::printf("%s apps: %d checks of releases, their checks and installs, finding apps and opening them, %d failed\n",
                failures ? "FAIL" : "PASS", passed + failures, failures);
    return failures ? 1 : 0;
}
