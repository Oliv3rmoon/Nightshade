#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2026 Oliv3rmoon
# SPDX-License-Identifier: GPL-3.0-or-later
"""Host check of Nightshade's Apps (headless/apps_check.cpp, headless/apps_release.h).

Builds the check under the address and undefined-behaviour sanitizers, warnings as errors, with
the self-update kit it installs through (update_check.c without its network side, github_release.c,
self_update.c, self_update_sha256.c), the self-update helper (self_update_helper/updater.cpp,
files.cpp, archive.cpp) and miniz, and runs it against fixtures/apps: the releases/latest answers
GitHub gave for the three original projects. A good ZIP, a wrong hash, a missing hash and a missing
release are among its cases.

usage: tools/check-apps.py [<folder with PPSA99003.zip, PPSA99002.zip and PPSA98510.zip>]
With the originals' own ZIPs (from their releases) each is installed as well. CC and CXX name other
compilers.
"""
from pathlib import Path
import os, subprocess, sys, tempfile

root = Path(__file__).resolve().parents[1]
headless = root / 'headless'
kit = ['update_check.c', 'github_release.c', 'self_update.c', 'self_update_sha256.c']
helper = ['updater.cpp', 'files.cpp', 'archive.cpp']
flags = ['-O1', '-g', '-fsanitize=address,undefined', '-fno-sanitize-recover=all', '-Wall', '-Wextra', '-Werror',
         '-DUPDATE_CHECK_NO_NETWORK=1', '-UNDEBUG']

with tempfile.TemporaryDirectory() as tmp:
    objects = []
    for name in kit:
        objects.append(str(Path(tmp) / (name + '.o')))
        subprocess.run([os.environ.get('CC', 'cc'), '-std=c11', *flags, '-I' + str(headless / 'update_check'), '-c',
                        str(headless / 'update_check' / name), '-o', objects[-1]], check=True)
    for name in ('miniz', 'miniz_tdef', 'miniz_tinfl', 'miniz_zip'):
        objects.append(str(Path(tmp) / (name + '.o')))
        subprocess.run([os.environ.get('CC', 'cc'), '-O1', '-w', '-I' + str(root / 'third_party/miniz'), '-c',
                        str(root / 'third_party/miniz' / (name + '.c')), '-o', objects[-1]], check=True)
    exe = Path(tmp) / 'eden-apps-check'
    subprocess.run([os.environ.get('CXX', 'c++'), '-std=c++20', '-pthread', *flags, '-I' + str(headless),
                    '-I' + str(headless / 'prosperoeden'), '-I' + str(headless / 'self_update_helper'),
                    '-I' + str(headless / 'update_check'), '-I' + str(root / 'third_party'),
                    str(headless / 'apps_check.cpp'), str(headless / 'apps_release.cpp'),
                    *[str(headless / 'self_update_helper' / name) for name in helper], *objects, '-o', str(exe)],
                   check=True)
    result = subprocess.run([str(exe), str(root / 'fixtures/apps'), *sys.argv[1:2]], capture_output=True, text=True,
                            timeout=600)
    sys.stdout.write(result.stdout)
    if result.returncode != 0:
        sys.stderr.write(result.stderr[-3000:])
        sys.exit(f'apps check failed: exit {result.returncode}')
