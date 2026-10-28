// SPDX-FileCopyrightText: Copyright 2026 Oliv3rmoon
// SPDX-License-Identifier: GPL-3.0-or-later
#include "apps_release.h"

#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <string_view>
#include <sys/stat.h>
#include <vector>

#include "update_check/self_update_protocol.h"
#include "update_check/update_check.h"

namespace Eden::Apps {
namespace {

// A small file whole (a param.json, a record of ShadowMountPlus's); false when it can't be read
// or is longer than `limit`.
bool ReadSmall(const std::string& path, std::size_t limit, std::string* text) {
    std::FILE* file = std::fopen(path.c_str(), "rb");
    if (!file) return false;
    text->assign(limit + 1, '\0');
    const std::size_t length = std::fread(text->data(), 1, text->size(), file);
    std::fclose(file);
    if (length > limit) return false;
    text->resize(length);
    return true;
}

bool IsDirectory(const std::string& path) {
    struct stat info {};
    return stat(path.c_str(), &info) == 0 && S_ISDIR(info.st_mode);
}

bool IsFile(const std::string& path) {
    struct stat info {};
    return stat(path.c_str(), &info) == 0 && S_ISREG(info.st_mode);
}

// The folder ShadowMountPlus recorded it mounted a title from: <registered>/app/<title>/mount.lnk
// holds its path (as the self-update helper reads it, self_update_helper/updater.cpp). Empty: none.
std::string MountedFrom(const std::string& registered, const std::string& title) {
    std::string path;
    if (registered.empty() || !ReadSmall(registered + "/app/" + title + "/mount.lnk", 511, &path)) return {};
    while (!path.empty() && (path.back() == '\n' || path.back() == '\r' || path.back() == ' ' ||
                             path.back() == '\0' || path.back() == '/'))
        path.pop_back();
    return path.size() > 1 && path[0] == '/' ? path : std::string{};
}

// The folder `folder` when it holds the title.
bool Holds(const std::string& folder, const std::string& title, Installed* out) {
    std::string id, version;
    if (!ReadParam(folder + "/sce_sys/param.json", &id, &version) || id != title) return false;
    out->folder = folder;
    out->version = version;
    return true;
}

std::string Parent(const std::string& path) {
    const std::size_t slash = path.rfind('/');
    return slash == std::string::npos || slash == 0 ? std::string{} : path.substr(0, slash);
}

} // namespace

bool ReadParam(const std::string& path, std::string* title, std::string* version) {
    std::string body;
    char id[10];
    char content[12];
    if (!ReadSmall(path, 64 * 1024, &body) ||
        update_check_json_string(body.data(), body.size(), "titleId", id, sizeof(id)) != 1 ||
        update_check_json_string(body.data(), body.size(), "contentVersion", content, sizeof(content)) != 1)
        return false;
    *title = id;
    *version = content;
    return true;
}

std::string OwnFolder(const std::string& own_title, const std::string& registered, const std::string& install_dir) {
    Installed own;
    const std::string mounted = MountedFrom(registered, own_title);
    if (!mounted.empty() && Holds(mounted, own_title, &own)) return own.folder;
    return Holds(install_dir, own_title, &own) ? own.folder : std::string{};
}

Installed Find(const std::string& title, const std::string& registered, const std::string& own_folder,
               const std::string& homebrew) {
    Installed found;
    const std::string mounted = MountedFrom(registered, title);
    if (!mounted.empty() && Holds(mounted, title, &found)) return found;
    // An image: the console keeps a copy of its sce_sys beside the record.
    if (!registered.empty() && IsFile(registered + "/app/" + title + "/mount_img.lnk")) {
        std::string id;
        found.image = true;
        found.folder = registered + "/app/" + title;
        if (!ReadParam(found.folder + "/sce_sys/param.json", &id, &found.version) || id != title)
            found.version.clear();
        return found;
    }
    std::vector<std::string> roots;
    if (const std::string beside = Parent(own_folder); !beside.empty()) roots.push_back(beside);
    if (!homebrew.empty() && (roots.empty() || roots[0] != homebrew)) roots.push_back(homebrew);
    // The folder named after the title first, as Nightshade installs it; then any name.
    for (const std::string& root : roots)
        if (Holds(root + "/" + title, title, &found)) return found;
    for (const std::string& root : roots) {
        DIR* directory = opendir(root.c_str());
        if (!directory) continue;
        std::vector<std::string> names;
        while (const dirent* entry = readdir(directory))
            if (entry->d_name[0] != '.' && names.size() < 4096) names.emplace_back(entry->d_name);
        closedir(directory);
        for (const std::string& name : names)
            if (name != title && IsDirectory(root + "/" + name) && Holds(root + "/" + name, title, &found))
                return found;
    }
    return found;
}

TagKind TagVersion(const std::string& tag, std::string* version) {
    char text[12];
    if (update_check_version_parse(tag.c_str(), nullptr)) {
        *version = tag;
        return TagKind::exact;
    }
    if (github_release_tag_version(tag.c_str(), text)) {
        *version = text;
        return TagKind::exact;
    }
    // Three numbers of at most two, three and three digits, with or without a 'v'.
    std::string_view rest = tag;
    if (!rest.empty() && rest.front() == 'v') rest.remove_prefix(1);
    unsigned parts[3] = {};
    for (int part = 0; part < 3; ++part) {
        std::size_t digits = 0;
        while (digits < rest.size() && rest[digits] >= '0' && rest[digits] <= '9') {
            parts[part] = parts[part] * 10 + static_cast<unsigned>(rest[digits] - '0');
            ++digits;
        }
        if (digits == 0 || digits > (part == 0 ? 2u : 3u)) return TagKind::none;
        rest.remove_prefix(digits);
        if (part < 2) {
            if (rest.empty() || rest.front() != '.') return TagKind::none;
            rest.remove_prefix(1);
        }
    }
    if (!rest.empty()) return TagKind::none;
    std::snprintf(text, sizeof(text), "%02u.%03u.%03u", parts[0], parts[1], parts[2]);
    *version = text;
    return TagKind::likely;
}

Offer Decide(const Installed& installed, bool found, const std::string& tag) {
    if (installed.folder.empty()) return found ? Offer::get : Offer::none;
    if (!found || installed.image) return Offer::none;
    std::string version;
    int comparable = 0;
    if (TagVersion(tag, &version) == TagKind::none) return Offer::update;
    const int order = update_check_version_compare(version.c_str(), installed.version.c_str(), &comparable);
    return !comparable || order > 0 ? Offer::update : Offer::current;
}

Release Check(const self_update_platform* platform, const pe::ui::AppListing& app) {
    Release release;
    release.repository = app.repository;
    release.reason = github_release_asset_check(platform, app.repository, app.asset, &release.asset, &release.report);
    if (release.reason == GITHUB_RELEASE_NONE && app.fallback != nullptr && app.fallback[0] != '\0') {
        release.fallback = true;
        release.repository = app.fallback;
        release.reason = github_release_asset_check(platform, app.fallback, app.asset, &release.asset, &release.report);
    }
    return release;
}

bool MakeOffer(const pe::ui::AppListing& app, const Release& release, const Installed& installed,
               const char* asking, self_update_offer* offer) {
    std::string version;
    if (offer == nullptr || release.reason != GITHUB_RELEASE_NEWER || installed.image ||
        (!installed.folder.empty() && !update_check_version_parse(installed.version.c_str(), nullptr)))
        return false;
    std::memset(offer, 0, sizeof(*offer));
    const bool exact = TagVersion(release.asset.tag, &version) == TagKind::exact;
    std::snprintf(offer->title, sizeof(offer->title), "%s", app.title);
    std::snprintf(offer->name, sizeof(offer->name), "%s", app.name);
    std::snprintf(offer->installed, sizeof(offer->installed), "%s",
                  installed.folder.empty() ? "none" : installed.version.c_str());
    std::snprintf(offer->available, sizeof(offer->available), "%s", exact ? version.c_str() : "any");
    std::snprintf(offer->version, sizeof(offer->version), "%s", release.asset.tag);
    std::snprintf(offer->artifact, sizeof(offer->artifact), "%s", release.asset.url);
    std::snprintf(offer->sha256, sizeof(offer->sha256), "%s", release.asset.sha256);
    std::snprintf(offer->page, sizeof(offer->page), "%s", release.asset.page);
    std::snprintf(offer->beside, sizeof(offer->beside), "%s", asking);
    offer->size = release.asset.size;
    return offer->sha256[0] != '\0' && self_update_url_allowed(offer->artifact, 0);
}

std::string AskLaunch(const self_update_channel& channel, const std::string& title, const std::string& asking) {
    const std::string request = "PSL1\n" + title + "\n" + asking + "\n";
    for (std::size_t done = 0; done < request.size();) {
        const long count = channel.send(channel.user, request.data() + done, request.size() - done);
        if (count <= 0) return "The launch payload stopped";
        done += static_cast<std::size_t>(count);
    }
    std::string line;
    for (;;) {
        char byte = 0;
        if (channel.receive(channel.user, &byte, 1) <= 0) return "The launch payload didn't answer";
        if (byte == '\n') break;
        if (line.size() >= SELF_UPDATE_LINE) return "The launch payload didn't answer";
        line.push_back(byte);
    }
    if (line == "waiting") return {};
    return line.rfind("fail ", 0) == 0 ? line.substr(5) : "The launch payload didn't answer";
}

} // namespace Eden::Apps
