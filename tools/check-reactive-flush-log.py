#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2026 Oliv3rmoon
# SPDX-License-Identifier: GPL-3.0-or-later
"""Check the lines that say what a guest read of GPU-written memory copied back, where a guest thread
read unmapped memory, and where it jumped (Oliv3rmoon/Nightshade#1).

  headless/reactive_flush_log.h   a session's first 64 copies, then each power of two; the first 16
                                  also in stderr.log; the bytes at the read before and after
  headless/unmapped_log.h         a session's first 32 unmapped reads with the guest's pc, the
                                  first 4 also in stderr.log; a new session counts again
  headless/guest_fault.h          the stderr.log line for a guest thread's jump to an unmapped address
  headless/crash_report.cpp       after a crash, Eden's log is written out after stderr and stdout

Checks the build's edits are in place (headless/CMakeLists.txt, tools/prepare-vulkan-port.py,
headless/main.cpp), builds the three headers into a small program and runs it. With an Eden source
tree as the argument (the patched one the build uses), also checks that every anchor is there once.
"""
from pathlib import Path
import os, re, subprocess, sys, tempfile

root = Path(__file__).resolve().parents[1]
cmake = (root / 'headless/CMakeLists.txt').read_text()
port = (root / 'tools/prepare-vulkan-port.py').read_text()
main = (root / 'headless/main.cpp').read_text()
crash = (root / 'headless/crash_report.cpp').read_text()


def bracket(name):
    match = re.search(r'set\(' + name + r' \[=\[(.*?)\]=\]\)', cmake, re.S)
    assert match, f'{name} is set in headless/CMakeLists.txt'
    return match.group(1)


assert 'message(FATAL_ERROR "Pinned reactive flush in GPU::Impl::OnCPURead changed")' in cmake
assert '#include \\"${EDEN_PORT_DIR}/reactive_flush_log.h\\"' in cmake
assert '::Eden::ReactiveFlushLog::Active() = eden_active;' in bracket('reactive_flush_new')
assert 'message(FATAL_ERROR "Pinned A64 unmapped read pc log changed")' in cmake
assert '::Eden::UnmappedLog::PcWanted(&eden_seen)' in bracket('unmapped_pc_new')
assert cmake.count('::Eden::FaultLine(eden_name, thread->GetThreadId(), eden_context.pc,') == 1
assert '::Eden::RecordGuestFault(\\"guest thread \\" + eden_name + \\" jumped to an unmapped address\\");' in cmake
assert 'Eden::ReactiveFlushLog::Reset();' in main and 'Eden::UnmappedLog::Reset();' in main
assert 'Eden::Crash::SetLogFlush([] { Common::Log::Stop(); });' in main
assert 'if (void (*const flush)() = log_flush.load(std::memory_order_acquire)) flush();' in crash
# The image lines go in after the download batch: their anchor is part of download_prefix.
batch_at = port.index("(download_prefix, download_prefix + (port / 'vulkan_download_batch.inc')")
image_at = port.index("(port / 'reactive_flush_image.inc').read_text()")
assert batch_at < image_at, 'the image lines follow the download batch'
assert '#include "reactive_flush_log.h"' in port

if len(sys.argv) > 1:
    source = Path(sys.argv[1])
    def once(relative, text):
        count = (source / relative).read_text().count(text)
        assert count == 1, f'{relative}: anchor found {count} times: {text[:70]!r}'
    once('src/video_core/gpu.cpp', bracket('reactive_flush_old'))
    once('src/core/arm/dynarmic/arm_dynarmic_64.cpp', bracket('unmapped_pc_old'))
    once('src/core/hle/kernel/physical_core.cpp',
         '            } else {\n                interface->LogBacktrace(process);\n            }\n'
         '            thread->RequestSuspend(kernel, SuspendType::Debug);')
    once('src/video_core/texture_cache/texture_cache.h',
         '    ForEachImageInRegion(cpu_addr, size, [this, &images](ImageId image_id, ImageBase& image) {\n'
         '        if (!IsDownloadable(image)) {\n')
    print('Reactive flush log: the anchors are in', source)

