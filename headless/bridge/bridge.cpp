// SPDX-FileCopyrightText: Copyright 2026 Oliv3rmoon
// SPDX-License-Identifier: GPL-3.0-or-later
#include "bridge.h"

#include <cstring>

#include "update_check/update_check.h"

namespace Eden::Bridge {

std::string ReleaseTag(const std::string& version) {
    if (update_check_version_parse(version.c_str(), nullptr) != 1) return {};
    // make-dist.py: 'v' + the version without its first leading zero.
    return "v" + (version.front() == '0' ? version.substr(1) : version);
}

std::string ReleaseZip(const std::string& version) {
    const std::string tag = ReleaseTag(version);
    return tag.empty() ? std::string{} : "Nightshade-" + tag + ".zip";
}

Apps::Release Check(const self_update_platform* platform, const std::string& version) {
    Apps::Release release;
    release.repository = kRepository;
    const std::string tag = ReleaseTag(version);
    const std::string zip = ReleaseZip(version);
    if (tag.empty()) return release; // GITHUB_RELEASE_BAD_ARGUMENT
    release.reason = github_release_tag_asset_check(platform, kRepository, tag.c_str(), zip.c_str(), &release.asset,
                                                    &release.report);
    return release;
}

bool MakeOffer(const Apps::Release& release, const std::string& version, self_update_offer* offer) {
    const std::string zip = ReleaseZip(version);
    // Nightshade as Settings > Apps lists an app, for the kit's offer: its ZIP is the release's.
    const pe::ui::AppListing nightshade{kName, kNightshade, kRepository, nullptr, zip.c_str(), "", pe::ui::AppIcon::disc};
    if (zip.empty() || !Apps::MakeOffer(nightshade, release, {}, kTitle, offer)) return false;
    // The helper installs it only at the version the release names: this one.
    return std::strcmp(offer->available, version.c_str()) == 0 && std::strcmp(offer->installed, "none") == 0;
}

Opened Classify(const std::string& answer) {
    if (answer.empty()) return Opened::closing;
    return answer == Apps::kNotListed ? Opened::not_listed : Opened::refused;
}

bool AlreadyInstalled(const std::string& error) {
    return error.rfind("The app is already installed in ", 0) == 0;
}

std::vector<std::string> LaunchArguments(const ForwardedArgs& forwarded) {
    return ForwardedArgv(forwarded, kTitle);
}

} // namespace Eden::Bridge
