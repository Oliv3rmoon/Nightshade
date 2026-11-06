// SPDX-License-Identifier: GPL-3.0-or-later
// Eden PS5 port: a look into a core the freeze watchdog found in the guest's code (stderr.log).
//
// headless/guest_progress.h asks; PhysicalCore::RunThread answers here (headless/CMakeLists.txt),
// on the core's own host thread, after the JIT has stopped and its context is let go, and before
// the thread goes on: where a system call would be handled. Only a core reported stuck is asked,
// so nothing here runs while a game plays normally.
//
//   kLookPc       pc and lr only (EDEN_STUCK_PC)
//   kLookFull     the thread, its registers, return chain, the code around pc and the memory its
//                 pointer registers show (EDEN_STUCK_GUEST, _REGS, _FRAMES, _CODE, _DATA)
//   kLookThreads  that, and every thread of the game (EDEN_STUCK_THREADS, EDEN_STUCK_THREAD)
//
// The lines that read only the thread's context and code are printed first: the memory a register
// points at may be the GPU's to flush first, and a flush that never ends must not take the rest.
// The threads' states and lock owners are read under the scheduler's lock, their names and return
// chains (the guest's memory) after it.
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include "common/common_types.h"
#include "common/typed_address.h"
#include "core/arm/arm_interface.h"
#include "core/arm/debug.h"
#include "core/hle/kernel/k_light_lock.h"
#include "core/hle/kernel/k_process.h"
#include "core/hle/kernel/k_scheduler.h"
#include "core/hle/kernel/k_thread.h"
#include "core/hle/kernel/kernel.h"
#include "core/hle/kernel/svc_types.h"
#include "core/memory.h"
#include "common/page_table.h"
#include "guest_progress.h"
#include "totk_guard.h"

