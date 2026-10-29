#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2026 Oliv3rmoon
# SPDX-License-Identifier: GPL-3.0-or-later
"""Host check of Nightshade's bridge (headless/bridge/bridge.h), the PPSA99008 app of the release ZIP.

Builds headless/bridge/preview.cpp with the bridge's screen (view.cpp, picture.cpp), the launcher's
font and catalogs, and runs it: every step the screen shows, in English and in every catalog, must
fit above the buttons' hints. With a folder, its pictures are written there as PNG files.

Then what the bridge must never do: its code names no path in ProsperoEden's data folder
(/data/prosperoeden), which only Nightshade's first start moves or copies (headless/migrate.h); and
its build (tools/build-bridge.sh) compiles nothing of the emulator.

Its install and launch are checked by tools/check-apps.py (apps_check.cpp, bridge()), what the old
installs' helpers do with the ZIP by tools/check-update-helper.py.

usage: tools/check-bridge.py [<folder for the pictures>]
"""
from pathlib import Path
import os, re, subprocess, sys, tempfile

root = Path(__file__).resolve().parents[1]
bridge = root / 'headless/bridge'
launcher = root / 'headless/prosperoeden'

for path in sorted(bridge.glob('*.[ch]pp')):
    code = re.sub(r'//[^\n]*', '', path.read_text())
    assert '/data/prosperoeden' not in code, f'{path.name} names ProsperoEden\'s data folder'
build = (root / 'tools/build-bridge.sh').read_text()
compiled = re.findall(r'compile "([^"]+)"', build) + re.findall(r'"\$headless/[^"]+\.cpp"', build)
assert not any(word in build for word in ('src/core', 'video_core', 'eden-headless', 'main.cpp"\n')), \
    'the bridge builds nothing of the emulator'
assert '"$headless/bridge/main.cpp"' in build and '"$headless/main.cpp"' not in build

cxx = os.environ.get('CXX', 'c++')
with tempfile.TemporaryDirectory() as tmp:
    binary = Path(tmp) / 'bridge-preview'
    includes = [f'-I{root / "headless"}', f'-I{launcher}', f'-I{root / "third_party"}', f'-I{root / "tools/launcher/stb"}']
    objects = []
    for source in (bridge / 'preview.cpp', bridge / 'view.cpp', bridge / 'picture.cpp', bridge / 'no_system_fonts.cpp',
                   launcher / 'pe/gfx/font.cpp', launcher / 'pe/gfx/bidi.cpp', launcher / 'pe/core/strings.cpp'):
        objects.append(str(Path(tmp) / (source.stem + '.o')))
        # The bridge's own code with warnings as errors; the launcher's, as the launcher's checks build
        # it; the preview writes its pictures with stb_image_write, whose warnings are not ours to tidy.
        warnings = ['-w'] if source.name == 'preview.cpp' else \
            ['-Wall', '-Wextra', '-Werror'] if source.parent == bridge else ['-Wall', '-Wextra']
        subprocess.run([cxx, '-std=c++20', '-O2', *warnings, *includes, '-c', str(source), '-o', objects[-1]], check=True)
    subprocess.run([cxx, *objects, '-o', str(binary)], check=True)
    result = subprocess.run([str(binary), str(launcher / 'ui'), *sys.argv[1:2]], capture_output=True, text=True,
                            timeout=600)
    sys.stdout.write(result.stdout)
    sys.stderr.write(result.stderr)
    if result.returncode != 0:
        sys.exit(f'bridge check failed: exit {result.returncode}')
print('bridge: names nothing of ProsperoEden\'s data folder, builds nothing of the emulator PASS')
