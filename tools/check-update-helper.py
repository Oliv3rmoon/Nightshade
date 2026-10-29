#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2026 Oliv3rmoon
# SPDX-License-Identifier: GPL-3.0-or-later
"""Check the self-update helper on the host (headless/self_update_helper).

Builds tools/launcher/test-launcher-update-compat.cpp with the helper's updater, files and archive
code, the kit's JSON reader and miniz, and runs it: the launcher compatibility checks, the bounded
JSON readers, and the wait for the app to close before its files are replaced (wait_for_close: a
sandbox the system leaves behind once the app's connection has ended, Swordpdf/ProsperoEden#232).
Also checks that the helper writes its log to /data/nightshade/logs/updater.log.

Then headless/update_helper_check.cpp, the helper's whole conversation in a console made in the
temporary folder: the app's own update, and another app installed or updated (Nightshade's Apps).
"""
from pathlib import Path
import os, subprocess, sys, tempfile

root = Path(__file__).resolve().parents[1]
helper = root / 'headless/self_update_helper'
assert '"/data/nightshade/logs/updater.log"' in (helper / 'main.cpp').read_text()
assert 'wait_for_close(' in (helper / 'updater.cpp').read_text()

cc = os.environ.get('CC', 'cc')
cxx = os.environ.get('CXX', 'c++')
with tempfile.TemporaryDirectory() as scratch:
    scratch = Path(scratch)
    objects = []
    for name in ('update_check', 'self_update_sha256'):
        out = scratch / f'{name}.o'
        subprocess.run([cc, '-std=c11', '-O1', '-DUPDATE_CHECK_NO_NETWORK', f'-I{root / "headless/update_check"}',
                        '-c', str(root / f'headless/update_check/{name}.c'), '-o', str(out)], check=True)
        objects.append(str(out))
    for name in ('miniz', 'miniz_tdef', 'miniz_tinfl', 'miniz_zip'):
        out = scratch / f'{name}.o'
        subprocess.run([cc, '-O1', '-w', f'-I{root / "third_party/miniz"}', '-c',
                        str(root / f'third_party/miniz/{name}.c'), '-o', str(out)], check=True)
        objects.append(str(out))
    binary = scratch / 'update-helper-check'
    subprocess.run([cxx, '-std=c++20', '-O1', '-Wall', '-Wextra', f'-I{helper}', f'-I{root / "headless/update_check"}',
                    f'-I{root / "third_party"}', str(root / 'tools/launcher/test-launcher-update-compat.cpp'),
                    str(helper / 'updater.cpp'), str(helper / 'files.cpp'), str(helper / 'archive.cpp'),
                    *objects, '-pthread', '-o', str(binary)], check=True)
    cases = scratch / 'cases'
    cases.mkdir()
    result = subprocess.run([str(binary), str(cases)], capture_output=True, text=True)
    sys.stdout.write(result.stdout)
    sys.stderr.write(result.stderr)
    if result.returncode != 0:
        sys.exit(result.returncode)
    conversation = scratch / 'update-helper-conversation'
    subprocess.run([cxx, '-std=c++20', '-O1', '-Wall', '-Wextra', '-Werror', f'-I{root / "headless"}', f'-I{helper}',
                    f'-I{root / "headless/update_check"}', f'-I{root / "third_party"}',
                    str(root / 'headless/update_helper_check.cpp'), str(helper / 'updater.cpp'),
                    str(helper / 'files.cpp'), str(helper / 'archive.cpp'), *objects, '-pthread', '-o',
                    str(conversation)], check=True)
    result = subprocess.run([str(conversation)], capture_output=True, text=True, timeout=300)
    sys.stdout.write(result.stdout)
    sys.stderr.write(result.stderr)
    sys.exit(result.returncode)
