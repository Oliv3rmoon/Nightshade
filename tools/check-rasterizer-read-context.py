#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2026 Oliv3rmoon
# SPDX-License-Identifier: GPL-3.0-or-later
"""Exercise GPU readback on patched Eden guest cores and host workers.

Usage: python3 tools/check-rasterizer-read-context.py PATCHED_EDEN_SOURCE
"""

import os
from pathlib import Path
import subprocess
import sys
import tempfile


def extract(source, start, end):
    offset = source.index(start)
    return source[offset:source.index(end, offset)]


source = Path(sys.argv[1]) / "src"
memory = (source / "core/memory.cpp").read_text()
manager = (source / "core/device_memory_manager.h").read_text()
download = extract(memory, "    void HandleRasterizerDownload(", "    void HandleRasterizerWrite(")
lookups = extract(manager, "    template <typename Func>\n    void ApplyOpOnPAddr(",
                  "    PAddr GetPhysicalRawAddressFromDAddr(")

fixture = r'''
#include <array>
#include <atomic>
#include <barrier>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <thread>
#include "common/scratch_buffer.h"

using u8 = std::uint8_t;
using u32 = std::uint32_t;
using PAddr = std::uint64_t;
using VAddr = std::uint64_t;
using DAddr = std::uint64_t;

thread_local size_t core_id = 0;
thread_local size_t worker = 0;

struct Area {
    DAddr start_address{};
    DAddr end_address{};
};

struct DeviceMemory {
    static constexpr u32 page_bits = 12;
    static constexpr u32 page_mask = (1 << page_bits) - 1;
    static constexpr u32 MULTI_FLAG_BITS = 30;
    std::array<u8, 8192> backing{};
    std::array<u32, 2> compressed_device_addr{3, 1u << MULTI_FLAG_BITS};
    std::array<const void*, 3> scratch_addresses{};
    std::barrier<>* rendezvous{};

    template <typename T>
    PAddr GetRawPhysicalAddr(const T* p) const { return p - backing.data(); }

    void InnerGatherDeviceAddresses(Common::ScratchBuffer<u32>& scratch, PAddr) {
        scratch.resize_destructive(2);
        scratch[0] = 5;
        scratch[1] = 9;
        if (rendezvous) {
            scratch_addresses[worker] = scratch.data();
            rendezvous->arrive_and_wait();
            assert(scratch[0] == 5 && scratch[1] == 9);
        }
    }

    DeviceMemory& MemoryManager() { return *this; }
    // DEVICE_LOOKUPS
};

struct Renderer {
    std::array<std::atomic<unsigned>, 10> downloads{};
    [[nodiscard]] Area OnCPURead(DAddr address, size_t size) {
        assert(size == 32);
        const auto page = address >> 12;
        ++downloads.at(page);
        return {page << 12, (page + 1) << 12};
    }
};

struct System {
    DeviceMemory device;
    Renderer renderer;
    DeviceMemory& Host1x() { return device; }
    Renderer& GPU() { return renderer; }
    size_t GetCurrentHostThreadID() { return core_id; }
};

struct Memory {
    System system;
    std::array<Area, 4> rasterizer_read_areas{};
    std::array<Common::ScratchBuffer<u32>, 4> rasterizer_read_scratch{};
    std::array<Common::ScratchBuffer<u32>, 4> scratch_buffers{};

    template <typename Unmapped, typename Cached>
    const u8* GetPointerImpl(VAddr address, Unmapped, Cached) const {
        return system.device.backing.data() + address;
    }

    // DOWNLOAD
};

int main() {
    Memory memory;
    auto& device = memory.system.device;
    auto& downloads = memory.system.renderer.downloads;
    core_id = 3;
    memory.HandleRasterizerDownload(64, 32);
    memory.HandleRasterizerDownload(128, 32);
    assert(downloads[3] == 1);

    std::thread worker_read([&] {
        core_id = UINT8_MAX;
        memory.HandleRasterizerDownload(64, 32);
        memory.HandleRasterizerDownload(64, 32);
    });
    worker_read.join();
    memory.HandleRasterizerDownload(64, 32);
    assert(downloads[3] == 3);

    std::barrier rendezvous(3);
    device.rendezvous = &rendezvous;
    std::thread guest([&] {
        core_id = 3;
        worker = 0;
        memory.HandleRasterizerDownload(4096 + 64, 32);
    });
    std::thread host([&] {
        core_id = UINT8_MAX;
        worker = 1;
        memory.HandleRasterizerDownload(4096 + 64, 32);
    });
    worker = 2;
    unsigned aliases = 0;
    device.ApplyOpOnPointer(device.backing.data() + 4096 + 64, memory.scratch_buffers[3],
                            [&](DAddr) { ++aliases; });
    guest.join();
    host.join();
    const auto [guest_scratch, host_scratch, write_scratch] = device.scratch_addresses;
    assert(guest_scratch && host_scratch && write_scratch);
    assert(guest_scratch != host_scratch && host_scratch != write_scratch &&
           guest_scratch != write_scratch);
    assert(aliases == 2 && downloads[5] == 2 && downloads[9] == 2);
    assert(memory.rasterizer_read_areas[3].start_address == 9 * 4096);
    std::puts("Rasterizer readback: PASS guest-core caching and isolated host/read/write alias lookups");
}
'''.replace("// DEVICE_LOOKUPS", lookups).replace("// DOWNLOAD", download)

with tempfile.TemporaryDirectory(prefix="rasterizer-read-") as scratch:
    test = Path(scratch) / "test.cpp"
    binary = test.with_suffix("")
    test.write_text(fixture)
    subprocess.run([os.environ.get("CXX", "c++"), "-std=c++20", "-pthread", "-Wall", "-Wextra",
                    "-Werror", f"-I{source}", str(test), "-o", str(binary)], check=True)
    subprocess.run([binary], check=True, timeout=10)
