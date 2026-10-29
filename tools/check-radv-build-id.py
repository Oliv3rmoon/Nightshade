#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""The driver's pin, and the RADV build id its shader cache is keyed by.

RADV keys its shader cache on disk by its build id. The driver is Belladonna (tools/deps.json,
ps5-mesa) at its pinned commit, built with shader_cache_id as the build id, so the caches players
made with an earlier driver stay valid while Belladonna compiles shaders as before.
  1. The pin: ps5-mesa is Belladonna at a full commit in ../belladonna, with a shader_cache_id, and
     tools/ci/build-release.sh links the three RADV checkouts. With Belladonna's checkout here
     (RADV_MESA_SOURCE, or ../belladonna): it has the pinned commit, and shader_cache_id is that
     commit or one before it.
  2. The wiring, run: tools/build-radv-dependencies.sh, with stand-ins for the three checkouts and
     for meson, ninja and ccache, runs the recipe's own tools/build-radv.sh at the ps5-vulkan-tools
     pin (from RADV_RECIPE_SOURCE, or ../mihawk-vulkan-review). Meson is given
     -Dradv-build-id=<shader_cache_id> for Belladonna's commit, PROVENANCE.txt records both, an
     unchanged driver is not built again, a changed shader_cache_id configures and builds again, and
     the rewrite of build-radv.sh changes it once however often it runs. Without the recipe this
     step is skipped with a note.
  3. prepare-build.sh's decision, run with a stub builder: a cached driver built from the pinned
     commit with shader_cache_id is kept; one from another commit, with another build id, or with
     none recorded is built again.
