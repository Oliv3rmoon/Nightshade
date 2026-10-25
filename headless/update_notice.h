// SPDX-License-Identifier: GPL-3.0-or-later
// Updates: once per launch the app asks whether a newer release of itself is out, and can
// replace itself with it when the player says so.
//
// Where it asks is the build's: the homebrew.page catalog, which lists the official releases, or,
// when the build names a GitHub repository (EDEN_UPDATE_REPOSITORY, headless/CMakeLists.txt),
// that repository's newest release (update_check/github_release.h).
//
// Both are the PS5 Native App Boilerplate's self-update kit (headless/update_check, see its
// README.md). The catalog's check fetches the catalog's signed manifest and the app's own entry
// and verifies the Ed25519 signature and the hashes; GitHub's reads the release and its
// SHA256SUMS over HTTPS, with no signature: the trust of a player who downloads the release by
// hand. Either compares the content version with this build's (sce_sys/param.json). The update
// downloads the release ZIP from GitHub over HTTPS, checks its SHA-256 and streams it to the
// self-update helper (self-updater.elf in the app's folder), which the app sends to the console's
// payload loader: the helper checks and unpacks the release beside the app (it must be this
// title at the offered version), and once the app has closed it replaces the app's files and
// posts a notification. Nothing is changed before the player confirms, and a failure or a cancel
// leaves the app as it was.
#pragma once
#include <cstdint>
#include <string>

namespace Eden::UpdateNotice {
// Starts the check on a thread of its own; only the first call of a launch does anything.
void Start();

// Where the check asks, as the launcher's update texts name it: "homebrew.page", or "GitHub" for a
// build that names a repository.
const char* Source();

struct Offer {
    bool installable = false;  // the release can be installed by the app itself
    std::string version;       // the release's name ("v1.000.060")
    std::string available;     // its content version ("01.000.060")
    std::uint64_t size = 0;    // the ZIP's size in bytes; 0 when the catalog doesn't know
    std::string notes;         // the release notes, plain text; empty when there are none
    bool notes_truncated = false;
};
// A newer release than this one, once, when the check's answer has come: false when there is
// nothing (yet) to tell.
bool Take(Offer* offer);

// In the order of pe::ui::UpdatePhase (eden_services.cpp converts one to the other by number).
enum class Phase { idle, starting, downloading, unpacking, ready, applying, cancelled, failed };
struct Progress {
    Phase phase = Phase::idle;
    std::uint64_t done = 0;
    std::uint64_t total = 0;      // 0 while it isn't known
    std::string time_left;        // "about 20 s left"; empty until it can be said
    std::string error;            // why it failed
};
// The update of the offered release: begins the download and the staging (true: begun), where
// it is now, stop it (nothing changed until Apply), and the go-ahead once staged (true: the
// helper waits for the app to close; close it now).
bool Begin();
Progress Poll();
void Cancel();
bool Apply();
// After a cancel or a failure, before beginning again.
void Finish();
} // namespace Eden::UpdateNotice
