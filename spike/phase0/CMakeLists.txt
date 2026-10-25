# JSRF for PS5 - phase 0: what JSRF Unleashed's runtime needs from the PS5, as a test title (decision log D1).
cmake_minimum_required(VERSION 3.16)
project(jsrf_phase0 C CXX)

add_executable(jsrf_phase0 main.c screen.c ${PROSPERO_SUPPORT_SOURCES})
target_compile_options(jsrf_phase0 PRIVATE $<$<COMPILE_LANGUAGE:C>:-std=gnu11>)
target_link_libraries(jsrf_phase0 PRIVATE OpenGL::GL)
set_property(TARGET jsrf_phase0 APPEND_STRING PROPERTY LINK_FLAGS " ${PS5_FINAL_LINK_FLAGS}")
