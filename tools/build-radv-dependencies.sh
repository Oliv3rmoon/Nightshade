#!/usr/bin/env bash
# Build isolated, pinned RADV dependencies; never deploy or replace the old SDK.
# The driver is Belladonna (tools/deps.json, ps5-mesa): Mihawk's PS5_Mesa with Nightshade's changes
# as commits, built at its pinned commit as it is, with Mihawk's PS5_Vulkan recipe.
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
refs=$(dirname "$root")
export PS5_MESA_FORK="$refs/belladonna"
export PS5_PAYLOAD_SDK_FORK="$refs/mihawk-sdk-review"
vulkan="$refs/mihawk-vulkan-review"
# The three checkouts must sit at the revisions tools/deps.json pins (the only place they are set).
pin() { python3 -c 'import json, sys; print(next(i[sys.argv[3]] for i in json.load(open(sys.argv[1]))["items"] if i["name"] == sys.argv[2]))' "$root/tools/deps.json" "$1" "${2:-commit}"; }
for check in "ps5-vulkan-tools $vulkan" "ps5-mesa $PS5_MESA_FORK" "ps5-payload-sdk-fork $PS5_PAYLOAD_SDK_FORK"; do
    read -r name path <<< "$check"
    [[ $(git -C "$path" rev-parse HEAD) == "$(pin "$name")" ]] ||
        { echo "$path is not at the $name revision pinned in tools/deps.json" >&2; exit 1; }
done
PS5_MESA_REVISION=$(pin ps5-mesa)
# RADV's shader cache on disk is keyed by the build id. It stays shader_cache_id while Belladonna
# compiles shaders as before, so the caches players have stay valid across driver updates.
RADV_BUILD_ID=$(pin ps5-mesa shader_cache_id)
export PS5_MESA_REVISION RADV_BUILD_ID
# 24 parallel jobs unless RADV_BUILD_JOBS says otherwise: hosted CI runners have a few cores
# and too little memory for 24 compilers (.github/workflows/release.yml sets it).
jobs=${RADV_BUILD_JOBS:-24}
[[ $jobs =~ ^[1-9][0-9]*$ ]] || { echo "RADV_BUILD_JOBS must be a positive number: $jobs" >&2; exit 2; }
export BUILD_JOBS=$jobs CMAKE_BUILD_PARALLEL_LEVEL=$jobs
mkdir -p "$root/build/radv-tools"
# Upstream calls ninja directly; enforce the same bounded parallelism everywhere.
ninja=$(command -v ninja) || { echo 'ninja not found' >&2; exit 1; }
printf '#!/bin/sh\nexec %s -j%s "$@"\n' "$ninja" "$jobs" > "$root/build/radv-tools/ninja"
chmod +x "$root/build/radv-tools/ninja"
export NINJA="$root/build/radv-tools/ninja"
command -v ccache >/dev/null
# Cross compilers do not get Meson's automatic native ccache detection.
# Keep this two-line build-only adaptation reproducible on a fresh checkout.
python3 - "$vulkan/tooling/radv/ps5-cross.ini" <<'PY'
from pathlib import Path
import sys
p = Path(sys.argv[1])
s = p.read_text()
for lang, compiler in [('c', 'prospero-clang'), ('cpp', 'prospero-clang++')]:
    plain = f"{lang} = sdk / 'bin/{compiler}'"
    cached = f"{lang} = ['ccache', sdk / 'bin/{compiler}']"
    assert plain in s or cached in s, 'Unexpected upstream cross compiler configuration'
    s = s.replace(plain, cached)
p.write_text(s)
PY
# The recipe names the RADV build id after the Mesa revision it builds. Take it from RADV_BUILD_ID,
# record it in PROVENANCE.txt and build again when it differs; like ps5-cross.ini above, only the
# checkout's working tree is changed, never a commit.
python3 - "$vulkan/tools/build-radv.sh" <<'PY'
from pathlib import Path
import sys
p = Path(sys.argv[1])
s = p.read_text()
build_id = '${RADV_BUILD_ID:-$mesa_revision}'
revision_kept = 'grep -q "^revision: $mesa_revision$" "$install/PROVENANCE.txt" &&\n'
for old, new, count in [
        ('-Dradv-build-id="$mesa_revision"', '-Dradv-build-id="' + build_id + '"', 2),
        ('/.radv-build-id" 2>/dev/null) != "$mesa_revision" ]]', '/.radv-build-id" 2>/dev/null) != "' + build_id + '" ]]', 1),
        ('printf \'%s\\n\' "$mesa_revision" > "$build/.radv-build-id"',
         'printf \'%s\\n\' "' + build_id + '" > "$build/.radv-build-id"', 1),
        ('revision: $mesa_revision\nassertions:', 'revision: $mesa_revision\nradv build id: ' + build_id + '\nassertions:', 1),
        (revision_kept, revision_kept + '    grep -q "^radv build id: ' + build_id + '$" "$install/PROVENANCE.txt" &&\n', 1)]:
    if s.count(new) == count:
        continue
    assert s.count(old) == count, f'Unexpected upstream build-radv.sh: {old!r}'
    s = s.replace(old, new)
p.write_text(s)
PY
bash "$vulkan/tools/setup-native-dependencies.sh"
bash "$vulkan/tools/build-radv.sh" release
# For the build log: the commit built, and the build id the compiler was given (RADV's
# RADV_BUILD_ID_OVERRIDE, which the shader cache's key comes from).
provenance="$vulkan/.deps/native/radv-release/PROVENANCE.txt"
cat "$provenance"
commands="$vulkan/.deps/work/radv-build-ps5-release/compile_commands.json"
compiled=$(grep -oE 'RADV_BUILD_ID_OVERRIDE=[^0-9a-f]*[0-9a-f]+' "$commands" | grep -oE '[0-9a-f]+$' | sort -u || true)
echo "==> [radv] Belladonna ${PS5_MESA_REVISION}, compiled with RADV_BUILD_ID_OVERRIDE=${compiled//$'\n'/,}"
grep -qx "revision: $PS5_MESA_REVISION" "$provenance" && grep -qx "radv build id: $RADV_BUILD_ID" "$provenance" &&
    [[ $compiled == "$RADV_BUILD_ID" ]] ||
    { echo "RADV was not built from Belladonna $PS5_MESA_REVISION with build id $RADV_BUILD_ID" >&2; exit 1; }
