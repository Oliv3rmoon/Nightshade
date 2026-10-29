// SPDX-FileCopyrightText: Copyright 2026 Oliv3rmoon
// SPDX-License-Identifier: GPL-3.0-or-later
// Nightshade's Apps (apps.h), the parts that need no console, checked on the host
// (apps_check.cpp): where an app is installed, which release is offered for it and what that
// offers, the offer the self-update kit's job installs it from, and the launch payload's
// conversation (app_launch/main.c).
#pragma once
#include <cstddef>
#include <string>
#include <vector>

#include "pe/ui/apps_list.hpp"
#include "update_check/github_release.h"
#include "update_check/self_update.h"

namespace Eden::Apps {

// ---- where an app is ----

struct Installed {
    std::string folder;   // empty: not found
    std::string version;  // its sce_sys/param.json's contentVersion
    bool image = false;   // ShadowMountPlus mounts it from an image: it opens, it isn't updated here
};
// titleId and contentVersion of a param.json; false when it can't be read or lacks either.
bool ReadParam(const std::string& path, std::string* title, std::string* version);
// Nightshade's own folder as it lies on its drive (not the PS5's mount of it, /app0): where
// ShadowMountPlus recorded it mounted it from (<registered>/app/<title>/mount.lnk), else
// install_dir when the app is there.
std::string OwnFolder(const std::string& own_title, const std::string& registered, const std::string& install_dir);
// An app's folder: where ShadowMountPlus recorded it mounted it from, else the folder beside
// Nightshade's own (own_folder's parent) or in `homebrew` whose param.json names the title, the one
// named after it first. registered is the console's /user, homebrew /data/homebrew.
Installed Find(const std::string& title, const std::string& registered, const std::string& own_folder,
               const std::string& homebrew);

// ---- what a release offers ----

// The content version a release's tag names. Exact: the tag is one ("01.000.090", as ProsperoTV
// names its releases) or as tools/ci/make-dist.py names Nightshade's ("v1.005.100"). Likely: three
// numbers, read as a content version writes them ("1.5.9" and "v1.5.9" as 01.005.009, which
// SwanStationPS5's 1.5.9 holds); the helper is not held to a likely one. None: no version.
enum class TagKind { none, likely, exact };
TagKind TagVersion(const std::string& tag, std::string* version);

// What there is to do: nothing (no release found, or an image), get it, update it, or nothing
// newer (current). A release whose tag names no version is offered to an installed app all the
// same: whether it is newer can't be told.
enum class Offer { none, get, update, current };
Offer Decide(const Installed& installed, bool found, const std::string& tag);

// The release offered for an app: the newest of its Nightshade repository, or while that has none
// (GitHub answers 404: no release, or no repository yet) the newest of the project it is based on.
struct Release {
    github_release_reason reason = GITHUB_RELEASE_BAD_ARGUMENT;  // GITHUB_RELEASE_NEWER: found
    bool fallback = false;   // it is the project's
    std::string repository;  // where it is
    github_release_asset asset{};
    github_release_report report{};  // for the log
};
Release Check(const self_update_platform* platform, const pe::ui::AppListing& app);

// The self-update kit's offer that installs the release (self_update_start): beside the asking
// app's folder when the app isn't installed, in place of its own entries when it is. False when
// there is nothing to install.
bool MakeOffer(const pe::ui::AppListing& app, const Release& release, const Installed& installed,
               const char* asking, self_update_offer* offer);

// ---- opening it ----

// The launch payload's conversation, on a connection to it: the title to open and the one asking,
// and the launch arguments to open it with (PSL2; none: PSL1, as Settings > Apps opens an app).
// Empty: the payload waits for the asking app to close and then opens the title. Otherwise why
// not (English).
std::string AskLaunch(const self_update_channel& channel, const std::string& title, const std::string& asking,
                      const std::vector<std::string>& arguments = {});
// What PSL2 passes on: at most this many arguments, each shorter than kLaunchArgumentSize bytes and
// without a line end (app_launch/main.c).
inline constexpr std::size_t kLaunchArguments = 8;
inline constexpr std::size_t kLaunchArgumentSize = 1024;
// What the payload answers for an app the console doesn't list yet (a new one the homebrew loader
// has not scanned).
inline constexpr const char* kNotListed = "The console doesn't list the app yet";

} // namespace Eden::Apps
