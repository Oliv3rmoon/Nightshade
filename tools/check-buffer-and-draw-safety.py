#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2026 Oliv3rmoon
# SPDX-License-Identifier: GPL-3.0-or-later
"""Host ASan/UBSan checks for buffer cache, texture blacklist, DMA, and indirect-draw fixes.

Usage: check-buffer-and-draw-safety.py EDEN_SOURCE

Exercises the real patched headers/functions from Eden's source tree:
  - UsageTracker (patch 24): 12-byte indirect args, unaligned cross-unit tails,
    exact page boundaries, zero sizes, and u64 overflow bounds under UBSan.
  - BufferCache readback/download/virtual-span invariants (patches 25..28).
  - DelayedDestructionRing::ClearAfterSynchronization (patch 29).
  - TextureCache::FillImageViews NULL_IMAGE_ID guard (patch 30).
  - DmaPusher unbound subchannel checks (patch 31).
  - IsDrawImplausible 1,048,576-instance ceiling with zero-count no-op (patch 32).
"""
from pathlib import Path
import os
import subprocess
import sys
import tempfile

source = Path(sys.argv[1])
usage_h = (source / 'src/video_core/buffer_cache/usage_tracker.h').read_text()
ring_h = (source / 'src/video_core/delayed_destruction_ring.h').read_text()
buf_h = (source / 'src/video_core/buffer_cache/buffer_cache.h').read_text()
tex_h = (source / 'src/video_core/texture_cache/texture_cache.h').read_text()
dma_cpp = (source / 'src/video_core/dma_pusher.cpp').read_text()
vk_rast = (source / 'src/video_core/renderer_vulkan/vk_rasterizer.cpp').read_text()
gl_rast = (source / 'src/video_core/renderer_opengl/gl_rasterizer.cpp').read_text()
macro_cpp = (source / 'src/video_core/macro.cpp').read_text()

# Source-level invariants for patches 25..31.
assert 'const auto post_op = VideoCommon::ObtainBufferOperation::DoNothing;' in vk_rast
assert 'buffer->MarkUsage(offset, 12);' in vk_rast
assert 'const auto post_op = VideoCommon::ObtainBufferOperation::DoNothing;' in gl_rast
assert buf_h.count('buffer.MarkUsage(buffer.Offset(binding.device_addr), binding.size);') >= 3
assert 'for (const auto& pending : committed_gpu_modified_ranges)' in buf_h
assert 'const DAddr buffer_begin = buffer.CpuAddr();' in buf_h
assert 'const u64 virtual_size = is_written ? aligned_size : rounded_end - aligned_gpu_addr;' in buf_h
assert 'Storage translation discontinuity:' in buf_h
assert 'delayed_destruction_ring.ClearAfterSynchronization()' in buf_h
assert 'if (image_view.image_id == NULL_IMAGE_ID)' in tex_h
assert dma_cpp.count('if (subchannel == nullptr)') == 2

# Extract real IsDrawImplausible from patched macro.cpp.
start = macro_cpp.index('bool IsDrawImplausible(')
end = macro_cpp.index('\n}\n', start) + 3
is_draw_implausible = macro_cpp[start:end]

# Extract real ClearDownload pending-cancellation loop from patched buffer_cache.h.
cd_start = buf_h.index('        const DAddr cancel_end = ')
cd_end = buf_h.index('    }\n    async_downloads.DeleteAll(device_addr, size);', cd_start)
cancel_block = buf_h[cd_start:cd_end]

# Extract real DownloadBufferMemory clipping block from patched buffer_cache.h.
dl_start = buf_h.index('const DAddr buffer_begin = buffer.CpuAddr();')
dl_end = buf_h.index('    boost::container::small_vector<BufferCopy, 1> copies;', dl_start)
dl_clip_block = buf_h[dl_start:dl_end]

# Extract real StorageBufferBinding virtual-span calculation.
sb_start = buf_h.index('const GPUVAddr gpu_end = gpu_addr + size;')
sb_end = buf_h.index('    const u32 binding_size = static_cast<u32>(virtual_size);', sb_start)
sb_span_block = buf_h[sb_start:sb_end]

