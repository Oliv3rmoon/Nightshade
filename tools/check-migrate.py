#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2026 Oliv3rmoon
# SPDX-License-Identifier: GPL-3.0-or-later
"""Host check of the move from ProsperoEden's data folder (headless/migrate_check.cpp, headless/migrate.h).

Builds the check under the address and undefined-behaviour sanitizers, warnings as errors, and runs
it with folders made in the temporary folder: when the question is asked and when never again, Move,
Copy and Start fresh, what a copy leaves out and where the games stay, a drive without room (known
before, or filling up part way), a cancel, a failure at each step and what is undone, the record,
the EDEN_MIGRATE lines and the worker's thread. ProsperoEden's folder is compared after every case
with what it held before.

usage: tools/check-migrate.py
CXX names another compiler.
"""
from pathlib import Path
import os, subprocess, sys, tempfile

root = Path(__file__).resolve().parents[1]
headless = root / 'headless'
flags = ['-O1', '-g', '-fsanitize=address,undefined', '-fno-sanitize-recover=all', '-Wall', '-Wextra', '-Werror',
         '-UNDEBUG']

with tempfile.TemporaryDirectory() as tmp:
    exe = Path(tmp) / 'eden-migrate-check'
    subprocess.run([os.environ.get('CXX', 'c++'), '-std=c++20', '-pthread', *flags, '-I' + str(headless),
                    '-I' + str(headless / 'prosperoeden'), str(headless / 'migrate_check.cpp'),
                    str(headless / 'migrate.cpp'), '-o', str(exe)], check=True)
    work = Path(tmp) / 'work'
    work.mkdir()
    result = subprocess.run([str(exe)], capture_output=True, text=True, timeout=600,
                            env={**os.environ, 'TMPDIR': str(work), 'TZ': 'UTC'})
    sys.stdout.write(result.stdout)
    if result.returncode != 0:
        sys.stderr.write(result.stderr[-3000:])
        sys.exit(f'migrate check failed: exit {result.returncode}')
