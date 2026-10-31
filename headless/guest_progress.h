// SPDX-License-Identifier: GPL-3.0-or-later
// Eden PS5 port: what the guest is doing while its frames keep coming (stderr.log).
//
// A Tears of the Kingdom session on v1.010.007 stopped 71 minutes in, on what its draw counts say
// was a loading screen (its last textures were converted on frames of 383 draws, headless/
// astc_policy.h), and stayed there for ten minutes until the tester closed it. The game handed in a frame every 33 ms the whole time, at a steadier 30 FPS
// than in play; the pool had 1.8 GB free, nothing waited for memory, no write was refused and no
// graphics stall began. So the GPU, the presenter and the memory service were all running, and
// what stopped was on the CPU side: a guest thread that the load waits for. The log could not say
// which of these it was:
//
//   - an emulated core held in the emulator's own code (a system call's handler, a lock, the
//     memory service's write fault) so that the guest threads it runs never run again;
//   - an emulated core in the guest's code for good (the game in a loop, or a fault handler that
//     never returns to it);
//   - a service request that never finished (a file read, an audio or GPU call), or one waiting
//     for its service's lock behind it;
//   - none of those: the game waiting on something of its own (another guest thread, or an event
//     only a service signals) that never comes.
//
// So every build now keeps, cheaply: each core's state (idle, in the guest's code, in a system
// call, in the emulator) with a count of its changes; each service request in progress (its
// service, command and calling thread, since when, and whether it has its service's lock yet);
// the requests finished per kind of service; each guest thread's last request; and whether the
// memory service's write fault handler is busy (src/guest_memory_service.inc). A watchdog thread
// looks once a second, while a game runs (headless/main.cpp):
//
//   EDEN_STUCK   at once, when a core has stayed in one busy state, a request has been in
//                progress, or the fault handler busy, for 10 s, and again at 60 s and 300 s. At
//                60 s it also prints the last service calls (EDEN_IPC_RING, headless/ipc_ring.h).
//   EDEN_PROGRESS every 30 s: each core's state and how often it changed, the requests finished
//                by kind (files, GPU, audio, the display, others), the oldest one in progress,
//                and the guest threads that made many requests and then none for 60 s or more.
//
// The next freeze then says which it was: a core stuck in a system call names the call; a request
// in progress names its service and command; and with none of those, which threads went quiet
// and which kinds of requests stopped (a load whose file reads stop, an audio thread that stops
// updating) is what is left to look at. tools/check-guest-progress.py runs these rules on the host.
#pragma once
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string_view>
#include <thread>
#include "ipc_ring.h"

extern "C" unsigned eden_guest_fault_busy(long long* since_ns) __attribute__((weak));

