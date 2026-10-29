// SPDX-FileCopyrightText: Copyright 2026 Oliv3rmoon
// SPDX-License-Identifier: GPL-3.0-or-later
// Nightshade's bridge (PPSA99008) on the console: what bridge.h describes, one step at a time, on a
// short screen that says Nightshade has its own app now, what is happening and what went wrong.
//
// Each step that waits (the launch payload, GitHub, the install) runs on a thread of its own; the
// loop draws when something changed and reads the controller: Circle closes the bridge, Cross tries
// again after a failure. Without a screen (VideoOut refused) the steps run all the same, and a
// failure is told in a notification. The log is /download0/nightshade-bridge.log, in the bridge's own
// sandbox, and the kernel log.
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <functional>
#include <memory>
#include <pthread.h>
#include <string>
#include <unistd.h>
#include <utility>
#include <vector>

#include "bridge.h"
#include "pe/core/strings.hpp"
#include "pe/gfx/font.hpp"
#include "ps5_pad.hpp"
#include "ps5_system_language.hpp"
#include "screen.hpp"
#include "view.hpp"

extern "C" {
int sceSystemServiceHideSplashScreen();
int sceKernelDebugOutText(int channel, const char* text);
int sceKernelSendNotificationRequest(int device, void* request, std::size_t size, int blocking);
int eden_exit_app(void); // src/lifecycle.c
}

namespace {
using namespace Eden::Bridge;

constexpr const char* kAppFolder = "/app0";
constexpr const char* kLogFile = "/download0/nightshade-bridge.log";
constexpr const char* kLoaderPort = "9021";
// How long the launch payload may take to answer, how often the console is asked whether it lists
// Nightshade once it is installed, and for how long.
constexpr int kLaunchAnswerMs = 5000;
constexpr double kListedEvery = 4.0;
constexpr double kListedFor = 300.0;
// How long "Opening Nightshade" shows before the bridge closes, which the launch payload waits for.
constexpr double kClosingSeconds = 1.2;
// libcurl and OpenSSL want more stack than a default thread of the console has (headless/apps.cpp).
constexpr std::size_t kStackSize = 1024 * 1024;

std::FILE* log_file = nullptr;

void Log(const char* format, ...) {
    char line[1024];
    va_list arguments;
    va_start(arguments, format);
    std::vsnprintf(line, sizeof(line), format, arguments);
    va_end(arguments);
    char kernel[1100];
    std::snprintf(kernel, sizeof(kernel), "[nightshade-bridge] %s\n", line);
    (void)sceKernelDebugOutText(0, kernel);
    if (log_file) {
        std::fprintf(log_file, "%s\n", line);
        std::fflush(log_file);
    }
}

void Notify(const std::string& message) {
    struct Request {
        std::uint8_t reserved[45];
        char message[3075];
    };
    static Request request;
    std::memset(&request, 0, sizeof(request));
    std::snprintf(request.message, sizeof(request.message), "%s", message.c_str());
    (void)sceKernelSendNotificationRequest(0, &request, sizeof(request), 0);
}

double Now() {
    timespec now{};
    clock_gettime(CLOCK_MONOTONIC, &now);
    return static_cast<double>(now.tv_sec) + static_cast<double>(now.tv_nsec) / 1e9;
}

bool ReadFile(const std::string& path, std::string* out) {
    std::FILE* file = std::fopen(path.c_str(), "rb");
    if (!file) return false;
    out->clear();
    char buffer[65536];
    for (std::size_t count; (count = std::fread(buffer, 1, sizeof(buffer), file)) > 0;) out->append(buffer, count);
    std::fclose(file);
    return true;
}

// A step that waits, on a thread of its own with a stack curl can use. The result is taken once done.
template <typename Result>
class Task {
public:
    bool start(std::function<Result()> work) {
        auto state = std::make_shared<State>();
        state->work = std::move(work);
        auto* held = new std::shared_ptr<State>(state);
        pthread_attr_t attributes;
        pthread_t thread;
        bool started = false;
        if (pthread_attr_init(&attributes) == 0) {
            pthread_attr_setstacksize(&attributes, kStackSize);
            pthread_attr_setdetachstate(&attributes, PTHREAD_CREATE_DETACHED);
            started = pthread_create(&thread, &attributes, Run, held) == 0;
            pthread_attr_destroy(&attributes);
        }
        if (!started) {
            delete held;
            return false;
        }
        state_ = std::move(state);
        return true;
    }
    bool running() const { return state_ != nullptr; }
    bool done() const { return state_ != nullptr && state_->done.load(); }
    Result take() {
        Result result = std::move(state_->result);
        state_.reset();
        return result;
    }

private:
    struct State {
        std::function<Result()> work;
        Result result{};
        std::atomic<bool> done{false};
    };
    static void* Run(void* argument) {
        const std::unique_ptr<std::shared_ptr<State>> held(static_cast<std::shared_ptr<State>*>(argument));
        State& state = **held;
        state.result = state.work();
        state.done = true;
        return nullptr;
    }
    std::shared_ptr<State> state_;
};

// What asking the launch payload to open Nightshade came to.
struct OpenResult {
    bool no_loader = false;
    Opened opened = Opened::refused;
    std::string reason;
};

OpenResult Open(const std::vector<std::string>& arguments) {
    OpenResult result;
    self_update_channel channel{};
    const std::string payload = std::string{kAppFolder} + "/app-launch.elf";
    if (self_update_open_payload(payload.c_str(), &channel) != 1) {
        result.no_loader = true;
        return result;
    }
    // It answers at once.
    (void)self_update_channel_timeout(&channel, kLaunchAnswerMs);
    result.reason = Eden::Apps::AskLaunch(channel, kNightshade, kTitle, arguments);
    channel.close(channel.user);
    result.opened = Classify(result.reason);
    return result;
}

// The controller's buttons pressed since the last look (Cross, Circle); none without a controller.
struct Pad {
    std::int32_t handle = -1;
    std::uint32_t held = 0;
    bool open() {
        std::int32_t user = -1;
        if (scePadInit() < 0 || sceUserServiceInitialize(nullptr) < 0 || sceUserServiceGetInitialUser(&user) < 0)
            return false;
        handle = scePadOpen(user, ps5::pad::kPortTypeStandard, 0, nullptr);
        if (handle < 0) handle = scePadGetHandle(user, ps5::pad::kPortTypeStandard, 0);
        return handle >= 0;
    }
    std::uint32_t pressed() {
        if (handle < 0) return 0;
        ps5::pad::Data data{};
        if (scePadReadState(handle, &data) < 0) return 0;
        const std::uint32_t now = ps5::pad::is_usable(data) ? data.buttons : 0;
        const std::uint32_t pressed = now & ~held;
        held = now;
        return pressed;
    }
};

} // namespace

