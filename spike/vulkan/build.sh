#!/usr/bin/env bash
# JSRF for PS5 - builds the Vulkan phase 0 title (decision log D32) into pkg-vk/PPSA69430, with Mihawk's RADV port
# as rush2-ps5 has it (RADV_ROOT): its release archive, its link recipe (tools/radv-link.sh), its CRT and C++ runtime,
# its AGC link stubs and libc.prx, the way tools/build-radv-title.sh builds its smoke title. Shaders are compiled
# with glslangValidator in rush2-ps5's build image. Nothing in rush2-ps5 is changed.
set -euo pipefail
here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
workspace=$(cd -- "$here/../../.." && pwd)
root=${RADV_ROOT:-$workspace/rush2-ps5/radv/mihawk-vulkan-review}
mesa=${PS5_MESA:-$workspace/rush2-ps5/radv/PS5_Mesa}
archive="$root/.deps/native/radv-release/lib/libvulkan_radeon.ps5.a"
sdk_root="$root/.deps/native/ps5-payload-sdk"
native="$root/tooling/native"
# Mihawk's converter is built in rush2-ps5's image (a newer libstdc++ than this host's): run there.
tool() { docker run --rm -u "$(id -u):$(id -g)" -v "$workspace:$workspace" -w "$here" rush2-ps5-build \
    "$root/build/host/ps5-native-tool" "$@"; }
work="$here/../../build-vk"
title_id=PPSA69430
mkdir -p "$work/obj" "$work/stubs" "$work/gen"
cc() { PS5_PAYLOAD_SDK="$sdk_root" sh "$root/tooling/prospero-clang18" "$@"; }

# the shaders as SPIR-V arrays, compiled in the build image (glslangValidator is not on this host)
docker run --rm -v "$here:/src" -v "$(cd "$work" && pwd):/out" rush2-ps5-build sh -c '
    glslangValidator -V --target-env vulkan1.0 --vn drawcost_vert /src/shaders/draw.vert -o /out/gen/vert.h >/dev/null &&
    glslangValidator -V --target-env vulkan1.0 --vn drawcost_frag /src/shaders/draw.frag -o /out/gen/frag.h >/dev/null &&
    cat /out/gen/vert.h /out/gen/frag.h > /out/gen/drawcost_shaders.h &&
    chown '"$(id -u):$(id -g)"' /out/gen/*'

cc -std=c11 -O2 -Wall -Wextra -ffunction-sections -fdata-sections -I "$mesa/include" -I "$work/gen" \
    -c "$here/vk_drawcost.c" -o "$work/obj/vk_drawcost.o"
cc -std=c++20 -O2 -fno-exceptions -fno-rtti -ffunction-sections -fdata-sections -c "$native/app_crt.cpp" -o "$work/obj/app_crt.o"
cc -std=c++20 -O2 -fno-exceptions -fno-rtti -ffunction-sections -fdata-sections -c "$native/app_cpp_runtime.cpp" -o "$work/obj/app_cpp_runtime.o"
stub() {
    cc -std=c11 -O2 -fPIC -c "$root/$2" -o "$work/obj/$1_stub.o"
    "$sdk_root/bin/prospero-lld" --shared -soname "$1.prx" -o "$work/stubs/$1.so" "$work/obj/$1_stub.o"
}
stub libSceAgc vendor/ps5/sdk/stubs/agc_canary_link_stub.c
stub libSceAgcDriver vendor/ps5/sdk/stubs/agc_driver_canary_link_stub.c
# shellcheck source=/dev/null
source "$root/tools/radv-link.sh"
radv_link_recipe "$root" "$sdk_root" "$archive" || exit 2
"$sdk_root/bin/prospero-lld" "${radv_linker_script[@]}" --eh-frame-hdr "${radv_link_flags[@]}" \
    --version-script "$native/app-symbols.map" --exclude-libs=ALL -e _start -o "$work/llvm-pie.elf" \
    "$work/obj/app_crt.o" "$work/obj/app_cpp_runtime.o" "$work/obj/vk_drawcost.o" \
    "$work/stubs/libSceAgc.so" "$work/stubs/libSceAgcDriver.so" "${radv_link_inputs[@]}" \
    --as-needed "$sdk_root"/target/lib/*.so
tool link --in "$work/llvm-pie.elf" --out "$work/eboot.elf" --stub-dir "$sdk_root/target/lib" \
    --stub "$work/stubs/libSceAgc.so" --stub "$work/stubs/libSceAgcDriver.so" --module-sdk 0x02000009 \
    --companion-sdk 0x08050001 --file-name eboot.elf
app="$here/../../pkg-vk/$title_id"
rm -rf -- "$app"
mkdir -p "$app/sce_sys" "$app/sce_module"
tool self --sign --in "$work/eboot.elf" --out "$app/eboot.bin" --magic 0x1D3D154F
cp "$here/param.json" "$app/sce_sys/param.json"
cp "$here/../icon0.png" "$app/sce_sys/icon0.png"
cp "$root/runtime/libc.prx" "$app/sce_module/libc.prx"
ls -la "$app"
