#!/usr/bin/env bash
# Links the Vulkan build of JSRF (decision log D34) with Mihawk's RADV link recipe, as rush2-ps5's ps5/link-title.sh
# and spike/vulkan/build.sh do: the RADV archive whole, the SDK fork's platform layer, libc++ and CRT, the AGC link
# stubs. CMake calls it as the executable link rule (ps5/ps5-vk.cmake):
#   link-vk.sh <output> <link flags, objects and libraries as CMake passes them>
# It writes <output>, the unsigned ELF; build-vk.sh converts and signs it.
set -euo pipefail
: "${RADV_ROOT:?set RADV_ROOT to the PS5_Vulkan checkout}"
: "${PACBREW:?set PACBREW to the PacBrew sysroot (user/homebrew)}"
out=$1
shift
here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
root=$RADV_ROOT
sdk_root="$root/.deps/native/ps5-payload-sdk"
archive="$root/.deps/native/radv-release/lib/libvulkan_radeon.ps5.a"
native="$root/tooling/native"
work="$(dirname "$out")/ps5-link-vk"
mkdir -p "$work/obj" "$work/stubs"
cc() { PS5_PAYLOAD_SDK="$sdk_root" sh "$root/tooling/prospero-clang18" "$@"; }

# CMake's flags and inputs: our --wrap names kept, PacBrew -l names as archives, system modules from the SDK's stubs.
inputs=()
wraps=()
for arg in "$@"; do
    case $arg in
        -Wl,--wrap=*) wraps+=("${arg#-Wl,}") ;;
        -lm|-latomic|-pthread|-ldl|-lpthread|-Wl,*) ;;
        -l*)
            lib="$PACBREW/lib/lib${arg#-l}.a"
            [[ -f $lib ]] || { echo "link-vk.sh: no archive for $arg" >&2; exit 2; }
            inputs+=("$lib") ;;
        -*) ;;
        *.so) ;;
        *) inputs+=("$arg") ;;
    esac
done
# As the OpenGL build: curl's group (ps5-gl.cmake), which OpenSSL comes with.
for lib in libcurl libpsl libzstd libssl libcrypto; do inputs+=("$PACBREW/lib/$lib.a"); done
# glslang, built for the PS5 in build-vk/glslang (the phase 0 spike's, D32): the Vulkan device compiles its shaders.
glslang="$here/../build-vk/glslang/glslang"
inputs+=("$glslang/libglslang.a" "$glslang/libglslang-default-resource-limits.a")

cc -std=c++20 -O2 -fno-exceptions -fno-rtti -c "$native/app_crt.cpp" -o "$work/obj/app_crt.o"
cc -std=c++20 -O2 -fno-exceptions -fno-rtti -c "$native/app_cpp_runtime.cpp" -o "$work/obj/app_cpp_runtime.o"
stub() {
    cc -std=c11 -O2 -fPIC -c "$root/$2" -o "$work/obj/$1_stub.o"
    "$sdk_root/bin/prospero-lld" --shared -soname "$1.prx" -o "$work/stubs/$1.so" "$work/obj/$1_stub.o"
}
stub libSceAgc vendor/ps5/sdk/stubs/agc_canary_link_stub.c
stub libSceAgcDriver vendor/ps5/sdk/stubs/agc_driver_canary_link_stub.c

# shellcheck source=/dev/null
source "$root/tools/radv-link.sh"
radv_link_recipe "$root" "$sdk_root" "$archive" || exit 2
# Every symbol stays in the title (ps5-symbols.map): the converter publishes no exports.
"$sdk_root/bin/prospero-lld" "${radv_linker_script[@]}" --eh-frame-hdr "${radv_link_flags[@]}" "${wraps[@]}" \
    --version-script "$here/ps5-symbols.map" --exclude-libs=ALL -e _start -o "$out" \
    "$work/obj/app_crt.o" "$work/obj/app_cpp_runtime.o" \
    "$here/ps5-vk-extra.ld" --start-group "${inputs[@]}" --end-group \
    "$work/stubs/libSceAgc.so" "$work/stubs/libSceAgcDriver.so" "${radv_link_inputs[@]}" \
    --as-needed "$sdk_root"/target/lib/*.so
echo "link-vk.sh: $out ($(stat -c %s "$out") bytes)"
