#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Checks a release ZIP as tools/ci/make-dist.py writes it (.github/workflows/release.yml).

  tools/ci/check-release-zip.py dist/Nightshade-vX.Y.Z.zip

Intact; every entry 0777 (the console only starts an app whose files are open to all); the app as
PPSA64448/ and the bridge as PPSA99008/, each with eboot.bin and its own title ID in
sce_sys/param.json, at the same version, the one the ZIP is named after; nothing else but the
README, LICENSE and THIRD_PARTY_NOTICES beside them.

Both update helpers take their own folder from this one ZIP (self_update_helper/archive.cpp): the
app folder is the one holding sce_sys/param.json closest to the top, the one named after the title
asking when two are as close. Two folders at the top named after neither title would be refused as
ambiguous, so the ZIP holds exactly these two.
"""
import json
import sys
import zipfile

if len(sys.argv) != 2:
    sys.exit(__doc__.strip())
archive = sys.argv[1]
name = archive.rsplit('/', 1)[-1]
with zipfile.ZipFile(archive) as z:
    assert z.testzip() is None, "damaged entry"
    names = z.namelist()
    wrong = [i.filename for i in z.infolist() if (i.external_attr >> 16) & 0o777 != 0o777]
    assert not wrong, f"entries not stored as 0777: {wrong[:3]}"
    tops = {entry.split('/', 1)[0] for entry in names if '/' in entry}
    assert tops == {"PPSA64448", "PPSA99008"}, f"top folders {sorted(tops)}, expected PPSA64448 and PPSA99008"
    loose = sorted(entry for entry in names if '/' not in entry)
    assert set(loose) <= {"README.md", "LICENSE", "THIRD_PARTY_NOTICES.md"}, f"unexpected files at the top: {loose}"
    versions = {}
    for title in ("PPSA64448", "PPSA99008"):
        assert f"{title}/eboot.bin" in names, f"no {title}/eboot.bin"
        param = json.loads(z.read(f"{title}/sce_sys/param.json"))
        assert param["titleId"] == title, (title, param["titleId"])
        versions[title] = param["contentVersion"]
        # Only the folders' own param.json: a deeper one would be another app inside it.
        deeper = [entry for entry in names if entry.startswith(title + "/") and entry.endswith("sce_sys/param.json")
                  and entry != f"{title}/sce_sys/param.json"]
        assert not deeper, f"another param.json in {title}/: {deeper}"
    version = versions["PPSA64448"]
    assert versions["PPSA99008"] == version, f"the bridge is {versions['PPSA99008']}, the app {version}"
    assert name == f"Nightshade-v{version.removeprefix('0')}.zip", (name, version)
    bridge = sum(1 for entry in names if entry.startswith("PPSA99008/"))
    print(f"{name}: {len(names)} entries ({bridge} of the bridge), all 0777, PPSA64448 and PPSA99008 at version {version}")
