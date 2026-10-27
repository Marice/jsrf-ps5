#!/usr/bin/env bash
# Builds the Vulkan build of JSRF Unleashed for the PS5 into pkg-vk/<title>/ (decision log D33, D34): the game's
# sources on Mihawk's RADV port as rush2-ps5 has it (RADV_ROOT): the SDK fork's toolchain and platform layer,
# its link recipe (ps5/link-vk.sh), its converter (run in rush2-ps5's build image, whose libstdc++ it needs).
#
#   tools/setup-source.sh   once: upstream at the pinned commit, with patches/jsrf.patch
#   ./build-vk.sh
#
# The title is PPSA69431: its eboot.bin goes next to the unpacked game.
set -euo pipefail

root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
workspace=$(cd -- "$root/.." && pwd)
title_id=PPSA69431
: "${RADV_ROOT:=$workspace/rush2-ps5/radv/mihawk-vulkan-review}"
sdk_root="$RADV_ROOT/.deps/native/ps5-payload-sdk"
PACBREW=$("$root/tools/setup-pacbrew.sh")
export RADV_ROOT PACBREW PS5_PAYLOAD_SDK="$sdk_root"
tool() { docker run --rm -u "$(id -u):$(id -g)" -v "$workspace:$workspace" -w "$root" rush2-ps5-build \
    "$RADV_ROOT/build/host/ps5-native-tool" "$@"; }

[ -f "$root/src-upstream/port/recomp/gen/recomp_types.h" ] || { echo "build-vk.sh: run tools/setup-source.sh and generate the code first (README)" >&2; exit 1; }
for need in "$RADV_ROOT/build/host/ps5-native-tool" "$RADV_ROOT/runtime/libc.prx" "$sdk_root/toolchain/prospero.cmake"; do
    [ -e "$need" ] || { echo "build-vk.sh: $need not found" >&2; exit 1; }
done

build="$root/build-ps5-vk"
cmake -B "$build" -S "$root" \
    -DCMAKE_TOOLCHAIN_FILE="$sdk_root/toolchain/prospero.cmake" \
    -DCMAKE_PROJECT_INCLUDE_BEFORE="$root/ps5/ps5-vk.cmake" \
    -DPKG_CONFIG_EXECUTABLE="$root/ps5/ps5-pkg-config" \
    -DCMAKE_PREFIX_PATH="$PACBREW" -DCMAKE_FIND_ROOT_PATH="$PACBREW" \
    -DCMAKE_BUILD_TYPE=Release
# JOBS: fewer parallel compiles for a machine that swaps on the generated sources (each a few GB at -O2)
cmake --build "$build" -j"${JOBS:-$(nproc)}"

# The ELF becomes a title binary: converted against the SDK fork's module stubs and the AGC link stubs, then signed.
pkg="$root/pkg-vk"
title="$pkg/$title_id"
stubs="$build/port/ps5-link-vk/stubs"
mkdir -p "$title/sce_sys" "$title/sce_module"
tool link --in "$build/port/jsrf_recomp" --out "$pkg/eboot.elf" --stub-dir "$sdk_root/target/lib" \
    --stub "$stubs/libSceAgc.so" --stub "$stubs/libSceAgcDriver.so" --module-sdk 0x02000009 \
    --companion-sdk 0x08050001 --file-name eboot.elf
tool self --sign --in "$pkg/eboot.elf" --out "$title/eboot.bin" --magic 0x1D3D154F
# The C library the title imports, RADV's (D2: without one, PRX_SCE_MODULE_LOAD_ERROR).
cp "$RADV_ROOT/runtime/libc.prx" "$title/sce_module/libc.prx"
cp "$root/sce_sys/param.json" "$root/sce_sys/icon0.png" "$title/sce_sys/"
for pic in pic0.dds pic1.dds; do
    [ -f "$root/sce_sys/$pic" ] && cp "$root/sce_sys/$pic" "$title/sce_sys/"
done
# The player's disc image goes here (README); an empty folder shows where.
mkdir -p "$title/dump"
printf 'Put your Jet Set Radio Future (USA) disc image (.iso) in this folder.\n' > "$title/dump/PUT-THE-DISC-IMAGE-HERE.txt"
ls -la "$title"