int main(int argc, char** argv) {
    log_file = std::fopen(kLogFile, "w");
    const Eden::ForwardedArgs forwarded = Eden::ParseForwardedArgs(argc, argv);
    const std::vector<std::string> arguments = LaunchArguments(forwarded);
    std::string title, version;
    const bool known = Eden::Apps::ReadParam(std::string{kAppFolder} + "/sce_sys/param.json", &title, &version);
    Log("bridge %s %s: %d argument(s); passes on %zu (game: %s%s)", known ? title.c_str() : "?",
        known ? version.c_str() : "?", argc, arguments.size(), forwarded.rom.empty() ? "none" : forwarded.rom.c_str(),
        forwarded.exit_after_game ? ", --exit-after-game" : "");

    // The player's language, and the launcher's font for it.
    const std::string_view language = ps5::i18n::current_language_tag();
    for (const std::string& candidate : pe::catalog_candidates(language)) {
        std::string po;
        if (ReadFile(std::string{kAppFolder} + "/ui/lang/" + candidate + ".po", &po)) {
            Log("language %s: %zu texts", candidate.c_str(), pe::catalog().load(po));
            break;
        }
    }
    pe::gfx::Font font;
    std::string font_data;
    const bool has_font = ReadFile(std::string{kAppFolder} + "/ui/fonts/montserrat-medium.pefont", &font_data) &&
                          font.load(font_data);
    if (has_font) UseFont(&font);
    Screen screen;
    Picture picture;
    const bool has_screen = has_font && screen.open();
    Log("screen: %s; font: %s", has_screen ? "yes" : "no", has_font ? "yes" : font.error().c_str());
    Pad pad;
    if (!pad.open()) Log("the controller could not be opened");

    const std::string label = known ? ReleaseTag(version).substr(1) : std::string{};
    const std::string name = std::string{kName} + (label.empty() ? "" : " " + label);
    View view;
    Task<OpenResult> opening;
    Task<Eden::Apps::Release> finding;
    self_update_job job{};
    bool job_running = false;
    double step_at = Now(), asked_at = 0.0;
    bool dirty = true;

    const auto go = [&](Step step, std::string status, std::string detail = {}) {
        view.step = step;
        view.status = std::move(status);
        view.detail = std::move(detail);
        view.progress = -1.0;
        step_at = Now();
        dirty = true;
        if (step == Step::failed && !has_screen) Notify(view.status + (view.detail.empty() ? "" : " " + view.detail));
    };
    const auto open_nightshade = [&] {
        if (!opening.start([arguments] { return Open(arguments); }))
            go(Step::failed, Say("{0} could not be opened: {1}", {kName, "the bridge's thread could not start"}));
        asked_at = Now();
    };
    const auto begin = [&] {
        go(Step::opening, Say("Opening {0}...", {kName}));
        open_nightshade();
    };
    begin();

    for (;;) {
        const double now = Now();
        const std::uint32_t pressed = pad.pressed();
        if (pressed & ps5::pad::kButtonCircle) {
            Log("closed by the player");
            if (job_running) {
                self_update_cancel(&job);
                self_update_finish(&job);
            }
            break;
        }
        if ((pressed & ps5::pad::kButtonCross) && view.step == Step::failed && !opening.running() && !finding.running()) {
            Log("trying again");
            begin();
        }

        switch (view.step) {
        case Step::opening:
        case Step::waiting:
            if (opening.done()) {
                const OpenResult result = opening.take();
                if (result.no_loader) {
                    Log("open: no payload loader took app-launch.elf");
                    go(Step::failed,
                       Say("No payload loader is listening on port {0}, so {1} can't be opened from here. Open it from the "
                           "PS5's home screen.",
                           {kLoaderPort, kName}));
                } else if (result.opened == Opened::closing) {
                    Log("open: the launch payload opens %s once the bridge has closed", kNightshade);
                    go(Step::closing, Say("Opening {0}. This app closes now.", {kName}));
                } else if (result.opened == Opened::refused) {
                    Log("open: %s", result.reason.c_str());
                    go(Step::failed, Say("{0} could not be opened: {1}", {kName, result.reason}));
                } else if (view.step == Step::waiting) {
                    if (now - step_at >= kListedFor) {
                        Log("open: still not listed after %.0f s", kListedFor);
                        go(Step::failed,
                           Say("The PS5 doesn't list {0} yet. The homebrew loader lists it after its next scan or a restart.",
                               {kName}));
                    }
                } else if (!known) {
                    Log("open: not listed, and the bridge's own version could not be read");
                    go(Step::failed, Say("{0} could not be installed: {1}", {kName, "the bridge's version could not be read"}));
                } else {
                    // Not listed: not installed (or installed and not listed yet, which the helper says).
                    Log("open: %s is not listed; installing %s from %s", kNightshade, version.c_str(), kRepository);
                    go(Step::finding, Say("Preparing"), name);
                    if (!finding.start([version] { return Check(self_update_console(), version); }))
                        go(Step::failed, Say("{0} could not be installed: {1}", {kName, "the bridge's thread could not start"}));
                }
            } else if (view.step == Step::waiting && !opening.running() && now - asked_at >= kListedEvery) {
                open_nightshade();
            }
            break;
        case Step::finding:
            if (finding.done()) {
                const Eden::Apps::Release release = finding.take();
                Log("release %s of %s: reason=%s http=%d error=%d sha256=%s", ReleaseTag(version).c_str(), kRepository,
                    github_release_reason_text(release.reason), release.report.http_status,
                    release.report.platform_error,
                    release.asset.sha256[0] ? (release.asset.from_sums ? "SHA256SUMS" : "digest") : "-");
                self_update_offer offer{};
                if (release.reason != GITHUB_RELEASE_NEWER) {
                    go(Step::failed, Say("{0} could not be installed: {1}", {kName, Problem(release.reason, ReleaseZip(version))}));
                } else if (!MakeOffer(release, version, &offer)) {
                    go(Step::failed, Say("{0} could not be installed: {1}", {kName, Say("GitHub's answer could not be read.")}));
                } else {
                    // (The kit clears the job itself as it starts, and again after a failure.)
                    job_running = self_update_start(&job, self_update_console(), &offer) == 1;
                    Log("install %s %s from %s: %s", kNightshade, offer.available, offer.artifact,
                        job_running ? "begun" : "could not begin");
                    if (job_running)
                        go(Step::installing, Say("Preparing"), name);
                    else
                        go(Step::failed, Say("{0} could not be installed: {1}", {kName, "the install could not begin"}));
                }
            }
            break;
        case Step::installing: {
            self_update_status status{};
            self_update_poll(&job, &status);
            const double progress = status.total > 0 ? static_cast<double>(status.done) / static_cast<double>(status.total) : -1.0;
            std::string headline = view.status;
            if (status.phase == SELF_UPDATE_DOWNLOADING) headline = Say("Downloading {0}", {name});
            else if (status.phase == SELF_UPDATE_UNPACKING) headline = Say("Unpacking");
            else if (status.phase == SELF_UPDATE_READY || status.phase == SELF_UPDATE_APPLYING) headline = Say("Installing");
            if (headline != view.status || (progress >= 0.0 && static_cast<int>(progress * 100.0) != static_cast<int>(view.progress * 100.0))) {
                view.status = headline;
                view.detail = status.phase == SELF_UPDATE_DOWNLOADING ? TimeLeft(status) : std::string{};
                view.progress = progress;
                dirty = true;
            }
            if (status.phase == SELF_UPDATE_INSTALLED || status.phase == SELF_UPDATE_FAILED ||
                status.phase == SELF_UPDATE_CANCELLED) {
                self_update_finish(&job);
                job_running = false;
                const std::string error = status.error;
                if (status.phase == SELF_UPDATE_INSTALLED || AlreadyInstalled(error)) {
                    Log("install: %s", status.phase == SELF_UPDATE_INSTALLED
                                           ? (std::string{"installed "} + status.version + " in " + status.folder).c_str()
                                           : error.c_str());
                    go(Step::waiting, Say("Waiting for the console to list {0}", {kName}),
                       Say("{0} is installed. The homebrew loader lists it after its next scan or a restart.", {kName}));
                    asked_at = 0.0;
                } else if (error.rfind("The update helper couldn't be started", 0) == 0) {
                    Log("install: %s", error.c_str());
                    go(Step::failed, Say("No payload loader is listening on port {0}, so {1} can't be installed.",
                                         {kLoaderPort, kName}));
                } else {
                    Log("install: %s", status.phase == SELF_UPDATE_CANCELLED ? "cancelled" : error.c_str());
                    go(Step::failed, Say("{0} could not be installed: {1}", {kName, error.empty() ? "cancelled" : error}));
                }
            }
            break;
        }
        case Step::closing:
            if (now - step_at >= kClosingSeconds) {
                Log("closing: the launch payload opens %s now", kNightshade);
                if (log_file) std::fclose(log_file);
                log_file = nullptr;
                (void)eden_exit_app();
                return 0;
            }
            break;
        case Step::failed:
            // Without a screen nobody can try again: the notification said why.
            if (!has_screen && now - step_at >= 8.0) {
                Log("closing: failed without a screen");
                if (log_file) std::fclose(log_file);
                log_file = nullptr;
                return 1;
            }
            break;
        }

        // The piece that goes back and forth moves while something is waited for.
        const bool moving = view.step != Step::failed && view.step != Step::closing && view.progress < 0.0;
        if (has_screen && (dirty || moving)) {
            Draw(picture, font, view, now);
            // The console's launch picture stays until the first picture of the bridge is shown.
            static bool splash_hidden = false;
            if (screen.show(picture) && !splash_hidden) {
                (void)sceSystemServiceHideSplashScreen();
                splash_hidden = true;
            }
            dirty = false;
        } else {
            usleep(16000);
        }
    }
    if (log_file) std::fclose(log_file);
    log_file = nullptr;
    return 0;
}
