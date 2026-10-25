#!/usr/bin/env bash
# JSRF for PS5 - builds the phase 0 check (spike/phase0) into the title folder pkg/PPSA69429 (decision log D1).
#
# PacBrew and the OpenGL SDK come from tools/setup-*.sh (checked by hash); ps5-native-tool, the console's module
# stubs and sce_module/libc.prx from the sibling projects (olise-player, stk-ps5), as in a2-racer-2-ps5.
set -euo pipefail

here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
root=$(cd -- "$here/.." && pwd)
workspace=$(cd -- "$root/.." && pwd)
title_id=PPSA69429
: "${PS5_PAYLOAD_SDK:=/opt/ps5-payload-sdk}"
: "${PS5_NATIVE_TOOL:=$workspace/olise-player/build/host/ps5-native-tool}"
: "${PS5_STUBS:=$workspace/stk-ps5/pkg/stubs}"
: "${PS5_LIBC_PRX:=$workspace/stk-ps5/pkg/PPSA69421/sce_module/libc.prx}"
PACBREW=$("$root/tools/setup-pacbrew.sh")
PS5_OPENGL_SDK=$("$root/tools/setup-glsdk.sh")
export PS5_PAYLOAD_SDK PACBREW

for need in "$PS5_NATIVE_TOOL" "$PS5_LIBC_PRX" "$PS5_STUBS"; do
    [ -e "$need" ] || { echo "spike/build.sh: $need not found" >&2; exit 1; }
done

build="$root/build-phase0"
cmake -B "$build" -S "$here/phase0" \
    -DCMAKE_TOOLCHAIN_FILE="$PS5_PAYLOAD_SDK/toolchain/prospero.cmake" \
    -DCMAKE_PROJECT_INCLUDE_BEFORE="$root/ps5/ps5-gl.cmake" \
    -DPKG_CONFIG_EXECUTABLE="$root/ps5/ps5-pkg-config" \
    -DPS5_OPENGL_SDK="$PS5_OPENGL_SDK" \
    -DCMAKE_PREFIX_PATH="$PACBREW" -DCMAKE_FIND_ROOT_PATH="$PACBREW" \
    -DCMAKE_BUILD_TYPE=Release
cmake --build "$build" -j"$(nproc)"

pkg="$root/pkg"
title="$pkg/$title_id"
mkdir -p "$title/sce_sys" "$title/sce_module" "$pkg/stubs"
cp "$PS5_STUBS/"*.so "$pkg/stubs/"
"$PS5_NATIVE_TOOL" link --in "$build/jsrf_phase0" --out "$pkg/eboot.elf" --stub-dir "$pkg/stubs" \
    --module-sdk 0x02000009 --companion-sdk 0x08050001 --file-name eboot.elf
"$PS5_NATIVE_TOOL" self --sign --in "$pkg/eboot.elf" --out "$title/eboot.bin" --magic 0x1D3D154F
# The C library the title imports; without it: PRX_SCE_MODULE_LOAD_ERROR (a2-racer-2-ps5 D2).
cp "$PS5_LIBC_PRX" "$title/sce_module/libc.prx"
cp "$here/param.json" "$here/icon0.png" "$title/sce_sys/"
ls -la "$title"
