#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Reject unresolved weak imports before the native title converter sees them."""
from pathlib import Path
import shutil
import subprocess
import sys

root = Path(__file__).resolve().parents[1]
if len(sys.argv) == 2:
    binary = Path(sys.argv[1])
else:
    assert len(sys.argv) == 1
    scratch = Path((root / '.local/headless-cache').read_text().strip())
    binary = scratch / 'native-local/bin/eden-headless'
nm = shutil.which('llvm-nm-18') or shutil.which('llvm-nm')
assert nm, 'LLVM nm is required'
result = subprocess.run([nm, '--dynamic', '--undefined-only', '--format=posix', str(binary)],
                        check=True, capture_output=True, text=True)
weak = []
strong = 0
for line in result.stdout.splitlines():
    fields = line.split()
    if len(fields) < 2:
        continue
    if fields[1] in ('w', 'v'):
        weak.append(fields[0])
    elif fields[1] == 'U':
        strong += 1
if weak:
    sys.exit(f'Native weak imports: {len(weak)} unresolved optional symbols remain dynamic; '
             f'the title converter cannot import them. First entries: {", ".join(weak[:6])}')
print(f'Native weak imports PASS: optional symbols resolve locally; '
      f'{strong} required imports remain for SDK module validation')
