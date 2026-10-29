#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Release files from a built PPSA64448 folder and its bridge (make release).

  tools/ci/make-dist.py APP_DIR [BRIDGE_DIR]

dist/Nightshade-vX.Y.Z.zip (the folder as PPSA64448/, the bridge as PPSA99008/ when BRIDGE_DIR is
given, plus README, LICENSE and THIRD_PARTY_NOTICES; fixed timestamps, so equal inputs give equal
bytes), SHA256SUMS (of the ZIP) and release-notes.md (the changelog's "vX.Y.Z" section,
docs/CHANGELOG.md).

The bridge (headless/bridge) is what an install made before Nightshade had its own title ID
updates itself to: every release up to 1.010 is PPSA99008, and its update helper takes the folder
of the ZIP named after its own title. The bridge installs PPSA64448 from the same release and
starts it. It carries the same version as the app, which is above every PPSA99008 release, so
those installs take it as an update.
"""
import hashlib
import json
import os
import pathlib
import re
import shutil
import sys
import zipfile
from urllib.parse import urljoin

root = pathlib.Path(__file__).resolve().parents[2]
args = sys.argv[1:]
if len(args) not in (1, 2):
    sys.exit(__doc__.strip())
app = pathlib.Path(args[0]).resolve()
bridge = pathlib.Path(args[1]).resolve() if len(args) == 2 else None
param = json.loads((app / 'sce_sys/param.json').read_text())
if param['titleId'] != 'PPSA64448':
    sys.exit(f'{app}: titleId {param["titleId"]}, expected PPSA64448')
version = param['contentVersion']
if bridge is not None:
    bridge_param = json.loads((bridge / 'sce_sys/param.json').read_text())
    if bridge_param['titleId'] != 'PPSA99008':
        sys.exit(f'{bridge}: titleId {bridge_param["titleId"]}, expected PPSA99008')
    # The old installs' helper takes only the version their app was offered: the release's.
    if bridge_param['contentVersion'] != version:
        sys.exit(f'{bridge}: version {bridge_param["contentVersion"]}, the app is {version}')
tag = 'v' + version.removeprefix('0')
dist = root / 'dist'
shutil.rmtree(dist, ignore_errors=True)
dist.mkdir()
archive = dist / f'Nightshade-{tag}.zip'
files = [(p, 'PPSA64448/' + p.relative_to(app).as_posix()) for p in sorted(app.rglob('*')) if p.is_file()]
if bridge is not None:
    files += [(p, 'PPSA99008/' + p.relative_to(bridge).as_posix()) for p in sorted(bridge.rglob('*')) if p.is_file()]
files += [(root / name, name) for name in ('README.md', 'LICENSE', 'THIRD_PARTY_NOTICES.md') if (root / name).exists()]
with zipfile.ZipFile(archive, 'w', zipfile.ZIP_DEFLATED, compresslevel=9) as zip_file:
    for path, name in files:
        info = zipfile.ZipInfo(name, (2026, 1, 1, 0, 0, 0))
        info.compress_type = zipfile.ZIP_DEFLATED
        # Every entry stored as 0777: the console only starts an app whose files are open to all
        # ("Can't start the game or app", CE-107750-0, otherwise), and a tool that unpacks the
        # ZIP keeping its permissions would leave 0644 files.
        info.create_system = 3  # Unix: the high half of external_attr is the mode
        info.external_attr = 0o100777 << 16
        zip_file.writestr(info, path.read_bytes())
with zipfile.ZipFile(archive) as check:
    closed = [i.filename for i in check.infolist() if (i.external_attr >> 16) & 0o777 != 0o777]
    if closed:
        sys.exit(f'ZIP entries not stored as 0777: {closed[:3]}')
digest = hashlib.sha256(archive.read_bytes()).hexdigest()
(dist / 'SHA256SUMS').write_text(f'{digest}  {archive.name}\n')
# The unstripped executable of this release, to name the functions in a crash report later
# (tools/symbolize-crash.py). Kept out of dist/: it is not a release file.
unstripped = root / 'build/headless-native/llvm-pie.elf'
if unstripped.exists():
    symbols = root / 'build/symbols' / f'Nightshade-{tag}.elf'
    symbols.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(unstripped, symbols)
    print(f'{symbols} (keep it with the release: crash reports are read with it)')
# The release notes: this version's section of the changelog. A link that is relative there (to
# another guide, to another version) would lead nowhere on the release's page, so it is pointed
# at the file as the tag has it.
changelog = (root / 'docs/CHANGELOG.md').read_text()
match = re.search(rf'^## {re.escape(tag)}\n(.*?)(?=^## |\Z)', changelog, re.M | re.S)
if match:
    repository = os.environ.get('GITHUB_REPOSITORY', 'blackbearreloaded/ProsperoEden')
    here = f'https://github.com/{repository}/blob/{tag}/docs/CHANGELOG.md'
    notes = re.sub(r'\]\((?!https?://|mailto:)([^)\s]+)\)', lambda link: f']({urljoin(here, link.group(1))})',
                   match.group(1).strip())
    (dist / 'release-notes.md').write_text(notes + '\n')
else:
    print(f'docs/CHANGELOG.md has no "## {tag}" section; no release-notes.md', file=sys.stderr)
print(f'{archive} {digest}')
