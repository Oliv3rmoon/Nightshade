#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2026 Oliv3rmoon
# SPDX-License-Identifier: GPL-3.0-or-later
"""Check what the launcher says when Eden cannot load a game (headless/load_failure.h).

"Loader status 28" was all a player read for a missing key (Swordpdf/ProsperoEden#206, #279,
#295). This compiles the header and checks its sentences, checks that headless/main.cpp passes
it Eden's own text, the key statuses and the newest master key, and, when the pinned Eden source
is in .deps, that the loader statuses are where main.cpp expects them: 21 is the missing key
area key, and the last one is the one main.cpp bounds the table read with.
"""
from pathlib import Path
import glob, json, os, re, subprocess, tempfile

root = Path(__file__).resolve().parents[1]
main = (root / 'headless/main.cpp').read_text()
start = main.index('                if (loaded != Core::SystemResultStatus::Success) {')
branch = main[start:main.index('                }\n', main.index('throw std::runtime_error(text);', start))]
assert 'Loader::GetResultStatusString(result)' in branch
assert 'Eden::LoadFailure::Text(status, base, message, key_problem, newest_master_key)' in branch
assert 'status - base <= static_cast<unsigned>(Loader::ResultStatus::ErrorIntegrityVerificationFailed)' in branch
assert 'keys.HasKey(Core::Crypto::S128KeyType::Master, revision)' in branch
key_statuses = re.findall(r'result == ResultStatus::(\w+)', branch)
assert 'ErrorMissingKeyAreaKey' in key_statuses and len(key_statuses) == 9, key_statuses
assert '#include "load_failure.h"' in main and '#include "core/loader/loader.h"' in main
assert '#include "core/crypto/key_manager.h"' in main

# The pinned Eden: the statuses main.cpp names, and the numbers players see.
manifest = json.loads((root / 'UPSTREAM.json').read_text())
mirrors = sorted(glob.glob(str(root / f'.deps/mirror-{manifest["eden_commit"]}/src/core/loader/loader.h')))
if mirrors:
    header = Path(mirrors[0]).read_text()
    body = header[header.index('enum class ResultStatus'):]
    body = body[body.index('{') + 1:body.index('};')]
    names = [re.match(r'\s*(\w+)', line).group(1) for line in body.splitlines()
             if line.strip() and not line.strip().startswith('//')]
    assert names.index('ErrorMissingKeyAreaKey') == 21, names.index('ErrorMissingKeyAreaKey')
    assert names[-1] == 'ErrorIntegrityVerificationFailed', names[-1]
    assert all(name in names for name in key_statuses)
    source = Path(mirrors[0]).with_name('loader.cpp').read_text()
    assert f'std::array<const char*, {len(names)}> RESULT_MESSAGES' in source
    core = Path(mirrors[0]).parents[1] / 'core.h'
    statuses = core.read_text()
    statuses = statuses[statuses.index('enum class SystemResultStatus'):]
    system = re.findall(r'^\s*(\w+),', statuses[statuses.index('{') + 1:statuses.index('};')], re.M)
    assert system.index('ErrorLoader') == 7, system
    print('Loader message: Eden status 28 is loader status 21, ErrorMissingKeyAreaKey')
else:
    print('Loader message: SKIPPED the pinned Eden statuses (no .deps Eden source)')

code = r'''
#include "load_failure.h"
#include <cassert>
#include <cstdio>
#include <string>
using Eden::LoadFailure::Text;
int main() {
    const std::string area = "The key area key for this application type and crypto revision could not be found.";
    const std::string missing = Text(28, 7, area, true, 0x0f);
    assert(missing == "Loader status 28 (21): " + area +
           " The keys file (prod.keys) lacks a key this game needs. The newest master key in it is master_key_0f;"
           " a game made for a newer system version needs keys from that version. See stderr.log.");
    assert(Text(20, 7, "The production keys file was not found", true, -1) ==
           "Loader status 20 (13): The production keys file was not found. The keys file (prod.keys) lacks a key"
           " this game needs. It holds no master key. See stderr.log.");
    assert(Text(9, 7, "There is no NCA with a program.", false, 3) ==
           "Loader status 9 (2): There is no NCA with a program. Check this ROM, its keys and firmware; see stderr.log.");
    // Below ErrorLoader (no loader text): the number alone, as before.
    assert(Text(4, 7, "", false, -1) == "Loader status 4. Check this ROM, its keys and firmware; see stderr.log.");
    std::puts("ok");
}
'''
with tempfile.TemporaryDirectory() as tmp:
    exe = Path(tmp) / 'loader-message'
    subprocess.run([os.environ.get('CXX', 'c++'), '-std=c++20', '-Wall', '-Wextra', '-Werror',
                    '-I' + str(root / 'headless'), '-x', 'c++', '-', '-o', str(exe)],
                   input=code, text=True, check=True)
    subprocess.run([str(exe)], check=True, capture_output=True)
print('Loader message: PASS a key the keys file lacks is named as one, with the newest master key in it')