namespace Eden::Progress {

inline long long NowNs() noexcept {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

// ---- The emulated cores (headless/CMakeLists.txt, PhysicalCore) ----

enum CoreState : unsigned { kIdle = 0, kGuest = 1, kHost = 2, kSvc = 3 };
inline constexpr std::size_t kCores = 4;

struct alignas(64) Core {
    std::atomic<unsigned> state{kIdle};
    std::atomic<unsigned> svc{0};
    std::atomic<std::uint64_t> thread{0};  // the guest thread it last entered
    std::atomic<std::uint64_t> changes{0}; // written by the core's own host thread only
};
// Function-local statics: inline variables of one header do not initialize in order on the PS5.
inline Core* Cores() noexcept {
    static Core cores[kCores];
    return cores;
}

inline void SetCore(std::size_t core, unsigned state, unsigned svc = 0) noexcept {
    if (core >= kCores) return;
    Core& entry = Cores()[core];
    entry.state.store(state, std::memory_order_relaxed);
    entry.svc.store(svc, std::memory_order_relaxed);
    entry.changes.store(entry.changes.load(std::memory_order_relaxed) + 1, std::memory_order_release);
}
// The core enters the guest's code for `thread` (RunThread's EnterContext).
inline void CoreGuest(std::size_t core, std::uint64_t thread) noexcept {
    if (core < kCores) Cores()[core].thread.store(thread, std::memory_order_relaxed);
    SetCore(core, kGuest);
}
// Back in the emulator (after ExitContext, and after an idle wait).
inline void CoreHost(std::size_t core) noexcept { SetCore(core, kHost); }
// Handling a system call of the guest thread it ran.
inline void CoreSvc(std::size_t core, unsigned svc) noexcept { SetCore(core, kSvc, svc); }
// Nothing to run: waiting for an interrupt (PhysicalCore::Idle).
inline void CoreIdle(std::size_t core) noexcept { SetCore(core, kIdle); }

// ---- Service requests (headless/CMakeLists.txt, ServiceFrameworkBase::HandleSyncRequest) ----

enum Kind : unsigned { kFiles = 0, kGpu, kAudio, kDisplay, kOther, kKinds };
inline unsigned KindOf(const char* service) noexcept {
    if (service == nullptr) return kOther;
    const std::string_view name{service};
    if (name.starts_with("IStorage") || name.starts_with("IFile") || name.starts_with("IDirectory") ||
        name.starts_with("fsp"))
        return kFiles;
    if (name.starts_with("nvdrv")) return kGpu;
    if (name.starts_with("IAudio") || name.starts_with("aud")) return kAudio;
    if (name.starts_with("IHOSBinderDriver") || name.starts_with("vi:") || name.starts_with("IApplicationDisplay"))
        return kDisplay;
    return kOther;
}
inline const char* KindName(unsigned kind) noexcept {
    static const char* const names[kKinds] = {"files", "gpu", "audio", "display", "other"};
    return kind < kKinds ? names[kind] : "?";
}

// One per host thread that handles requests (the service threads): written by that thread only.
inline constexpr unsigned kRequestSlots = 64;
struct alignas(64) RequestSlot {
    std::atomic<const char*> service{nullptr};
    std::atomic<unsigned> command{0};
    std::atomic<std::uint64_t> thread{0};
    std::atomic<long long> since{0};
    std::atomic<unsigned> state{0}; // 0 none, 1 waiting for its service's lock, 2 being handled
};
inline RequestSlot* RequestSlots() noexcept {
    static RequestSlot slots[kRequestSlots];
    return slots;
}
inline std::atomic<unsigned>& RequestSlotsTaken() noexcept {
    static std::atomic<unsigned> taken{0};
    return taken;
}
inline std::atomic<std::uint64_t>* Finished() noexcept {
    static std::atomic<std::uint64_t> finished[kKinds];
    return finished;
}
inline int MyRequestSlot() noexcept {
    thread_local int slot = -2;
    if (slot == -2) {
        const unsigned taken = RequestSlotsTaken().fetch_add(1, std::memory_order_relaxed);
        slot = taken < kRequestSlots ? static_cast<int>(taken) : -1;
    }
    return slot;
}

// Each guest thread's last request: who went quiet. Open addressing by thread ID; a full table
// leaves later threads out.
inline constexpr unsigned kGuestThreads = 256;
struct GuestThread {
    std::atomic<std::uint64_t> id{0};
    std::atomic<const char*> service{nullptr};
    std::atomic<unsigned> command{0};
    std::atomic<long long> last{0};
    std::atomic<std::uint64_t> calls{0};
};
inline GuestThread* GuestThreads() noexcept {
    static GuestThread threads[kGuestThreads];
    return threads;
}
inline void NoteGuestThread(std::uint64_t id, const char* service, unsigned command, long long now) noexcept {
    if (id == 0) return;
    GuestThread* const table = GuestThreads();
    const unsigned start = static_cast<unsigned>((id * 0x9e3779b97f4a7c15ull) >> 56) % kGuestThreads;
    for (unsigned probe = 0; probe < 16; ++probe) {
        GuestThread& entry = table[(start + probe) % kGuestThreads];
        std::uint64_t seen = entry.id.load(std::memory_order_acquire);
        if (seen == 0 && entry.id.compare_exchange_strong(seen, id, std::memory_order_acq_rel)) seen = id;
        if (seen != id) continue;
        entry.service.store(service, std::memory_order_relaxed);
        entry.command.store(command, std::memory_order_relaxed);
        entry.last.store(now, std::memory_order_relaxed);
        entry.calls.fetch_add(1, std::memory_order_relaxed);
        return;
    }
}

// From the request's start to its reply. A nested request on the same host thread (a domain's
// object) puts the outer one back when it ends.
class RequestScope {
public:
    RequestScope(const char* service, unsigned command, std::uint64_t thread) noexcept
        : slot_{MyRequestSlot()}, service_{service}, command_{command}, thread_{thread}, start_{NowNs()} {
        if (slot_ < 0) return;
        RequestSlot& entry = RequestSlots()[slot_];
        outer_state_ = entry.state.load(std::memory_order_relaxed);
        outer_service_ = entry.service.load(std::memory_order_relaxed);
        outer_command_ = entry.command.load(std::memory_order_relaxed);
        outer_thread_ = entry.thread.load(std::memory_order_relaxed);
        outer_since_ = entry.since.load(std::memory_order_relaxed);
        Publish(entry, service, command, thread, start_, 1);
    }
    RequestScope(const RequestScope&) = delete;
    RequestScope& operator=(const RequestScope&) = delete;
    // It has its service's lock: from here it is being handled.
    void Locked() noexcept {
        if (slot_ >= 0) RequestSlots()[slot_].state.store(2, std::memory_order_release);
    }
    ~RequestScope() {
        if (slot_ >= 0)
            Publish(RequestSlots()[slot_], outer_service_, outer_command_, outer_thread_, outer_since_, outer_state_);
        Finished()[KindOf(service_)].fetch_add(1, std::memory_order_relaxed);
        NoteGuestThread(thread_, service_, command_, start_);
    }

private:
    static void Publish(RequestSlot& entry, const char* service, unsigned command, std::uint64_t thread,
                        long long since, unsigned state) noexcept {
        entry.state.store(0, std::memory_order_release);
        entry.service.store(service, std::memory_order_relaxed);
        entry.command.store(command, std::memory_order_relaxed);
        entry.thread.store(thread, std::memory_order_relaxed);
        entry.since.store(since, std::memory_order_relaxed);
        entry.state.store(state, std::memory_order_release);
    }
    int slot_;
    const char* service_;
    unsigned command_;
    std::uint64_t thread_;
    long long start_;
    unsigned outer_state_ = 0, outer_command_ = 0;
    const char* outer_service_ = nullptr;
    std::uint64_t outer_thread_ = 0;
    long long outer_since_ = 0;
};

// ---- The watchdog ----

// How often it looks, how often it prints EDEN_PROGRESS, and the ages EDEN_STUCK is printed at
// (milliseconds). The host check makes them short.
struct Timing {
    long long look_ms = 1000;
    long long progress_ms = 30000;
    long long stuck_ms[3] = {10000, 60000, 300000};
    long long quiet_ms = 60000;     // a thread with no request for this long is quiet...
    std::uint64_t quiet_calls = 100; // ...when it had made at least this many
};
inline Timing& Times() noexcept {
    static Timing timing;
    return timing;
}

inline const char* CoreStateName(unsigned state) noexcept {
    switch (state) {
    case kIdle: return "idle";
    case kGuest: return "guest";
    case kHost: return "emulator";
    case kSvc: return "svc";
    default: return "?";
    }
}

inline void Line(const char* text) noexcept {
    std::fputs(text, stderr);
    std::fflush(stderr);
}

// Which of the ages an incident of `age` ms has crossed since it was last reported (`reported`:
// how many of them were), and so whether to print it now.
inline bool Crossed(long long age, unsigned& reported) noexcept {
    const Timing& timing = Times();
    if (reported < 3 && age >= timing.stuck_ms[reported]) {
        while (reported < 3 && age >= timing.stuck_ms[reported]) ++reported;
        return true;
    }
    return false;
}

class Watchdog {
public:
    void Start() {
        Stop();
        for (unsigned kind = 0; kind < kKinds; ++kind) Finished()[kind].store(0, std::memory_order_relaxed);
        for (unsigned i = 0; i < kGuestThreads; ++i) {
            GuestThread& entry = GuestThreads()[i];
            entry.calls.store(0, std::memory_order_relaxed);
            entry.last.store(0, std::memory_order_relaxed);
            entry.service.store(nullptr, std::memory_order_relaxed);
            entry.id.store(0, std::memory_order_release);
        }
        {
            const std::lock_guard lock{mutex_};
            stop_ = false;
        }
        thread_ = std::thread([this] { Loop(); });
    }
    void Stop() {
        {
            const std::lock_guard lock{mutex_};
            stop_ = true;
        }
        wake_.notify_all();
        if (thread_.joinable()) thread_.join();
    }
    ~Watchdog() { Stop(); }

private:
    struct CoreSeen {
        std::uint64_t changes = ~std::uint64_t{0};
        long long same_since = 0;
        std::uint64_t window_start = 0;
        unsigned reported = 0;
    };
    struct RequestSeen {
        long long since = 0;
        unsigned reported = 0;
    };

    void Loop() {
        const long long begun = NowNs();
        long long next_progress = begun + Times().progress_ms * 1000000;
        CoreSeen cores[kCores];
        RequestSeen requests[kRequestSlots];
        long long fault_since_seen = 0;
        unsigned fault_reported = 0;
        bool ring_printed = false;
        std::uint64_t finished_before[kKinds] = {};
        for (std::size_t core = 0; core < kCores; ++core)
            cores[core].window_start = Cores()[core].changes.load(std::memory_order_acquire);
        std::unique_lock lock{mutex_};
        while (!stop_) {
            wake_.wait_for(lock, std::chrono::milliseconds(Times().look_ms), [this] { return stop_; });
            if (stop_) break;
            lock.unlock();
            const long long now = NowNs();
            bool stuck_long = false;
            char line[512];
            // The cores: one busy state with no change since the last look.
            for (std::size_t core = 0; core < kCores; ++core) {
                const Core& entry = Cores()[core];
                const std::uint64_t changes = entry.changes.load(std::memory_order_acquire);
                CoreSeen& seen = cores[core];
                if (changes != seen.changes) {
                    seen.changes = changes;
                    seen.same_since = now;
                    seen.reported = 0;
                    continue;
                }
                const unsigned state = entry.state.load(std::memory_order_relaxed);
                if (state == kIdle) continue;
                const long long age = (now - seen.same_since) / 1000000;
                if (!Crossed(age, seen.reported)) continue;
                stuck_long |= seen.reported >= 2;
                std::snprintf(line, sizeof(line),
                              "EDEN_STUCK core=%zu state=%s svc=0x%x thread=%llu seconds=%lld (%s)\n", core,
                              CoreStateName(state), entry.svc.load(std::memory_order_relaxed),
                              static_cast<unsigned long long>(entry.thread.load(std::memory_order_relaxed)),
                              age / 1000,
                              state == kGuest ? "in the guest's code, or a fault handler under it"
                              : state == kSvc ? "in this system call's handler"
                                              : "in the emulator between guest threads");
                Line(line);
            }
            // Requests in progress.
            const unsigned slots = std::min(RequestSlotsTaken().load(std::memory_order_relaxed), kRequestSlots);
            const char* oldest_service = nullptr;
            unsigned oldest_command = 0, oldest_state = 0, busy = 0;
            long long oldest_age = -1;
            for (unsigned i = 0; i < slots; ++i) {
                const RequestSlot& entry = RequestSlots()[i];
                const unsigned state = entry.state.load(std::memory_order_acquire);
                const long long since = entry.since.load(std::memory_order_relaxed);
                const char* service = entry.service.load(std::memory_order_relaxed);
                const unsigned command = entry.command.load(std::memory_order_relaxed);
                const std::uint64_t thread = entry.thread.load(std::memory_order_relaxed);
                RequestSeen& seen = requests[i];
                if (state == 0) {
                    seen = {};
                    continue;
                }
                ++busy;
                if (since != seen.since) seen = {since, 0};
                const long long age = (now - since) / 1000000;
                if (age > oldest_age) {
                    oldest_age = age;
                    oldest_service = service;
                    oldest_command = command;
                    oldest_state = state;
                }
                if (!Crossed(age, seen.reported)) continue;
                stuck_long |= seen.reported >= 2;
                std::snprintf(line, sizeof(line),
                              "EDEN_STUCK request service=%s command=%u thread=%llu seconds=%lld (%s)\n",
                              service ? service : "?", command, static_cast<unsigned long long>(thread), age / 1000,
                              state == 1 ? "waiting for its service's lock" : "being handled");
                Line(line);
            }
            // The memory service's write fault handler.
            long long fault_since = 0;
            const unsigned faulting = eden_guest_fault_busy ? eden_guest_fault_busy(&fault_since) : 0;
            if (faulting == 0 || fault_since != fault_since_seen) {
                fault_since_seen = faulting ? fault_since : 0;
                fault_reported = 0;
            }
            if (faulting != 0) {
                const long long age = (now - fault_since) / 1000000;
                if (Crossed(age, fault_reported)) {
                    stuck_long |= fault_reported >= 2;
                    std::snprintf(line, sizeof(line),
                                  "EDEN_STUCK fault_handler threads=%u seconds=%lld (a write of the guest's memory)\n",
                                  faulting, age / 1000);
                    Line(line);
                }
            }
            if (stuck_long && !ring_printed) {
                ring_printed = true;
                ::Eden::IpcRing::Dump(0, [](const char* text) {
                    char ring[256];
                    std::snprintf(ring, sizeof(ring), "%s\n", text);
                    Line(ring);
                });
            }
            if (now < next_progress) {
                lock.lock();
                continue;
            }
            next_progress = now + Times().progress_ms * 1000000;
            // EDEN_PROGRESS: cores, finished requests by kind, the oldest one in progress, quiet threads.
            int at = std::snprintf(line, sizeof(line), "EDEN_PROGRESS cores=");
            for (std::size_t core = 0; core < kCores && at > 0 && at < int(sizeof(line)); ++core) {
                const Core& entry = Cores()[core];
                const std::uint64_t changes = entry.changes.load(std::memory_order_acquire);
                at += std::snprintf(line + at, sizeof(line) - at, "%s%s:%llu", core ? "," : "",
                                    CoreStateName(entry.state.load(std::memory_order_relaxed)),
                                    static_cast<unsigned long long>(changes - cores[core].window_start));
                cores[core].window_start = changes;
            }
            if (at > 0 && at < int(sizeof(line))) at += std::snprintf(line + at, sizeof(line) - at, " finished=");
            for (unsigned kind = 0; kind < kKinds && at > 0 && at < int(sizeof(line)); ++kind) {
                const std::uint64_t finished = Finished()[kind].load(std::memory_order_relaxed);
                at += std::snprintf(line + at, sizeof(line) - at, "%s%s:%llu", kind ? "," : "", KindName(kind),
                                    static_cast<unsigned long long>(finished - finished_before[kind]));
                finished_before[kind] = finished;
            }
            if (at > 0 && at < int(sizeof(line))) {
                if (oldest_age >= 0)
                    at += std::snprintf(line + at, sizeof(line) - at, " in_progress=%u oldest=%s:%u:%llds:%s", busy,
                                        oldest_service ? oldest_service : "?", oldest_command, oldest_age / 1000,
                                        oldest_state == 1 ? "lock" : "handler");
                else
                    at += std::snprintf(line + at, sizeof(line) - at, " in_progress=0");
            }
            if (at > 0 && at < int(sizeof(line))) at += std::snprintf(line + at, sizeof(line) - at, " quiet=");
            // The three quietest threads that had made many requests.
            const GuestThread* quiet[3] = {};
            long long quiet_age[3] = {-1, -1, -1};
            for (unsigned i = 0; i < kGuestThreads; ++i) {
                const GuestThread& entry = GuestThreads()[i];
                if (entry.id.load(std::memory_order_acquire) == 0 ||
                    entry.calls.load(std::memory_order_relaxed) < Times().quiet_calls)
                    continue;
                const long long age = (now - entry.last.load(std::memory_order_relaxed)) / 1000000;
                if (age < Times().quiet_ms) continue;
                for (int k = 0; k < 3; ++k) {
                    if (age <= quiet_age[k]) continue;
                    for (int m = 2; m > k; --m) {
                        quiet[m] = quiet[m - 1];
                        quiet_age[m] = quiet_age[m - 1];
                    }
                    quiet[k] = &entry;
                    quiet_age[k] = age;
                    break;
                }
            }
            if (quiet[0] == nullptr && at > 0 && at < int(sizeof(line)))
                at += std::snprintf(line + at, sizeof(line) - at, "none");
            for (int k = 0; k < 3 && quiet[k] && at > 0 && at < int(sizeof(line)); ++k) {
                const char* service = quiet[k]->service.load(std::memory_order_relaxed);
                at += std::snprintf(line + at, sizeof(line) - at, "%s%llu:%s:%u:%llds", k ? "," : "",
                                    static_cast<unsigned long long>(quiet[k]->id.load(std::memory_order_relaxed)),
                                    service ? service : "?", quiet[k]->command.load(std::memory_order_relaxed),
                                    quiet_age[k] / 1000);
            }
            if (at > 0 && at < int(sizeof(line))) {
                long long fault_age = faulting ? (now - fault_since) / 1000000000 : 0;
                std::snprintf(line + at, sizeof(line) - at, " fault_handler=%u:%llds\n", faulting, fault_age);
            } else {
                line[sizeof(line) - 2] = '\n';
                line[sizeof(line) - 1] = '\0';
            }
            Line(line);
            lock.lock();
        }
    }

    std::mutex mutex_;
    std::condition_variable wake_;
    bool stop_ = false;
    std::thread thread_;
};

inline Watchdog& TheWatchdog() {
    static Watchdog watchdog;
    return watchdog;
}
// A game runs (headless/main.cpp, after System::Run) / is being stopped (before the cores suspend,
// which would read as cores held in the emulator).
inline void Start() { TheWatchdog().Start(); }
inline void Stop() { TheWatchdog().Stop(); }

} // namespace Eden::Progress
