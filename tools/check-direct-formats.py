#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2026 Oliv3rmoon
# SPDX-License-Identifier: GPL-3.0-or-later
"""Start homebrew and the advanced files in the emulator, and read them as the Library does.

The core fixture (build/fixture/core-homebrew.nro: the guest checks of fixtures/core-services.c) is
made into each format the Library offers beside games (headless/game_files.h), and eden-headless
starts each with Eden's own loaders and the build's patches to them (headless/CMakeLists.txt):
  - the NRO with assets (ASET: a JPEG icon and a NACP);
  - a lone NSO, its segments raw, LZ4 and ZBIC (the lone-NSO layout patch; ZBIC decoded by Eden);
  - an unpacked ExeFS chosen by its main (main a ZBIC NSO, beside its main.npdm: the IdentifyFile
    patch);
  - a KIP.
Each must run the guest's checks to their end (EDEN_CORE_FIXTURE_PASS) and close cleanly. Then
eden-formats-check (headless/formats_check.cpp) reads the same files as the Library does: Eden's
identification, the kinds listed, title IDs, names, icons and languages. An NCA or a NAX needs the
console's keys: those are checked as far as no key is needed (what is refused without keys; a NAX
named by its content ID reaching its key derivation).
"""
import base64
import hashlib
import json
import os
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import nso_files  # noqa: E402

# A 16x16 baseline JPEG: the icon of the NRO's assets and of the ExeFS folder.
ICON = base64.b64decode('''
    /9j/4AAQSkZJRgABAQAASABIAAD/4QBMRXhpZgAATU0AKgAAAAgAAYdpAAQAAAABAAAAGgAAAAAAA6ABAAMAAAABAAEAAKAC
    AAQAAAABAAAAEKADAAQAAAABAAAAEAAAAAD/7QA4UGhvdG9zaG9wIDMuMAA4QklNBAQAAAAAAAA4QklNBCUAAAAAABDUHYzZ
    jwCyBOmACZjs+EJ+/8AAEQgAEAAQAwEiAAIRAQMRAf/EAB8AAAEFAQEBAQEBAAAAAAAAAAABAgMEBQYHCAkKC//EALUQAAIB
    AwMCBAMFBQQEAAABfQECAwAEEQUSITFBBhNRYQcicRQygZGhCCNCscEVUtHwJDNicoIJChYXGBkaJSYnKCkqNDU2Nzg5OkNE
    RUZHSElKU1RVVldYWVpjZGVmZ2hpanN0dXZ3eHl6g4SFhoeIiYqSk5SVlpeYmZqio6Slpqeoqaqys7S1tre4ubrCw8TFxsfI
    ycrS09TV1tfY2drh4uPk5ebn6Onq8fLz9PX29/j5+v/EAB8BAAMBAQEBAQEBAQEAAAAAAAABAgMEBQYHCAkKC//EALURAAIB
    AgQEAwQHBQQEAAECdwABAgMRBAUhMQYSQVEHYXETIjKBCBRCkaGxwQkjM1LwFWJy0QoWJDThJfEXGBkaJicoKSo1Njc4OTpD
    REVGR0hJSlNUVVZXWFlaY2RlZmdoaWpzdHV2d3h5eoKDhIWGh4iJipKTlJWWl5iZmqKjpKWmp6ipqrKztLW2t7i5usLDxMXG
    x8jJytLT1NXW19jZ2uLj5OXm5+jp6vLz9PX29/j5+v/bAEMABAQEBAQEBgQEBgkGBgYJDAkJCQkMDwwMDAwMDxIPDw8PDw8S
    EhISEhISEhUVFRUVFRkZGRkZHBwcHBwcHBwcHP/bAEMBBAUFBwcHDAcHDB0UEBQdHR0dHR0dHR0dHR0dHR0dHR0dHR0dHR0d
    HR0dHR0dHR0dHR0dHR0dHR0dHR0dHR0dHf/dAAQAAf/aAAwDAQACEQMRAD8A+V/+RK/6fPtn/bPb5f8A31nO726Uf8iV/wBP
    n2z/ALZ7fL/76znd7dKP+RK/6fPtn/bPb5f/AH1nO726Uf8AIlf9Pn2z/tnt8v8A76znd7dK834vO/y5rflb8T9Y/gf9OvZf
    9vew5v8A077X58lz/9k=
''')

root = Path(__file__).resolve().parents[1]
cache = Path((root / '.local/headless-cache').read_text().strip())
eden = cache / 'source'
binary = cache / 'build/bin/eden-headless'
reader = cache / 'build/bin/eden-formats-check'
fixture = (root / 'build/fixture/core-homebrew.nro').read_bytes()

