#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2026 Oliv3rmoon
# SPDX-License-Identifier: GPL-3.0-or-later
"""Host ASan/UBSan checks for NVDRV SMMU reclaim, sparse pin ownership, and backing bounds.

Usage: check-nvmap-sparse-pins.py EDEN_SOURCE
"""
from pathlib import Path
import os
import subprocess
import sys
import tempfile

source = Path(sys.argv[1])
as_h = (source / 'src/core/hle/service/nvdrv/devices/nvhost_as_gpu.h').read_text()
as_cpp = (source / 'src/core/hle/service/nvdrv/devices/nvhost_as_gpu.cpp').read_text()
nvmap_cpp = (source / 'src/core/hle/service/nvdrv/core/nvmap.cpp').read_text()
nvdec_cpp = (source / 'src/core/hle/service/nvdrv/devices/nvhost_nvdec_common.cpp').read_text()
heap_h = (source / 'src/core/hle/service/nvdrv/core/heap_mapper.h').read_text()

# Source invariants for patches 33..41.
assert 'if (unmap_queue.empty()) {' in nvmap_cpp
assert 'if (freeHandleDesc->d_address) {' in nvmap_cpp
assert 'if (handle_description->aligned_size > std::numeric_limits<u32>::max())' in nvmap_cpp
assert 'if (low_area_pin && !map_low_area()) return 0;' in nvmap_cpp
assert 'if (mapped_for_pin && !pin_committed)' in nvmap_cpp
assert 'if (pin_address == 0) {' in nvdec_cpp
assert 'std::scoped_lock lock(mutex);' in as_cpp[as_cpp.index('NvResult nvhost_as_gpu::Remap('):as_cpp.index('NvResult nvhost_as_gpu::MapBufferEx(')]
assert 'alloc-- == allocation_map.begin()' not in as_cpp
assert 'Sparse GPU backing range rejected:' in as_cpp
assert 'GPU backing range rejected:' in as_cpp
assert 'static_cast<void>(FreeMappingLocked(params.offset));' in as_cpp
assert as_h.index('sparse_mapping_pins;') < as_h.index('std::unique_ptr<Tegra::MemoryManager> gmmu;')

# Extract real SparseMappingOwnership class from nvhost_as_gpu.h.
cls_start = as_h.index('class SparseMappingOwnership {')
cls_end = as_h.index('\n};\n', cls_start) + 3
sparse_class = as_h[cls_start:cls_end]

# Extract real HeapMapper::IsInBounds body from heap_mapper.h.
hb_start = heap_h.index('bool IsInBounds(VAddr start, size_t size) const {')
hb_end = heap_h.index('\n    }\n', hb_start) + 6
heap_bounds_fn = heap_h[hb_start:hb_end]

test_cpp = r'''
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <stdexcept>
#include <utility>
#include <vector>

using u64 = std::uint64_t;
using VAddr = u64;

''' + sparse_class + r'''

struct MockHeapMapper {
    VAddr m_vaddress;
    size_t m_size;
    ''' + heap_bounds_fn + r'''
};

struct PinTracker {
    std::vector<int>* events;
    int id;
    PinTracker(std::vector<int>* e, int i) : events(e), id(i) { events->push_back(+id); }
    ~PinTracker() { events->push_back(-id); }
};

static void TestSparseMappingOwnership() {
    std::vector<int> events;
    {
        SparseMappingOwnership ownership;
        auto pin1 = std::make_shared<PinTracker>(&events, 1);
        int applied = 0;
        ownership.Replace(0x1000, 0x3000, std::move(pin1), [&] { ++applied; });
        assert(applied == 1 && events == (std::vector<int>{+1}));

        // Split middle [0x2000, 0x3000) with pin 2: pin 1 must stay alive for left & right halves.
        auto pin2 = std::make_shared<PinTracker>(&events, 2);
        ownership.Replace(0x2000, 0x1000, std::move(pin2), [&] { ++applied; });
        assert(applied == 2 && events == (std::vector<int>{+1, +2}));

        // Throwing apply() must leave old owners untouched and immediately drop new owner (pin 3).
        bool threw = false;
        try {
            auto pin3 = std::make_shared<PinTracker>(&events, 3);
            ownership.Replace(0x1000, 0x3000, std::move(pin3), [&]() -> void {
                throw std::runtime_error("gmmu failure");
            });
        } catch (const std::runtime_error&) {
            threw = true;
        }
        assert(threw && events == (std::vector<int>{+1, +2, +3, -3}));

        // Clear left half [0x1000, 0x2000): pin 1 still survives on right half [0x3000, 0x4000).
        ownership.Replace(0x1000, 0x1000, {}, [&] { ++applied; });
        assert(events == (std::vector<int>{+1, +2, +3, -3}));

        // Clear right half [0x3000, 0x4000): pin 1 must now release strictly AFTER apply().
        ownership.Replace(0x3000, 0x1000, {}, [&] {
            // Inside apply(), pin 1 is still alive!
            assert(events == (std::vector<int>{+1, +2, +3, -3}));
            ++applied;
        });
        assert(events == (std::vector<int>{+1, +2, +3, -3, -1}));

        // Overflow must throw before invoking apply().
        bool overflow_threw = false;
        try {
            ownership.Replace(std::numeric_limits<u64>::max() - 10, 100, {}, [&] {
                assert(false && "apply must not run on range overflow");
            });
        } catch (const std::invalid_argument&) {
            overflow_threw = true;
        }
        assert(overflow_threw);
    }
    // Destruction releases surviving pin 2 cleanly.
    assert(events == (std::vector<int>{+1, +2, +3, -3, -1, -2}));
}

static void TestHeapMapperBounds() {
    MockHeapMapper hm{0x10000, 0x4000};
    assert(hm.IsInBounds(0x10000, 0x4000));
    assert(hm.IsInBounds(0x12000, 0x2000));
    assert(!hm.IsInBounds(0x0f000, 0x1000));
    assert(!hm.IsInBounds(0x12000, 0x2001));
    assert(!hm.IsInBounds(0x12000, std::numeric_limits<size_t>::max()));
    MockHeapMapper high{std::numeric_limits<VAddr>::max() - 0x1000, 0x1000};
    assert(high.IsInBounds(std::numeric_limits<VAddr>::max() - 0x800, 0x800));
    assert(!high.IsInBounds(std::numeric_limits<VAddr>::max() - 0x800, 0x801));
}

int main() {
    TestSparseMappingOwnership();
    TestHeapMapperBounds();
    std::puts("NVDRV/NvMap safety: PASS sparse pin ownership split/rollback/destruction, overflow-safe HeapMapper bounds, SMMU/low-alias invariants");
    return 0;
}
'''

with tempfile.TemporaryDirectory(prefix='eden-nvmap-sparse-') as tmp:
    exe = Path(tmp) / 'test'
    subprocess.run([
        os.environ.get('CXX', 'c++'), '-std=c++20', '-Wall', '-Wextra', '-Werror',
        '-fsanitize=address,undefined', '-fno-sanitize-recover=undefined',
        '-x', 'c++', '-', '-o', str(exe),
    ], input=test_cpp, text=True, check=True)
    subprocess.run([str(exe)], check=True, timeout=60)
