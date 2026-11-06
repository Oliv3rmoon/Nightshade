// SPDX-License-Identifier: GPL-3.0-or-later
#include "update_notice.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <pthread.h>
#include <string>

#include "diagnostics.h"
#include "insider_url.h"
#include "prosperoeden/version.h"
#include "storage_paths.h"
#include "update_check/github_release.h"
#include "update_check/insider.h"
#include "update_check/self_update.h"
#include "update_check/update_check.h"
#include "update_repository.h"
#ifdef EDEN_DEV_PROFILE
#include <fstream>
#endif

// The kit's paths, the app's own (update_check/eden_paths.h): after elevation /app0 and /download0
// are gone, so they follow storage_paths.h.
extern "C" const char* eden_self_update_path(int which) {
    static const std::string helper = Eden::AppFile("self-updater.elf");
    static const std::string param = Eden::AppFile("sce_sys/param.json");
    static const std::string sequence = Eden::ConfigFile("self-update-sequence");
    return which == 0 ? helper.c_str() : which == 1 ? param.c_str() : sequence.c_str();
}

namespace Eden::UpdateNotice {
namespace {
// libcurl and OpenSSL want more stack than a default thread of the console has.
constexpr std::size_t kStackSize = 1024 * 1024;
// Where the check asks (EDEN_UPDATE_REPOSITORY, headless/CMakeLists.txt): empty, the catalog;
// "owner/name", that GitHub repository's releases.
constexpr const char* kRepository = EDEN_UPDATE_REPOSITORY;
// The app's name as the update says it (the helper's notification: "Nightshade was updated to
// v1.005.100"): a build that asks a repository's releases is the Nightshade release line.
constexpr const char* kAppName = kRepository[0] != '\0' ? "Nightshade" : "ProsperoEden";
// The insider service (EDEN_INSIDER_URL, headless/CMakeLists.txt): empty, no insider builds at all.
constexpr const char* kInsiderUrl = EDEN_INSIDER_URL;
// The app's id at the service (nightly-insider's docs/API.md).
constexpr const char* kInsiderApp = "nightshade";

std::mutex lock;
bool started = false;
bool test_builds = false;  // Settings > Diagnostics > Test builds, as the app opened
bool found = false;  // an offer nobody has been told about yet
self_update_check_result answer = SELF_UPDATE_UNKNOWN;
self_update_offer offer{};
self_update_job job{};  // zero until the first Begin, as the kit asks
bool begun = false;
// Insider builds: the kept code (read once), and what the service said at this launch's check.
bool code_read = false;
std::string code;
insider_status insider_answer = INSIDER_NOT_ASKED;

std::string CodeFile() { return ConfigFile("insider-code"); }

// The kept code: the file holds it and a line end, nothing else (insider_code_read). With `lock` held.
const std::string& KeptCode() {
    if (!code_read && kInsiderUrl[0] != '\0') {
        code_read = true;
        char text[16];
        std::size_t length = 0;
        if (FILE* file = std::fopen(CodeFile().c_str(), "rb")) {
            length = std::fread(text, 1, sizeof(text), file);
            std::fclose(file);
        }
        char kept[INSIDER_CODE_LENGTH + 1];
        if (insider_code_read(text, length, kept)) code = kept;
    }
    return code;
}

// The service's answer as the log says it, on one line: never the code, never the download's token
// (its host and the build's file name).
std::string InsiderLine(const insider_report& report) {
    std::string line = std::string("insider: status=") +
                       (report.answer[0] ? report.answer : insider_status_text(report.status)) +
                       " http=" + std::to_string(report.http_status);
    if (report.platform_error) line += " error=" + std::to_string(report.platform_error);
    if (report.status == INSIDER_REFUSED)
        line += std::string(" refused=") + (report.why ? report.why : "?");
    else if (report.answer[0] && std::strcmp(report.answer, insider_status_text(report.status)) != 0)
        line += std::string(" result=") + insider_status_text(report.status);
    if (report.tag[0]) line += std::string(" tag=") + report.tag;
    if (report.host[0]) line += std::string(" host=") + report.host + " file=" + (report.file[0] ? report.file : "-");
    return line;
}

const char* ResultName(self_update_check_result result) {
    static const char* const names[] = {"available", "up-to-date", "unknown", "untrusted", "not-installable"};
    return static_cast<unsigned>(result) < 5 ? names[result] : "?";
}

#ifdef EDEN_DEV_PROFILE
// Development: update-offer.txt in the app folder replaces the check's answer (it skips the
// catalog's signature, or GitHub's SHA256SUMS), so the update can be tried before a newer release
// is out. Five lines, as the boilerplate's example takes them: the new content version, the
// release's name, its ZIP on GitHub, its SHA-256, its size in bytes; any further lines are the
// release notes.
bool DevelopmentOffer(self_update_offer& out) {
    std::ifstream file(AppFile("update-offer.txt"));
    std::string available, version, artifact, sha256, size;
    if (!file || !std::getline(file, available) || !std::getline(file, version) || !std::getline(file, artifact) ||
        !std::getline(file, sha256) || !std::getline(file, size))
        return false;
    self_update_offer filled{};
    if (!update_check_read_param(AppFile("sce_sys/param.json").c_str(), filled.title, filled.installed)) return false;
    std::snprintf(filled.name, sizeof(filled.name), "%s", kAppName);
    std::snprintf(filled.available, sizeof(filled.available), "%s", available.c_str());
    std::snprintf(filled.version, sizeof(filled.version), "%s", version.c_str());
    std::snprintf(filled.artifact, sizeof(filled.artifact), "%s", artifact.c_str());
    std::snprintf(filled.sha256, sizeof(filled.sha256), "%s", sha256.c_str());
    filled.size = std::strtoull(size.c_str(), nullptr, 10);
    std::string notes, line;
    while (std::getline(file, line)) notes += (notes.empty() ? "" : "\n") + line;
    std::snprintf(filled.notes, sizeof(filled.notes), "%s", notes.c_str());
    // Like the catalog, only a newer version is offered (content versions compare as text).
    if (std::strcmp(filled.available, filled.installed) <= 0) return false;
    out = filled;
    return true;
}
#endif

void* Check(void*) {
    self_update_offer result{};
    github_release_report github{};
    insider_report insider{};
    insider.status = INSIDER_NOT_ASKED;
    // Test builds ask the repository's releases list, pre-releases with it; a build that asks the
    // catalog has none to offer, and asks as it always does.
    const bool tests = test_builds && kRepository[0] != '\0';
    // With Test builds on, a build that names the insider service and a kept code ask the service
    // first; when it offers nothing, the same check as below follows (insider_check_tests_from).
    std::string kept;
    if (test_builds && kInsiderUrl[0] != '\0') {
        const std::lock_guard guard(lock);
        kept = KeptCode();
    }
    // With no repository named, this is self_update_check_self(): the catalog, as before.
    self_update_check_result state =
        !kept.empty() ? self_update_check_self_insider(kInsiderUrl, kept.c_str(), kInsiderApp, kRepository, kAppName,
                                                       kAppVersion, &result, &insider, &github)
        : tests       ? self_update_check_self_tests(kRepository, kAppName, &result, &github)
                      : self_update_check_self_from(kRepository, kAppName, &result, &github);
    if (insider.status != INSIDER_NOT_ASKED) {
        Report("update check", InsiderLine(insider).c_str());
        const std::lock_guard guard(lock);
        insider_answer = insider.status;
    }
    const bool from_insider = insider.status == INSIDER_UPDATE;
    if (from_insider) {
        // The insider build: GitHub was not asked.
    } else if (tests) {
        char line[640];
        std::snprintf(line, sizeof(line),
                      "GitHub %s releases (test builds): reason=%s http=%d redirects=%d error=%d releases=%d tag=%s "
                      "prerelease=%d zip=%s",
                      kRepository, github_release_reason_text(github.reason), github.http_status, github.redirects,
                      github.platform_error, github.listed, github.tag[0] ? github.tag : "-", github.prerelease,
                      github.zip[0] ? github.zip : "-");
        Report("update check", line);
    } else if (kRepository[0] != '\0') {
        char line[640];
        std::snprintf(line, sizeof(line),
                      "GitHub %s releases/latest: reason=%s http=%d redirects=%d error=%d tag=%s zip=%s", kRepository,
                      github_release_reason_text(github.reason), github.http_status, github.redirects,
                      github.platform_error, github.tag[0] ? github.tag : "-", github.zip[0] ? github.zip : "-");
        Report("update check", line);
    } else if (test_builds) {
        Report("update check", "Test builds: this build asks homebrew.page, which has none; asking it as usual");
    }
#ifdef EDEN_DEV_PROFILE
    if (DevelopmentOffer(result)) {
        Report("update check", "Development: update-offer.txt replaces the check's answer");
        state = SELF_UPDATE_AVAILABLE;
    }
#endif
    // A test build is of the program's own line as well as of sce_sys/param.json's: an install of
    // 01.010 is never offered 01.011 (another title ID and folder) or 01.012 (the Switch 2 test line).
    // An insider build is a test build too.
    if ((tests || from_insider) && (state == SELF_UPDATE_AVAILABLE || state == SELF_UPDATE_NOT_INSTALLABLE) &&
        !github_release_same_line(result.available, kAppVersion)) {
        Report("update check", (std::string{"Test builds: "} + result.available + " is not of the program's line (" +
                                kAppVersion + "): not offered")
                                   .c_str());
        state = SELF_UPDATE_UNKNOWN;
    }
    // The program's own version counts too (issue 230): an install that replaced the program but
    // kept an older sce_sys/param.json read as that older version, and the version the console
    // already ran was offered at every start.
    int comparable = 0;
    if ((state == SELF_UPDATE_AVAILABLE || state == SELF_UPDATE_NOT_INSTALLABLE) &&
        update_check_version_compare(result.available, kAppVersion, &comparable) <= 0 && comparable) {
        Report("update check", (std::string{"sce_sys/param.json names "} + (result.installed[0] ? result.installed : "-") +
                                ", the program is " + kAppVersion + ": nothing newer to offer")
                                   .c_str());
        state = SELF_UPDATE_UP_TO_DATE;
    }
    char line[300];
    std::snprintf(line, sizeof(line), "result=%s installed=%s available=%s version=%s size=%llu%s",
                  ResultName(state), result.installed[0] ? result.installed : "-",
                  result.available[0] ? result.available : "-", result.version[0] ? result.version : "-",
                  static_cast<unsigned long long>(result.size), result.prerelease ? " test-build" : "");
    Report("update check", line);
    if (state == SELF_UPDATE_AVAILABLE || state == SELF_UPDATE_NOT_INSTALLABLE) {
        const std::lock_guard guard(lock);
        answer = state;
        offer = result;
        found = true;
    }
    return nullptr;
}

Phase FromKit(self_update_phase phase) {
    switch (phase) {
    case SELF_UPDATE_STARTING: return Phase::starting;
    case SELF_UPDATE_DOWNLOADING: return Phase::downloading;
    case SELF_UPDATE_UNPACKING: return Phase::unpacking;
    case SELF_UPDATE_READY: return Phase::ready;
    case SELF_UPDATE_APPLYING: return Phase::applying;
    case SELF_UPDATE_CANCELLED: return Phase::cancelled;
    case SELF_UPDATE_FAILED: return Phase::failed;
    default: return Phase::idle;
    }
}
} // namespace

const char* Source() { return kRepository[0] != '\0' ? "GitHub" : "homebrew.page"; }

void Start(bool tests) {
    {
        const std::lock_guard guard(lock);
        if (started) return;
        started = true;
        test_builds = tests;
    }
    // An insider build is downloaded from the service's own host; nothing else changes.
    if (kInsiderUrl[0] != '\0' && !self_update_allow_insider(kInsiderUrl))
        Report("update check", "insider: the service's address is not one the kit downloads from");
    pthread_attr_t attributes;
    if (pthread_attr_init(&attributes) != 0) return;
    pthread_attr_setstacksize(&attributes, kStackSize);
    pthread_attr_setdetachstate(&attributes, PTHREAD_CREATE_DETACHED);
    pthread_t thread;
    if (pthread_create(&thread, &attributes, Check, nullptr) != 0)
        Report("update check", "The thread could not start");
    pthread_attr_destroy(&attributes);
}

bool Take(Offer* out) {
    const std::lock_guard guard(lock);
    if (!found) return false;
    found = false;
    if (out) {
        out->installable = answer == SELF_UPDATE_AVAILABLE;
        out->version = offer.version[0] != '\0' ? offer.version : offer.available;
        out->available = offer.available;
        out->size = offer.size;
        out->notes = offer.notes;
        out->notes_truncated = offer.notes_truncated != 0;
        out->test_build = offer.prerelease != 0;
        out->title = offer.release_title;
    }
    return true;
}

bool Begin() {
    const std::lock_guard guard(lock);
    if (answer != SELF_UPDATE_AVAILABLE) return false;
    if (begun) self_update_finish(&job);
    begun = self_update_start(&job, self_update_console(), &offer) == 1;
    Report("update", begun ? ("Updating to " + std::string(offer.available) + (offer.prerelease ? " (a test build)" : "")).c_str()
                           : "The update could not begin");
    return begun;
}

Progress Poll() {
    Progress progress;
    const std::lock_guard guard(lock);
    if (!begun) return progress;
    self_update_status status{};
    self_update_poll(&job, &status);
    progress.phase = FromKit(status.phase);
    progress.done = status.done;
    progress.total = status.total;
    progress.time_left = status.time_left;
    progress.error = status.error;
    return progress;
}

void Cancel() {
    const std::lock_guard guard(lock);
    if (begun) self_update_cancel(&job);
}

bool Apply() {
    const std::lock_guard guard(lock);
    if (!begun) return false;
    const bool going = self_update_apply(&job) == 1;
    Report("update", going ? "Staged: the helper replaces the files once ProsperoEden has closed"
                           : "The helper did not take the go-ahead");
    return going;
}

void Finish() {
    const std::lock_guard guard(lock);
    if (!begun) return;
    self_update_finish(&job);
    begun = false;
}

bool InsiderAvailable() { return kInsiderUrl[0] != '\0'; }

bool InsiderCodeSet() {
    const std::lock_guard guard(lock);
    return !KeptCode().empty();
}

bool InsiderRefused() {
    const std::lock_guard guard(lock);
    return kInsiderUrl[0] != '\0' && insider_status_hints(insider_answer);
}

InsiderCode SetInsiderCode(const std::string& typed) {
    if (kInsiderUrl[0] == '\0') return InsiderCode::failed;
    const std::string file = CodeFile();
    // Nothing typed (spaces aside): no code is kept.
    if (typed.find_first_not_of(" \t\r\n") == std::string::npos) {
        if (std::remove(file.c_str()) != 0 && FileExists(file)) return InsiderCode::failed;
        const std::lock_guard guard(lock);
        code_read = true;
        code.clear();
        insider_answer = INSIDER_NOT_ASKED; // that answer was about the code that is gone
        return InsiderCode::cleared;
    }
    char normalised[INSIDER_CODE_LENGTH + 1];
    if (!insider_code_normalise(typed.c_str(), normalised)) return InsiderCode::invalid;
    // The code and a line end, written beside the file and moved over it.
    (void)mkdir(ConfigDir().c_str(), 0777);
    const std::string temporary = file + ".tmp";
    FILE* out = std::fopen(temporary.c_str(), "wb");
    if (!out) return InsiderCode::failed;
    bool written = std::fprintf(out, "%s\n", normalised) == INSIDER_CODE_LENGTH + 1;
    written = std::fclose(out) == 0 && written;
    if (!written || std::rename(temporary.c_str(), file.c_str()) != 0) {
        std::remove(temporary.c_str());
        return InsiderCode::failed;
    }
    const std::lock_guard guard(lock);
    code_read = true;
    code = normalised;
    insider_answer = INSIDER_NOT_ASKED;
    return InsiderCode::saved;
}
} // namespace Eden::UpdateNotice
