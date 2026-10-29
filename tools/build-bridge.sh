#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Builds the bridge (headless/bridge/bridge.h): the PPSA99008 app the release ZIP carries beside
# Nightshade for the installs made before Nightshade had its own title ID. It opens PPSA64448, and
# installs it first when the console doesn't list it.
#
#   tools/build-bridge.sh OUT_DIR      OUT_DIR becomes the app's folder, PPSA99008/ of the ZIP
#
# The smallest app that does that, from Nightshade's own sources and none of the emulator: the
# update kit and Settings > Apps' install and launch (headless/update_check, apps_release.cpp, with
# PacBrew's libcurl and OpenSSL), the launcher's font and catalogs (pe/gfx/font.cpp,
# pe/core/strings.cpp), the shared heap (third_party/app_heap.c) and the end of a title
# (src/lifecycle.c). It is compiled with the boilerplate's compiler wrapper, linked as the app is
# (tools/link-headless-native.sh, without its driver) and packaged by the same native tool. It ships
# the payloads the app's package built (tools/package-headless-native.sh: self-updater.elf,
# app-launch.elf), so it runs after that.
#
# BRIDGE_TEMPLATE, BRIDGE_SDK, BRIDGE_CDEPS, BRIDGE_CLANG and BRIDGE_TOOL name other places for the
# boilerplate, its Payload SDK, PacBrew's prefix, Clang 18 and the native tool; BRIDGE_NO_PACKAGE=1
# stops after the link (a check of the code without the packaging tool).
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
app=${1:?usage: tools/build-bridge.sh OUT_DIR}
template=${BRIDGE_TEMPLATE:-$root/../ps5-native-app-boilerplate}
sdk=${BRIDGE_SDK:-$template/.deps/native/ps5-payload-sdk}
cdeps=${BRIDGE_CDEPS:-${EDEN_CDEPS_SOURCE:-$root/.deps/pacbrew-0.40.2/opt/ps5-payload-sdk/target/user/homebrew}}
clang=${BRIDGE_CLANG:-$(command -v clang-18 || true)}
tool=${BRIDGE_TOOL:-$root/build/host/ps5-native-tool}
out=$root/build/bridge
[[ -x $clang ]] || { echo "Clang 18 not found (BRIDGE_CLANG)" >&2; exit 2; }
for need in "$template/tooling/prospero-clang18" "$template/tooling/native/app_crt.cpp" \
            "$template/tooling/native/ps5-pie.ld" "$sdk/bin/prospero-lld" "$sdk/target/lib/libc++.a" \
            "$cdeps/lib/libcurl.a" "$cdeps/include/curl/curl.h"; do
    [[ -e $need ]] || { echo "missing $need (make prepare)" >&2; exit 2; }
done
rm -rf "$out"
mkdir -p "$out/obj"

