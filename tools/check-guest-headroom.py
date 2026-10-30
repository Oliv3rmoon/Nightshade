#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Host check of the guest's headroom in the pool (performance.h ViewOfPool, the reserve of
src/guest_memory_service.inc): what the graphics caches see of the pool, and when they see it short.

The guest's 1 GiB of headroom used to be only counted as taken: the caches saw the largest free
block less 1 GiB, while the memory stayed in the pool for anything to take or cut up. Now the memory
service holds it as blocks; the caches see the pool less the part not held yet, and see memory short
once a burst of the game's writes has taken half of it. This checks those rules and that the service
and the PS5's free-memory probe are wired to them. The service's PS5 part (blocks of direct memory
taken every 100 ms) runs only on the console.
"""
from pathlib import Path
import subprocess, tempfile

root = Path(__file__).resolve().parents[1]
code = r"""
#include "performance.h"
#include <cassert>
#include <cstdio>
using Eden::Performance::ViewOfPool;
using Eden::Performance::kGuestReserve;
using Eden::Performance::kShortMemory;
int main() {
    constexpr std::size_t MiB = std::size_t{1} << 20, GiB = std::size_t{1} << 30;
    // The pool could not be read: nothing free, and short.
    auto view = ViewOfPool(false, 8 * GiB, 0, 0);
    assert(view.free == 0 && view.short_memory);
    // No service (a 32-bit game): the whole headroom counted as taken, as before.
    view = ViewOfPool(true, 4 * GiB, 0, 0);
    assert(view.free == 4 * GiB - kGuestReserve && !view.short_memory);
    view = ViewOfPool(true, kGuestReserve + kShortMemory - MiB, 0, 0);
    assert(view.short_memory);
    view = ViewOfPool(true, 512 * MiB, 0, 0);
    assert(view.free == 0 && view.short_memory);
    // The service holds all of its headroom: the caches see the pool as it is.
    view = ViewOfPool(true, 4 * GiB, GiB, GiB);
    assert(view.free == 4 * GiB && !view.short_memory);
    // Holding part of it: the rest is counted as taken, and not short while half is held.
    view = ViewOfPool(true, 4 * GiB, 768 * MiB, GiB);
    assert(view.free == 4 * GiB - 256 * MiB && !view.short_memory);
    view = ViewOfPool(true, 4 * GiB, 512 * MiB, GiB);
    assert(view.free == 4 * GiB - 512 * MiB && !view.short_memory);
    // A burst took more than half of it: short, however much the pool has, so the caches give
    // memory back before a write has to wait.
    view = ViewOfPool(true, 6 * GiB, 512 * MiB - 2 * MiB, GiB);
    assert(view.short_memory && view.free == 6 * GiB - 512 * MiB - 2 * MiB);
    view = ViewOfPool(true, 6 * GiB, 0, GiB);
    assert(view.short_memory && view.free == 5 * GiB);
    // Held in full but the pool itself low: short as before.
    view = ViewOfPool(true, kShortMemory - MiB, GiB, GiB);
    assert(view.short_memory && view.free == kShortMemory - MiB);
    // The same total as the old rule: 1 GiB held plus the largest block, against 1 GiB counted.
    assert(ViewOfPool(true, 3 * GiB, GiB, GiB).free == ViewOfPool(true, 4 * GiB, 0, 0).free);
    return 0;
}
"""
# performance.h reads Eden's wall clock in its inline code; this program never calls it.
clock = r"""
#include <chrono>
namespace Common {
struct WallClock {
    std::chrono::nanoseconds GetTimeNS() const {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch());
    }
    long long GetCNTPCT() const { return 0; }
};
inline WallClock g_wall_clock;
}
"""
with tempfile.TemporaryDirectory() as tmp:
    (Path(tmp) / 'common').mkdir()
    (Path(tmp) / 'common' / 'cpu_features.h').write_text(clock)
    exe = Path(tmp) / 'headroom'
    subprocess.run(['c++', '-std=c++20', '-Wall', '-Wextra', '-Werror', '-pthread', '-fsanitize=address,undefined',
                    '-I' + tmp, '-I' + str(root / 'headless'), '-x', 'c++', '-', '-o', str(exe)],
                   input=code, text=True, check=True)
    subprocess.run([str(exe)], check=True)

# Wired as described: the service holds 1 GiB, says what it holds, is topped up every 100 ms by its
# own thread and gives it back at the end; the PS5's probe reads it and takes the view above.
service = (root / 'src/guest_memory_service.inc').read_text()
assert 'constexpr std::size_t ReserveBlocks = 512;      // 1 GiB' in service
assert 'extern "C" void eden_guest_memory_reserve(std::size_t* held, std::size_t* target) noexcept {' in service
keeper = service.split('void ReserveKeeper() {', 1)[1].split('\n}\n', 1)[0]
assert 'std::chrono::milliseconds(100)' in keeper and 'FillReserve();' in keeper
start = service.split('void ServiceStart(SparseRange& range) noexcept {', 1)[1].split('\n}\n', 1)[0]
assert 'FillReserve(ReserveAtStart);' in start and 'keeper = std::thread(ReserveKeeper);' in start
stop = service.split('void ServiceStop(SparseRange& range) noexcept {', 1)[1].split('\n}\n', 1)[0]
assert stop.index('keeper.join()') < stop.index('EmptyReserve();')
# A write that finds no block in the pool takes one from the reserve, before it waits.
promote = service.split('bool PromoteZero(SparseRange& range, std::size_t slot) noexcept {', 1)[1].split('\n}\n', 1)[0]
assert 'if (!ZeroedBlock(&physical) && !ReserveBlock(&physical)) return false;' in promote
performance = (root / 'headless/performance.cpp').read_text()
refresh = performance.split('static void RefreshFreeMemory() {', 1)[1].split('\n}\n', 1)[0]
assert 'eden_guest_memory_reserve(&held, &target);' in refresh
assert 'const PoolView view = ViewOfPool(known, largest, held, target);' in refresh
assert 'graphics_memory_short.store(view.short_memory' in refresh
assert 'kGuestReserve' not in refresh  # the rule is ViewOfPool's alone
report = performance.split('void ReportMemory() {', 1)[1].split('\n}\n', 1)[0]
assert 'reserve=%zuMiB/%zuMiB' in report
print('Guest headroom: PASS the caches see the pool less the headroom not held yet, short once half of it '
      'is used, as before without a service; the service holds 1 GiB, kept full every 100 ms')
