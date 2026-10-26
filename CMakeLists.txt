# JSRF Unleashed for PS5: upstream's port/ (src-upstream, patched by patches/jsrf.patch) built for a PS5 title, with
# the PS5 layer in ps5/ (decision log D1, D2). build.sh runs this with the payload SDK's toolchain and ps5/ps5-gl.cmake.
cmake_minimum_required(VERSION 3.20)
project(jsrf_ps5 C CXX)

# The renderer's window and context come through SDL; SDL's GL calls go to one EGL 3.3 core context (ps5/jsrf_ps5_gl.c).
set(NV2A_GL_CONTEXT "sdl" CACHE STRING "" FORCE)
set(JSRF_PS5 ON)
add_compile_definitions(JSRF_PS5=1)
add_compile_options(-Wno-unused-command-line-argument)
# The guest's registers in one thread-local block (src-upstream/src/platform/recomp_regfile.h, decision log D46):
# before every source's own includes, so recomp_types.h and xbox_memory_layout.h see RECOMP_REGFILE.
add_compile_options("SHELL:-include ${CMAKE_CURRENT_SOURCE_DIR}/src-upstream/src/platform/recomp_regfile.h")
if(JSRF_PS5_VULKAN)
  # Mihawk's RADV instead of the OpenGL SDK (ps5/ps5-vk.cmake, decision log D34): upstream's renderer without its
  # OpenGL device, and our pthread_create and fflush ahead of the platform layer's wraps of the same names.
  add_compile_definitions(JSRF_PS5_RADV=1 pthread_create=jsrf_pthread_create fflush=jsrf_fflush)
  set(NV2A_GL_CONTEXT "none" CACHE STRING "" FORCE)
  # The Vulkan device (src/nv2a_vk): Vulkan's headers from Mihawk's Mesa, glslang's from the source built for the
  # PS5 in build-vk/glslang (the phase 0 spike's, D32; ps5/link-vk.sh links it).
  set(NV2A_VK_INCLUDE_DIRS "${CMAKE_SOURCE_DIR}/../rush2-ps5/radv/PS5_Mesa/include;${CMAKE_SOURCE_DIR}/.deps/glslang;${CMAKE_SOURCE_DIR}/build-vk/glslang/include" CACHE STRING "" FORCE)
endif()

# pthreads are in the PS5's libc and libkernel: FindThreads (src/platform) is told so.
set(CMAKE_HAVE_LIBC_PTHREAD TRUE CACHE BOOL "" FORCE)
set(THREADS_PREFER_PTHREAD_FLAG OFF)

add_subdirectory(src-upstream/port "${CMAKE_BINARY_DIR}/port")

if(JSRF_PS5_VULKAN)
  target_sources(jsrf_recomp PRIVATE ps5/jsrf_ps5_vk.c ps5/jsrf_ps5_main.c ps5/jsrf_ps5_menu.c ps5/jsrf_ps5_screen.c ${PROSPERO_SUPPORT_SOURCES})
else()
target_sources(jsrf_recomp PRIVATE ps5/jsrf_ps5_gl.c ps5/jsrf_ps5_main.c ps5/jsrf_ps5_screen.c ps5/jsrf_ps5_menu.c
  ${PROSPERO_SUPPORT_SOURCES})
endif()
target_include_directories(jsrf_recomp PRIVATE src-upstream/port)
target_include_directories(jsrf_recomp PRIVATE ps5)
if(JSRF_PS5_VULKAN)
  target_link_libraries(jsrf_recomp PRIVATE SDL2::SDL2-static m)
  # a title's lstat refuses every path; stat does not (ps5/ps5-posix.c, D28)
  target_link_options(jsrf_recomp PRIVATE -Wl,--wrap=lstat)
  return()
endif()
target_link_libraries(jsrf_recomp PRIVATE SDL2::SDL2-static OpenGL::GL m)
set(PS5_GL_WRAPS
  -Wl,--wrap=SDL_CreateWindow -Wl,--wrap=SDL_GL_CreateContext -Wl,--wrap=SDL_GL_DeleteContext
  -Wl,--wrap=SDL_GL_MakeCurrent -Wl,--wrap=SDL_GL_SwapWindow -Wl,--wrap=SDL_GL_GetDrawableSize
  -Wl,--wrap=SDL_GL_SetAttribute -Wl,--wrap=SDL_GL_SetSwapInterval)
target_link_options(jsrf_recomp PRIVATE ${PS5_GL_WRAPS}
  # the log written in blocks: stderr flushes at most once a second (ps5/ps5-posix.c, D7)
  -Wl,--wrap=fflush
  # a title's lstat refuses every path; stat does not (ps5/ps5-posix.c, D28)
  -Wl,--wrap=lstat
  # a present's time, measured (ps5/jsrf_ps5_gl.c, D8)
  -Wl,--wrap=SDL_PollEvent)
set_property(TARGET jsrf_recomp APPEND_STRING PROPERTY LINK_FLAGS " ${PS5_FINAL_LINK_FLAGS}")
