cmake_minimum_required(VERSION 3.13)
project(anyps5_retro_ipc LANGUAGES CXX)

find_package(Threads REQUIRED)
add_library(anyps5_retro_ipc STATIC
  retro_ipc_json.cpp
  retro_ipc_transport.cpp
  retro_ipc_frames.cpp
  retro_host_proxy.cpp
)
target_compile_features(anyps5_retro_ipc PUBLIC cxx_std_17)
target_include_directories(anyps5_retro_ipc PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(anyps5_retro_ipc PUBLIC Threads::Threads)
set_target_properties(anyps5_retro_ipc PROPERTIES POSITION_INDEPENDENT_CODE ON)
if(WIN32)
  target_compile_definitions(anyps5_retro_ipc PRIVATE NOMINMAX)
endif()
