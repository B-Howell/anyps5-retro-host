# SPDX-License-Identifier: GPL-2.0-or-later
# SPDX-FileCopyrightText: 2026 AnyPS5 Desktop contributors

cmake_minimum_required(VERSION 3.20)
project(anyps5_retro_host VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(ANYPS5_RETRO_HOST_TESTS "Build the fake core used by the conformance tests" ON)

find_package(Threads REQUIRED)

add_library(retro_host_core STATIC
  src/audio.cpp
  src/channel.cpp
  src/game.cpp
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
  add_executable(resolution_test tests/resolution_test.cpp)
  target_link_libraries(resolution_test PRIVATE retro_host_core)
  add_test(NAME resolution COMMAND resolution_test)
  add_executable(gpu_thread_test tests/gpu_thread_test.cpp)
  target_link_libraries(gpu_thread_test PRIVATE retro_host_core)
  add_test(NAME gpu_thread COMMAND gpu_thread_test)
  set_tests_properties(gpu_thread PROPERTIES SKIP_RETURN_CODE 77)
endif()
