#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Prepares everything the PS5 build needs besides Eden itself (make prepare), after the pinned
# inputs are in place (make deps): this checkout's build cache, Eden's source tree, the Payload
# SDK, FFmpeg for the PS5, the native packaging tool, the runtime libc.prx, the libSceAgcDriver
# link stub and the isolated RADV driver. Each step is skipped when its result already exists.
set -euo pipefail
export PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
cd "$root"
step() { echo "== $*"; }

# Thousands of small files: keep the build cache on a native Linux filesystem, one per checkout.
cache_parent="${XDG_CACHE_HOME:-$HOME/.cache}"
if [[ ! -f .local/headless-cache ]] || [[ ! -d "$(cat .local/headless-cache)" ]]; then
    scratch="$cache_parent/ps5-eden-headless.$(printf '%s' "$root" | sha256sum | cut -c1-12)"
    mkdir -p .local "$scratch"
    printf '%s\n' "$root" > "$scratch/owner"
    printf '%s\n' "$scratch" > .local/headless-cache
fi
scratch=$(cat .local/headless-cache)
[[ "$(cat "$scratch/owner")" == "$root" ]] || { echo "Build cache $scratch belongs to another checkout" >&2; exit 1; }

eden="$scratch/source"
eden_archive=eden-50fa86bb.tar.gz
# A tree unpacked from another archive is the source of another Eden revision: it is unpacked
# again (Eden's package cache, downloads only, is kept). A tree left from before an update of
# the pin was built as if it were the new one, and what Eden had changed went unseen.
if [[ ! -f $eden/CMakeLists.txt || $(cat "$eden/.unpacked-from" 2>/dev/null) != "$eden_archive" ]]; then
    step "Eden source"
    if [[ -d $eden/.cache ]]; then rm -rf "$scratch/source-cache" && mv "$eden/.cache" "$scratch/source-cache"; fi
    rm -rf "$eden"
    mkdir -p "$eden"
    tar -xzf ".deps/$eden_archive" --strip-components=1 -C "$eden"
    if [[ -d $scratch/source-cache ]]; then mv "$scratch/source-cache" "$eden/.cache"; fi
    printf '%s\n' "$eden_archive" > "$eden/.unpacked-from"
