# JSRF Unleashed for PS5: upstream's port/ (src-upstream, patched by patches/jsrf.patch) built for a PS5 title, with
# the PS5 layer in ps5/ (decision log D1, D2). build.sh runs this with the payload SDK's toolchain and ps5/ps5-gl.cmake.
cmake_minimum_required(VERSION 3.20)
project(jsrf_ps5 C CXX)

# The renderer's window and context come through SDL; SDL's GL calls go to one EGL 3.3 core context (ps5/jsrf_ps5_gl.c).
set(NV2A_GL_CONTEXT "sdl" CACHE STRING "" FORCE)
set(JSRF_PS5 ON)
add_compile_definitions(JSRF_PS5=1)
add_compile_options(-Wno-unused-command-line-argument)

# pthreads are in the PS5's libc and libkernel: FindThreads (src/platform) is told so.
set(CMAKE_HAVE_LIBC_PTHREAD TRUE CACHE BOOL "" FORCE)
set(THREADS_PREFER_PTHREAD_FLAG OFF)

add_subdirectory(src-upstream/port "${CMAKE_BINARY_DIR}/port")

target_sources(jsrf_recomp PRIVATE ps5/jsrf_ps5_gl.c ps5/jsrf_ps5_main.c ps5/jsrf_ps5_screen.c
  ${PROSPERO_SUPPORT_SOURCES})
target_include_directories(jsrf_recomp PRIVATE src-upstream/port)
target_include_directories(jsrf_recomp PRIVATE ps5)
target_link_libraries(jsrf_recomp PRIVATE SDL2::SDL2-static OpenGL::GL m)
set(PS5_GL_WRAPS
  -Wl,--wrap=SDL_CreateWindow -Wl,--wrap=SDL_GL_CreateContext -Wl,--wrap=SDL_GL_DeleteContext
  -Wl,--wrap=SDL_GL_MakeCurrent -Wl,--wrap=SDL_GL_SwapWindow -Wl,--wrap=SDL_GL_GetDrawableSize
  -Wl,--wrap=SDL_GL_SetAttribute -Wl,--wrap=SDL_GL_SetSwapInterval)
target_link_options(jsrf_recomp PRIVATE ${PS5_GL_WRAPS}
  # the log written in blocks: stderr flushes at most once a second (ps5/ps5-posix.c, D7)
  -Wl,--wrap=fflush
  # a present's time, measured (ps5/jsrf_ps5_gl.c, D8)
  -Wl,--wrap=SDL_PollEvent)
set_property(TARGET jsrf_recomp APPEND_STRING PROPERTY LINK_FLAGS " ${PS5_FINAL_LINK_FLAGS}")
