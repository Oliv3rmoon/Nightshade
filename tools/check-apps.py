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

Then the launch payload itself (headless/app_launch/main.c) runs on the host, its console calls
stood in for: what it opens, with which launch arguments (PSL2: the bridge passing a forwarder's on,
headless/bridge), and what it refuses.
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
                    str(headless / 'bridge/bridge.cpp'),
                    *[str(headless / 'self_update_helper' / name) for name in helper], *objects, '-o', str(exe)],
                   check=True)
    result = subprocess.run([str(exe), str(root / 'fixtures/apps'), *sys.argv[1:2]], capture_output=True, text=True,
                            timeout=600)
    sys.stdout.write(result.stdout)
    if result.returncode != 0:
        sys.stderr.write(result.stderr[-3000:])
        sys.exit(f'apps check failed: exit {result.returncode}')


# ---- the launch payload, on the host ----------------------------------------------------------------
STUBS = r"""
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef struct { uint32_t size; int user_id; uint32_t app_opt; uint64_t crash_report; uint64_t check_flag; } param_t;
static int looks;
uint32_t sceLncUtilGetAppIdOfRunningBigApp(void) { return 7; }
int sceLncUtilGetAppTitleId(uint32_t app, char *title) { (void)app; strcpy(title, looks++ < 2 ? ASKING : "NPXS40002"); return 0; }
int sceUserServiceInitialize(void *param) { (void)param; return 0; }
int sceUserServiceGetForegroundUser(int *user) { *user = 0x10000000; return 0; }
int sceSystemServiceLaunchApp(const char *title, const char *argv[], param_t *param) { (void)title; (void)argv; (void)param; return -1; }
int sceLncUtilLaunchApp(const char *title, const char *argv[], param_t *param) {
    FILE *file = fopen(LAUNCHED, "w");
    fprintf(file, "%s user=%d\n", title, param->user_id);
    for (int i = 0; argv[i] != NULL; i++) fprintf(file, "[%s]\n", argv[i]);
    fclose(file);
    return 0;
}
"""
with tempfile.TemporaryDirectory() as tmp:
    tmp = Path(tmp)
    (tmp / 'user/app/PPSA64448').mkdir(parents=True)
    (tmp / 'stubs.c').write_text(STUBS)
    payload = tmp / 'app-launch'
    defines = [f'-DLAUNCH_LOG="{tmp / "app-launch.log"}"', f'-DLAUNCH_USER="{tmp / "user"}"',
               f'-DLAUNCHED="{tmp / "launched.txt"}"', '-DASKING="PPSA99008"']
    subprocess.run([os.environ.get('CC', 'cc'), '-std=c11', '-O1', '-Wall', '-Wextra', '-Werror', *defines,
                    str(headless / 'app_launch/main.c'), str(tmp / 'stubs.c'), '-o', str(payload)], check=True)
    launched = tmp / 'launched.txt'
    def run(request):
        launched.unlink(missing_ok=True)
        done = subprocess.run([str(payload)], input=request.encode(), capture_output=True, timeout=60)
        return done.stdout.decode(), launched.read_text() if launched.exists() else None
    checks = [
        ('PSL1\nPPSA64448\nPPSA99008\n', ('waiting\n', 'PPSA64448 user=268435456\n'), 'PSL1 opens the app with no arguments'),
        ('PSL2\nPPSA64448\nPPSA99008\n4\n--from=PPSA99008\n--rom\n/data/prosperoeden/roms/My Game.nsp\n--exit-after-game\n',
         ('waiting\n', 'PPSA64448 user=268435456\n[--from=PPSA99008]\n[--rom]\n[/data/prosperoeden/roms/My Game.nsp]\n'
                       '[--exit-after-game]\n'), 'PSL2 opens it with the arguments passed on'),
        ('PSL2\nPPSA64448\nPPSA99008\n0\n', ('waiting\n', 'PPSA64448 user=268435456\n'), 'PSL2 with no arguments'),
        ('PSL2\nPPSA64448\nPPSA99008\n9\n', ('fail The request was refused\n', None), 'more than eight arguments are refused'),
        ('PSL2\nPPSA64448\nPPSA99008\n2\n--rom\n', ('fail The request was refused\n', None), 'arguments cut short are refused'),
        ('PSL2\nPPSA64448\nPPSA99008\nx\n', ('fail The request was refused\n', None), 'a count that is not one is refused'),
        ('PSL3\nPPSA64448\nPPSA99008\n', ('fail The request was refused\n', None), 'another conversation is refused'),
        ('PSL2\nPPSA99003\nPPSA99008\n0\n', ("fail The console doesn't list the app yet\n", None), 'an app not listed is not opened'),
    ]
    failed = 0
    for request, expected, what in checks:
        got = run(request)
        if got != expected:
            failed += 1
            print(f'FAIL launch payload: {what}: {got!r}')
    print(f'{"FAIL" if failed else "PASS"} launch payload: {len(checks)} conversations, {failed} failed')
    if failed:
        sys.exit(1)
