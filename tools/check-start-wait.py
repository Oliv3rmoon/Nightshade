#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2026 Oliv3rmoon
# SPDX-License-Identifier: GPL-3.0-or-later
"""Check the report of a start that shows no picture for long (headless/start_wait.h).

Compiles the header with recorded service calls and walks a start through its last step: nothing
before 45 s, one line and the calls at 45 s, one line at 90 s, a line and the calls at 180 s, each
once; nothing for other steps; a new start reports again. And that the loading screen's tick calls
it, in both renderers (headless/graphics.cpp).
"""
from pathlib import Path
import os, subprocess, sys, tempfile

root = Path(__file__).resolve().parents[1]
graphics = (root / 'headless/graphics.cpp').read_text()
assert '#include "start_wait.h"' in graphics
assert graphics.count('WatchStart(idle);') == 2, 'the Vulkan and the OpenGL loading ticks both watch the start'

code = r'''
#include "start_wait.h"
#include <cassert>
#include <cstdio>
#include <string>
#include <vector>
using namespace Eden;
int main() {
    std::vector<std::string> lines;
    auto line = [&](const char* text) { lines.push_back(text); };
    StartWait::Watch watch;
    // No calls yet: the line says so.
    watch.Check(true, 1000, 1000 + 45000, true, line);
    assert(lines.size() == 1 && lines[0].find("45 s") != std::string::npos);
    assert(lines[0].find("gpu=idle, no service calls") != std::string::npos);
    lines.clear();
    std::uint32_t reply[8] = {0, 0, 0x4F434653, 0, 0, 0, 0, 0};
    for (int i = 0; i < 70; ++i) IpcRing::Record("fsp-srv", 7, 18, 5, reply, 8);
    // Another start: before 45 s nothing, at its last step only.
    watch.Check(true, 50000, 50000 + 44999, false, line);
    watch.Check(false, 50000, 50000 + 200000, false, line);
    assert(lines.empty());
    watch.Check(true, 50000, 50000 + 45000, false, line);
    assert(lines.size() == 1 + IpcRing::kEntries);
    assert(lines[0].find("EDEN_START_WAIT 45 s") == 0 && lines[0].find("gpu=busy, 70 service calls") != std::string::npos);
    assert(lines[1].find("EDEN_IPC_RING #6 other") == 0 && lines[1].find("service=fsp-srv command=18") != std::string::npos);
    watch.Check(true, 50000, 50000 + 60000, false, line);
    assert(lines.size() == 1 + IpcRing::kEntries);
    lines.clear();
    watch.Check(true, 50000, 50000 + 90000, true, line);
    assert(lines.size() == 1 && lines[0].find("EDEN_START_WAIT 90 s") == 0);
    lines.clear();
    watch.Check(true, 50000, 50000 + 180000, true, line);
    watch.Check(true, 50000, 50000 + 900000, true, line);
    assert(lines.size() == 1 + IpcRing::kEntries && lines[0].find("EDEN_START_WAIT 180 s") == 0);
    lines.clear();
    // A late first check prints each step it passed, once, in order.
    StartWait::Watch late;
    late.Check(true, 0, 200000, true, line);
    assert(lines.size() == 3 + 2 * IpcRing::kEntries);
    assert(lines[0].find(" 45 s") != std::string::npos && lines[1 + IpcRing::kEntries].find(" 90 s") != std::string::npos);
    std::puts("Start wait: PASS nothing before 45 s, then 45, 90 and 180 s once each with the last calls, a new start again");
}
'''

with tempfile.TemporaryDirectory() as scratch:
    source = Path(scratch) / 'start_wait_check.cpp'
    binary = Path(scratch) / 'start_wait_check'
    source.write_text(code)
    compiler = os.environ.get('CXX', 'c++')
    subprocess.run([compiler, '-std=c++20', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
                    f'-I{root / "headless"}', str(source), '-o', str(binary)], check=True)
    result = subprocess.run([str(binary)], capture_output=True, text=True)
    sys.stdout.write(result.stdout)
    sys.stderr.write(result.stderr)
    sys.exit(result.returncode)
