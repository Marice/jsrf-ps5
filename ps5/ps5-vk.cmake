# JSRF for PS5 on Vulkan (decision log D33, D34): what ps5-gl.cmake is to the OpenGL build, for Mihawk's RADV port.
# build-vk.sh passes it as CMAKE_PROJECT_INCLUDE_BEFORE with the SDK fork's toolchain (RADV_ROOT). The compile is the
# OpenGL build's; the link is ps5/link-vk.sh, which links RADV the way rush2-ps5 does (tools/radv-link.sh) and takes
# from the platform layer what ps5-gl.cmake took from our own: the heap in direct memory, thread stacks, fflush.
if(NOT PS5_PACBREW)
    if(DEFINED ENV{PACBREW})
        set(PS5_PACBREW "$ENV{PACBREW}")
    else()
        message(FATAL_ERROR "Set PACBREW to the PacBrew sysroot's user/homebrew directory")
    endif()
endif()
set(JSRF_PS5_VULKAN ON)

# No OpenGL: upstream's renderer is built without its OpenGL device (NV2A_GL_USE_NONE, as its iOS build).
set(ENV{PKG_CONFIG_LIBDIR} "${PS5_PACBREW}/lib/pkgconfig")
set(ENV{PKG_CONFIG_PATH} "${PS5_PACBREW}/lib/pkgconfig")
set(ENV{PKG_CONFIG_SYSROOT_DIR} "")
set(PKG_CONFIG_USE_CMAKE_PREFIX_PATH OFF)
if(NOT TARGET SDL2::SDL2-static)
    add_library(SDL2::SDL2-static STATIC IMPORTED GLOBAL)
    set_target_properties(SDL2::SDL2-static PROPERTIES
        IMPORTED_LOCATION "${PS5_PACBREW}/lib/libSDL2.a"
        INTERFACE_INCLUDE_DIRECTORIES "${PS5_PACBREW}/include;${PS5_PACBREW}/include/SDL2"
        INTERFACE_LINK_LIBRARIES "${PS5_PACBREW}/lib/libiconv.a")
endif()
if(NOT TARGET SDL2::SDL2)
    add_library(SDL2::SDL2 ALIAS SDL2::SDL2-static)
endif()
link_directories("${PS5_PACBREW}/lib")

# Our wraps over the platform's libc. Not malloc and its kin, pthread_create or fflush: the platform wraps those
# (radv-link.sh), so ours are called jsrf_pthread_create and jsrf_fflush there (ps5-posix.c, JSRF_PS5_RADV).
set(PROSPERO_SUPPORT_SOURCES
    "${CMAKE_CURRENT_LIST_DIR}/ps5-posix.c"
    "${CMAKE_CURRENT_LIST_DIR}/ps5-support.c")
set(PS5_WRAPS
    "-Wl,--wrap=getenv" "-Wl,--wrap=setenv" "-Wl,--wrap=putenv"
    "-Wl,--wrap=unsetenv" "-Wl,--wrap=getcwd" "-Wl,--wrap=chdir"
    "-Wl,--wrap=access" "-Wl,--wrap=realpath" "-Wl,--wrap=readlink"
    "-Wl,--wrap=pathconf" "-Wl,--wrap=system"
    "-Wl,--wrap=opendir" "-Wl,--wrap=readdir" "-Wl,--wrap=closedir"
    "-Wl,--wrap=sceKernelDebugOutText" "-Wl,--wrap=_Exit"
    "-Wl,--wrap=abort" "-Wl,--wrap=__assert"
    "-Wl,--wrap=open" "-Wl,--wrap=_open" "-Wl,--wrap=ioctl"
    "-Wl,--wrap=printf" "-Wl,--wrap=vprintf" "-Wl,--wrap=puts"
    "-Wl,--wrap=putchar"
    "-Wl,--wrap=pipe" "-Wl,--wrap=socketpair" "-Wl,--wrap=socket")
string(REPLACE ";" " " PS5_WRAPS_STR "${PS5_WRAPS}")
set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} ${PS5_WRAPS_STR}")
# What the engine and our layer call as pthread_create and fflush goes to ours first: CMakeLists.txt's
# JSRF_PS5_VULKAN branch renames the two for every source it compiles.

set(CMAKE_C_LINK_EXECUTABLE "bash ${CMAKE_CURRENT_LIST_DIR}/link-vk.sh <TARGET> <LINK_FLAGS> <OBJECTS> <LINK_LIBRARIES>")
set(CMAKE_CXX_LINK_EXECUTABLE "${CMAKE_C_LINK_EXECUTABLE}")
set(PS5_FINAL_LINK_FLAGS "")

set(CMAKE_THREAD_LIBS_INIT "")
set(CMAKE_HAVE_THREADS_LIBRARY 1)
set(CMAKE_USE_WIN32_THREADS_INIT 0)
set(CMAKE_USE_PTHREADS_INIT 1)
set(THREADS_PREFER_PTHREAD_FLAG ON)
if(NOT TARGET Threads::Threads)
    add_library(Threads::Threads INTERFACE IMPORTED GLOBAL)
endif()
