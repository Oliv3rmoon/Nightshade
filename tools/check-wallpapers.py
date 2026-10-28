#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2026 Oliv3rmoon
# SPDX-License-Identifier: GPL-3.0-or-later
"""Host check of the wallpaper folders (headless/wallpapers_check.cpp, headless/wallpapers.h).

Builds the check under the address and undefined-behaviour sanitizers, warnings as errors, and runs
it in the temporary folder: the game files folder's wallpapers/ (the folder copied over USB) and
/data/prosperoeden/wallpapers, an image in either or both, a folder that isn't there.

usage: tools/check-wallpapers.py
CXX names another compiler.
"""
from pathlib import Path
import os, subprocess, sys, tempfile

root = Path(__file__).resolve().parents[1]
headless = root / 'headless'
flags = ['-O1', '-g', '-fsanitize=address,undefined', '-fno-sanitize-recover=all', '-Wall', '-Wextra', '-Werror',
         '-UNDEBUG']

with tempfile.TemporaryDirectory() as tmp:
    exe = Path(tmp) / 'eden-wallpapers-check'
    subprocess.run([os.environ.get('CXX', 'c++'), '-std=c++20', *flags, '-I' + str(headless),
                    str(headless / 'wallpapers_check.cpp'), '-o', str(exe)], check=True)
    result = subprocess.run([str(exe)], capture_output=True, text=True, timeout=120,
                            env={**os.environ, 'TMPDIR': tmp})
    sys.stdout.write(result.stdout)
    if result.returncode != 0:
        sys.stderr.write(result.stderr[-3000:])
        sys.exit(f'wallpapers check failed: exit {result.returncode}')
