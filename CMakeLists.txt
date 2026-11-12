cmake_minimum_required(VERSION 3.18)
project(anyps5_retro_host LANGUAGES CXX)

option(ANYPS5_RETRO_HOST_TESTS "Build the retro host tests" OFF)
if(NOT DEFINED ANYPS5_RETRO_VULKAN)
  set(ANYPS5_RETRO_VULKAN ON)
endif()

add_subdirectory(protocol)

set(HOST_SOURCES src/host_main.cpp src/host_server.cpp)

if(WIN32)
  include(FetchContent)
  if(POLICY CMP0135)
    cmake_policy(SET CMP0135 NEW)
  endif()
  FetchContent_Declare(vulkan_headers
    URL https://codeload.github.com/KhronosGroup/Vulkan-Headers/tar.gz/2fa203425eb4af9dfc6b03f97ef72b0b5bcb8350
    URL_HASH SHA256=66e0dc7932203def85d94bf65a4fa3a3b29e61366713404f0465cdeb02e6ffd3
    SOURCE_SUBDIR include
  )
  FetchContent_MakeAvailable(vulkan_headers)
  add_executable(anyps5-retro-host ${HOST_SOURCES} src/windows/windows_backend.cpp src/windows/retro_vulkan_video.cpp)
  target_include_directories(anyps5-retro-host PRIVATE src src/core src/windows "${vulkan_headers_SOURCE_DIR}/include")
  target_compile_definitions(anyps5-retro-host PRIVATE NOMINMAX UNICODE _UNICODE)
  target_compile_options(anyps5-retro-host PRIVATE /W4 /WX /wd4100 /EHsc /utf-8)
  target_link_libraries(anyps5-retro-host PRIVATE anyps5_retro_ipc dxgi d3d11 d3dcompiler winmm)
  # Cores run on worker threads created with the default stack size.
  target_link_options(anyps5-retro-host PRIVATE "/STACK:8388608")
else()
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(RETRO_RENDER REQUIRED IMPORTED_TARGET egl epoxy)
  pkg_check_modules(RETRO_AUDIO REQUIRED IMPORTED_TARGET libpulse)
  add_subdirectory(src/core)
  add_executable(anyps5-retro-host ${HOST_SOURCES} src/linux/linux_backend.cc src/linux/retro_linux_audio.cc
    src/linux/retro_linux_video.cc)
  target_include_directories(anyps5-retro-host PRIVATE src src/linux)
  target_compile_options(anyps5-retro-host PRIVATE -Wall -Wextra -Werror)
  target_link_libraries(anyps5-retro-host PRIVATE anyps5_retro_ipc PkgConfig::RETRO_RENDER PkgConfig::RETRO_AUDIO
    ${CMAKE_DL_LIBS})
  if(ANYPS5_RETRO_VULKAN)
    target_link_libraries(anyps5-retro-host PRIVATE anyps5_retro_vulkan)
    target_compile_definitions(anyps5-retro-host PRIVATE ANYPS5_RETRO_VULKAN=1)
  else()
    target_link_libraries(anyps5-retro-host PRIVATE anyps5_retro)
  endif()
endif()
target_compile_features(anyps5-retro-host PRIVATE cxx_std_17)
install(TARGETS anyps5-retro-host RUNTIME DESTINATION .)
install(FILES LICENSE README.md DESTINATION .)

if(ANYPS5_RETRO_HOST_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
