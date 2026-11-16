# SPDX-License-Identifier: GPL-2.0-or-later
# SPDX-FileCopyrightText: 2026 AnyPS5 Desktop contributors

cmake_minimum_required(VERSION 3.20)
project(anyps5_retro_host VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(ANYPS5_RETRO_HOST_TESTS "Build the fake core used by the conformance tests" ON)
option(ANYPS5_RETRO_HOST_LRPS2 "Build LRPS2, the PS2 core, from the cores/lrps2 submodule with the host's patches" OFF)
set(ANYPS5_RETRO_HOST_LRPS2_JOBS "" CACHE STRING "How many compilers LRPS2's build runs at once; empty for its tool's default")

find_package(Threads REQUIRED)

add_library(retro_host_core STATIC
  src/audio.cpp
  src/channel.cpp
  src/game.cpp
  src/hash.cpp
  src/host.cpp
  src/json.cpp
  src/paths.cpp
  src/resolution.cpp
  src/ring.cpp
)
target_sources(retro_host_core PRIVATE src/gpu.cpp)
if(WIN32)
  target_sources(retro_host_core PRIVATE src/platform_windows.cpp src/gpu_wgl.cpp)
  target_link_libraries(retro_host_core PUBLIC opengl32 gdi32 user32)
elseif(APPLE)
  target_sources(retro_host_core PRIVATE src/platform_posix.cpp src/gpu_cgl.cpp)
  target_link_libraries(retro_host_core PUBLIC "-framework OpenGL")
else()
  target_sources(retro_host_core PRIVATE src/platform_posix.cpp src/gpu_egl.cpp)
endif()
target_include_directories(retro_host_core PUBLIC src
  PRIVATE 3rdparty/libretro-common/include 3rdparty/miniaudio)
target_include_directories(retro_host_core SYSTEM PRIVATE 3rdparty/xxHash)
target_compile_definitions(retro_host_core PRIVATE ANYPS5_RETRO_HOST_VERSION="${PROJECT_VERSION}")
target_link_libraries(retro_host_core PUBLIC Threads::Threads ${CMAKE_DL_LIBS})
if(WIN32)
  target_compile_definitions(retro_host_core PUBLIC NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
endif()
if(APPLE)
  target_link_libraries(retro_host_core PUBLIC "-framework CoreFoundation" "-framework CoreAudio" "-framework AudioToolbox")
endif()

if(MSVC)
  target_compile_options(retro_host_core PRIVATE /W4 /WX /utf-8)
  set_source_files_properties(src/audio.cpp PROPERTIES COMPILE_OPTIONS "/W0")
else()
  target_compile_options(retro_host_core PRIVATE -Wall -Wextra -Werror)
  set_source_files_properties(src/audio.cpp PROPERTIES COMPILE_OPTIONS "-w")
endif()

add_executable(anyps5-retro-host src/main.cpp)
target_link_libraries(anyps5-retro-host PRIVATE retro_host_core)
install(TARGETS anyps5-retro-host RUNTIME DESTINATION .)

if(ANYPS5_RETRO_HOST_TESTS)
  add_library(fake_core MODULE tests/fake_core.c)
  target_include_directories(fake_core PRIVATE 3rdparty/libretro-common/include)
  set_target_properties(fake_core PROPERTIES PREFIX "")
  add_library(ring_access MODULE tests/ring_access.c)
  set_target_properties(ring_access PROPERTIES PREFIX "")
  if(NOT MSVC)
    target_compile_options(ring_access PRIVATE -fno-sanitize=all)
    target_link_options(ring_access PRIVATE -fno-sanitize=all)
  endif()
  add_executable(json_test tests/json_test.cpp)
  target_link_libraries(json_test PRIVATE retro_host_core)
  enable_testing()
  add_test(NAME json COMMAND json_test)
  add_executable(hash_test tests/hash_test.cpp)
  target_link_libraries(hash_test PRIVATE retro_host_core)
  add_test(NAME hash COMMAND hash_test)
  add_executable(resolution_test tests/resolution_test.cpp)
  target_link_libraries(resolution_test PRIVATE retro_host_core)
  add_test(NAME resolution COMMAND resolution_test)
  add_executable(gpu_thread_test tests/gpu_thread_test.cpp)
  target_link_libraries(gpu_thread_test PRIVATE retro_host_core)
  add_test(NAME gpu_thread COMMAND gpu_thread_test)
  set_tests_properties(gpu_thread PROPERTIES SKIP_RETURN_CODE 77)
endif()

# LRPS2 (docs/ps2.md "The core"): built from source in its own CMake build, from a patched copy of
# the cores/lrps2 submodule that cores/prepare-lrps2.cmake makes and refreshes before each build.
# The options are the ones the core build identity names (cores/patches/lrps2-1-core-build-identity.patch,
# tools/core_identity.py), so every system's build reports the same identity. The library is
# copied to cores/ in this build folder.
if(ANYPS5_RETRO_HOST_LRPS2)
  include(ExternalProject)
  set(lrps2_source "${CMAKE_BINARY_DIR}/lrps2-source")
  set(lrps2_build "${CMAKE_BINARY_DIR}/lrps2-build")
  set(lrps2_prepare "${CMAKE_COMMAND}" "-DSUBMODULE=${CMAKE_SOURCE_DIR}/cores/lrps2"
      "-DPATCHES=${CMAKE_SOURCE_DIR}/cores/patches" "-DOUT=${lrps2_source}"
      -P "${CMAKE_SOURCE_DIR}/cores/prepare-lrps2.cmake")
  get_property(multi_config GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
  if(multi_config)
    set(lrps2_config_folder "/Release")
  else()
    set(lrps2_config_folder "")
  endif()
  if(WIN32)
    set(lrps2_library "${lrps2_build}/libretro${lrps2_config_folder}/pcsx2_libretro.dll")
  elseif(APPLE)
    set(lrps2_library "${lrps2_build}/libretro${lrps2_config_folder}/pcsx2_libretro.dylib")
  else()
    set(lrps2_library "${lrps2_build}/bin/pcsx2_libretro.so")
  endif()
  set(lrps2_arguments -DCMAKE_BUILD_TYPE=Release -DCMAKE_POLICY_VERSION_MINIMUM=3.18 -DC89_EMITTER=ON
      -DDISABLE_ADVANCE_SIMD=ON -DUSE_OPENGL=ON -DUSE_VULKAN=ON)
  if(NOT multi_config)
    list(APPEND lrps2_arguments "-DCMAKE_C_COMPILER=${CMAKE_C_COMPILER}" "-DCMAKE_CXX_COMPILER=${CMAKE_CXX_COMPILER}")
  endif()
  if(CMAKE_OSX_ARCHITECTURES)
    list(APPEND lrps2_arguments "-DCMAKE_OSX_ARCHITECTURES=${CMAKE_OSX_ARCHITECTURES}")
  endif()
  set(lrps2_jobs --parallel)
  if(ANYPS5_RETRO_HOST_LRPS2_JOBS)
    list(APPEND lrps2_jobs "${ANYPS5_RETRO_HOST_LRPS2_JOBS}")
  endif()
  ExternalProject_Add(lrps2
    SOURCE_DIR "${lrps2_source}"
    BINARY_DIR "${lrps2_build}"
    DOWNLOAD_COMMAND ${lrps2_prepare}
    UPDATE_COMMAND ${lrps2_prepare}
    CMAKE_ARGS ${lrps2_arguments}
    BUILD_COMMAND "${CMAKE_COMMAND}" --build "${lrps2_build}" --config Release --target pcsx2_libretro ${lrps2_jobs}
    INSTALL_COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_BINARY_DIR}/cores"
            COMMAND "${CMAKE_COMMAND}" -E copy "${lrps2_library}" "${CMAKE_BINARY_DIR}/cores"
    BUILD_BYPRODUCTS "${lrps2_library}"
    USES_TERMINAL_BUILD ON)
endif()
