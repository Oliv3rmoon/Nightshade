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
//   EDEN_PROGRESS every 30 s: the time (UTC), each core's state and how often it changed, the
//                requests finished by kind (files, GPU, audio, the display, others), the oldest
//                one in progress, and the guest threads that made many requests and then none
//                for 60 s or more.
//
// The next freeze then says which it was: a core stuck in a system call names the call; a request
// in progress names its service and command; and with none of those, which threads went quiet
// and which kinds of requests stopped (a load whose file reads stop, an audio thread that stops
// updating) is what is left to look at. tools/check-guest-progress.py runs these rules on the host.
//
// v1.010.008 and v1.010.009 then caught one: TOTK froze 110 minutes into a boss fight with
// EDEN_STUCK core=0 state=guest thread=78, no request in progress and the fault handler idle,
// and the stop suspended the cores in 3 ms. So a guest thread ran the guest's own code for 10 s
// with no system call, and the line could not say where. Now a core reported in the guest's code
// is asked to stop for a moment (its JIT halts as it does for a preemption, and the same thread
// goes on at once: headless/guest_sample.h answers on the core's own thread), and it prints:
//
//   EDEN_STUCK_GUEST  the thread (name, priority), pc and lr as main+0x..., sp, fp, then
//                     EDEN_STUCK_REGS, _FRAMES (the frame pointer chain), _CODE (the words around
//                     pc: the loop it spins in) and _DATA (the memory its pointer registers show);
//   EDEN_STUCK_PC     four more looks a second apart: a loop keeps to a few addresses;
//   EDEN_STUCK_THREAD with the first two reports of a session, every thread of the game: state and
//                     what it waits on (the lock's owner, the address), priority, core, and the
//                     saved pc, lr and return chain, to name the thread the spinning one waits for;
//   EDEN_STUCK_NO_ANSWER  a core that did not stop when asked: its host thread is held inside the
//                     emulator under the guest's code (a memory callback, a GPU flush, a fault
//                     handler), not in a guest loop.
//
// The looks' own stops are not changes (Core::moved), so a core that keeps spinning is still
// reported at 10, 60 and 300 s. A stuck write fault names its address and what it waits for.
#pragma once
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string_view>
#include <thread>
#include "ipc_ring.h"
#include "log_stamp.h"

extern "C" unsigned eden_guest_fault_busy(long long* since_ns) __attribute__((weak));
extern "C" std::uintptr_t eden_guest_fault_detail(unsigned* stage, unsigned* slot_state) __attribute__((weak));

