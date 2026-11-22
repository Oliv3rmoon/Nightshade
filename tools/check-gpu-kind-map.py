#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Exercise actual GPU small-page mapping and layout lookup with Eden's real RangeMap.

Usage: check-gpu-kind-map.py EDEN_SOURCE [--tsan]

Only the page-table/rasterizer boundary is modeled. The production methods and
RangeMap remain unchanged. A held reader lock must exclude map mutations, then
concurrent mapping/lookup must preserve valid layout values. --tsan runs the
real map stress under ThreadSanitizer (useful for the unpatched negative control).
"""
import os
from pathlib import Path
import subprocess
import sys
import tempfile


def function(text, signature):
    start = text.index(signature)
    brace = text.index("{", start)
    end, depth = brace + 1, 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end]


source = Path(sys.argv[1])
tsan = "--tsan" in sys.argv[2:]
manager = (source / "src/video_core/memory_manager.cpp").read_text()
methods = "\n".join(function(manager, signature) for signature in (
    "GPUVAddr MemoryManager::PageTableOp(",
    "PTEKind MemoryManager::GetPageKind(",
    "size_t MemoryManager::GetMemoryLayoutSize("))
range_map = (source / "src/common/range_map.h").read_text().replace(
    '#include "common/common_types.h"', "// Only standard key/value types are used by this fixture.")

code = r'''
#include <array>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <future>
#include <limits>
#include <mutex>
#include <thread>
#include "range_map.h"
using u32 = std::uint32_t;
using u64 = std::uint64_t;
namespace Tegra {
using GPUVAddr = u64;
using DAddr = u64;
enum class PTEKind { INVALID, Linear, Block };
struct ObservedMap : Common::RangeMap<GPUVAddr, PTEKind> {
    ObservedMap() : Common::RangeMap<GPUVAddr, PTEKind>{PTEKind::INVALID} {}
    std::atomic<unsigned> writes{};
    void Map(GPUVAddr begin, GPUVAddr end, PTEKind kind) {
        writes.fetch_add(1, std::memory_order_relaxed);
        Common::RangeMap<GPUVAddr, PTEKind>::Map(begin, end, kind);
    }
};
class MemoryManager {
public:
    enum class EntryType { Free, Mapped };
    GPUVAddr PageTableOp(GPUVAddr, DAddr, size_t, PTEKind, EntryType);
    PTEKind GetPageKind(GPUVAddr) const;
    size_t GetMemoryLayoutSize(GPUVAddr, size_t) const;
    static constexpr u64 page_size = 4096, cpu_page_bits = 12;
    static constexpr unsigned unique_identifier = 1;
    struct PageTable {
        std::array<u32, 64> pages{};
        void ReserveRange(GPUVAddr, size_t) {}
        u32& operator[](size_t i) { return pages.at(i); }
    } page_table;
    struct Rasterizer {
        void ModifyGPUMemory(unsigned, GPUVAddr, size_t) {}
    } rasterizer_object;
    Rasterizer* rasterizer = &rasterizer_object;
    EntryType GetEntry(GPUVAddr, bool) { return EntryType::Free; }
    void SetEntry(GPUVAddr, EntryType, bool) {}
    size_t PageEntryIndex(GPUVAddr address, bool) { return address / page_size; }
    mutable std::mutex guard;
    ObservedMap kind_map;
};
// PRODUCTION_METHODS
} // namespace Tegra

int main() {
    using namespace Tegra;
#ifndef RACE_STRESS_ONLY
    // Represent a layout reader holding the real reader mutex. A small-page
    // writer must not enter the actual map until that reader gives up the lock.
    {
        MemoryManager manager;
        std::unique_lock held(manager.guard);
        std::promise<void> started;
        auto ready = started.get_future();
        std::thread writer([&] {
            started.set_value();
            manager.PageTableOp(4096, 4096, 4096, PTEKind::Linear,
                                MemoryManager::EntryType::Mapped);
        });
        ready.wait();
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        const bool violated = manager.kind_map.writes.load() != 0;
        held.unlock();
        writer.join();
        if (violated) {
            std::fputs("FAIL: small-page writer mutated layout while reader held guard\n", stderr);
            return 1;
        }
        assert(manager.kind_map.writes == 1);
        assert(manager.GetPageKind(4096) == PTEKind::Linear);
        assert(manager.GetMemoryLayoutSize(4096, 4096) == 4096);
    }
#endif
    // Actual std::map node insertion/erasure versus actual production lookups.
    // The unpatched method races here under TSan, independently of the first check.
    MemoryManager manager;
    manager.PageTableOp(4096, 4096, 4096, PTEKind::Linear,
                        MemoryManager::EntryType::Mapped);
    std::atomic<bool> begin{}, done{};
    std::atomic<u64> reads{};
    std::thread reader([&] {
        while (!begin.load(std::memory_order_acquire)) std::this_thread::yield();
        unsigned i = 0;
        do {
            const auto address = 4096 + (++i % 32) * 4096;
            const auto kind = manager.GetPageKind(address);
            assert(kind == PTEKind::INVALID || kind == PTEKind::Linear || kind == PTEKind::Block);
            (void)manager.GetMemoryLayoutSize(address, 4096);
            ++reads;
        } while (!done.load(std::memory_order_acquire));
    });
    begin.store(true, std::memory_order_release);
    for (unsigned i = 0; i < 20000; ++i) {
        const auto address = 4096 + (i % 32) * 4096;
        const auto kind = i % 3 == 0 ? PTEKind::INVALID :
                          i % 3 == 1 ? PTEKind::Linear : PTEKind::Block;
        manager.PageTableOp(address, address, 4096, kind,
                            MemoryManager::EntryType::Mapped);
    }
    done.store(true, std::memory_order_release);
    reader.join();
    assert(reads > 0);
    std::printf("GPU kind map: PASS reader/writer serialization, 20000 production small-page maps, %llu lookups\n",
                static_cast<unsigned long long>(reads.load()));
}
'''.replace("// PRODUCTION_METHODS", methods)

with tempfile.TemporaryDirectory(prefix="gpu-kind-map-") as temporary:
    work = Path(temporary)
    (work / "range_map.h").write_text(range_map)
    (work / "check.cpp").write_text(code)
    flags = ["-fsanitize=thread", "-DRACE_STRESS_ONLY"] if tsan else [
        "-fsanitize=address,undefined", "-fno-sanitize-recover=all"]
    subprocess.run([os.environ.get("CXX", "c++"), "-std=c++20", "-O1", "-g", "-pthread",
                    "-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter", *flags,
                    "-fno-omit-frame-pointer", str(work / "check.cpp"),
                    "-o", str(work / "check")], check=True)
    subprocess.run([str(work / "check")], check=True, timeout=60)