fi
printf '%s\n' '50fa86bbbc403ad565eb44fcc281f16f77c69e15' > "$eden/GIT-COMMIT"
printf '%s\n' 'ps5-headless' > "$eden/GIT-REFSPEC"
# Fixes to Eden's own source (tools/eden-patches, in name order). A later patch can change lines
# an earlier one changed (08 rewrites some of 06's), so the earlier one no longer reverses on its
# own: the patches go in as one set. The tree keeps the set's hash (.eden-patches); a tree with
# another set, or none, gets every file any patch touches back from the pinned archive, and then
# all of them in order.
command -v patch >/dev/null || { echo "patch is required (apt install patch)" >&2; exit 1; }
eden_patches=(tools/eden-patches/*.diff)
eden_patch_set=$(cat "${eden_patches[@]}" | sha256sum | cut -d' ' -f1)
if [[ $(cat "$eden/.eden-patches" 2>/dev/null) != "$eden_patch_set" ]]; then
    step "Eden patches (${#eden_patches[@]})"
    rm -f "$eden/.eden-patches"
    sed -n 's|^+++ b/||p' "${eden_patches[@]}" | sort -u | sed 's|^|*/|' |
        xargs tar -xzf ".deps/$eden_archive" --wildcards --strip-components=1 -C "$eden"
    for eden_patch in "${eden_patches[@]}"; do
        patch -d "$eden" -p1 -N -s < "$eden_patch" || { echo "$eden_patch does not apply to Eden's source" >&2; exit 1; }
    done
    printf '%s\n' "$eden_patch_set" > "$eden/.eden-patches"
fi
# Optional: seed Eden's package cache from another checkout's (CI reuses a development cache).
if [[ -n ${EDEN_CPM_CACHE_SEED:-} && ! -d $eden/.cache/cpm ]]; then
    step "Eden package cache from $EDEN_CPM_CACHE_SEED"
    mkdir -p "$eden/.cache"
    cp -a "$EDEN_CPM_CACHE_SEED" "$eden/.cache/cpm"
fi
# The FFmpeg source Eden pins, where Eden's package cache keeps it.
ffmpeg_source="$eden/.cache/cpm/ffmpeg/c7b5f1537d"
if [[ ! -f $ffmpeg_source/configure ]]; then
    step "FFmpeg source"
    mkdir -p "$(dirname "$ffmpeg_source")"
    cp -a .deps/ffmpeg-c7b5f1537d "$ffmpeg_source"
fi

if [[ ! -f $scratch/sdk/.complete ]]; then
    step "Payload SDK"
    mkdir -p "$scratch/sdk"
    cp -a ../ps5-native-app-boilerplate/.deps/native/ps5-payload-sdk/target "$scratch/sdk/"
    touch "$scratch/sdk/.complete"
fi
if [[ ! -f $scratch/ffmpeg-native/install/lib/libavcodec.a ]]; then
    step "FFmpeg for the PS5"
    bash tools/build-headless-ffmpeg.sh
fi
step "Native packaging tool"
bash tools/build-native-tool.sh
# The app's runtime libc.prx: the boilerplate's clean-room build, checked against its recorded digest.
if [[ ! -f ../ps5-native-app-boilerplate/runtime/libc.prx ]]; then
    step "Runtime libc.prx"
    bash ../ps5-native-app-boilerplate/tools/rebuild-libc.sh
fi
step "libSceAgcDriver link stub"
bash tools/build-agc-driver-stub.sh
bash tools/build-common-dialog-stub.sh
# The driver is Belladonna at the commit tools/deps.json pins, built with the RADV build id its
# shader cache is keyed by (shader_cache_id there). A driver built from another commit, or with
# another build id, is built again (only what changed compiles).
radv_release=../mihawk-vulkan-review/.deps/native/radv-release
mesa_pin=$(python3 -c 'import json; print(next(i["commit"] for i in json.load(open("tools/deps.json"))["items"] if i["name"] == "ps5-mesa"))')
build_id=$(python3 -c 'import json; print(next(i["shader_cache_id"] for i in json.load(open("tools/deps.json"))["items"] if i["name"] == "ps5-mesa"))')
if [[ -f $radv_release/lib/libvulkan_radeon.ps5.a ]]; then
    built=$(sed -n 's/^revision: //p' "$radv_release/PROVENANCE.txt" 2>/dev/null || true)
    built_id=$(sed -n 's/^radv build id: //p' "$radv_release/PROVENANCE.txt" 2>/dev/null || true)
    if [[ $built != "$mesa_pin" ]]; then
        step "RADV: built from ${built:0:12}, pinned Belladonna ${mesa_pin:0:12}"
        bash tools/build-radv-dependencies.sh
        rm -f build/radv-isolated/libvulkan_radeon.ps5.a
    elif [[ $built_id != "$build_id" ]]; then
        step "RADV: built with build id ${built_id:0:12}, pinned ${build_id:0:12}"
        bash tools/build-radv-dependencies.sh
        rm -f build/radv-isolated/libvulkan_radeon.ps5.a
    fi
fi
if [[ ! -f build/radv-isolated/libvulkan_radeon.ps5.a ]]; then
    if [[ ! -f $radv_release/lib/libvulkan_radeon.ps5.a ]]; then
        step "RADV (Mesa) for the PS5 - this takes a while"
        bash tools/build-radv-dependencies.sh
    fi
    step "Isolating RADV beside the OpenGL Mesa"
    python3 -B tools/isolate-radv.py
fi
step "Prepared: $scratch"
