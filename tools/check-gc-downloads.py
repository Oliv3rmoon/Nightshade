#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2026 Oliv3rmoon
# SPDX-License-Identifier: GPL-3.0-or-later
"""Exercise the generated Vulkan texture collector, including its real download batching.

Usage: check-gc-downloads.py EDEN_SOURCE

Uses the pinned Eden collector and the same port transformations as the build. Only the GPU
and image bookkeeping boundary are simulated. Queued copies become visible at Finish; tests
check publication order, released staging maps, eviction counts and GPU drains. A prefix of
kept images must not turn twenty dirty evictions into twenty synchronous drains.
"""
from pathlib import Path
import os
import subprocess
import sys
import tempfile

root = Path(__file__).resolve().parents[1]
source = Path(sys.argv[1])
script = (root / 'tools/prepare-vulkan-port.py').read_text()
begin = script.index('download_source = ')
end = script.index('texture_costs = []', begin)
scope = {'source': source, 'port': root / 'headless'}
exec(compile(script[begin:end], 'prepare-vulkan-port.py collector transforms', 'exec'), scope)
collector = scope['gc']

code = r'''
#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <ranges>
#include <span>
#include <utility>
#include <vector>
using u64 = std::uint64_t;
using ImageId = unsigned;
constexpr size_t operator""_MiB(unsigned long long n) { return size_t(n) << 20; }
constexpr unsigned IsDecoding=1, BadOverlap=2, CostlyLoad=4, Tracked=8, Converted=16, AcceleratedUpload=32;
namespace ImageFlagBits {
constexpr auto IsDecoding=::IsDecoding, BadOverlap=::BadOverlap, CostlyLoad=::CostlyLoad,
               Tracked=::Tracked, Converted=::Converted, AcceleratedUpload=::AcceleratedUpload;
}
bool True(unsigned flag) { return flag != 0; }
bool False(unsigned flag) { return flag == 0; }
namespace Common { u64 AlignUp(u64 n, u64 alignment) { return (n + alignment - 1) / alignment * alignment; } }
namespace boost::container { template<class T, size_t> using small_vector = std::vector<T>; }
namespace Eden::Performance {
inline std::atomic<bool> gc_count_visits{false};
inline bool keep_dirty = false;
bool KeepDirtyTextures() { return keep_dirty; }
inline unsigned gc_batched_drain=0, gc_unbatched_drain=0;
struct Timer { Timer(unsigned&, size_t) {} };
}
namespace Eden::TextureEvictions { void Freed(bool, bool, bool) {} }
template<class F> struct ScopeExit { F f; ~ScopeExit() { f(); } };
struct ScopeMaker { template<class F> auto operator+(F f) { return ScopeExit<F>{f}; } };
#define SCOPE_EXIT auto on_exit = ScopeMaker{} + [&]()
struct Info { int format=0; };
struct ImageBase {
    unsigned flags=0, gpu_addr=0;
    size_t guest_size_bytes=1_MiB, unswizzled_size_bytes=1_MiB;
    u64 scale_tick=0;
    Info info;
    bool HasScaled() const { return false; }
};
bool IsPixelFormatASTC(int) { return false; }
u64 GetScaledImageSizeBytes(const ImageBase&) { return 0; }
u64 TranscodedAstcSize(u64 n, int) { return n; }
std::vector<unsigned> FullDownloadCopies(const Info&) { return {1}; }
template<class T> T FixSmallVectorADL(T value) { return value; }
struct Allocation { std::vector<std::uint8_t> bytes; bool released=false; };
struct Map { std::shared_ptr<Allocation> allocation; std::span<std::uint8_t> mapped_span; };
namespace Vulkan {
struct TextureCacheRuntime {
    unsigned drains=0, deferred=0, freed=0, downloads=0;
    std::vector<std::pair<unsigned, std::shared_ptr<Allocation>>> pending;
    Map DownloadStagingBuffer(size_t n, bool pin=false) {
        deferred += pin;
        auto a=std::make_shared<Allocation>(); a->bytes.resize(n);
        return {a, a->bytes};
    }
    void FreeDeferredStagingBuffer(Map& map) {
        assert(!map.allocation->released); map.allocation->released=true; ++freed;
    }
    void Finish() {
        ++drains;
        for (auto& [id,a] : pending) {
            assert(!a->released);
            std::fill(a->bytes.begin(),a->bytes.end(),std::uint8_t(id+1));
        }
        pending.clear();
    }
};
}
struct Image : ImageBase {
    Vulkan::TextureCacheRuntime* runtime=nullptr;
    bool dirty=true, live=true;
    void DownloadMemory(Map& map, const std::vector<unsigned>&) {
        ++runtime->downloads; runtime->pending.emplace_back(gpu_addr,map.allocation);
    }
};
static std::vector<unsigned> published;
void SwizzleImage(int&, unsigned id, const Info&, const std::vector<unsigned>&,
                  std::span<std::uint8_t> bytes, std::vector<std::uint8_t>&) {
    assert(!bytes.empty());
    for (size_t i=0; i<bytes.size(); i+=4093) assert(bytes[i] == std::uint8_t(id+1));
    published.push_back(id);
}
template<class P> struct TextureCache {
    using Runtime=P;
    P runtime;
    std::vector<Image> slot_images;
    u64 total_used_memory=200_MiB, expected_memory=1_MiB, critical_memory=10000_MiB, frame_tick=1000;
    int memory=0, *gpu_memory=&memory;
    std::vector<std::uint8_t> swizzle_data_buffer;
    unsigned deleted=0;
    struct Lru {
        TextureCache* cache;
        unsigned visits=0;
        template<class F> void ForEachItemBelow(u64, F callback) {
            for (unsigned i=0; i<cache->slot_images.size(); ++i) {
                if (!cache->slot_images[i].live) continue;
                ++visits;
                if (callback(i)) break;
            }
        }
    } lru_cache{this};
    bool IsDownloadable(const Image& image) { return image.dirty; }
    void UntrackImage(Image&, ImageId) {}
    void UnregisterImage(ImageId) {}
    void DeleteImage(ImageId id, bool) {
        auto& image=slot_images[id]; assert(image.live); image.live=false;
        total_used_memory = image.unswizzled_size_bytes < total_used_memory
                                ? total_used_memory - image.unswizzled_size_bytes : 0;
        ++deleted;
    }
    void Add(unsigned n, unsigned flags, bool dirty=true) {
        for (unsigned i=0; i<n; ++i) {
            Image image; image.flags=flags; image.dirty=dirty;
            image.gpu_addr=unsigned(slot_images.size()); image.runtime=&runtime;
            slot_images.push_back(image);
        }
    }
    void RunGarbageCollector();
};
''' + 'template<class P>\n' + collector + r'''
int main() {
    using Cache=TextureCache<Vulkan::TextureCacheRuntime>;
    // Decoding images survive both passes. They must not exhaust the prefetch allowance:
    // the actual Cleanup counts twenty evictions after this prefix, not twenty visits.
    Cache cache;
    cache.Add(32,IsDecoding); cache.Add(20,0);
    published.clear();
    cache.RunGarbageCollector();
    assert(cache.deleted==20 && cache.runtime.downloads==20);
    std::printf("kept prefix: evictions=%u downloads=%u drains=%u\n", cache.deleted,
                cache.runtime.downloads, cache.runtime.drains);
    std::fflush(stdout);
    assert(cache.runtime.drains==1 && "kept LRU prefix must not force one GPU drain per dirty eviction");
    assert(cache.runtime.freed==20 && cache.runtime.pending.empty());
    assert(published.size()==20);
    for (unsigned i=0;i<20;++i) assert(published[i]==32+i);
    // Costly images retained in the first pass have the same behavior as decoding ones.
    Cache costly; costly.Add(32,CostlyLoad); costly.Add(20,0);
    costly.RunGarbageCollector();
    assert(costly.deleted==20 && costly.runtime.drains==1 && costly.runtime.freed==20);
    // Clean images still spend the eviction allowance. Prefetch must not copy the extra five
    // dirty images Cleanup will keep after its twenty evictions have been used.
    Cache mixed; mixed.Add(32,IsDecoding); mixed.Add(5,0,false); mixed.Add(20,0);
    mixed.RunGarbageCollector();
    assert(mixed.deleted==20 && mixed.runtime.downloads==15);
    assert(mixed.runtime.drains==1 && mixed.runtime.freed==15);
    // The diagnostic upstream visit-counting mode retains its original eviction allowance.
    Eden::Performance::gc_count_visits=true;
    Cache upstream; upstream.Add(32,IsDecoding); upstream.Add(20,0);
    upstream.RunGarbageCollector();
    assert(upstream.deleted==0 && upstream.runtime.downloads==0 && upstream.runtime.drains==0);
    Eden::Performance::gc_count_visits=false;
    // Lookahead itself is bounded when none of thousands of images may go.
    Cache bounded; bounded.Add(9000,IsDecoding); bounded.Add(20,0);
    bounded.RunGarbageCollector();
    assert(bounded.deleted==0 && bounded.runtime.downloads==0);
    assert(bounded.lru_cache.visits<=2*8193);
    // Clean evictions must subtract reclaimed bytes from usage so lookahead stops
    // once usage drops below expected_memory and does not copy retained dirty images.
    Cache clean_stops;
    clean_stops.total_used_memory = 10_MiB;
    clean_stops.expected_memory = 8_MiB;
    clean_stops.Add(3, 0, false); // 3 MiB clean brings usage to 7 MiB < expected_memory
    clean_stops.Add(5, 0, true);  // must NOT be prefetched or deleted
    clean_stops.RunGarbageCollector();
    assert(clean_stops.deleted == 3 && clean_stops.runtime.downloads == 0 && clean_stops.runtime.drains == 0);
    // Saturating usage subtraction when reclaimed bytes exceed tracked total_used_memory.
    Cache saturate;
    saturate.total_used_memory = 2_MiB;
    saturate.expected_memory = 1_MiB;
    saturate.Add(1, 0, false);
    saturate.slot_images[0].guest_size_bytes = 4_MiB;
    saturate.slot_images[0].unswizzled_size_bytes = 4_MiB;
    saturate.Add(2, 0, true);
    saturate.RunGarbageCollector();
    assert(saturate.deleted == 1 && saturate.runtime.downloads == 0);
    // Refilling on the exact final iteration (num_iterations == 0 in Cleanup) must still batch via include_current=true.
    Cache refill;
    refill.total_used_memory = 500_MiB;
    refill.expected_memory = 1_MiB;
    // 3 clean (1 MiB) + 16 dirty (4 MiB, fills 64 MiB batch) + 1 final dirty (4 MiB, hits num_iterations==0 on refill).
    refill.Add(3, 0, false);
    refill.Add(17, 0, true);
    for (unsigned i = 3; i < refill.slot_images.size(); ++i) {
        refill.slot_images[i].guest_size_bytes = 4_MiB;
        refill.slot_images[i].unswizzled_size_bytes = 4_MiB;
    }
    refill.RunGarbageCollector();
    assert(refill.deleted == 20 && refill.runtime.downloads == 17);
    assert(refill.runtime.drains == 2 && refill.runtime.freed == 17);
    std::puts("GC downloads: PASS kept-image prefixes, clean-usage stop, saturating accounting, refill batching, visit-mode control, bounded scan");
}
'''
with tempfile.TemporaryDirectory(prefix='eden-gc-downloads-') as temporary:
    tmp = Path(temporary)
    exe = tmp / 'gc-downloads'
    subprocess.run([os.environ.get('CXX', 'c++'), '-std=c++20', '-Wall', '-Wextra', '-Werror', '-pthread',
                    '-fsanitize=address,undefined', '-fno-sanitize-recover=undefined',
                    '-x', 'c++', '-', '-o', str(exe)], input=code, text=True, check=True)
    subprocess.run([str(exe)], check=True, timeout=60)
