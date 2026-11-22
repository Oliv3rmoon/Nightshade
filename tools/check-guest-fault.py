#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2026 Oliv3rmoon
# SPDX-License-Identifier: GPL-3.0-or-later
"""Host ASan/UBSan checks for guest fault/panic recovery and call-site evidence (headless/guest_fault.h)."""
from pathlib import Path
import os
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
cmake = (root / 'headless/CMakeLists.txt').read_text()
main = (root / 'headless/main.cpp').read_text()

assert '::Eden::FaultCallSite(' in cmake
assert '::Eden::FaultLine(' in cmake
assert '::Eden::PanicEvidence(' in cmake
assert '::Eden::RecordGuestPanic(' in cmake
assert 'Eden::GuestFaultMayRetry(completion->guest_fault)' in main
assert 'Render:Debug Render.Vulkan:Debug HW.GPU:Debug Shader:Debug Core.ARM:Debug' in main

code = r'''
#include "guest_fault.h"
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <string>
#include <unordered_map>
#include <vector>

int main() {
    // 1. GuestFaultMayRetry blocks deterministic panics, allows prefetch abort boot retries.
    assert(Eden::GuestFaultMayRetry("guest thread Main jumped to an unmapped address"));
    assert(!Eden::GuestFaultMayRetry("guest panic: reason 0x0 on thread 42"));
    assert(Eden::GuestFaultMayRetry(""));

    // 2. RecordGuestPanic persists first fault and does not overwrite an earlier fault.
    (void)Eden::TakeGuestFault();
    Eden::RecordGuestPanic(0x2, 99);
    Eden::RecordGuestFault("later fault must be ignored");
    const std::string taken = Eden::TakeGuestFault();
    assert(taken == "guest panic: reason 0x2 on thread 99");
    assert(Eden::TakeGuestFault().empty());

    // 3. FaultLine formats thread, registers, and capped backtrace.
    std::vector<std::uint64_t> bt;
    for (int i = 0; i < 20; ++i) bt.push_back(0x80000000ULL + i * 4);
    const std::string line = Eden::FaultLine("RenderThread", 7, 0x4, 0x1000, 0x2000, bt);
    assert(line.find("EDEN_GUEST_FAULT thread=RenderThread tid=7 pc=0x4 lr=0x1000 sp=0x2000 bt=0x80000000") == 0);
    // Must cap at 16 entries (0x8000003c is index 15, 0x80000040 is index 16).
    assert(line.find("0x8000003c") != std::string::npos);
    assert(line.find("0x80000040") == std::string::npos);

    // 4. FaultCallSite boundary checks including lr == UINT64_MAX - 15 (must not wrap!) and lr == 0x1000.
    std::vector<std::string> emitted;
    auto collect = [&](const char* s) { emitted.emplace_back(s); };
    Eden::FaultCallSite(12, [](std::uint64_t, std::uint64_t) { return true; },
                        [](std::uint64_t) { return 0u; }, collect);
    Eden::FaultCallSite(0x1002, [](std::uint64_t, std::uint64_t) { return true; },
                        [](std::uint64_t) { return 0u; }, collect);
    Eden::FaultCallSite((std::numeric_limits<std::uint64_t>::max)() - 15,
                        [](std::uint64_t, std::uint64_t) { return true; },
                        [](std::uint64_t) { return 0u; }, collect);
    assert(emitted.empty());

    Eden::FaultCallSite(0x1000,
                        [](std::uint64_t addr, std::uint64_t) { return addr != 0x1004; },
                        [](std::uint64_t addr) { return static_cast<std::uint32_t>(addr); }, collect);
    assert(emitted.size() == 8);
    assert(emitted[3].find("0xffc") != std::string::npos && emitted[3].find("before_lr=1") != std::string::npos);
    assert(emitted[5].find("0x1004 unmapped") != std::string::npos);

    // 5. PanicEvidence walks valid FP chain, reads 4-byte result, stops on cycle/unaligned/limit.
    emitted.clear();
    std::unordered_map<std::uint64_t, std::uint64_t> mem64;
    mem64[0x4000] = 0x4020; mem64[0x4008] = 0x81000;
    mem64[0x4020] = 0x4010; mem64[0x4028] = 0x82000; // next <= fp must stop!
    Eden::PanicEvidence(0x5000, 4, {0x80000, 0x80004, 0x3f00, 0x4000},
                        [&](std::uint64_t addr, std::uint64_t sz) {
                            return addr == 0x5000 || (sz == 16 && mem64.count(addr));
                        },
                        [](std::uint64_t) { return 0xdeadbeefu; },
                        [&](std::uint64_t addr) { return mem64[addr]; },
                        collect);
    assert(emitted.size() == 5); // context + result + 2 frames + end
    assert(emitted[1] == "EDEN_GUEST_PANIC_RESULT value=deadbeef");
    assert(emitted[2].find("index=0 fp=0x4000 lr=0x81000") != std::string::npos);
    assert(emitted[3].find("index=1 fp=0x4020 lr=0x82000") != std::string::npos);
    assert(emitted[4] == "EDEN_GUEST_PANIC_CONTEXT_END");

    // 6. PanicEvidence must reject base+size wraparound at UINT64_MAX - 3 and UINT64_MAX - 15
    // (mirrors Memory::IsValidVirtualAddressRange where end = base + size wraps to 0).
    emitted.clear();
    bool bad_read = false;
    auto is_valid_like_eden = [](std::uint64_t base, std::uint64_t size) {
        const std::uint64_t end = base + size;
        for (std::uint64_t page = base & ~std::uint64_t{0xfff}; page < end; page += 0x1000) {
            return false;
        }
        return true; // wrap-to-zero skips loop and falsely returns true!
    };
    const std::uint64_t wrap_info1 = (std::numeric_limits<std::uint64_t>::max)() - 3;
    const std::uint64_t wrap_fp = (std::numeric_limits<std::uint64_t>::max)() - 15; // 0xfffffffffffffff0 (8-byte aligned!)
    Eden::PanicEvidence(wrap_info1, 4, {0x80000, 0x80004, wrap_fp - 0x100, wrap_fp},
                        is_valid_like_eden,
                        [&](std::uint64_t) { bad_read = true; return 0u; },
                        [&](std::uint64_t) { bad_read = true; return 0ULL; },
                        collect);
    assert(!bad_read);
    assert(emitted.size() == 2); // only context + end, zero reads!

    std::puts("Guest fault & panic: PASS retry policy, panic recording, call-site & panic wrap guards, bounded frame walk");
    return 0;
}
'''

with tempfile.TemporaryDirectory(prefix='eden-guest-fault-') as tmp:
    exe = Path(tmp) / 'guest-fault'
    subprocess.run([
        os.environ.get('CXX', 'c++'), '-std=c++20', '-Wall', '-Wextra', '-Werror',
        '-fsanitize=address,undefined', '-fno-sanitize-recover=undefined',
        '-I' + str(root / 'headless'),
        '-x', 'c++', '-', '-o', str(exe),
    ], input=code, text=True, check=True)
    subprocess.run([str(exe)], check=True, timeout=60)