code = r'''
#include "guest_fault.h"
#include "reactive_flush_log.h"
#include "unmapped_log.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
namespace R = Eden::ReactiveFlushLog;
int main() {
    unsigned logged = 0, to_stderr = 0;
    std::uint64_t seen = 0;
    for (int i = 0; i < 1000000; ++i)
        if (R::Wanted(&seen)) { ++logged; to_stderr += R::ToStderr(seen); }
    assert(seen == 1000000 && logged == 64 + 13 && to_stderr == 16);
    R::Reset();
    assert(R::Wanted(&seen) && seen == 1);

    // The GPU thread's mark is its own.
    R::Active() = 7;
    std::uint64_t other = 1;
    std::thread([&] { other = R::Active(); }).join();
    assert(other == 0 && R::Active() == 7);
    R::Active() = 0;

    // The bytes stop at the end of the read's 4 KiB page.
    assert(R::PeekSize(0x2cb41000) == 32 && R::PeekSize(0x2cb41ff8) == 8 && R::PeekSize(0x2cb41fff) == 1);
    R::Peek before{}, after{};
    before.size = after.size = 8;
    const unsigned char pointer[8] = {0x40, 0x4f, 0x87, 0x34, 0x10, 0, 0, 0};
    std::memcpy(before.bytes, pointer, 8);
    const std::string changed = R::Line(1, 0x2cb41010, 8, 0x2cb41000, 0x2cb4f000, 2, before, after);
    assert(changed == "EDEN_REACTIVE_FLUSH #1 read 0x2cb41010+8 on guest core 2 copied [0x2cb41000, 0x2cb4f000) "
                      "(56 KiB): at the read 404f873410000000 before, 0000000000000000 after (changed)");
    const std::string same = R::Line(2, 0x10, 4, 0, 0x1000, 9, after, after);
    assert(same.find("on a host thread") != std::string::npos && same.ends_with("(unchanged)"));

    // A session's first 32 unmapped reads say where the guest was, the first 4 in stderr.log too.
    unsigned with_pc = 0, pc_stderr = 0;
    for (int i = 0; i < 1000; ++i)
        if (Eden::UnmappedLog::PcWanted(&seen)) { ++with_pc; pc_stderr += Eden::UnmappedLog::PcToStderr(seen); }
    assert(with_pc == 32 && pc_stderr == 4);
    Eden::UnmappedLog::Reset();
    assert(Eden::UnmappedLog::PcWanted(&seen) && seen == 1 && Eden::UnmappedLog::Wanted(&seen) && seen == 1);

    const std::string fault = Eden::FaultLine("GameThread", 100, 0x4, 0x83322a90, 0x11002837a0,
                                              {0x4, 0x83322a90, 0x83322c94});
    assert(fault == "EDEN_GUEST_FAULT guest thread 100 GameThread jumped to 0x4: lr 0x83322a90 sp 0x11002837a0, "
                    "calls 0x4 0x83322a90 0x83322c94");
    assert(Eden::FaultLine("?", 1, 0, 0, 0, {}).ends_with("calls unknown"));
    std::puts(changed.c_str());
    std::puts(fault.c_str());
    std::puts("Reactive flush log: PASS 77 lines for a million copies, 16 in stderr.log; 32 unmapped reads "
              "with the pc a session, 4 in stderr.log; the fault line; again from the start each session");
}
'''
with tempfile.TemporaryDirectory() as scratch:
    program = Path(scratch) / 'reactive_flush_log_check.cpp'
    binary = Path(scratch) / 'reactive_flush_log_check'
    program.write_text(code)
    subprocess.run([os.environ.get('CXX', 'c++'), '-std=c++20', '-Wall', '-Wextra', '-Werror', '-pthread',
                    f'-I{root / "headless"}', str(program), '-o', str(binary)], check=True)
    result = subprocess.run([str(binary)], capture_output=True, text=True)
    sys.stdout.write(result.stdout)
    sys.stderr.write(result.stderr)
    sys.exit(result.returncode)