"""
from pathlib import Path
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile

root = Path(__file__).resolve().parents[1]
deps = json.loads((root / 'tools/deps.json').read_text())
item = {i['name']: i for i in deps['items']}
mesa = item['ps5-mesa']
notes = []

# 1. The pin.
assert mesa['kind'] == 'git' and mesa['url'] == 'https://github.com/Oliv3rmoon/Belladonna.git', mesa['url']
assert mesa['path'] == '../belladonna', mesa['path']
assert re.fullmatch('[0-9a-f]{40}', mesa['commit']), mesa['commit']
assert re.fullmatch('[0-9a-f]{40}', mesa.get('shader_cache_id', '')), 'ps5-mesa has no full shader_cache_id'
assert 'Belladonna' in mesa['about'] and 'shader_cache_id' in mesa['about']
siblings = re.search(r'for sibling in ([^;]+);', (root / 'tools/ci/build-release.sh').read_text()).group(1).split()
for name in ('ps5-vulkan-tools', 'ps5-mesa', 'ps5-payload-sdk-fork'):
    assert item[name]['path'].removeprefix('../') in siblings, f'tools/ci/build-release.sh does not link {name}'
checkout = Path(os.environ.get('RADV_MESA_SOURCE') or root.parent / 'belladonna')


def git(*args, cwd=None, check=True):
    return subprocess.run(['git', *args], cwd=cwd, capture_output=True, text=True, check=check)


if git('-C', str(checkout), 'rev-parse', '--git-dir', check=False).returncode != 0:
    notes.append(f'no Belladonna checkout at {checkout}: did not look for the pinned commit in it')
else:
    assert git('-C', str(checkout), 'cat-file', '-e', mesa['commit'] + '^{commit}', check=False).returncode == 0, \
        f'{checkout} does not have the pinned commit {mesa["commit"]}'
    if git('-C', str(checkout), 'rev-parse', '--is-shallow-repository').stdout.strip() == 'true':
        notes.append(f'{checkout} is a shallow checkout: did not check where shader_cache_id is in its history')
    else:
        assert git('-C', str(checkout), 'merge-base', '--is-ancestor', mesa['shader_cache_id'], mesa['commit'],
                   check=False).returncode == 0, 'shader_cache_id is not the pinned commit or one before it'

# 2. The wiring, run.
recipe = Path(os.environ.get('RADV_RECIPE_SOURCE') or root.parent / 'mihawk-vulkan-review')
recipe_pin = item['ps5-vulkan-tools']['commit']
shown = {name: git('-C', str(recipe), 'show', f'{recipe_pin}:{name}', check=False)
         for name in ('tools/build-radv.sh', 'tooling/radv/ps5-cross.ini')}
if any(result.returncode for result in shown.values()):
    notes.append(f'no PS5_Vulkan recipe at {recipe_pin[:12]} in {recipe}: did not run the build wiring')
else:
    with tempfile.TemporaryDirectory() as folder:
        top = Path(folder)
        log = top / 'tools.log'
        stubs = top / 'bin'
        stubs.mkdir()
        stub = {
            # Configures by writing what src/amd/vulkan/meson.build gives the compiler for the build id.
            'meson': f'''import json, sys
from pathlib import Path
args = sys.argv[1:]
with open({str(log)!r}, 'a') as out:
    out.write('meson ' + ' '.join(args) + '\\n')
build = Path([a for a in args[1:] if not a.startswith('-')][0])
build.mkdir(parents=True, exist_ok=True)
(build / 'build.ninja').touch()
ids = [a.split('=', 1)[1] for a in args if a.startswith('-Dradv-build-id=')]
if ids:
    command = 'cc -DRADV_BUILD_ID_OVERRIDE="' + ids[-1] + '" -c src/amd/vulkan/radv_physical_device.c'
    (build / 'compile_commands.json').write_text(json.dumps([{{'command': command}}]))
''',
            # Builds each target it is asked for as an empty file.
            'ninja': f'''import sys
from pathlib import Path
args = sys.argv[1:]
with open({str(log)!r}, 'a') as out:
    out.write('ninja ' + ' '.join(args) + '\\n')
at = args.index('-C')
for target in args[at + 2:]:
    (Path(args[at + 1]) / target).parent.mkdir(parents=True, exist_ok=True)
    (Path(args[at + 1]) / target).write_bytes(b'')
''',
            'ccache': 'import os, sys\nos.execvp(sys.argv[1], sys.argv[1:])\n',
        }
        if not shutil.which('sha256sum'):
            stub['sha256sum'] = ('import hashlib, sys\n'
                                 'print(hashlib.sha256(open(sys.argv[1], "rb").read()).hexdigest() + "  " + sys.argv[1])\n')
        for name, body in stub.items():
            (stubs / name).write_text(f'#!{sys.executable}\n' + body)
            (stubs / name).chmod(0o755)

        quiet = ['-c', 'user.name=check', '-c', 'user.email=check@invalid', '-c', 'commit.gpgsign=false',
                 '-c', 'core.hooksPath=/dev/null']

        def repository(name, files):
            path = top / name
            for relative, text in files.items():
                (path / relative).parent.mkdir(parents=True, exist_ok=True)
                (path / relative).write_text(text)
            git('init', '-q', str(path))
            git(*quiet, 'add', '-A', cwd=path)
            git(*quiet, 'commit', '-q', '-m', name, cwd=path)
            return git('rev-parse', 'HEAD', cwd=path).stdout.strip()

        pins = {
            'ps5-mesa': repository('belladonna', {'include/vulkan/vulkan.h': '', 'meson.build': ''}),
            'ps5-payload-sdk-fork': repository('mihawk-sdk-review', {'README': ''}),
            'ps5-vulkan-tools': repository('mihawk-vulkan-review', {
                'tools/build-radv.sh': shown['tools/build-radv.sh'].stdout,
                'tooling/radv/ps5-cross.ini': shown['tooling/radv/ps5-cross.ini'].stdout,
                'tools/setup-native-dependencies.sh': 'cd "$(dirname "$0")/.."\nmkdir -p .deps/native/ps5-payload-sdk\n'
                                                       'echo stand-in > .deps/native/ps5-payload-sdk/.ps5-sdk-revision\n',
            }),
        }
        repo = top / 'nightshade'
        (repo / 'tools').mkdir(parents=True)
        shutil.copyfile(root / 'tools/build-radv-dependencies.sh', repo / 'tools/build-radv-dependencies.sh')
        cache_id = mesa['shader_cache_id']

        def build(shader_cache_id):
            # This repository's deps.json with the stand-ins' commits; shader_cache_id as given.
            sandbox = json.loads(json.dumps(deps))
            for entry in sandbox['items']:
                if entry['name'] in pins:
                    entry['commit'] = pins[entry['name']]
                if entry['name'] == 'ps5-mesa':
                    entry['shader_cache_id'] = shader_cache_id
            (repo / 'tools/deps.json').write_text(json.dumps(sandbox))
            log.write_text('')
            env = {k: v for k, v in os.environ.items()
                   if k not in ('MESON', 'NINJA', 'PS5_MESA_FORK', 'PS5_MESA_REVISION', 'RADV_BUILD_ID', 'RADV_BUILD_JOBS')}
            env['PATH'] = f'{stubs}{os.pathsep}{env["PATH"]}'
            run = subprocess.run(['bash', str(repo / 'tools/build-radv-dependencies.sh')], env=env, capture_output=True,
                                 text=True)
            if run.returncode:
                sys.stderr.write(run.stdout + run.stderr)
                raise SystemExit('tools/build-radv-dependencies.sh failed with the stand-ins')
            provenance = (top / 'mihawk-vulkan-review/.deps/native/radv-release/PROVENANCE.txt').read_text()
            return run.stdout, log.read_text().splitlines(), provenance

        rewritten = top / 'mihawk-vulkan-review/tools/build-radv.sh'
        out, calls, provenance = build(cache_id)
        configured = [c for c in calls if c.startswith('meson setup') and 'radv-build-ps5-release' in c]
        assert len(configured) == 1, calls
        assert f' -Dradv-build-id={cache_id} ' in configured[0] + ' ', configured[0]
        assert pins['ps5-mesa'] not in configured[0], 'the build id is the commit, not shader_cache_id'
        assert f'\nfork: {top / "belladonna"}\n' in provenance, provenance
        assert f'\nrevision: {pins["ps5-mesa"]}\nradv build id: {cache_id}\n' in provenance, provenance
        assert f'==> [radv] Belladonna {pins["ps5-mesa"]}, compiled with RADV_BUILD_ID_OVERRIDE={cache_id}\n' in out, out
        assert '-Dradv-build-id="$mesa_revision"' not in rewritten.read_text()
        once = rewritten.read_text()
        # Built already: kept, and the recipe is not changed again.
        out, calls, provenance = build(cache_id)
        assert not [c for c in calls if c.startswith('meson') and 'radv-build-ps5-release' in c], calls
        assert rewritten.read_text() == once, 'the rewrite of build-radv.sh changed it a second time'
        # Another shader cache id: configured with it and built again.
        other = 'c0ffee' * 6 + 'c0de'
        out, calls, provenance = build(other)
        configured = [c for c in calls if c.startswith('meson configure') and 'radv-build-ps5-release' in c]
        assert len(configured) == 1 and configured[0].endswith(f' -Dradv-build-id={other}'), calls
        assert any(c.startswith('ninja') and c.endswith('src/amd/vulkan/libvulkan_radeon.a') for c in calls), calls
        assert f'\nradv build id: {other}\n' in provenance, provenance
        assert f'compiled with RADV_BUILD_ID_OVERRIDE={other}\n' in out, out

# 3. prepare-build.sh's decision, with a cached driver and a stub builder.
prepare = (root / 'tools/prepare-build.sh').read_text()
start = prepare.index('radv_release=../mihawk-vulkan-review/.deps/native/radv-release')
end = prepare.index('\nif [[ ! -f build/radv-isolated/', start)
with tempfile.TemporaryDirectory() as folder:
    work = Path(folder) / 'nightshade'
    (work / 'tools').mkdir(parents=True)
    shutil.copyfile(root / 'tools/deps.json', work / 'tools/deps.json')
    (work / 'tools/build-radv-dependencies.sh').write_text('echo rebuilt >> calls\n')
    release = Path(folder) / 'mihawk-vulkan-review/.deps/native/radv-release'
    (release / 'lib').mkdir(parents=True)
    (release / 'lib/libvulkan_radeon.ps5.a').touch()
    body = 'set -euo pipefail\nstep() { :; }\n' + prepare[start:end]

    def rebuilt(*lines):
        (release / 'PROVENANCE.txt').write_text(''.join(line + '\n' for line in lines))
        (work / 'calls').unlink(missing_ok=True)
        subprocess.run(['bash', '-c', body], cwd=work, check=True)
        return (work / 'calls').exists()

    built = f'revision: {mesa["commit"]}'
    assert not rebuilt(built, f'radv build id: {mesa["shader_cache_id"]}'), 'the pinned driver must be kept'
    assert rebuilt(f'revision: {mesa["shader_cache_id"]}', f'radv build id: {mesa["shader_cache_id"]}'), \
        'a driver from another commit must be built again'
    assert rebuilt(built, f'radv build id: {mesa["commit"]}'), 'a driver with another build id must be built again'
    assert rebuilt(built), 'a driver with no build id recorded must be built again'

for note in notes:
    print(f'note: {note}')
print(f'PASS: Belladonna {mesa["commit"][:12]} is pinned and built with RADV build id {mesa["shader_cache_id"]} '
      '(shader_cache_id); a driver built otherwise is built again' + (' (with skips)' if notes else ''))
