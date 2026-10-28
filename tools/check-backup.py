#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2026 Oliv3rmoon
# SPDX-License-Identifier: GPL-3.0-or-later
"""Host check of Back up to USB (headless/backup_check.cpp, headless/backup.h).

Builds the check under the address and undefined-behaviour sanitizers, warnings as errors, with the
SHA-256 the app has (update_check/self_update_sha256.c), and runs it with a data folder and USB
drives made in the temporary folder: what is left out, the timestamped folder, backup.json and
SHA256SUMS, cancels and failures that leave nothing (or a folder still named .incomplete), a drive
without room refused before anything is written, a drive that fills up, and the worker's thread.
Then the finished backup it leaves is checked again here, with Python's own JSON and SHA-256: every
line of SHA256SUMS against its file, and backup.json's count and bytes against the folder.

usage: tools/check-backup.py
CC and CXX name other compilers.
"""
from pathlib import Path
import hashlib, json, os, subprocess, sys, tempfile

root = Path(__file__).resolve().parents[1]
headless = root / 'headless'
flags = ['-O1', '-g', '-fsanitize=address,undefined', '-fno-sanitize-recover=all', '-Wall', '-Wextra', '-Werror',
         '-UNDEBUG']

with tempfile.TemporaryDirectory() as tmp:
    sha = str(Path(tmp) / 'self_update_sha256.o')
    subprocess.run([os.environ.get('CC', 'cc'), '-std=c11', *flags, '-c', str(headless / 'update_check/self_update_sha256.c'),
                    '-o', sha], check=True)
    exe = Path(tmp) / 'eden-backup-check'
    subprocess.run([os.environ.get('CXX', 'c++'), '-std=c++20', '-pthread', *flags, '-I' + str(headless),
                    '-I' + str(headless / 'prosperoeden'), str(headless / 'backup_check.cpp'), str(headless / 'backup.cpp'),
                    sha, '-o', str(exe)], check=True)
    kept = Path(tmp) / 'kept'
    kept.mkdir()
    work = Path(tmp) / 'work'
    work.mkdir()
    result = subprocess.run([str(exe), str(kept)], capture_output=True, text=True, timeout=600,
                            env={**os.environ, 'TMPDIR': str(work), 'TZ': 'UTC'})
    sys.stdout.write(result.stdout)
    if result.returncode != 0:
        sys.stderr.write(result.stderr[-3000:])
        sys.exit(f'backup check failed: exit {result.returncode}')

    # The finished backup, read as a PC would: sha256sum -c and a JSON reader.
    backups = sorted(p for p in kept.iterdir() if p.is_dir())
    problems = []
    if len(backups) != 1 or backups[0].name.endswith('.incomplete'):
        problems.append(f'one whole backup expected, found {[p.name for p in backups]}')
    for backup in backups:
        manifest = json.loads((backup / 'backup.json').read_text(encoding='utf-8'))
        listed = {}
        for line in (backup / 'SHA256SUMS').read_text(encoding='utf-8').splitlines():
            digest, name = line.split('  ', 1)
            listed[name] = digest
            data = (backup / name).read_bytes()
            if hashlib.sha256(data).hexdigest() != digest:
                problems.append(f'{name}: SHA256SUMS says {digest}, the file is {hashlib.sha256(data).hexdigest()}')
        files = sorted(p.relative_to(backup).as_posix() for p in backup.rglob('*') if p.is_file())
        files = [f for f in files if f not in ('backup.json', 'SHA256SUMS')]
        if files != sorted(listed):
            problems.append(f'SHA256SUMS lists {sorted(listed)}, the folder has {files}')
        size = sum((backup / f).stat().st_size for f in files)
        for key, value in (('files', len(files)), ('bytes', size), ('version', '01.010.000'),
                           ('created', '2026-10-28T15:30:00'), ('app', 'Nightshade'), ('sums', 'SHA256SUMS')):
            if manifest.get(key) != value:
                problems.append(f'backup.json {key}: {manifest.get(key)!r}, expected {value!r}')
        if 'user/shader' not in manifest.get('excluded', []) or not manifest.get('restore'):
            problems.append('backup.json lacks what was left out or how to restore it')
        print(f"PASS backup read back: {backup.name}, {len(files)} files, {size} bytes, every SHA256SUMS line matches"
              if not problems else '')
    if problems:
        sys.exit('backup check failed:\n  ' + '\n  '.join(problems))
