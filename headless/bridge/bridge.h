// SPDX-FileCopyrightText: Copyright 2026 Oliv3rmoon
// SPDX-License-Identifier: GPL-3.0-or-later
// Nightshade's bridge (PPSA99008): the parts that need no console, checked on the host
// (headless/apps_check.cpp, tools/check-apps.py).
//
// Every Nightshade up to 1.010 ran under ProsperoEden's title ID, PPSA99008. Nightshade has its own
// now (PPSA64448), and an install made before can't update to it by itself: its update helper only
// takes a folder of the title it updates. So each release ZIP carries Nightshade as PPSA64448/ and
// this small app as PPSA99008/ (tools/ci/make-dist.py), at the same version. An old install's
// update takes PPSA99008/, and the next time it is opened, the bridge
//
//   1. asks the launch payload (app_launch/main.c) to open PPSA64448, with the launch arguments a
//      home screen forwarder gave it (--rom, --exit-after-game: forwarded_args.h), and closes;
//   2. when the console doesn't list PPSA64448, installs it from the release it came in, as
//      Settings > Apps installs an app (apps_release.h): the ZIP from GitHub, checked against the
//      release's SHA256SUMS, unpacked by the self-update helper in a new folder beside its own;
//   3. waits for the homebrew loader to list it, and opens it as in 1.
//
// It never touches ProsperoEden's data folder: what becomes of it is PPSA64448's question at its
// first start (migrate.h). It has no emulator and no filesystem access of its own: the payloads do
// the work outside its sandbox (main.cpp).
#pragma once
#include <string>
#include <vector>

#include "apps_release.h"
#include "forwarded_args.h"

namespace Eden::Bridge {

inline constexpr const char* kTitle = "PPSA99008";      // this app: ProsperoEden's title ID
inline constexpr const char* kNightshade = "PPSA64448"; // Nightshade's own
inline constexpr const char* kName = "Nightshade";
inline constexpr const char* kRepository = "Oliv3rmoon/Nightshade";

// The tag tools/ci/make-dist.py gives a version's release ("01.010.004": "v1.010.004"); empty when
// `version` is not a content version.
std::string ReleaseTag(const std::string& version);
// That release's ZIP, which holds PPSA64448/ and PPSA99008/: "Nightshade-v1.010.004.zip".
std::string ReleaseZip(const std::string& version);

// The release of `version` on GitHub (releases/tags/<tag>) and its ZIP, with the SHA-256 the
// release's SHA256SUMS gives it (github_release_tag_asset_check).
Apps::Release Check(const self_update_platform* platform, const std::string& version);
// The self-update kit's offer that installs PPSA64448 from it, in a new folder beside this app's.
// False when the release is not that version's, or can't be installed.
bool MakeOffer(const Apps::Release& release, const std::string& version, self_update_offer* offer);

// What became of asking the launch payload to open PPSA64448 (Apps::AskLaunch's answer).
enum class Opened { closing, not_listed, refused };
Opened Classify(const std::string& answer);
// Whether the helper's refusal of the install says PPSA64448 is installed already: then the
// homebrew loader has not listed it yet, and the bridge waits for it as after an install.
bool AlreadyInstalled(const std::string& error);

// The launch arguments PPSA64448 is opened with: those that start the forwarded game, if any.
std::vector<std::string> LaunchArguments(const ForwardedArgs& forwarded);

} // namespace Eden::Bridge
