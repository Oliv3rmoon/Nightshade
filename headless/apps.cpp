// SPDX-FileCopyrightText: Copyright 2026 Oliv3rmoon
// SPDX-License-Identifier: GPL-3.0-or-later
#include "apps.h"

#include <array>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <pthread.h>

#include "apps_release.h"
#include "diagnostics.h"
#include "storage_paths.h"

namespace Eden::Apps {
namespace {
// libcurl and OpenSSL want more stack than a default thread of the console has (update_notice.cpp).
constexpr std::size_t kStackSize = 1024 * 1024;
// The console's own copies of what it registered (ShadowMountPlus's records among them), and the
// usual folder of homebrew apps.
constexpr const char* kRegistered = "/user";
constexpr const char* kHomebrew = "/data/homebrew";

struct Known {
    bool looked = false;
    Installed installed;
    bool checked = false;
    Release release;
};

std::mutex lock;
std::array<Known, pe::ui::kAppCount> known;
bool asking = false;  // a thread looks (and maybe asks GitHub)
bool wanted = false;  // ... and GitHub is to be asked once it has looked
self_update_job job{};  // zero until the first Install, as the kit asks
bool begun = false;
int job_app = -1;
bool job_fresh = false;
bool job_told = false;  // its end is in the log, and the app was looked for again

// Nightshade's own title ID, the one the helper puts a new app beside.
const std::string& OwnTitle() {
    static const std::string title = [] {
        std::string id, version;
        return ReadParam(AppFile("sce_sys/param.json"), &id, &version) ? id : std::string{"PPSA99008"};
    }();
    return title;
}

// Where the apps are (all of them, or one), as far as the filesystem access lets it be seen.
void Look(int only) {
    if (!FilesystemAccess()) return;
    const std::string own = OwnFolder(OwnTitle(), kRegistered, kInstallDir);
    for (int app = 0; app < pe::ui::kAppCount; ++app) {
        if (only >= 0 && app != only) continue;
        const Installed found = Find(pe::ui::kApps[app].title, kRegistered, own, kHomebrew);
        char line[400];
        std::snprintf(line, sizeof(line), "%s: %s%s%s%s", pe::ui::kApps[app].title,
                      found.folder.empty() ? "not installed" : found.folder.c_str(), found.folder.empty() ? "" : " ",
                      found.version.c_str(), found.image ? " (image)" : "");
        Report("apps", line);
        const std::lock_guard guard(lock);
        known[static_cast<std::size_t>(app)].installed = found;
        known[static_cast<std::size_t>(app)].looked = true;
    }
}

void AskGitHub() {
    for (int app = 0; app < pe::ui::kAppCount; ++app) {
        const Release release = Check(self_update_console(), pe::ui::kApps[app]);
        char line[640];
        std::snprintf(line, sizeof(line), "%s releases/latest of %s: reason=%s http=%d redirects=%d error=%d tag=%s sha256=%s",
                      pe::ui::kApps[app].title, release.repository.c_str(), github_release_reason_text(release.reason),
                      release.report.http_status, release.report.redirects, release.report.platform_error,
                      release.report.tag[0] ? release.report.tag : "-",
                      release.asset.sha256[0] ? (release.asset.from_sums ? "SHA256SUMS" : "digest") : "-");
        Report("apps", line);
        const std::lock_guard guard(lock);
        known[static_cast<std::size_t>(app)].release = release;
        known[static_cast<std::size_t>(app)].checked = true;
    }
}

// The thread of Refresh: looks, then asks GitHub as long as that is wanted.
void* Ask(void*) {
    Look(-1);
    for (;;) {
        {
            const std::lock_guard guard(lock);
            if (!wanted) {
                asking = false;
                return nullptr;
            }
            wanted = false;
        }
        AskGitHub();
    }
}

pe::ui::AppProblem ProblemOf(github_release_reason reason) {
    switch (reason) {
    case GITHUB_RELEASE_NEWER: return pe::ui::AppProblem::none;
    case GITHUB_RELEASE_NONE: return pe::ui::AppProblem::no_release;
    case GITHUB_RELEASE_NETWORK:
    case GITHUB_RELEASE_HTTP_STATUS: return pe::ui::AppProblem::offline;
    case GITHUB_RELEASE_RATE_LIMITED: return pe::ui::AppProblem::rate_limited;
    case GITHUB_RELEASE_NO_ZIP: return pe::ui::AppProblem::no_file;
    case GITHUB_RELEASE_NO_HASH:
    case GITHUB_RELEASE_SUMS_NO_LINE: return pe::ui::AppProblem::no_hash;
    case GITHUB_RELEASE_HASHES_DIFFER:
    case GITHUB_RELEASE_SUMS_MALFORMED:
    case GITHUB_RELEASE_SUMS_UNREADABLE: return pe::ui::AppProblem::bad_hash;
    default: return pe::ui::AppProblem::unreadable;
    }
}

pe::ui::AppOffer OfferOf(Offer offer) {
    switch (offer) {
    case Offer::get: return pe::ui::AppOffer::get;
    case Offer::update: return pe::ui::AppOffer::update;
    case Offer::current: return pe::ui::AppOffer::current;
    default: return pe::ui::AppOffer::none;
    }
}
} // namespace

void Refresh(bool releases) {
    {
        const std::lock_guard guard(lock);
        if (releases) {
            wanted = true;
            for (Known& app : known) app.checked = false;
        }
        // One that runs asks GitHub once it has looked, when that is wanted now.
        if (asking) return;
        asking = true;
    }
    pthread_attr_t attributes;
    pthread_t thread;
    if (pthread_attr_init(&attributes) == 0) {
        pthread_attr_setstacksize(&attributes, kStackSize);
        pthread_attr_setdetachstate(&attributes, PTHREAD_CREATE_DETACHED);
        const bool started = pthread_create(&thread, &attributes, Ask, nullptr) == 0;
        pthread_attr_destroy(&attributes);
        if (started) return;
    }
    Report("apps", "The thread could not start");
    const std::lock_guard guard(lock);
    asking = false;
    wanted = false;
}

std::vector<pe::ui::AppState> List() {
    std::vector<pe::ui::AppState> list;
    const std::lock_guard guard(lock);
    for (const Known& app : known) {
        pe::ui::AppState state;
        state.looked = app.looked;
        state.installed = app.installed.folder.empty() ? std::string{} : app.installed.version;
        state.image = app.installed.image;
        state.checked = app.checked;
        if (app.checked) {
            const bool found = app.release.reason == GITHUB_RELEASE_NEWER;
            state.offer = OfferOf(Decide(app.installed, found, app.release.asset.tag));
            state.problem = ProblemOf(app.release.reason);
            state.release = found ? app.release.asset.tag : "";
            state.repository = app.release.repository;
            state.fallback = app.release.fallback;
            state.size = found ? app.release.asset.size : 0;
        }
        list.push_back(state);
    }
    return list;
}

bool Install(int app) {
    const std::lock_guard guard(lock);
    if (app < 0 || app >= pe::ui::kAppCount) return false;
    if (begun) {
        self_update_status status{};
        self_update_poll(&job, &status);
        if (status.phase != SELF_UPDATE_INSTALLED && status.phase != SELF_UPDATE_FAILED &&
            status.phase != SELF_UPDATE_CANCELLED)
            return false;  // one at a time
        self_update_finish(&job);
        begun = false;
    }
    const Known& found = known[static_cast<std::size_t>(app)];
    self_update_offer offer{};
    if (!found.checked || !MakeOffer(pe::ui::kApps[app], found.release, found.installed, OwnTitle().c_str(), &offer))
        return false;
    begun = self_update_start(&job, self_update_console(), &offer) == 1;
    job_app = app;
    job_fresh = std::strcmp(offer.installed, "none") == 0;
    job_told = false;
    char line[700];
    std::snprintf(line, sizeof(line), "%s %s %s (%s -> %s) from %s", begun ? "Installing" : "Could not begin installing",
                  pe::ui::kApps[app].title, offer.version, offer.installed, offer.available, offer.artifact);
    Report("apps", line);
    return begun;
}

pe::ui::AppJob Poll() {
    pe::ui::AppJob out;
    int look = -1;
    {
        const std::lock_guard guard(lock);
        if (!begun) return out;
        self_update_status status{};
        self_update_poll(&job, &status);
        switch (status.phase) {
        case SELF_UPDATE_STARTING: out.phase = pe::ui::AppPhase::starting; break;
        case SELF_UPDATE_DOWNLOADING: out.phase = pe::ui::AppPhase::downloading; break;
        case SELF_UPDATE_UNPACKING: out.phase = pe::ui::AppPhase::unpacking; break;
        case SELF_UPDATE_READY:
        case SELF_UPDATE_APPLYING: out.phase = pe::ui::AppPhase::installing; break;
        case SELF_UPDATE_INSTALLED: out.phase = pe::ui::AppPhase::installed; break;
        case SELF_UPDATE_CANCELLED: out.phase = pe::ui::AppPhase::cancelled; break;
        case SELF_UPDATE_FAILED: out.phase = pe::ui::AppPhase::failed; break;
        default: out.phase = pe::ui::AppPhase::idle; break;
        }
        out.app = job_app;
        out.fresh = job_fresh;
        out.done = status.done;
        out.total = status.total;
        out.version = status.version;
        out.error = status.error;
        // self_update.c's words when the payload loader did not take the helper.
        out.no_loader = out.phase == pe::ui::AppPhase::failed &&
                        out.error.rfind("The update helper couldn't be started", 0) == 0;
        const bool ended = out.phase == pe::ui::AppPhase::installed || out.phase == pe::ui::AppPhase::failed ||
                           out.phase == pe::ui::AppPhase::cancelled;
        if (ended && !job_told) {
            job_told = true;
            char line[600];
            std::snprintf(line, sizeof(line), "%s: %s%s%s", pe::ui::kApps[job_app].title,
                          out.phase == pe::ui::AppPhase::installed ? "installed " :
                          out.phase == pe::ui::AppPhase::cancelled ? "cancelled" : "failed: ",
                          out.phase == pe::ui::AppPhase::installed ? status.version : status.error,
                          status.folder[0] ? (std::string{" in "} + status.folder).c_str() : "");
            Report("apps", line);
            if (out.phase == pe::ui::AppPhase::installed) look = job_app;
        }
    }
    // The list says what is there now.
    if (look >= 0) Look(look);
    return out;
}

void Cancel() {
    const std::lock_guard guard(lock);
    if (begun) self_update_cancel(&job);
}

void Finish() {
    const std::lock_guard guard(lock);
    if (!begun) return;
    self_update_finish(&job);
    begun = false;
}

pe::ui::AppOpen Open(int app, std::string* reason) {
    if (app < 0 || app >= pe::ui::kAppCount) return pe::ui::AppOpen::refused;
    const std::string payload = AppFile("app-launch.elf");
    if (!FileExists(payload)) {
        *reason = "app-launch.elf is not in Nightshade's folder";
        Report("apps", ("Open " + std::string{pe::ui::kApps[app].title} + ": " + *reason).c_str());
        return pe::ui::AppOpen::refused;
    }
    self_update_channel channel{};
    if (self_update_open_payload(payload.c_str(), &channel) != 1) {
        Report("apps", ("Open " + std::string{pe::ui::kApps[app].title} + ": no payload loader took app-launch.elf").c_str());
        return pe::ui::AppOpen::no_loader;
    }
    const std::string answer = AskLaunch(channel, pe::ui::kApps[app].title, OwnTitle());
    channel.close(channel.user);
    Report("apps", ("Open " + std::string{pe::ui::kApps[app].title} + ": " +
                    (answer.empty() ? std::string{"the launch payload waits for Nightshade to close"} : answer))
                       .c_str());
    if (answer.empty()) return pe::ui::AppOpen::closing;
    *reason = answer;
    return answer == kNotListed ? pe::ui::AppOpen::not_listed : pe::ui::AppOpen::refused;
}

} // namespace Eden::Apps