namespace Eden::Progress {

inline long long NowNs() noexcept {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

// ---- The emulated cores (headless/CMakeLists.txt, PhysicalCore) ----

enum CoreState : unsigned { kIdle = 0, kGuest = 1, kHost = 2, kSvc = 3 };
inline constexpr std::size_t kCores = 4;

// A look the watchdog asks of a core in the guest's code (headless/guest_sample.h answers it).
enum Look : unsigned { kLookNone = 0, kLookPc = 1, kLookFull = 2, kLookThreads = 3 };

struct alignas(64) Core {
    std::atomic<unsigned> state{kIdle};
    std::atomic<unsigned> svc{0};
    std::atomic<std::uint64_t> thread{0};  // the guest thread it last entered
    std::atomic<std::uint64_t> changes{0}; // written by the core's own host thread only
    // The changes that were not a look's own stop and the same thread's return (EDEN_STUCK reads
    // these). Written by the core's own host thread only, as `own` and `own_thread` are: 2, the
    // next CoreHost is the look's; 1, the next CoreGuest of own_thread is.
    std::atomic<std::uint64_t> moved{0};
    std::atomic<unsigned> own{0};
    std::atomic<std::uint64_t> own_thread{0};
    // The look wanted (Look; the core takes it), the looks answered, and how to ask: a function
    // that halts the core's JIT, and its PhysicalCore (both cleared while no watchdog runs).
    std::atomic<unsigned> want{kLookNone};
    std::atomic<std::uint64_t> answers{0};
    std::atomic<void (*)(void*)> poke{nullptr};
    std::atomic<void*> poke_self{nullptr};
};
// Function-local statics: inline variables of one header do not initialize in order on the PS5.
inline Core* Cores() noexcept {
    static Core cores[kCores];
    return cores;
}

// `own`: the change is a look's (EDEN_STUCK_GUEST), not the core's: the state stays as it was.
inline void SetCore(std::size_t core, unsigned state, unsigned svc = 0, bool own = false) noexcept {
    if (core >= kCores) return;
    Core& entry = Cores()[core];
    if (!own) {
        entry.state.store(state, std::memory_order_relaxed);
        entry.svc.store(svc, std::memory_order_relaxed);
        entry.moved.store(entry.moved.load(std::memory_order_relaxed) + 1, std::memory_order_release);
    }
    entry.changes.store(entry.changes.load(std::memory_order_relaxed) + 1, std::memory_order_release);
}
// The core enters the guest's code for `thread` (RunThread's EnterContext).
inline void CoreGuest(std::size_t core, std::uint64_t thread) noexcept {
    if (core >= kCores) return;
    Core& entry = Cores()[core];
    const bool own = entry.own.load(std::memory_order_relaxed) == 1 &&
                     entry.own_thread.load(std::memory_order_relaxed) == thread;
    entry.own.store(0, std::memory_order_relaxed);
    entry.thread.store(thread, std::memory_order_relaxed);
    SetCore(core, kGuest, 0, own);
}
// Back in the emulator (after ExitContext, and after an idle wait).
inline void CoreHost(std::size_t core) noexcept {
    if (core >= kCores) return;
    Core& entry = Cores()[core];
    const bool own = entry.own.load(std::memory_order_relaxed) == 2;
    entry.own.store(own ? 1 : 0, std::memory_order_relaxed);
    SetCore(core, kHost, 0, own);
}
// Handling a system call of the guest thread it ran.
inline void CoreSvc(std::size_t core, unsigned svc) noexcept {
    if (core < kCores) Cores()[core].own.store(0, std::memory_order_relaxed);
    SetCore(core, kSvc, svc);
}
// Nothing to run: waiting for an interrupt (PhysicalCore::Idle).
inline void CoreIdle(std::size_t core) noexcept {
    if (core < kCores) Cores()[core].own.store(0, std::memory_order_relaxed);
    SetCore(core, kIdle);
}

// ---- Looks into a core held in the guest's code (headless/CMakeLists.txt, PhysicalCore) ----

// How the watchdog stops the core's JIT for a look (EnterContext, before CoreGuest): `poke(self)`
// halts it as a preemption does. Cheap enough for every entry.
inline void Pokeable(std::size_t core, void (*poke)(void*), void* self) noexcept {
    if (core >= kCores) return;
    Core& entry = Cores()[core];
    if (entry.poke_self.load(std::memory_order_relaxed) == self && entry.poke.load(std::memory_order_relaxed) == poke)
        return;
    entry.poke_self.store(self, std::memory_order_release);
    entry.poke.store(poke, std::memory_order_release);
}
// After the JIT stopped (after ExitContext, before CoreHost): the look wanted of this core, taken.
inline unsigned LookWanted(std::size_t core) noexcept {
    if (core >= kCores) return kLookNone;
    Core& entry = Cores()[core];
    if (entry.want.load(std::memory_order_relaxed) == kLookNone) return kLookNone;
    return entry.want.exchange(kLookNone, std::memory_order_acq_rel);
}
// The look was printed: this stop (the CoreHost that follows) and `thread`'s return to the guest's
// code (the CoreGuest after it, with nothing between) are the look's, not the core's. A look that
// waited for a kernel lock may end with its thread on another core (`same_core` false): then the
// core it was asked of has run other threads meanwhile, and only the answer is counted.
inline void LookAnswered(std::size_t core, std::uint64_t thread, bool same_core = true) noexcept {
    if (core >= kCores) return;
    Core& entry = Cores()[core];
    if (same_core) {
        entry.own_thread.store(thread, std::memory_order_relaxed);
        entry.own.store(2, std::memory_order_relaxed);
    }
    entry.answers.fetch_add(1, std::memory_order_release);
}

// The game's modules where they are loaded, for addresses as main+0x... (guest_sample.h fills it).
struct GuestModule {
    std::uint64_t base = 0, end = 0;
    char name[32] = {};
};
inline constexpr unsigned kGuestModules = 24;
struct GuestModules {
    GuestModule entry[kGuestModules];
    unsigned count = 0;
};
// The modules the NSO loader mapped into the game's process (headless/CMakeLists.txt, nso.cpp), by
// name. Core::FindModules names a module by the path its read-only data starts with, and Tears of
// the Kingdom's carry none: v1.010.010's looks printed bare addresses (main+0x6E1404 read as
// 0x80a03404). Written by the loader, read by a look on a core's thread. A module whose name the
// table already has starts a new process's list (a new process may lie where the last one did).
struct LoadedModuleTable {
    std::mutex mutex;
    const void* process = nullptr;
    GuestModules modules;
};
inline LoadedModuleTable& LoadedTable() noexcept {
    static LoadedModuleTable table;
    return table;
}
inline void NoteModule(const void* process, const char* name, std::uint64_t base, std::uint64_t size) noexcept {
    LoadedModuleTable& table = LoadedTable();
    const std::lock_guard lock{table.mutex};
    const char* const named = name && name[0] ? name : "?";
    bool again = table.process != process;
    for (unsigned i = 0; i < table.modules.count && !again; ++i)
        again = std::strncmp(table.modules.entry[i].name, named, sizeof(table.modules.entry[i].name) - 1) == 0;
    if (again) {
        table.process = process;
        table.modules.count = 0;
    }
    if (table.modules.count >= kGuestModules) return;
    GuestModule& entry = table.modules.entry[table.modules.count++];
    entry.base = base;
    entry.end = base + size;
    std::snprintf(entry.name, sizeof(entry.name), "%s", named);
}
// The loader's modules of `process` (none for another process).
inline GuestModules LoadedModules(const void* process) noexcept {
    LoadedModuleTable& table = LoadedTable();
    const std::lock_guard lock{table.mutex};
    return table.process == process ? table.modules : GuestModules{};
}

// "main+0x1a2b3c" for an address in a module, else "0x..." (always terminated).
inline void Where(char* out, std::size_t size, const GuestModules& modules, std::uint64_t address) noexcept {
    for (unsigned i = 0; i < modules.count && i < kGuestModules; ++i) {
        const GuestModule& module = modules.entry[i];
        if (address >= module.base && address < module.end) {
            std::snprintf(out, size, "%s+0x%llx", module.name[0] ? module.name : "?",
                          static_cast<unsigned long long>(address - module.base));
            return;
        }
    }
    std::snprintf(out, size, "0x%llx", static_cast<unsigned long long>(address));
}

// What a look found (guest_sample.h): the thread's registers, and with a full look the words around
// pc, the frame pointer chain's return addresses and the memory some registers point at.
struct GuestLook {
    std::size_t core = 0;
    std::uint64_t thread = 0;
    const char* name = "-";
    int priority = 0;
    std::uint64_t x[31] = {}; // x0-x28, fp (x29), lr (x30)
    std::uint64_t sp = 0, pc = 0;
    std::uint32_t pstate = 0;
    std::uint64_t code_from = 0;
    std::uint32_t code[32] = {};
    unsigned code_words = 0;
    std::uint64_t frames[12] = {};
    unsigned frame_count = 0;
    struct Data {
        unsigned reg = 0;
        std::uint64_t at = 0;
        std::uint64_t words[8] = {};
    } data[16];
    unsigned data_count = 0;
};
// The registers whose memory a full look shows (arguments, the indirect result, callee-saved).
inline constexpr unsigned kDataRegs[] = {0, 1, 2, 3, 8, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28};

class Text {
public:
    __attribute__((format(printf, 2, 3))) void Add(const char* format, ...) noexcept {
        if (at_ >= sizeof(line_) - 1) return;
        va_list args;
        va_start(args, format);
        const int wrote = std::vsnprintf(line_ + at_, sizeof(line_) - at_, format, args);
        va_end(args);
        if (wrote > 0) at_ = std::min(sizeof(line_) - 1, at_ + static_cast<std::size_t>(wrote));
    }
    void Where(const GuestModules& modules, std::uint64_t address) noexcept {
        char where[64];
        ::Eden::Progress::Where(where, sizeof(where), modules, address);
        Add("%s", where);
    }
    // Ends the line and writes it to stderr.log.
    void Print() noexcept;

private:
    char line_[1536] = {};
    std::size_t at_ = 0;
};

// The first lines of a look: the thread and where it is (EDEN_STUCK_GUEST, or EDEN_STUCK_PC for a
// look at pc and lr only), then its registers, its return chain and the code around pc.
inline void PrintLook(const GuestLook& look, const GuestModules& modules, unsigned kind) noexcept;
// The memory the pointer registers show (after the first lines: reading it can be slow).
inline void PrintLookData(const GuestLook& look) noexcept;

// One guest thread of the game, from the threads' list (guest_sample.h): its saved context, so a
// thread running on another core shows where it last switched out.
struct GuestThreadRow {
    std::uint64_t id = 0;
    const char* name = "-";
    const char* state = "?";
    int priority = 0, base_priority = 0, core = -1, running = -1;
    std::uint32_t suspend = 0;
    std::uint64_t owner = 0, key = 0, arbiter = 0;
    std::uint64_t pc = 0, lr = 0, sp = 0;
    std::uint64_t frames[6] = {};
    unsigned frame_count = 0;
};
inline void PrintThreadsHeader(std::size_t core, std::size_t count) noexcept;
inline void PrintThreadRow(const GuestThreadRow& row, const GuestModules& modules) noexcept;

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
    long long answer_ms = 2000;     // a look not answered by then: EDEN_STUCK_NO_ANSWER
    unsigned pc_looks = 4;          // pc-only looks after an incident's first report, one a look
    unsigned thread_lists = 2;      // reports a session that also list every guest thread
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

inline void Text::Print() noexcept {
    line_[at_ < sizeof(line_) - 1 ? at_ : sizeof(line_) - 2] = '\n';
    line_[at_ < sizeof(line_) - 1 ? at_ + 1 : sizeof(line_) - 1] = '\0';
    Line(line_);
    at_ = 0;
    line_[0] = '\0';
}

inline void PrintLook(const GuestLook& look, const GuestModules& modules, unsigned kind) noexcept {
    const unsigned long long thread = look.thread;
    Text text;
    if (kind == kLookPc) {
        text.Add("EDEN_STUCK_PC core=%zu thread=%llu pc=", look.core, thread);
        text.Where(modules, look.pc);
        text.Add(" lr=");
        text.Where(modules, look.x[30]);
        text.Print();
        return;
    }
    text.Add("EDEN_STUCK_GUEST core=%zu thread=%llu name=%s prio=%d pc=", look.core, thread,
             look.name && look.name[0] ? look.name : "-", look.priority);
    text.Where(modules, look.pc);
    text.Add(" lr=");
    text.Where(modules, look.x[30]);
    text.Add(" sp=0x%llx fp=0x%llx pstate=0x%x (where the guest's code runs: a loop shows the same pc again)",
             static_cast<unsigned long long>(look.sp), static_cast<unsigned long long>(look.x[29]), look.pstate);
    text.Print();
    text.Add("EDEN_STUCK_REGS core=%zu", look.core);
    for (unsigned r = 0; r < 31; ++r) text.Add(" x%u=%llx", r, static_cast<unsigned long long>(look.x[r]));
    text.Print();
    text.Add("EDEN_STUCK_FRAMES core=%zu", look.core);
    for (unsigned i = 0; i < look.frame_count && i < 12; ++i) {
        text.Add(" ");
        text.Where(modules, look.frames[i]);
    }
    if (look.frame_count == 0) text.Add(" none");
    text.Print();
    text.Add("EDEN_STUCK_CODE core=%zu from=", look.core);
    text.Where(modules, look.code_from);
    text.Add(" pc_word=%llu:", static_cast<unsigned long long>((look.pc - look.code_from) / 4));
    for (unsigned i = 0; i < look.code_words && i < 32; ++i) text.Add(" %08x", look.code[i]);
    text.Print();
}

inline void PrintLookData(const GuestLook& look) noexcept {
    Text text;
    for (unsigned i = 0; i < look.data_count && i < 16; ++i) {
        const GuestLook::Data& data = look.data[i];
        text.Add("EDEN_STUCK_DATA core=%zu x%u=0x%llx:", look.core, data.reg, static_cast<unsigned long long>(data.at));
        for (const std::uint64_t word : data.words) text.Add(" %016llx", static_cast<unsigned long long>(word));
        text.Print();
    }
}

inline void PrintThreadsHeader(std::size_t core, std::size_t count) noexcept {
    Text text;
    text.Add("EDEN_STUCK_THREADS core=%zu count=%zu (each thread's saved context: one running on another core "
             "shows where it last switched out)",
             core, count);
    text.Print();
}

inline void PrintThreadRow(const GuestThreadRow& row, const GuestModules& modules) noexcept {
    Text text;
    text.Add("EDEN_STUCK_THREAD id=%llu name=%s state=%s prio=%d/%d core=%d", static_cast<unsigned long long>(row.id),
             row.name && row.name[0] ? row.name : "-", row.state ? row.state : "?", row.priority, row.base_priority,
             row.core);
    if (row.running >= 0) text.Add(" running=%d", row.running);
    if (row.suspend) text.Add(" suspend=0x%x", row.suspend);
    if (row.owner) text.Add(" lock_owner=%llu", static_cast<unsigned long long>(row.owner));
    if (row.key) text.Add(" key=0x%llx", static_cast<unsigned long long>(row.key));
    if (row.arbiter) text.Add(" arbiter=0x%llx", static_cast<unsigned long long>(row.arbiter));
    text.Add(" pc=");
    text.Where(modules, row.pc);
    text.Add(" lr=");
    text.Where(modules, row.lr);
    text.Add(" sp=0x%llx frames=", static_cast<unsigned long long>(row.sp));
    for (unsigned i = 0; i < row.frame_count && i < 6; ++i) {
        if (i) text.Add(",");
        text.Where(modules, row.frames[i]);
    }
    if (row.frame_count == 0) text.Add("none");
    text.Print();
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
        ForgetPokes();
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
        // The session's cores go away after the stop: nothing may poke them any more.
        ForgetPokes();
    }
    ~Watchdog() { Stop(); }

private:
    struct CoreSeen {
        std::uint64_t moved = ~std::uint64_t{0};
        long long same_since = 0;
        std::uint64_t window_start = 0;
        unsigned reported = 0;
        // A look asked of the core and not answered yet, and the pc-only looks still to ask.
        bool asking = false;
        long long asked_at = 0;
        std::uint64_t answers = 0;
        unsigned pc_looks = 0;
    };

    static void ForgetPokes() noexcept {
        for (std::size_t core = 0; core < kCores; ++core) {
            Core& entry = Cores()[core];
            entry.want.store(kLookNone, std::memory_order_relaxed);
            entry.poke.store(nullptr, std::memory_order_release);
            entry.poke_self.store(nullptr, std::memory_order_release);
        }
    }

    // Asks a core in the guest's code for a look; false when it cannot be asked.
    static bool Ask(std::size_t core, unsigned look, CoreSeen& seen, long long now) noexcept {
        Core& entry = Cores()[core];
        void* const self = entry.poke_self.load(std::memory_order_acquire);
        void (*const poke)(void*) = entry.poke.load(std::memory_order_acquire);
        if (poke == nullptr || self == nullptr) return false;
        seen.answers = entry.answers.load(std::memory_order_acquire);
        seen.asking = true;
        seen.asked_at = now;
        // A pc look the core has not taken yet becomes this look: it is asked again later.
        if (entry.want.exchange(look, std::memory_order_acq_rel) == kLookPc && look != kLookPc) ++seen.pc_looks;
        poke(self);
        return true;
    }
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
        unsigned thread_lists = 0;
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
            // The cores: one busy state with no change since the last look (a look's own stop is
            // not a change: Core::moved).
            for (std::size_t core = 0; core < kCores; ++core) {
                Core& entry = Cores()[core];
                const std::uint64_t moved = entry.moved.load(std::memory_order_acquire);
                CoreSeen& seen = cores[core];
                if (seen.asking && entry.answers.load(std::memory_order_acquire) != seen.answers) seen.asking = false;
                if (moved != seen.moved) {
                    seen.moved = moved;
                    seen.same_since = now;
                    seen.reported = 0;
                    seen.pc_looks = 0;
                    // A core that moves answers at its next stop; one asked long ago is asked no more.
                    if (seen.asking && now - seen.asked_at >= Times().answer_ms * 1000000) {
                        entry.want.store(kLookNone, std::memory_order_relaxed);
                        seen.asking = false;
                    }
                    continue;
                }
                const unsigned state = entry.state.load(std::memory_order_relaxed);
                if (state == kIdle) continue;
                const unsigned long long thread = entry.thread.load(std::memory_order_relaxed);
                if (seen.asking && now - seen.asked_at >= Times().answer_ms * 1000000) {
                    // It did not stop when asked, in the guest's code since before it was asked: not
                    // a guest loop, but the core's host thread held in the emulator under the guest's
                    // code. (One asked between two runs of the guest's code is only asked no more.)
                    entry.want.store(kLookNone, std::memory_order_relaxed);
                    seen.asking = false;
                    seen.pc_looks = 0;
                    if (state == kGuest && seen.same_since <= seen.asked_at) {
                        std::snprintf(line, sizeof(line),
                                      "EDEN_STUCK_NO_ANSWER core=%zu thread=%llu seconds=%lld (asked to stop for a "
                                      "look, it did not: its host thread is held in the emulator under the guest's "
                                      "code, in a memory callback, a GPU flush or a fault handler)\n",
                                      core, thread, (now - seen.asked_at) / 1000000000);
                        Line(line);
                    }
                }
                const long long age = (now - seen.same_since) / 1000000;
                const unsigned reported_before = seen.reported;
                if (!Crossed(age, seen.reported)) {
                    // The pc-only looks after an incident's first report, one a look.
                    if (state == kGuest && !seen.asking && seen.pc_looks > 0) {
                        --seen.pc_looks;
                        (void)Ask(core, kLookPc, seen, now);
                    }
                    continue;
                }
                stuck_long |= seen.reported >= 2;
                std::snprintf(line, sizeof(line),
                              "EDEN_STUCK core=%zu state=%s svc=0x%x thread=%llu seconds=%lld (%s)\n", core,
                              CoreStateName(state), entry.svc.load(std::memory_order_relaxed), thread,
                              age / 1000,
                              state == kGuest ? "in the guest's code, or a fault handler under it"
                              : state == kSvc ? "in this system call's handler"
                                              : "in the emulator between guest threads");
                Line(line);
                // In the guest's code: where (EDEN_STUCK_GUEST), and with the first reports of a
                // session every guest thread too, with where the other cores' threads run now (their
                // saved contexts are only where they last switched out).
                if (state == kGuest) {
                    const bool threads = thread_lists < Times().thread_lists;
                    if (Ask(core, threads ? kLookThreads : kLookFull, seen, now) && threads) {
                        ++thread_lists;
                        for (std::size_t other = 0; other < kCores; ++other)
                            if (other != core && !cores[other].asking &&
                                Cores()[other].state.load(std::memory_order_relaxed) == kGuest)
                                (void)Ask(other, kLookPc, cores[other], now);
                    }
                    if (reported_before == 0) seen.pc_looks += Times().pc_looks;
                }
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
                    // What the latest write to fault waits for (src/guest_memory_service.inc).
                    unsigned stage = 0, slot_state = 0;
                    const std::uintptr_t address =
                        eden_guest_fault_detail ? eden_guest_fault_detail(&stage, &slot_state) : 0;
                    std::snprintf(line, sizeof(line),
                                  "EDEN_STUCK fault_handler threads=%u seconds=%lld address=0x%llx stage=%s slot=0x%x "
                                  "(a write of the guest's memory)\n",
                                  faulting, age / 1000, static_cast<unsigned long long>(address),
                                  stage == 1   ? "slot_lock"
                                  : stage == 2 ? "making_writable"
                                  : stage == 3 ? "waiting_for_memory"
                                               : "?",
                                  slot_state);
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
            // EDEN_PROGRESS: the time (log_stamp.h), cores, finished requests by kind, the oldest one
            // in progress, quiet threads.
            char utc[LogStamp::kUtcSize];
            LogStamp::Utc(utc);
            int at = std::snprintf(line, sizeof(line), "EDEN_PROGRESS utc=%s cores=", utc);
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
