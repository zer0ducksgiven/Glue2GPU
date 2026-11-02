# rexgpu-regpu: ReGPU as a ReXGlue GPU plugin (loaded by --gpu_plugin=regpu).
# Configure with CMAKE_PREFIX_PATH at an installed ReXGlue/ReIGlue SDK, built with the same
# compiler (ReIGlue's win-amd64 preset: clang/clang++).
cmake_minimum_required(VERSION 3.25)
project(rexgpu_regpu CXX)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(GLUE2GPU_REXGLUE "${CMAKE_CURRENT_SOURCE_DIR}/../thirdparty/ReIGlue" CACHE PATH "ReIGlue / ReXGlue source (headers, SIMDe)")
set(GLUE2GPU_REGPU "${CMAKE_CURRENT_SOURCE_DIR}/../thirdparty/ReGPU" CACHE PATH "ReGPU checkout (rexglue-driver branch)")

find_package(rexglue CONFIG REQUIRED)

# Profile-guided build (ReGPU and the plugin): GLUE2GPU_PGO=generate builds the counting DLL (it
# writes its profile itself, see regpu_graphics_system.cpp; LLVM_PROFILE_FILE names the file), then
# GLUE2GPU_PGO=<merged .profdata> builds the optimised one. Empty: an ordinary build.
# pgo/drive.profdata: the replayed free-roam drive, 70-130 s (2026-11-02): FH1 99.3 -> 105.5 fps, two
# alternating pairs, lows 93.5 -> 99. Built against older sources it still applies where it matches
# (out-of-date warnings are off); record it again after large changes to the draw path.
set(GLUE2GPU_PGO "" CACHE STRING "Profile-guided build: generate, or a .profdata path to use")
if(GLUE2GPU_PGO STREQUAL "generate")
    add_compile_options(-fprofile-instr-generate)
    add_link_options(-fprofile-instr-generate)
    add_compile_definitions(GLUE2GPU_PGO_GENERATE)
elseif(GLUE2GPU_PGO)
    add_compile_options("-fprofile-instr-use=${GLUE2GPU_PGO}" -Wno-profile-instr-unprofiled -Wno-profile-instr-out-of-date)
endif()

# ReGPU without its Vd* exports, against ReXGlue's PPCContext (regpu::driver).
set(REGPU_GUEST_ABI rexglue)
set(REGPU_REXGLUE_DIR "${GLUE2GPU_REXGLUE}")
add_subdirectory("${GLUE2GPU_REGPU}" regpu)

# ReXGlue's loader looks for rexgpu-<name> beside the executable.
add_library(rexgpu-regpu SHARED
    src/fh1_boot_screen.cpp
    src/fh1_shaders.cpp
    src/plugin_main.cpp
    src/regpu_graphics_system.cpp
    src/stack_dump.cpp)
target_link_libraries(rexgpu-regpu PRIVATE regpu rex::runtime dbghelp shell32)
# ReGPU's microcode and texture decoders (the shader search, the boot screen's art).
target_include_directories(rexgpu-regpu PRIVATE "${GLUE2GPU_REGPU}/src")