results = root / 'results'
results.mkdir(exist_ok=True)
run = Path(tempfile.mkdtemp(prefix='direct-formats-', dir=results))
print(f'Direct formats evidence: {run}', flush=True)
codecs = nso_files.build_codecs(eden, run / 'codecs')
files = run / 'files'
(files / 'keys').mkdir(parents=True)
(files / 'Fixture').mkdir()
(files / 'Lone').mkdir()

segments, bss = nso_files.nro_segments(fixture)
raw = nso_files.make_nso(segments, bss)
zbic = nso_files.make_nso(segments, bss, 'zbic', codecs, run)
flags, entries, _ = nso_files.read_nso(zbic)
assert flags & nso_files.NSO_ZBIC and all(zbic[o:o + 4] == nso_files.ZBIC_MAGIC for o, _, _, _, _ in entries)
made = {
    'homebrew.nro': fixture + nso_files.make_aset(ICON, nso_files.make_nacp(['Nightshade Fixture'] * 2,
                                                                             0x0100F00DCAFE0000, 0x3)),
    'plain.nro': fixture,
    'homebrew.nso': raw,
    'homebrew-lz4.nso': nso_files.make_nso(segments, bss, 'lz4', codecs, run),
    'homebrew-zbic.nso': zbic,
    'not-homebrew.nro': zbic,
    'Fixture/main': zbic,
    'Fixture/main.npdm': nso_files.make_npdm(0x0100F00DCAFE2000),
    'Fixture/control.nacp': nso_files.make_nacp(['Fixture ExeFS'], 0x0100F00DCAFE2000, 0x1),
    'Fixture/icon_AmericanEnglish.dat': ICON,
    'Lone/main': raw,
    'homebrew.kip': nso_files.make_kip(segments, bss, 0x0100F00DCAFE1000),
    'broken.kip': b'KIP1' + bytes(0x7C),
    'garbage.nca': hashlib.sha256(b'not an NCA').digest() * 0x200,
}
# A NAX header ("NAX0" after its HMAC; 0x200 bytes of contents after 0x4000 of header), under the
# name an SD card gives it (its content ID) and under another.
nax = bytearray(0x4200)
nax[0x20:0x24] = b'NAX0'
nax[0x48:0x50] = (0x200).to_bytes(8, 'little')
made['0123456789abcdef0123456789abcdef.nax'] = bytes(nax)
made['renamed.nax'] = bytes(nax)
for name, data in made.items():
    (files / name).write_bytes(data)

boots = ['homebrew.nro', 'homebrew.nso', 'homebrew-lz4.nso', 'homebrew-zbic.nso', 'Fixture/main', 'homebrew.kip']
failures = []
for name in boots:
    work = run / name.replace('/', '-')
    (work / 'user').mkdir(parents=True)
    timed_out = False
    try:
        result = subprocess.run([str(binary), str(files / name)], cwd=work, capture_output=True, text=True,
                                timeout=60)
        stdout, stderr, status = result.stdout, result.stderr, result.returncode
    except subprocess.TimeoutExpired as error:
        timed_out = True
        stdout = (error.stdout or b'').decode(errors='replace')
        stderr = (error.stderr or b'').decode(errors='replace')
        status = None
    (work / 'stdout.txt').write_text(stdout)
    (work / 'stderr.txt').write_text(stderr)
    console = stdout + stderr
    logs = '\n'.join(file.read_text(errors='replace') for file in (work / 'user/log').glob('*.txt')) or console
    valid = (not timed_out and status == 0 and 'EDEN_CORE_FIXTURE_PASS' in logs and
             'EDEN_CORE_FIXTURE_FAIL' not in logs and 'HEADLESS_COMPLETE\tPASS' in console)
    (work / 'result.json').write_text(json.dumps({'file': name, 'passed': bool(valid), 'exit_code': status,
                                                  'timed_out': timed_out}, indent=2) + '\n')
    print(f'{name}: {"PASS" if valid else "FAIL"}', flush=True)
    if not valid:
        print(logs[-12000:])
        print(console[-4000:])
        failures.append(name)
if failures:
    raise SystemExit('Not started to the end: ' + ', '.join(failures))

subprocess.run([str(reader), str(files)], check=True, timeout=120)
print('Direct formats PASS: the fixture ran to its end as an NRO with assets, a lone NSO (raw, LZ4, ZBIC), '
      'an unpacked ExeFS (ZBIC main) and a KIP; the Library reads them')
