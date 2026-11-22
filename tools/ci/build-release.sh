#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Release build for CI (.github/workflows/release.yml): `make release` in this checkout.
# EDEN_DEV_CHECKOUT optionally names a development checkout whose inputs are reused instead of
# fetched: its sibling repositories and .deps inputs are linked in (never copied or modified),
# and its Eden package cache seeds this checkout's. See docs/BUILDING.md.
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
cd "$root"
if [[ -n ${EDEN_DEV_CHECKOUT:-} ]]; then
    dev=$(cd -- "$EDEN_DEV_CHECKOUT" && pwd)
    if [[ $dev != "$root" ]]; then
        read -r belladonna_pin cache_id < <(python3 -c 'import json; mesa = next(i for i in json.load(open("tools/deps.json"))["items"] if i["name"] == "ps5-mesa"); print(mesa["commit"], mesa["shader_cache_id"])')
        dev_driver=$(git -C "$dev/../belladonna" rev-parse HEAD 2>/dev/null || true)
        dev_radv="$dev/../mihawk-vulkan-review/.deps/native/radv-release"
        reuse_radv=0
        if [[ $dev_driver == "$belladonna_pin" && -f $dev_radv/lib/libvulkan_radeon.ps5.a &&
              -f $dev_radv/PROVENANCE.txt ]] &&
           grep -Fxq "revision: $belladonna_pin" "$dev_radv/PROVENANCE.txt" &&
           grep -Fxq "radv build id: $cache_id" "$dev_radv/PROVENANCE.txt"; then
            reuse_radv=1
        fi
        for sibling in ps5-native-app-boilerplate mihawk-vulkan-review belladonna mihawk-sdk-review; do
            if [[ ! -e ../$sibling && -d $dev/../$sibling ]]; then
                [[ $sibling != belladonna || $dev_driver == "$belladonna_pin" ]] || continue
                [[ $sibling != mihawk-vulkan-review || $reuse_radv == 1 ]] || continue
                ln -s "$(cd -- "$dev/../$sibling" && pwd)" "../$sibling"
            fi
        done
        mkdir -p .deps build
        for input in "$dev"/.deps/eden-67bada77.tar.gz "$dev"/.deps/mirror-* "$dev"/.deps/ffmpeg-* \
                     "$dev"/.deps/fmt-12.1.0 "$dev"/.deps/compiler-rt-18.1.8 "$dev"/.deps/pacbrew-* \
                     "$dev"/.deps/ps5-opengl-sdk-*; do
            [[ -e $input && ! -e .deps/${input##*/} ]] && ln -s "$input" ".deps/${input##*/}"
        done
        if [[ $reuse_radv == 1 && ! -e build/radv-isolated && -d $dev/build/radv-isolated ]]; then
            ln -s "$dev/build/radv-isolated" build/radv-isolated
        fi
        if [[ -f $dev/.local/headless-cache ]]; then
            export EDEN_CPM_CACHE_SEED="$(cat "$dev/.local/headless-cache")/source/.cache/cpm"
        fi
    fi
fi
make release