test_cpp = r'''
#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <vector>

using u32 = std::uint32_t;
using u64 = std::uint64_t;
using s32 = std::int32_t;
using DAddr = u64;
using GPUVAddr = u64;

namespace Core {
constexpr u64 DEVICE_PAGESIZE = 4096;
}
namespace Common {
constexpr u64 AlignUp(u64 value, u64 size) {
    return (value + size - 1) & ~(size - 1);
}
constexpr u64 AlignDown(u64 value, u64 size) {
    return value & ~(size - 1);
}
}
#define LOG_WARNING(...) ((void)0)

''' + is_draw_implausible + r'''

#include "video_core/buffer_cache/usage_tracker.h"
#include "video_core/delayed_destruction_ring.h"

struct BufferCopy {
    u64 src_offset;
    u64 dst_offset;
    u64 size;
};

static void TestUsageTracker() {
    VideoCommon::UsageTracker tracker(8192);
    // 12-byte indirect compute args at offset 0 (previously shift-by-64 UB!)
    assert(!tracker.IsUsed(0, 12));
    tracker.Track(0, 12);
    assert(tracker.IsUsed(0, 12));
    assert(tracker.IsUsed(60, 4));
    assert(!tracker.IsUsed(64, 64));

    // Unaligned range crossing 64-byte boundary: [124, 132) touches bits 1 and 2.
    tracker.Reset();
    tracker.Track(124, 8);
    assert(tracker.IsUsed(64, 64));
    assert(tracker.IsUsed(128, 64));
    assert(!tracker.IsUsed(192, 64));
    assert(!tracker.IsUsed(0, 64));

    // Exact page-aligned range [0, 4096) must NOT touch page 1!
    tracker.Reset();
    tracker.Track(0, 4096);
    assert(tracker.IsUsed(0, 4096));
    assert(tracker.IsUsed(4095, 1));
    assert(!tracker.IsUsed(4096, 64));

    // Single-page query past tracked bits on same page must return false.
    tracker.Reset();
    tracker.Track(64, 64);
    assert(!tracker.IsUsed(0, 64));
    assert(!tracker.IsUsed(128, 64));

    // Zero size and overflow handling.
    tracker.Track(0, 0);
    assert(!tracker.IsUsed(0, 0));
    tracker.Track(std::numeric_limits<u64>::max() - 16, 64);
    assert(tracker.IsUsed(std::numeric_limits<u64>::max() - 16, 64));
    assert(tracker.IsUsed(16384, 64)); // out-of-range conservatively true
}

static void TestDelayedDestructionRing() {
    int alive = 0;
    struct Obj {
        int* counter;
        explicit Obj(int* c) : counter(c) { ++(*counter); }
        Obj(Obj&& o) noexcept : counter(o.counter) { o.counter = nullptr; }
        Obj& operator=(Obj&& o) noexcept {
            if (this != &o) {
                if (counter) --(*counter);
                counter = o.counter;
                o.counter = nullptr;
            }
            return *this;
        }
        ~Obj() { if (counter) --(*counter); }
    };
    VideoCommon::DelayedDestructionRing<Obj, 6> ring;
    ring.Push(Obj(&alive));
    ring.Tick();
    ring.Push(Obj(&alive));
    ring.Tick();
    ring.Push(Obj(&alive));
    assert(alive == 3);
    const size_t freed = ring.ClearAfterSynchronization();
    assert(freed == 3 && alive == 0);
    assert(ring.ClearAfterSynchronization() == 0);
}

static void TestCancelPendingDownloads() {
    std::vector<std::vector<BufferCopy>> pending_downloads = {
        {
            BufferCopy{0x1000, 0, 0x400},    // split into [0x1000,0x1100) and [0x1300,0x1400)
            BufferCopy{0x1100, 0x400, 0x100}, // fully canceled
            BufferCopy{0x2000, 0x500, 0x200}, // untouched
        }
    };
    const DAddr device_addr = 0x1100;
    const u64 size = 0x200;
    {
''' + cancel_block + r'''
    }
    assert(pending_downloads[0].size() == 3);
    assert(pending_downloads[0][0].src_offset == 0x1000 && pending_downloads[0][0].size == 0x100 && pending_downloads[0][0].dst_offset == 0);
    assert(pending_downloads[0][1].src_offset == 0x2000 && pending_downloads[0][1].size == 0x200);
    assert(pending_downloads[0][2].src_offset == 0x1300 && pending_downloads[0][2].size == 0x100 && pending_downloads[0][2].dst_offset == 0x300);

    // Overflowing size must clamp instead of wrapping cancel_end to 0.
    std::vector<std::vector<BufferCopy>> pending2 = {
        {BufferCopy{0x5000, 0, 0x100}}
    };
    {
        auto& pending_downloads = pending2;
        const DAddr device_addr = 0x5000;
        const u64 size = std::numeric_limits<u64>::max();
''' + cancel_block + r'''
    }
    assert(pending2[0].empty());
}

struct MockBuf {
    DAddr addr;
    u64 bytes;
    DAddr CpuAddr() const { return addr; }
    u64 SizeBytes() const { return bytes; }
};

static std::pair<DAddr, u64> ClipDownload(const MockBuf& buffer, DAddr device_addr, u64 size) {
    bool returned = false;
    auto run = [&]() {
''' + dl_clip_block.replace('return;', 'returned = true; return;') + r'''
    };
    run();
    return returned ? std::pair<DAddr, u64>{0, 0} : std::pair<DAddr, u64>{device_addr, size};
}

static void TestDownloadClip() {
    MockBuf b{0x2000, 0x1000};
    assert((ClipDownload(b, 0x1000, 0x5000) == std::pair<DAddr, u64>{0x2000, 0x1000}));
    assert((ClipDownload(b, 0x1000, 0x1000) == std::pair<DAddr, u64>{0, 0}));
    assert((ClipDownload(b, 0x3000, 0x1000) == std::pair<DAddr, u64>{0, 0}));
    assert((ClipDownload(b, 0x2800, std::numeric_limits<u64>::max()) == std::pair<DAddr, u64>{0x2800, 0x800}));
}

static u64 ComputeStorageSpan(GPUVAddr gpu_addr, u32 size, u32 alignment, bool is_written,
                              DAddr aligned_cpu, DAddr unaligned_cpu) {
    const GPUVAddr aligned_gpu_addr = Common::AlignDown(gpu_addr, alignment);
    const u64 aligned_size = gpu_addr - aligned_gpu_addr + size;
    [[maybe_unused]] const GPUVAddr ssbo_addr = gpu_addr;
    const DAddr* aligned_device_addr = &aligned_cpu;
    const DAddr* device_addr = &unaligned_cpu;
    constexpr u64 NULL_BINDING = 0;
''' + sb_span_block + r'''
    return virtual_size;
}

static void TestStorageSpan() {
    // Physical discontinuity (unaligned_cpu < aligned_cpu) previously wrapped to ~4 GiB!
    // gpu_addr=0x10000120 aligned to 0x100 -> aligned_gpu=0x10000100, gpu_end=0x10001120, rounded_end=0x10002000 -> span=0x1f00.
    const u64 span = ComputeStorageSpan(0x10000120ULL, 0x1000, 0x100, false, 0x90000000ULL, 0x8e5c0120ULL);
    assert(span == 0x1f00);
    const u64 write_span = ComputeStorageSpan(0x10000120ULL, 0x1000, 0x100, true, 0x90000000ULL, 0x8e5c0120ULL);
    assert(write_span == 0x1020);
    // Overflowing payload span rejected as NULL_BINDING (0).
    const u64 bad = ComputeStorageSpan(std::numeric_limits<u64>::max() - 0x10, 0x100, 0x100, false, 0x1000, 0x1000);
    assert(bad == 0);
}

static void TestIndirectDrawGuard() {
    assert(IsDrawImplausible(6, 27232513));
    assert(IsDrawImplausible(6, 1000593163));
    assert(IsDrawImplausible(1, (u32{1} << 20) + 1));
    assert(!IsDrawImplausible(0, 31126795)); // zero-count no-op remains allowed
    assert(!IsDrawImplausible(6, 100000));
    assert(!IsDrawImplausible(1000, 1000));
    assert(IsDrawImplausible(100000, 3000)); // > 2^28 total vertices
}

int main() {
    TestUsageTracker();
    TestDelayedDestructionRing();
    TestCancelPendingDownloads();
    TestDownloadClip();
    TestStorageSpan();
    TestIndirectDrawGuard();
    std::puts("Buffer & draw safety: PASS usage masks, destruction ring, pending download cancel, download clip, storage virtual span, indirect draw ceiling");
    return 0;
}
'''

with tempfile.TemporaryDirectory(prefix='eden-buffer-draw-safety-') as tmp:
    t = Path(tmp)
    # Minimal stub headers for common_types.h and alignment.h so real Eden headers compile standalone.
    (t / 'common').mkdir()
    (t / 'common/common_types.h').write_text('#pragma once\n#include <cstdint>\nusing u32=std::uint32_t;\nusing u64=std::uint64_t;\n')
    (t / 'common/alignment.h').write_text('#pragma once\n#include <cstddef>\nnamespace Common { template<class T> constexpr T AlignUp(T v, size_t a) { return (v + a - 1) & ~(a - 1); } }\n')
    exe = t / 'test'
    subprocess.run([
        os.environ.get('CXX', 'c++'), '-std=c++20', '-Wall', '-Wextra', '-Werror',
        '-fsanitize=address,undefined', '-fno-sanitize-recover=undefined',
        '-I' + str(t), '-I' + str(source / 'src'),
        '-x', 'c++', '-', '-o', str(exe),
    ], input=test_cpp, text=True, check=True)
    subprocess.run([str(exe)], check=True, timeout=60)
