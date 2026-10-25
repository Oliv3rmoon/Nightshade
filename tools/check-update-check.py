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
repository makes the catalog's requests as before.

Then headless/update_repository.cmake, which refuses an EDEN_UPDATE_REPOSITORY when configuring
(cmake -P), with names it must take and refuse: it takes and refuses what the C code does. CC, CXX
and CMAKE name other programs; without cmake that part is skipped, saying so.
"""
from pathlib import Path
import os, shutil, subprocess, sys, tempfile

# GitHub repositories as owner/name, and whether the update check takes them.
REPOSITORIES = [
    ('Oliv3rmoon/Nightshade', True), ('Oliv3rmoon/ProsperoEden', True), ('o-1/.github', True),
    ('x/y.z_w-1', True), ('a-b-/c', True), ('x/...', True), ('a' * 39 + '/x', True), ('a' * 40 + '/x', False),
    ('x/' + 'n' * 100, True), ('x/' + 'n' * 101, False), ('-x/y', False), ('x/..', False), ('x/.', False),
    ('x', False), ('/x', False), ('x/', False), ('x/y/z', False), ('a b/c', False), ('x\\y/z', False),
    ('x/y@z', False), ('x/y;z', False), ('x/y?z', False), ('x/y#z', False), ('\u00fcber/x', False),
    ('x_y/z', False), ('x.y/z', False),
]

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

    cmake = os.environ.get('CMAKE') or shutil.which('cmake')
    if not cmake:
        print('EDEN_UPDATE_REPOSITORY in CMake: skipped, no cmake')
        sys.exit(0)
    script = str(headless / 'update_repository.cmake')
    wrong = []
    for name, valid in REPOSITORIES + [('', None)]:
        taken = subprocess.run([cmake, '-DEDEN_UPDATE_REPOSITORY=' + name, '-P', script], capture_output=True,
                               text=True).returncode == 0
        if valid is None:  # empty: the catalog, which CMake takes and the C code is never asked
            if not taken:
                wrong.append('CMake refuses the empty name')
            continue
        c_takes = subprocess.run([str(exe), '--repository', name], capture_output=True, text=True,
                                 check=True).stdout.strip() == '1'
        if (taken, c_takes) != (valid, valid):
            wrong.append(f'{name!r}: expected {valid}, CMake {taken}, C {c_takes}')
    if wrong:
        sys.exit('EDEN_UPDATE_REPOSITORY in CMake and C differ:\n  ' + '\n  '.join(wrong))
    print(f'EDEN_UPDATE_REPOSITORY in CMake: takes and refuses what the update check does '
          f'({len(REPOSITORIES)} names and the empty one) PASS')
