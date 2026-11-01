#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Host check of the freeze watchdog (headless/guest_progress.h) and of where the build marks the
cores, the service requests and the GPU stall (headless/CMakeLists.txt).

Usage: check-guest-progress.py [EDEN_SOURCE]

With Eden's pinned source (the build's $scratch/source), every site headless/CMakeLists.txt
writes a mark at is found once, in the text as the build has it by then, and only the stop's own
suspension is reported as a shutdown step.

Then it builds the watchdog with short times (under ThreadSanitizer where the compiler has it) and
plays the freeze of a Tears of the Kingdom session on v1.010.007 three ways, each with a display
thread that keeps handing in frames (as the game did for ten minutes on its loading screen) while
the load stops: in a file read that never ends, in a core held in a system call's handler, and
quietly (a thread that made many requests makes no more). Each must be named: EDEN_STUCK for the
request, the core or the fault handler, at its three ages and no more; EDEN_PROGRESS with the
frames' requests still finishing, no file reads, and the quiet thread. A core that keeps changing
state, an idle core and requests that finish are never reported.

And the freeze of v1.010.009 (TOTK, 110 minutes into a boss fight: EDEN_STUCK core=0 state=guest
thread=78) two ways: a thread that spins in the guest's code and answers the watchdog's looks, which
must give EDEN_STUCK_GUEST with pc and lr as main+0x..., its code, registers and memory at each of
the three ages, four EDEN_STUCK_PC looks after the first, and every thread with the first two
reports, while the looks' own stops never count as the core moving (still three EDEN_STUCK lines);
and a core that never stops when asked (its host thread held under the guest's code), which must
give EDEN_STUCK_NO_ANSWER after each ask. With the source, the core's halt for a look is taken
under m_guard in EnterContext, and the look is answered after ExitContext, before CoreHost. It does
not run a game.
"""
from pathlib import Path
import re, shutil, subprocess, sys, tempfile

root = Path(__file__).resolve().parents[1]
cmake = (root / 'headless/CMakeLists.txt').read_text()
failures = []


def unquote(text):
    return text.replace('\\n', '\n').replace('\\"', '"').replace('\\\\', '\\')


def cmake_set(name):
    """The value of set(name "...") or set(name [=[...]=]) in headless/CMakeLists.txt."""
    match = re.search(r'set\(' + re.escape(name) + r' "((?:[^"\\]|\\.)*)"\)', cmake)
    if match:
        return unquote(match.group(1))
    match = re.search(r'set\(' + re.escape(name) + r' \[=\[(.*?)\]=\]\)', cmake, re.S)
    assert match, name
    return match.group(1)


def replacement(name):
    """The text string(REPLACE "${name}" ...) writes, as CMake reads it."""
    at = cmake.index('string(REPLACE "${' + name + '}"')
    rest = cmake[at + len('string(REPLACE "${' + name + '}"'):].lstrip()
    if rest.startswith('[=['):
        return rest[3:rest.index(']=]')]
    match = re.match(r'"((?:[^"\\]|\\.)*)"', rest)
    assert match, name
    return unquote(match.group(1))


def expand(text, values):
    for key, value in values.items():
        text = text.replace('${' + key + '}', value)
    return text


def check_sites(source):
    # The cores: the run loop, the system call and the idle wait (after the build's own Idle body).
    core = (source / 'src/core/hle/kernel/physical_core.cpp').read_text()
    idle_old = cmake_set('idle_old')
    assert core.count(idle_old) == 1, 'Idle'
    core = core.replace(idle_old, replacement('idle_old'))
    names = ('progress_enter_old', 'progress_exit_old', 'progress_svc_old', 'progress_idle_begin_old',
             'progress_idle_end_old')
    values = {name: cmake_set(name) for name in names}
    for name in names:
        if core.count(values[name]) != 1:
            failures.append(f'physical_core.cpp: {name} found {core.count(values[name])} times')
            continue
        core = core.replace(values[name], expand(replacement(name), values))
    for mark in ('CoreGuest(m_core_index, thread->GetThreadId())', 'CoreHost(m_core_index)',
                 'CoreSvc(m_core_index, interface->GetSvcNumber())', 'CoreIdle(m_core_index)'):
        if mark not in core:
            failures.append(f'physical_core.cpp: no {mark}')
    # A look (EDEN_STUCK_GUEST): EnterContext leaves the watchdog a way to halt the JIT, under the
    # core's lock and only while a thread runs, before the core is marked in the guest's code; the
    # look is answered after ExitContext (the JIT stopped, its context let go) and before CoreHost.
    run = core[core.index('void PhysicalCore::RunThread('):core.index('void PhysicalCore::LoadContext(')]
    enter = run[run.index('const auto EnterContext = [&]() {'):run.index('const auto ExitContext = [&]() {')]
    if not ('Pokeable(m_core_index,' in enter and enter.index('Pokeable(m_core_index,') <
            enter.index('CoreGuest(m_core_index, thread->GetThreadId())') < enter.index('interface->LockThread(thread)')):
        failures.append('physical_core.cpp: the core is not made pokeable in EnterContext before CoreGuest')
    poke = enter[enter.index('Pokeable(m_core_index,'):enter.index('CoreGuest(m_core_index')]
    for needed in ('std::scoped_lock poke_lock{core->m_guard};', 'if (core->m_arm_interface)',
                   'SignalInterrupt(core->m_current_thread)'):
        if needed not in poke:
            failures.append(f'physical_core.cpp: the look\'s halt lacks {needed}')
    loop = run[run.index('hr = interface->RunThread(thread);'):run.index('// Determine why we stopped.')]
    if not ('LookWanted(m_core_index)' in loop and loop.index('ExitContext();') <
            loop.index('if (hr == Core::HaltReason::BreakLoop) {') < loop.index('LookWanted(m_core_index)') <
            loop.index('::Eden::Sample::Answer(kernel, *process, *interface, *thread, m_core_index, eden_look);') <
            loop.index('if (kernel.CurrentPhysicalCoreIndex() != m_core_index) return;') <
            loop.index('CoreHost(m_core_index)')):
        failures.append('physical_core.cpp: the look is not answered after ExitContext, for the look\'s halt only, '
                        'and before CoreHost on the core it ends on')
    # Only a stop for an interrupt is answered, and that stop returns from the run loop anyway.
    after = run[run.index('// Handle external interrupt sources.'):]
    if 'if (interrupt || m_is_single_core) {\n            return;' not in after:
        failures.append('physical_core.cpp: an interrupt no longer ends the run loop; review the look\'s early return')
    derived = cmake[cmake.index('write_derived("${PORT_BUILD_DIR}/physical_core.cpp"'):]
    derived = derived[:derived.index(')\n')]
    if 'guest_sample.h' not in derived:
        failures.append('CMakeLists.txt: physical_core.cpp does not include guest_sample.h')
    # The system call is marked before it is made, and the idle wait at both ends.
    body = core[core.index('void PhysicalCore::Idle() {'):]
    body = body[:body.index('\n}') + 2]
    if not (body.index('CoreIdle(') < body.index('m_on_interrupt.wait') < body.index('CoreHost(')):
        failures.append('physical_core.cpp: the idle wait is not marked at both ends')
    # Service requests: before the service's lock, and Locked() after it.
    service = (source / 'src/core/hle/service/service.cpp').read_text()
    site = cmake_set('watch_site')
    if service.count(site) != 1:
        failures.append(f'service.cpp: the request site found {service.count(site)} times')
    else:
        written = expand(replacement('watch_site'), {'watch_site': site})
        written = service.replace(site, written)
        handler = written[written.index('Result ServiceFrameworkBase::HandleSyncRequest'):]
        if not (handler.index('RequestScope eden_request{') < handler.index('LockService()') <
                handler.index('eden_request.Locked()') < handler.index('switch (ctx.GetCommandType())')):
            failures.append('service.cpp: the request is not marked around its service\'s lock')
    # The GPU stall, in every build: outside the development block.
    ctrl = (source / 'src/core/hle/service/nvdrv/devices/nvhost_ctrl.cpp').read_text()
    stall = cmake_set('stall_old')
    if ctrl.count(stall) != 1:
        failures.append('nvhost_ctrl.cpp: the stall found %d times' % ctrl.count(stall))
    stall_at = cmake.index('set(stall_old')
    dev_at = cmake.rfind('\nif(EDEN_DEV_PROFILE)\n', 0, stall_at)
    end_at = cmake.rfind('\nendif()', 0, stall_at)
    if dev_at > end_at:
        failures.append('CMakeLists.txt: the GPU stall lines are inside a development-only block')
    if 'EDEN_GPU_STALL begin' not in replacement('stall_old') or 'EDEN_GPU_STALL end' not in replacement('stall_old'):
        failures.append('CMakeLists.txt: the GPU stall is not logged at both ends')
    # Only the stop reports its suspension as a shutdown step.
    system = (source / 'src/core/core.cpp').read_text()
    stop = cmake_set('stop_suspend_old')
    if system.count(stop) != 1:
        failures.append(f'core.cpp: the stop\'s suspension found {system.count(stop)} times')
    stages = cmake[cmake.index('    foreach(stage "gpu_core->NotifyShutdown();"'):]
    stages = stages[:stages.index(')\n')]
    if 'kernel.SuspendEmulation(true);' in stages:
        failures.append('CMakeLists.txt: every SuspendEmulation(true) (GPU stalls too) is a shutdown step again')
    if system.count('kernel.SuspendEmulation(true);') < 2:
        failures.append('core.cpp: Pause or StallApplication no longer suspends: review the stop step')
    # The memory service's handler is counted for the watchdog, and the session starts and stops it.
    service_inc = (root / 'src/guest_memory_service.inc').read_text()
    fault = service_inc[service_inc.index('bool HandleGuestWriteFault(void* fault) noexcept {'):]
    fault = fault[:fault.index('\n}\n')]
    if fault.index('const FaultInside inside{address};') > fault.index('LockSlot('):
        failures.append('guest_memory_service.inc: the fault handler is counted after its slot lock')
    if not (fault.index('LockSlot(') < fault.index('inside.Stage(2, state);') and
            fault.index('inside.Stage(3, state);') < fault.index('WaitForBlock(promote)')):
        failures.append('guest_memory_service.inc: the fault handler\'s stages are not marked where they begin')
    main = (root / 'headless/main.cpp').read_text()
    stop = 'Eden::Progress::Stop();\n                if (streamed) streamed->stop();\n                system.ShutdownMainProcess();'
    if not (main.count('Eden::Progress::Start();') == 1 and
            main.index('Eden::Progress::Start();') > main.index('system.Run();') and main.count(stop) == 1):
        failures.append('main.cpp: the watchdog does not run from System::Run to the stop')


SIMULATION = r'''
#include <cassert>
#include <cstdlib>
#include "guest_progress.h"

static std::atomic<unsigned> fault_threads{0};
static std::atomic<long long> fault_from{0};
extern "C" unsigned eden_guest_fault_busy(long long* since_ns) {
    if (since_ns) *since_ns = fault_from.load();
    return fault_threads.load();
}
// The busy handler's write: waiting for its slot's lock (a slot never let go).
extern "C" std::uintptr_t eden_guest_fault_detail(unsigned* stage, unsigned* slot_state) {
    if (stage) *stage = 1;
    if (slot_state) *slot_state = 0x81;
    return 0x8012345000ull;
}

using namespace Eden::Progress;
static void Sleep(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

// The core's JIT, halted for a look: PhysicalCore's poke sets the JIT's halt flag.
static void Poke(void* self) { static_cast<std::atomic<bool>*>(self)->store(true); }

// What headless/guest_sample.h finds and prints for a look at a thread spinning in the game's code.
static void AnswerLook(std::size_t core, std::uint64_t thread, unsigned look, unsigned& answered) {
    GuestModules modules;
    modules.entry[0] = {0x80000000ull, 0x84000000ull, "main"};
    modules.entry[1] = {0x84000000ull, 0x85000000ull, "sdk"};
    modules.count = 2;
    GuestLook out;
    out.core = core;
    out.thread = thread;
    out.name = "GameMain";
    out.priority = 44;
    for (unsigned r = 0; r < 31; ++r) out.x[r] = 0x1000ull * r;
    out.x[19] = 0x2a0000040ull;
    out.x[30] = 0x84000123ull + 4 * (answered % 2);
    out.sp = 0x2b0000000ull;
    out.pc = 0x80001234ull + 4 * (answered % 3); // a loop of three instructions
    out.pstate = 0x60000000;
    if (look == kLookPc) {
        PrintLook(out, modules, kLookPc);
        ++answered;
        return;
    }
    out.code_from = out.pc - 64;
    for (unsigned i = 0; i < 32; ++i) out.code[i] = 0xd503201fu; // nop
    out.code_words = 32;
    out.frames[0] = 0x80002000ull;
    out.frames[1] = 0x84000400ull;
    out.frame_count = 2;
    PrintLook(out, modules, kLookFull);
    out.data[0].reg = 19;
    out.data[0].at = out.x[19];
    out.data[0].words[0] = 1;
    out.data_count = 1;
    PrintLookData(out);
    if (look == kLookThreads) {
        PrintThreadsHeader(core, 2);
        GuestThreadRow spinning;
        spinning.id = thread;
        spinning.name = "GameMain";
        spinning.state = "runnable";
        spinning.priority = spinning.base_priority = 44;
        spinning.core = int(core);
        spinning.running = int(core);
        spinning.pc = out.pc;
        PrintThreadRow(spinning, modules);
        GuestThreadRow waiting;
        waiting.id = 80;
        waiting.name = "Worker";
        waiting.state = "waiting:arbitration";
        waiting.priority = waiting.base_priority = 45;
        waiting.core = int(core);
        waiting.owner = thread;
        waiting.key = 0x2a0000040ull;
        waiting.pc = 0x84000800ull;
        waiting.frames[0] = 0x80003000ull;
        waiting.frame_count = 1;
        PrintThreadRow(waiting, modules);
    }
    ++answered;
}

int main(int argc, char** argv) {
    Timing& timing = Times();
    timing.look_ms = 20;
    timing.progress_ms = 400;
    timing.stuck_ms[0] = 150;
    timing.stuck_ms[1] = 300;
    timing.stuck_ms[2] = 600;
    timing.quiet_ms = 250;
    timing.quiet_calls = 20;
    timing.answer_ms = 100;
    const std::string mode = argc > 1 ? argv[1] : "";
    const bool looks = mode == "spin" || mode == "deaf";
    if (looks) {
        // Room for the four pc looks (one a look), under a sanitizer on a busy machine too.
        timing.stuck_ms[1] = 600;
        timing.stuck_ms[2] = 1200;
    }
    std::atomic<bool> stop{false};
    static std::atomic<bool> jit_halted{false}, display_halted{false};
    // The game's frames keep coming: a display thread on core 0 hands in a frame, the GPU's
    // requests with it, and its core changes state all the time.
    // In spin mode its thread runs the game's code between frames, where the watchdog's look at
    // every thread also asks for its pc.
    std::thread display([&] {
        unsigned display_answers = 0;
        while (!stop.load()) {
            if (mode == "spin") Pokeable(0, &Poke, &display_halted);
            CoreGuest(0, 77);
            { RequestScope request{"IHOSBinderDriver", 2, 77}; request.Locked(); }
            if (mode == "spin") {
                Sleep(5);
                if (display_halted.exchange(false)) {
                    if (const unsigned look = LookWanted(0)) {
                        AnswerLook(0, 77, look, display_answers);
                        LookAnswered(0, 77);
                    }
                }
            }
            CoreSvc(0, 0x21);
            { RequestScope request{"nvdrv", 1, 77}; request.Locked(); }
            CoreHost(0);
            if (mode != "spin") Sleep(5);
        }
    });
    // Core 1 has nothing to run.
    CoreIdle(1);
    Eden::IpcRing::Record("IStorage", 99, 0, 4, nullptr, 0);
    Start();
    std::thread loader;
    if (mode == "file") {
        // The load's thread reads files on core 2, then one read never ends; a second request to the
        // same service waits for its lock behind it.
        loader = std::thread([&] {
            for (int i = 0; i < 30; ++i) {
                RequestScope request{"IStorage", 0, 99};
                request.Locked();
            }
            RequestScope request{"IStorage", 0, 99};
            request.Locked();
            std::thread behind([&] {
                RequestScope waiting{"IStorage", 0, 98};
                while (!stop.load()) Sleep(5);
            });
            while (!stop.load()) Sleep(5);
            behind.join();
        });
    } else if (mode == "core") {
        // The load's core is held in a system call's handler (a lock that is never let go).
        loader = std::thread([&] {
            for (int i = 0; i < 30; ++i) {
                CoreGuest(2, 99);
                { RequestScope request{"IStorage", 0, 99}; request.Locked(); }
                CoreHost(2);
            }
            CoreGuest(2, 99);
            CoreSvc(2, 0x2c);
            while (!stop.load()) Sleep(5);
        });
    } else if (mode == "spin" || mode == "deaf") {
        // v1.010.009: thread 78 runs the game's own code on core 2 with no system call, so its core
        // never changes state. Its JIT stops only when the watchdog pokes it; then it answers the
        // look (or, deaf, its host thread is held under the guest's code and never sees the poke)
        // and the same thread goes on, as PhysicalCore::RunThread does.
        loader = std::thread([&] {
            for (int i = 0; i < 30; ++i) {
                Pokeable(2, &Poke, &jit_halted);
                CoreGuest(2, 78);
                { RequestScope request{"IStorage", 0, 78}; request.Locked(); }
                CoreHost(2);
            }
            unsigned answered = 0;
            Pokeable(2, &Poke, &jit_halted);
            CoreGuest(2, 78);
            while (!stop.load()) {
                if (mode == "spin" && jit_halted.exchange(false)) {
                    if (const unsigned look = LookWanted(2)) {
                        AnswerLook(2, 78, look, answered);
                        LookAnswered(2, 78);
                    }
                    CoreHost(2);
                    Pokeable(2, &Poke, &jit_halted);
                    CoreGuest(2, 78);
                }
                Sleep(1);
            }
        });
    } else if (mode == "quiet") {
        // The load's thread reads files, then waits on something of the game's own: no request, its
        // core idle. And the memory service's write handler is busy on core 3 (a slot never let go).
        loader = std::thread([&] {
            for (int i = 0; i < 30; ++i) {
                RequestScope request{"IStorage", 0, 99};
                request.Locked();
            }
            CoreIdle(2);
            CoreGuest(3, 55);
            fault_from.store(NowNs());
            fault_threads.store(1);
            while (!stop.load()) Sleep(5);
        });
    }
    Sleep(looks ? 2000 : 1300);
    stop.store(true);
    loader.join();
    display.join();
    const long long before = NowNs();
    Stop();
    // Stopping waits for one look at most.
    if (NowNs() - before > 200'000'000) { std::fputs("SLOW_STOP\n", stderr); return 1; }
    // Started again, nothing is left over: a quiet second of nothing reports nothing stuck.
    std::fputs("RESTARTED\n", stderr);
    fault_threads.store(0);
    for (std::size_t core = 0; core < kCores; ++core) CoreIdle(core);
    Start();
    Sleep(300);
    Stop();
    std::fputs("SIMULATION_DONE\n", stderr);
    return 0;
}
'''


def run_simulation():
    cxx = shutil.which('clang++-18') or shutil.which('clang++') or shutil.which('c++')
    if cxx is None:
        failures.append('no C++ compiler')
        return
    with tempfile.TemporaryDirectory(prefix='eden-progress-') as tmp:
        tmp = Path(tmp)
        (tmp / 'simulation.cpp').write_text(SIMULATION)
        binary = tmp / 'simulation'
        base = [cxx, '-std=c++20', '-O1', '-g', '-pthread', '-Wall', '-Wextra', '-I' + str(root / 'headless'),
                str(tmp / 'simulation.cpp'), '-o', str(binary)]
        sanitized = subprocess.run(base[:1] + ['-fsanitize=thread'] + base[1:], capture_output=True, text=True)
        if sanitized.returncode != 0:
            subprocess.run(base, check=True)
            print('guest progress: ThreadSanitizer is not available here; built without it')
        for mode in ('file', 'core', 'quiet', 'spin', 'deaf'):
            result = subprocess.run([str(binary), mode], capture_output=True, text=True, timeout=60)
            text = result.stderr
            lines = text.splitlines()
            if result.returncode != 0 or 'SIMULATION_DONE' not in text:
                failures.append(f'{mode}: exit {result.returncode}\n' + '\n'.join(lines[-20:]))
                continue
            if 'WARNING: ThreadSanitizer' in text:
                failures.append(f'{mode}: ThreadSanitizer found a race\n' + text[:3000])
            stuck = [line for line in lines if line.startswith('EDEN_STUCK ')]
            looks = [line for line in lines if line.startswith('EDEN_STUCK_')]
            progress = [line for line in lines if line.startswith('EDEN_PROGRESS')]
            # What is never stuck: the display's core, the idle core, finished requests.
            for line in stuck:
                if 'core=0 ' in line or 'core=1 ' in line or 'IHOSBinderDriver' in line or 'nvdrv' in line:
                    failures.append(f'{mode}: a busy or idle core or a finished request was reported: {line}')
            if not progress:
                failures.append(f'{mode}: no EDEN_PROGRESS line')
                continue
            last = progress[-1]
            # The frames' requests kept finishing (display and GPU), while the file reads stopped.
            if not re.search(r'display:[1-9]', last) or not re.search(r'gpu:[1-9]', last):
                failures.append(f'{mode}: EDEN_PROGRESS does not show the frames still coming: {last}')
            if 'files:0' not in last:
                failures.append(f'{mode}: EDEN_PROGRESS does not show the file reads stopped: {last}')

            def ages(pattern):
                return [line for line in stuck if re.search(pattern, line)]

            if mode == 'file':
                held = ages(r'request service=IStorage command=0 thread=99 .*\(being handled\)')
                behind = ages(r'request service=IStorage command=0 thread=98 .*\(waiting for its service\'s lock\)')
                if len(held) != 3 or len(behind) != 3:
                    failures.append(f'file: the read and the one behind it were not each reported three times: {stuck}')
                if not re.search(r'in_progress=[23] oldest=IStorage:0:', last):
                    failures.append(f'file: EDEN_PROGRESS does not name the oldest request: {last}')
            elif mode == 'core':
                held = ages(r'core=2 state=svc svc=0x2c thread=99 .*system call')
                if len(held) != 3:
                    failures.append(f'core: the core held in svc 0x2c was not reported three times: {stuck}')
                if 'svc:0' not in last.split('cores=')[1].split()[0].split(',')[2]:
                    failures.append(f'core: EDEN_PROGRESS does not show core 2 unchanged in svc: {last}')
            elif mode == 'quiet':
                handler = ages(r'fault_handler threads=1 ')
                if len(handler) != 3:
                    failures.append(f'quiet: the busy fault handler was not reported three times: {stuck}')
                if not re.search(r'quiet=99:IStorage:0:', last):
                    failures.append(f'quiet: EDEN_PROGRESS does not name the thread that went quiet: {last}')
                if ages(r'core=2 '):
                    failures.append(f'quiet: the idle core of the quiet thread was reported: {stuck}')
                # Core 3 in the guest's code under the busy handler: reported as the guest's code.
                if len(ages(r'core=3 state=guest thread=55|core=3 state=guest svc=0x0 thread=55')) != 3:
                    failures.append(f'quiet: core 3 under the handler was not reported: {stuck}')
                # What the handler waits for: its write's address and stage.
                if not all(re.search(r'address=0x[0-9a-f]+ stage=slot_lock slot=0x81 ', line) for line in handler):
                    failures.append(f'quiet: the busy fault handler does not say what it waits for: {handler}')
                # A core that cannot be asked (no poke) is reported, never looked at.
                if looks:
                    failures.append(f'quiet: a core that cannot be asked was looked at: {looks[:3]}')
            elif mode in ('spin', 'deaf'):
                held = ages(r'^EDEN_STUCK core=2 state=guest svc=0x0 thread=78 ')
                if len(held) != 3:
                    failures.append(f'{mode}: the spinning core was not reported at its three ages (a look '
                                    f'counted as a change?): {stuck}')
                guest = [line for line in looks if line.startswith('EDEN_STUCK_GUEST ')]
                pcs = [line for line in looks if line.startswith('EDEN_STUCK_PC core=2 ')]
                others = [line for line in looks if line.startswith('EDEN_STUCK_PC ') and line not in pcs]
                lists = [line for line in looks if line.startswith('EDEN_STUCK_THREADS ')]
                rows = [line for line in looks if line.startswith('EDEN_STUCK_THREAD ')]
                silent = [line for line in looks if line.startswith('EDEN_STUCK_NO_ANSWER ')]
                if mode == 'spin':
                    if len(guest) != 3 or not all(
                            re.search(r'core=2 thread=78 name=GameMain prio=44 pc=main\+0x123[48c] lr=sdk\+0x12[37]', line)
                            for line in guest):
                        failures.append(f'spin: a look at each report did not say where thread 78 runs: {guest}')
                    if len(pcs) != 4 or not all(re.search(r'core=2 thread=78 pc=main\+0x123[48c] ', line) for line in pcs):
                        failures.append(f'spin: four pc looks did not follow the first report: {pcs}')
                    for kind, count in (('EDEN_STUCK_REGS ', 3), ('EDEN_STUCK_FRAMES ', 3), ('EDEN_STUCK_CODE ', 3),
                                        ('EDEN_STUCK_DATA ', 3)):
                        if len([line for line in looks if line.startswith(kind)]) != count:
                            failures.append(f'spin: not {count} {kind.strip()} lines: {looks}')
                    if not any('EDEN_STUCK_FRAMES core=2 main+0x2000 sdk+0x400' in line for line in looks):
                        failures.append(f'spin: the return chain is not named by module: {looks}')
                    if not any(re.search(r'EDEN_STUCK_CODE core=2 from=main\+0x1[0-9a-f]+ pc_word=16: d503201f', line)
                               for line in looks):
                        failures.append(f'spin: the code around pc is not shown: {looks}')
                    if len(lists) != 2 or len(rows) != 4:
                        failures.append(f'spin: every thread was not listed with the first two reports: {lists} {rows}')
                    if not any(re.search(r'id=80 name=Worker state=waiting:arbitration prio=45/45 core=2 lock_owner=78 '
                                         r'key=0x2a0000040 pc=sdk\+0x800 .*frames=main\+0x3000', line) for line in rows):
                        failures.append(f'spin: the waiting thread does not name its lock owner and address: {rows}')
                    if silent:
                        failures.append(f'spin: a core that answered was said not to: {silent}')
                    # The display's core, in the game's code but never stuck: its pc with each list.
                    if not 1 <= len(others) <= 2 or not all(line.startswith('EDEN_STUCK_PC core=0 thread=77 pc=main+0x')
                                                            for line in others):
                        failures.append(f'spin: the other core in the guest\'s code was not looked at with the '
                                        f'thread lists: {others}')
                else:
                    if len(silent) != 3 or not all('core=2 thread=78 ' in line for line in silent):
                        failures.append(f'deaf: a core that never stopped was not named after each ask: {silent}')
                    if guest or pcs or lists:
                        failures.append(f'deaf: a look was printed for a core that never answered: {looks}')
            # The last service calls, once, after an incident reached its second age.
            ring = [line for line in lines if line.startswith('EDEN_IPC_RING')]
            if len(ring) != 1 or 'service=IStorage' not in ring[0]:
                failures.append(f'{mode}: the last service calls were not printed once: {ring}')
            # After the restart, nothing is reported as stuck any more.
            if 'EDEN_STUCK' in text.split('RESTARTED', 1)[-1]:
                failures.append(f'{mode}: something was reported stuck after the watchdog started again')
            print(f'guest progress ({mode}): {len(stuck)} EDEN_STUCK lines; {last}')


if len(sys.argv) > 1:
    check_sites(Path(sys.argv[1]))
run_simulation()
if failures:
    sys.exit('guest progress check failed:\n  ' + '\n  '.join(failures))
print('Guest progress: PASS a frozen load named three ways (a file read in progress, a core held in a '
      'system call, a thread gone quiet with the fault handler busy) while the frames keep coming; a thread '
      'spinning in the guest\'s code looked at (pc, code, registers, every thread) at each report without '
      'its looks counting as moves, and a core that never stops when asked named'
      + (', and every mark at its site in the pinned source' if len(sys.argv) > 1 else ''))