namespace Eden::Sample {

// The modules as the last full look found them, one copy per core (cores answer on their own).
inline Progress::GuestModules& ModulesOf(std::size_t core) {
    static Progress::GuestModules modules[Progress::kCores];
    return modules[core < Progress::kCores ? core : 0];
}

// The loader's list of the process's modules (Progress::NoteModule); Eden's own search, which
// reads each module's path, where the loader noted none.
inline void FindModules(Kernel::KProcess& process, Progress::GuestModules& out) {
    out = Progress::LoadedModules(&process);
    if (out.count != 0) return;
    for (const auto& [base, name] : Core::FindModules(&process)) {
        if (out.count >= Progress::kGuestModules) break;
        Progress::GuestModule& entry = out.entry[out.count++];
        entry.base = base;
        entry.end = GetInteger(Core::GetModuleEnd(&process, base)) + 1;
        std::snprintf(entry.name, sizeof(entry.name), "%s", name.c_str());
    }
}

// The return addresses of the frame pointer chain (AArch64 frame records: the previous fp, then lr;
// none for a 32-bit game).
inline unsigned Chain(Kernel::KProcess& process, std::uint64_t fp, std::uint64_t* out, unsigned most) {
    if (!process.Is64Bit()) return 0;
    Core::Memory::Memory& memory = process.GetMemory();
    unsigned count = 0;
    while (count < most && fp != 0 && (fp & 7) == 0 && memory.IsValidVirtualAddressRange(fp, 16)) {
        const std::uint64_t next = memory.Read64(fp);
        const std::uint64_t ret = memory.Read64(fp + 8);
        if (ret == 0) break;
        out[count++] = ret;
        if (next <= fp) break; // a chain goes up the stack
        fp = next;
    }
    return count;
}

inline const char* StateName(const Kernel::KThread& thread) {
    switch (thread.GetState()) {
    case Kernel::ThreadState::Initialized:
        return "initialized";
    case Kernel::ThreadState::Runnable:
        return "runnable";
    case Kernel::ThreadState::Terminated:
        return "terminated";
    case Kernel::ThreadState::Waiting:
        switch (thread.GetWaitReasonForDebugging()) {
        case Kernel::ThreadWaitReasonForDebugging::Sleep:
            return "waiting:sleep";
        case Kernel::ThreadWaitReasonForDebugging::IPC:
            return "waiting:ipc";
        case Kernel::ThreadWaitReasonForDebugging::Synchronization:
            return "waiting:sync";
        case Kernel::ThreadWaitReasonForDebugging::ConditionVar:
            return "waiting:condvar";
        case Kernel::ThreadWaitReasonForDebugging::Arbitration:
            return "waiting:arbitration";
        case Kernel::ThreadWaitReasonForDebugging::Suspended:
            return "waiting:suspended";
        default:
            return "waiting";
        }
    default:
        return "?";
    }
}

// Every thread of the game: what it waits on, and where it last switched out (the looked-at
// thread, `current`, where it is now: `live`).
inline void Threads(Kernel::KernelCore& kernel, Kernel::KProcess& process, const Progress::GuestModules& modules,
                    std::size_t core, const Kernel::KThread* current, const Kernel::Svc::ThreadContext& live) {
    struct Row {
        const Kernel::KThread* thread;
        Progress::GuestThreadRow row;
        std::uint64_t fp;
    };
    std::vector<Row> rows;
    rows.reserve(256);
    // Threads leave the list under its lock: held until the last row is printed.
    Kernel::KScopedLightLock list{process.GetListLock()};
    {
        Kernel::KScopedSchedulerLock scheduler{kernel};
        for (const Kernel::KThread& thread : process.GetThreadList()) {
            Row entry{&thread, {}, 0};
            Progress::GuestThreadRow& row = entry.row;
            row.id = thread.GetThreadId();
            row.state = StateName(thread);
            row.priority = thread.GetPriority();
            row.base_priority = thread.GetBasePriority();
            row.core = thread.GetActiveCore();
            row.suspend = thread.GetSuspendFlags();
            if (const Kernel::KThread* owner = thread.GetLockOwner(kernel)) row.owner = owner->GetThreadId();
            row.key = GetInteger(thread.GetAddressKey());
            row.arbiter = thread.GetAddressArbiterKey();
            for (std::size_t i = 0; i < Progress::kCores; ++i)
                if (kernel.Scheduler(i).GetSchedulerCurrentThread() == &thread) row.running = static_cast<int>(i);
            const Kernel::Svc::ThreadContext& context = &thread == current ? live : thread.GetContext();
            row.pc = context.pc;
            row.lr = context.lr;
            row.sp = context.sp;
            entry.fp = context.fp;
            rows.push_back(entry);
        }
    }
    Progress::PrintThreadsHeader(core, rows.size());
    for (Row& entry : rows) {
        const std::string name = Core::GetThreadName(entry.thread).value_or("-");
        entry.row.name = name.c_str();
        entry.row.frame_count = Chain(process, entry.fp, entry.row.frames, 6);
        Progress::PrintThreadRow(entry.row, modules);
    }
}

// The look Progress::LookWanted gave (after ExitContext, before CoreHost, when the JIT stopped only
// for the look's halt: the run loop then returns, as for any interrupt).
inline void Answer(Kernel::KernelCore& kernel, Kernel::KProcess& process, Core::ArmInterface& interface,
                   Kernel::KThread& thread, std::size_t core, unsigned look) {
    Kernel::Svc::ThreadContext context{};
    interface.GetContext(context);
    // From here a look may take kernel locks (the page table's for the modules, then the threads'
    // list and the scheduler's), as a system call does, and a wait for one may move the thread to
    // another core.
    Progress::GuestModules& modules = ModulesOf(core);
    if (look >= Progress::kLookFull || modules.count == 0) FindModules(process, modules);
    Progress::GuestLook out{};
    out.core = core;
    out.thread = thread.GetThreadId();
    for (std::size_t i = 0; i < 29; ++i) out.x[i] = context.r[i];
    out.x[29] = context.fp;
    out.x[30] = context.lr;
    out.sp = context.sp;
    out.pc = context.pc;
    out.pstate = context.pstate;
    if (look < Progress::kLookFull) {
        Progress::PrintLook(out, modules, Progress::kLookPc);
        Progress::LookAnswered(core, out.thread, kernel.CurrentPhysicalCoreIndex() == core);
        return;
    }
    Core::Memory::Memory& memory = process.GetMemory();
    const std::string name = Core::GetThreadName(&thread).value_or("-");
    out.name = name.c_str();
    out.priority = thread.GetPriority();
    out.code_from = context.pc - 4 * 16;
    for (unsigned i = 0; i < 32; ++i) {
        const std::uint64_t at = out.code_from + 4 * i;
        out.code[i] = memory.IsValidVirtualAddressRange(at, 4) ? memory.Read32(at) : 0;
    }
    out.code_words = 32;
    out.frame_count = Chain(process, context.fp, out.frames, 12);
    Progress::PrintLook(out, modules, Progress::kLookFull);
    for (const unsigned reg : Progress::kDataRegs) {
        const std::uint64_t at = out.x[reg];
        if (out.data_count >= 16 || at < 0x1000 || !memory.IsValidVirtualAddressRange(at, 64)) continue;
        Progress::GuestLook::Data& data = out.data[out.data_count++];
        data.reg = reg;
        data.at = at;
        for (unsigned word = 0; word < 8; ++word) data.words[word] = memory.Read64(at + 8 * word);
    }
    Progress::PrintLookData(out);
    if (look >= Progress::kLookThreads) Threads(kernel, process, modules, core, &thread, context);
    Progress::LookAnswered(core, out.thread, kernel.CurrentPhysicalCoreIndex() == core);
}

// The bounded loop's system call in Tears of the Kingdom (headless/totk_guard.h), made before the
// call itself (svcGetSystemTick) goes on as usual: the count it found, and around it.
inline void TotkTrap(Kernel::KProcess& process, Core::ArmInterface& interface, Kernel::KThread& thread,
                     unsigned svc) {
    if (svc != TotkGuard::kTrapSvc) return;
    const std::uint64_t base = TotkGuard::PatchedBase().load(std::memory_order_acquire);
    if (base == 0) return;
    Kernel::Svc::ThreadContext context{};
    interface.GetContext(context);
    if (context.pc != base + TotkGuard::kTrapReturn) return;
    TotkGuard::Trap trap{};
    trap.count = static_cast<std::uint32_t>(context.r[11]);
    trap.count_at = context.r[10];
    trap.src = context.r[9];
    trap.dst = context.r[19];
    trap.thread = thread.GetThreadId();
    // Whether the GPU's caches track the count's page (a CPU read of it may download the GPU's copy).
    const Common::PageTable& table = process.GetPageTable().GetImpl();
    const std::uint64_t at = trap.count_at & 0xffffffffffffULL;
    if ((at >> table.GetAddressSpaceBits()) == 0) {
        switch (table.entries[at >> 12].Type()) {
        case Common::PageType::Memory:
            trap.page = "memory";
            break;
        case Common::PageType::RasterizerCachedMemory:
            trap.page = "gpu-cached";
            break;
        case Common::PageType::DebugMemory:
            trap.page = "debug";
            break;
        default:
            trap.page = "unmapped";
            break;
        }
    }
    // The words around it as guest memory holds them, read past the GPU's caches (no download).
    Core::Memory::Memory& memory = process.GetMemory();
    const std::uint64_t from = trap.count_at - 0x20;
    if (trap.count_at >= 0x20 && memory.IsValidVirtualAddressRange(from, 0x40)) {
        trap.words_read = true;
        for (unsigned i = 0; i < 16 && trap.words_read; ++i) {
            const std::uint8_t* const word = memory.GetPointerSilent(from + 4 * i);
            if (word == nullptr) trap.words_read = false;
            else std::memcpy(&trap.words[i], word, sizeof(trap.words[i]));
        }
    }
    TotkGuard::Report(trap);
}

} // namespace Eden::Sample