compile() { # compile <source> <flags...>
    local source=$1 object
    shift
    object="$out/obj/$(echo "${source#"$root/"}" | tr '/' '_').o"
    env PS5_PAYLOAD_SDK="$sdk" PS5_CLANG="$clang" USE_CCACHE="${USE_CCACHE:-0}" \
        sh "$template/tooling/prospero-clang18" "$@" -O2 -ffunction-sections -fdata-sections \
        -c "$source" -o "$object"
    objects+=("$object")
}
objects=()
headless=$root/headless
includes=(-I"$headless" -I"$headless/prosperoeden" -I"$headless/update_check" -I"$root/third_party")
# The update kit: what the self-update helper and Settings > Apps install with, libcurl beside it.
for name in update_check console_curl github_release self_update self_update_ps5 self_update_sha256; do
    compile "$headless/update_check/$name.c" -std=c11 -Wall -Wextra -DCURL_STATICLIB=1 \
        -I"$headless/update_check" -I"$cdeps/include"
done
# The shared heap (the C library's own is small) and the end of a title.
compile "$root/third_party/app_heap.c" -std=c11 -w
compile "$root/src/lifecycle.c" -std=c11 -Wall -Wextra
for source in "$headless/bridge/main.cpp" "$headless/bridge/bridge.cpp" "$headless/bridge/view.cpp" \
              "$headless/bridge/picture.cpp" "$headless/bridge/screen.cpp" \
              "$headless/bridge/no_system_fonts.cpp" "$headless/apps_release.cpp" \
              "$headless/prosperoeden/pe/gfx/font.cpp" "$headless/prosperoeden/pe/gfx/bidi.cpp" \
              "$headless/prosperoeden/pe/core/strings.cpp"; do
    compile "$source" -std=c++20 -Wall -Wextra -Werror -Wno-unused-parameter -fno-exceptions -fno-rtti "${includes[@]}"
done
compile "$template/tooling/native/app_crt.cpp" -std=c++20 -Wall -Wextra -fno-exceptions -fno-rtti

# As tools/link-headless-native.sh links the app, without RADV: the boilerplate's linker script, the
# heap's wrappers, and fcntl for libcurl's sockets (update_check/console_curl.h).
"$sdk/bin/prospero-lld" --defsym=__cxa_thread_atexit_impl=0 -L "$sdk/target/lib" \
    --defsym=__dlopen=0 --defsym=__dlsym=0 --defsym=__dladdr=0 --defsym=__dlclose=0 --defsym=__dlerror=0 \
    -T "$template/tooling/native/ps5-pie.ld" -T "$root/tools/unwind.ld" \
    --eh-frame-hdr --gc-sections --version-script "$root/tools/app-symbols.map" -e _start \
    --no-dynamic-linker --error-limit=0 -Map="$out/llvm-pie.elf.map" \
    --wrap=malloc --wrap=calloc --wrap=realloc --wrap=free --wrap=posix_memalign --wrap=malloc_usable_size \
    --wrap=fcntl \
    -o "$out/llvm-pie.elf" --start-group "${objects[@]}" \
    "$cdeps/lib/libcurl.a" "$cdeps/lib/libpsl.a" "$cdeps/lib/libssl.a" "$cdeps/lib/libcrypto.a" \
    "$cdeps/lib/libzstd.a" "$cdeps/lib/libz.a" \
    "$sdk/target/lib/libc++.a" "$sdk/target/lib/libc++abi.a" "$sdk/target/lib/libunwind.a" \
    --end-group --as-needed "$sdk/target/lib/libSceLibcInternal.so" "$sdk/target/lib/libkernel.so" \
    "$sdk/target/lib/libc.a" "$sdk/target/lib/libSceNet.so" "$sdk/target/lib/libScePad.so" \
    "$sdk/target/lib/libSceUserService.so" "$sdk/target/lib/libSceSystemService.so" \
    "$sdk/target/lib/libSceVideoOut.so"
echo "Linked $out/llvm-pie.elf ($(wc -c < "$out/llvm-pie.elf") bytes)"
[[ ${BRIDGE_NO_PACKAGE:-0} == 1 ]] && exit 0

[[ -x $tool ]] || { echo "missing $tool (make prepare)" >&2; exit 2; }
for payload in "$root/build/self-update/self-updater.elf" "$root/build/app-launch/app-launch.elf"; do
    [[ -f $payload ]] || { echo "missing $payload (tools/package-headless-native.sh builds it)" >&2; exit 2; }
done
"$tool" link --in "$out/llvm-pie.elf" --out "$out/eboot.elf" --stub-dir "$sdk/target/lib" \
    --module-sdk 0x02000009 --companion-sdk 0x08050001 --file-name eboot.elf
rm -rf "$app"
mkdir -p "$app/sce_sys" "$app/sce_module" "$app/ui/fonts" "$app/ui/lang"
"$tool" self --sign --in "$out/eboot.elf" --out "$app/eboot.bin" --magic 0x1D3D154F
(cd "$template/runtime" && sha256sum --check --strict libc.prx.sha256)
cp "$template/runtime/libc.prx" "$app/sce_module/libc.prx"
# Its param.json: the bridge's own, at the app's version (the version's one home is
# sce_sys/param.json); the home screen's picture is Nightshade's icon.
python3 - "$root/headless/bridge/sce_sys/param.json" "$root/sce_sys/param.json" "$app/sce_sys/param.json" <<'PY'
import json, sys
bridge, app = (json.load(open(path)) for path in sys.argv[1:3])
assert 'contentVersion' not in bridge, 'the bridge takes the app version: headless/bridge/sce_sys/param.json names none'
bridge['contentVersion'] = app['contentVersion']
open(sys.argv[3], 'w').write(json.dumps(dict(sorted(bridge.items())), indent=2) + '\n')
PY
cp "$root/sce_sys/icon0.png" "$app/sce_sys/icon0.png"
cp "$root/build/self-update/self-updater.elf" "$app/self-updater.elf"
cp "$root/build/app-launch/app-launch.elf" "$app/app-launch.elf"
cp "$headless/prosperoeden/ui/fonts/montserrat-medium.pefont" "$app/ui/fonts/"
cp "$headless/prosperoeden/ui/lang/"*.po "$app/ui/lang/"
# What an install made before keeps of a launcher pack it selected is checked against this file
# before its update helper takes the bridge (self_update_helper/updater.cpp,
# launcher_update_compatible), as for any release: the app's own.
cp "$root/launcher-runtime.json" "$app/launcher-runtime.json"
llvm-readobj-18 --dyn-symbols --needed-libs "$out/llvm-pie.elf" > "$out/imports.txt"
"$tool" self --inspect --file "$app/eboot.bin" > "$out/fself-inspection.txt"
echo "Built the bridge in $app"
