#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2026 Oliv3rmoon
# SPDX-License-Identifier: GPL-3.0-or-later
"""Check that unmapped guest accesses are logged sparingly (headless/unmapped_log.h).

A session logs its first 64, then each power of two with its count; a new session counts again.
Checks that Eden's unmapped read and write lines go through it and that main.cpp resets it.
"""
from pathlib import Path
import os, subprocess, sys, tempfile

root = Path(__file__).resolve().parents[1]
cmake = (root / 'headless/CMakeLists.txt').read_text()
main = (root / 'headless/main.cpp').read_text()
assert cmake.count('::Eden::UnmappedLog::Wanted(&eden_seen)') == 2, 'the read and the write line'
assert 'message(FATAL_ERROR "Pinned unmapped access log changed (${pair})")' in cmake
assert 'Eden::UnmappedLog::Reset();' in main

code = r'''
#include "unmapped_log.h"
#include <cassert>
#include <cstdio>
int main() {
    unsigned logged = 0;
    std::uint64_t seen = 0, last = 0;
    for (int i = 0; i < 1000000; ++i)
        if (Eden::UnmappedLog::Wanted(&seen)) { ++logged; last = seen; }
    assert(seen == 1000000);
    assert(logged == 64 + 13);  // 1..64, then 128 ... 524288
    assert(last == 524288);
    Eden::UnmappedLog::Reset();
    assert(Eden::UnmappedLog::Wanted(&seen) && seen == 1);
    std::puts("Unmapped log: PASS 77 lines for a million unmapped accesses, again from the start each session");
}
'''
with tempfile.TemporaryDirectory() as scratch:
    source = Path(scratch) / 'unmapped_log_check.cpp'
    binary = Path(scratch) / 'unmapped_log_check'
    source.write_text(code)
    subprocess.run([os.environ.get('CXX', 'c++'), '-std=c++20', '-Wall', '-Wextra', '-Werror',
                    f'-I{root / "headless"}', str(source), '-o', str(binary)], check=True)
    result = subprocess.run([str(binary)], capture_output=True, text=True)
    sys.stdout.write(result.stdout)
    sys.stderr.write(result.stderr)
    sys.exit(result.returncode)
