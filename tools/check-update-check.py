#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2026 Oliv3rmoon
# SPDX-License-Identifier: GPL-3.0-or-later
"""Host check of the update check (headless/update_check_check.cpp, eden-update-check in the host
build of make test), without that build.

Builds the check with the kit's files it tests (update_check.c built without its network side,
github_release.c, self_update.c, self_update_sha256.c) under the address and undefined-behaviour
sanitizers, warnings as errors, and runs it: the catalog's decision with this app's version
numbers, and a build that asks a GitHub repository's releases instead (EDEN_UPDATE_REPOSITORY)
through a stand-in for the network, down to the requests it makes, and that a build naming no
repository makes the catalog's requests as before. CC and CXX name other compilers.
"""
from pathlib import Path
import os, subprocess, sys, tempfile

root = Path(__file__).resolve().parents[1]
headless = root / 'headless'
kit = ['update_check.c', 'github_release.c', 'self_update.c', 'self_update_sha256.c']
flags = ['-O1', '-g', '-fsanitize=address,undefined', '-fno-sanitize-recover=all', '-Wall', '-Wextra', '-Werror',
         '-DUPDATE_CHECK_NO_NETWORK=1', '-UNDEBUG']

with tempfile.TemporaryDirectory() as tmp:
    objects = []
    for name in kit:
        objects.append(str(Path(tmp) / (name + '.o')))
        subprocess.run([os.environ.get('CC', 'cc'), '-std=c11', *flags, '-c', str(headless / 'update_check' / name),
                        '-o', objects[-1]], check=True)
    exe = Path(tmp) / 'eden-update-check'
    subprocess.run([os.environ.get('CXX', 'c++'), '-std=c++20', '-pthread', *flags, '-I' + str(headless),
                    str(headless / 'update_check_check.cpp'), *objects, '-o', str(exe)], check=True)
    result = subprocess.run([str(exe)], capture_output=True, text=True, timeout=120)
    sys.stdout.write(result.stdout)
    if result.returncode != 0:
        sys.stderr.write(result.stderr[-3000:])
        sys.exit(f'update check failed: exit {result.returncode}')
