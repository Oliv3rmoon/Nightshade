#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2026 Oliv3rmoon
# SPDX-License-Identifier: GPL-3.0-or-later
"""Long-session simulation of the memory pool (tools/memory-soak/soak.cpp).

Usage: check-memory-soak.py [--heap HEAP_ARENAS_INC] [--keep-going]

Builds the app's heap (third_party/app_heap.c with headless/heap_arenas.inc, as
headless/CMakeLists.txt derives it) with the simulator, which models the rest of the pool the CPU
shares with the GPU after the console's logs and uses the real transfer chunker and pressure
rules (headless/staging_chunks.h, headless/memory_budget.h). Then it runs:

- a synthetic three hours of Tears of the Kingdom: 18,881 cached pipelines built while the game
  starts, open-world streaming, the item menu's texture array of 121 layers every ten minutes,
  bursts of the game's own writes;
- the EDEN_MEMORY lines of four TOTK sessions that ended (tools/memory-soak/traces: v1.010.004's
  out-of-memory crash 83 minutes in, v1.010.006's staging refusal and both of issue #2's logs).

In each, no allocation may fail and the pool's largest free block must stay above the largest
request (the menu's array, 161 MiB, which has memory of its own). The heap must start near
v1.010.004's 2.56 GB. As a control, the synthetic session runs again with v1.010.006's graphics
rules (whole transfers, the pool's free block alone for the caches, nothing given back on a
refusal) and v1.010.006's heap (tools/memory-soak/heap-v1.010.006.inc); it must fail, or the
simulation does not tell the two apart. With --heap this tree's heap source is another file
(v1.010.006's, to see the heap's own bound fail).

The simulator reads only the CSV traces in the tree; its reader bounds every line and value. It
runs under a watchdog that kills it above 4 GB of memory.
"""
from pathlib import Path
import argparse, os, shutil, subprocess, sys, tempfile, threading, time

root = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser()
parser.add_argument('--heap', type=Path, default=root / 'headless/heap_arenas.inc')
parser.add_argument('--keep-going', action='store_true', help='run every case even after a failure')
args = parser.parse_args()

def heap_source(arenas_path):
    """The app's heap as headless/CMakeLists.txt derives it, with the given heap_arenas.inc."""
    heap = (root / 'third_party/app_heap.c').read_text()
    for old, new in (
    ('128 MiB', '6144 MiB'),
    ('(128u * 1024u * 1024u)', '((size_t)6144 << 20)'),
    ('base=%p size=%u\\n', 'base=%p size=%zu\\n'),
    ('#include <sys/mman.h>', '#include <sys/mman.h>\nvoid *eden_heap_pages(size_t);\nvoid eden_heap_pages_free(void *, size_t);'),
        ('"PS5-OpenGL"', '"Eden-headless"'),
    ):
        assert old in heap, old
        heap = heap.replace(old, new)
    begin = heap.index('static int ps5_heap_ready(void) {')
    end = heap.index('void ps5_opengl_heap_stats_print(unsigned iteration) {')
    arenas = arenas_path.read_text()
    if 'eden_heap_pause' not in arenas:
        # v1.010.006's heap spins with the x86 pause alone.
        arenas = '#if !defined(__x86_64__)\n#define __builtin_ia32_pause() ((void)0)\n#endif\n' + arenas
    return heap[:begin] + arenas + heap[end:]

LIMIT_KIB = 4 * 1024 * 1024  # 4 GB


def watched(command, timeout):
    """Runs a command, killing it above LIMIT_KIB of resident memory or after `timeout` seconds."""
    process = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    killed = []

    def watch():
        started = time.time()
        while process.poll() is None:
            try:
                rss = int(subprocess.run(['ps', '-o', 'rss=', '-p', str(process.pid)], capture_output=True,
                                         text=True).stdout.strip() or 0)
            except ValueError:
                rss = 0
            if rss > LIMIT_KIB or time.time() - started > timeout:
                killed.append(rss)
                process.kill()
                return
            time.sleep(0.2)

    watcher = threading.Thread(target=watch, daemon=True)
    watcher.start()
    output, _ = process.communicate()
    watcher.join()
    if killed:
        output += f'\nkilled by the watchdog at {killed[0]} KiB\n'
    return process.returncode, output


cases = [('fixed', None)] + [('fixed', path) for path in sorted((root / 'tools/memory-soak/traces').glob('*.csv'))] + \
        [('control', None)]
with tempfile.TemporaryDirectory(prefix='eden-soak-') as tmp:
    tmp = Path(tmp)
    cc = shutil.which('clang-18') or shutil.which('clang') or 'cc'
    cxx = shutil.which('clang++-18') or shutil.which('clang++') or 'c++'
    link = []
    if sys.platform == 'darwin':
        link = ['-Wl,-U,_eden_heap_trim_level', '-Wl,-U,_eden_heap_hole_usage', '-Wl,-U,_eden_heap_arena_usage']
    # This tree's heap, and v1.010.006's for the control.
    for name, arenas_path in (('soak', args.heap), ('soak-v1.010.006', root / 'tools/memory-soak/heap-v1.010.006.inc')):
        (tmp / f'{name}.c').write_text(heap_source(arenas_path))
        subprocess.run([cc, '-std=gnu11', '-O2', '-g', '-pthread', '-Wall', '-Wno-unused-function', '-c',
                        str(tmp / f'{name}.c'), '-o', str(tmp / f'{name}.o')], check=True)
        subprocess.run([cxx, '-std=c++20', '-O2', '-g', '-pthread', '-Wall', '-Wextra', '-I' + str(root / 'headless'),
                        str(root / 'tools/memory-soak/soak.cpp'), str(tmp / f'{name}.o'), '-o', str(tmp / name), *link],
                       check=True)
    results = {}
    failed = []
    for mode, trace in cases:
        name = f'{mode} {trace.name if trace else "synthetic"}'
        binary = tmp / ('soak' if mode == 'fixed' else 'soak-v1.010.006')
        code, output = watched([str(binary), mode] + ([str(trace)] if trace else []), timeout=1200)
        lines = output.splitlines()
        summary = next((line for line in reversed(lines) if line.startswith('SUMMARY')), '')
        sys.stdout.write('\n'.join(lines[:3] + [line for line in lines if line.startswith(('FAIL', 'killed'))][:5]) + '\n')
        if os.environ.get('SOAK_VERBOSE'):
            sys.stdout.write('\n'.join(lines) + '\n')
        sys.stdout.write(summary + '\n\n')
        results[name] = (code, summary)
        expected = 0 if mode == 'fixed' else 1
        if code != expected:
            failed.append(f'{name}: exit {code}, expected {expected}')
            if mode == 'fixed':
                sys.stdout.write('\n'.join(lines[-40:]) + '\n')
            if not args.keep_going:
                break
        # The heap at the start of a TOTK session, near v1.010.004's 2560 MiB (v1.010.006: 4480).
        if mode == 'fixed' and trace is None and summary:
            fields = dict(item.split('=', 1) for item in summary.split()[1:] if '=' in item)
            start = int(fields.get('heap_start', '0MiB').removesuffix('MiB'))
            if start > 3072:
                failed.append(f'{name}: the heap held {start} MiB at the start (more than 3072 MiB)')
                if not args.keep_going:
                    break
if failed:
    sys.exit('memory soak failed:\n  ' + '\n  '.join(failed))
print('Memory soak: PASS three hours of TOTK and four TOTK sessions that ended, with no allocation refused and the '
      'largest free block above the largest request; the heap starts near v1.010.004; v1.010.006\'s graphics rules '
      'fail the same session')
