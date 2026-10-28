// SPDX-FileCopyrightText: Copyright 2026 Oliv3rmoon
// SPDX-License-Identifier: GPL-3.0-or-later
// Nightshade's Apps (Settings > Apps): the apps of pe/ui/apps_list.hpp found on the console, their
// newest releases asked of GitHub, installed or updated by the self-update helper and opened by the
// launch payload. The decisions are apps_release.h's, tested on the host (apps_check.cpp).
//
// Where an app is needs the filesystem access (storage_paths.h). Without it nothing can be said of
// what is installed, and an app with a release is offered as a new one: the helper refuses one that
// is there already. GitHub is asked once per Refresh, on a thread of its own: one or two requests
// per app of the 60 an hour GitHub allows an address without an account. The install is the
// self-update kit's job with an install offer (self_update.h): the ZIP is checked against its
// SHA-256 as it arrives and again by the helper, which puts it in a folder beside Nightshade's, or
// in place of the app's own files. The homebrew loader lists a new app after its next scan or a
// restart. Opening one sends app-launch.elf from Nightshade's folder to the payload loader, which
// opens it once Nightshade has closed (app_launch/main.c).
#pragma once
#include <string>
#include <vector>

#include "pe/ui/services.hpp"

namespace Eden::Apps {
// Looks for the apps and, with `releases`, asks GitHub for their releases, on a thread of its own
// (nothing when one is looking already).
void Refresh(bool releases);
// Every app of kApps, in its order, as far as Refresh has come.
std::vector<pe::ui::AppState> List();
// Installs the offered release of an app (true: begun), where that is, stop it, and after it ended.
bool Install(int app);
pe::ui::AppJob Poll();
void Cancel();
void Finish();
// Sends the launch payload for an installed app. AppOpen::closing: it waits for Nightshade to close;
// otherwise *reason says why not (English). Blocking for a moment.
pe::ui::AppOpen Open(int app, std::string* reason);
} // namespace Eden::Apps
