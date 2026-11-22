#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""How the graphics pipelines a session built while it played relate to the ones before them.

Reads stderr.log files (one per session): its EDEN_PIPELINE lines (headless/pipeline_identity.h),
its EDEN_SHADER_START line (the end of the shader cache's build), its EDEN_PROGRESS lines (one
every 30 s) and its EDEN_ASYNC_SHADERS lines (headless/async_shaders.h). A pipeline printed after
EDEN_SHADER_START was new to the game's shader cache. Each is put in the first of these that fits:

  spirv seen      every stage's SPIR-V hash (spv=) was printed before: only state outside the
                  shaders is new, so with graphics pipeline libraries it would need a link and
                  no shader compile (logs from before spv= existed count none here)
  pair seen       its vertex and fragment programs were used together before, with other state
  stages seen     each program was used before, not together
  one new         one of its programs is new
  both new        all of them are new

    python3 tools/analyze-pipeline-reuse.py stderr.log [more.log ...]
"""
import collections
import sys

CATEGORIES = ('spirv seen', 'pair seen', 'stages seen', 'one new', 'both new')


def fields(line):
    return dict(part.split('=', 1) for part in line.split()[1:] if '=' in part)


def analyze(lines):
    seen_spirv, seen_pair, seen_program = set(), set(), set()
    cache = runtime = compute = 0
    started = False
    window = 0
    per_window = collections.Counter()
    kinds = collections.Counter()
    async_line = None
    for line in lines:
        if line.startswith('EDEN_SHADER_START '):
            started = True
        elif line.startswith('EDEN_PROGRESS '):
            window += 1
        elif line.startswith('EDEN_ASYNC_SHADERS '):
            async_line = line.strip()
        elif line.startswith('EDEN_PIPELINE ') and ' compute ' in line:
            compute += 1
        elif line.startswith('EDEN_PIPELINE ') and ' graphics ' in line:
            f = fields(line)
            programs = tuple(f.get(stage, '0') for stage in ('va', 'vb', 'tc', 'te', 'gs', 'fs'))
            used = [p for p in programs if p != '0']
            spirv = [s for s in f.get('spv', '').split(',') if s]
            if started:
                runtime += 1
                per_window[window] += 1
                if spirv and all(s in seen_spirv for s in spirv):
                    kinds['spirv seen'] += 1
                elif programs in seen_pair:
                    kinds['pair seen'] += 1
                elif all(p in seen_program for p in used):
                    kinds['stages seen'] += 1
                elif any(p in seen_program for p in used):
                    kinds['one new'] += 1
                else:
                    kinds['both new'] += 1
            else:
                cache += 1
            seen_spirv.update(spirv)
            seen_pair.add(programs)
            seen_program.update(used)
    return {
        'shader cache built': started,
        'graphics pipelines from the cache': cache,
        'graphics pipelines built while playing': runtime,
        'compute pipelines': compute,
        'kinds': {kind: kinds[kind] for kind in CATEGORIES},
        'most in one 30 s window': max(per_window.values(), default=0),
        'last EDEN_ASYNC_SHADERS': async_line,
    }


def main(paths):
    for path in paths:
        with open(path, 'rb') as file:
            result = analyze(file.read().decode('utf-8', 'replace').splitlines())
        print(f'== {path}')
        if not result['shader cache built']:
            print('  no EDEN_SHADER_START line: every pipeline counts as the cache\'s')
        for key in ('graphics pipelines from the cache', 'graphics pipelines built while playing',
                    'compute pipelines', 'most in one 30 s window'):
            print(f'  {key}: {result[key]}')
        total = result['graphics pipelines built while playing']
        for kind, count in result['kinds'].items():
            share = f' ({100.0 * count / total:.0f}%)' if total else ''
            print(f'  {kind}: {count}{share}')
        print(f'  last EDEN_ASYNC_SHADERS: {result["last EDEN_ASYNC_SHADERS"] or "none"}')


if __name__ == '__main__':
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    main(sys.argv[1:])
