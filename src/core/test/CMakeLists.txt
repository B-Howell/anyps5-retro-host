cmake_minimum_required(VERSION 3.13)
project(anyps5_retro_test LANGUAGES CXX)

enable_testing()
add_subdirectory(.. retro)

add_library(retro_fake_core SHARED fake_core.cpp)
add_executable(retro_native_test retro_native_test.cpp)
target_link_libraries(retro_native_test PRIVATE anyps5_retro)
add_test(NAME retro_native COMMAND retro_native_test "$<TARGET_FILE:retro_fake_core>")

# The same production input timeline and virtual NIC are compiled on both OSes.
foreach(name ps1_netplay_timeline ps2_virtual_lan)
  add_executable(${name}_test ${name}_test.cpp)
  target_compile_features(${name}_test PRIVATE cxx_std_17)
  if(MSVC)
    target_compile_options(${name}_test PRIVATE /UNDEBUG)
  else()
    target_compile_options(${name}_test PRIVATE -UNDEBUG)
  endif()
  add_test(NAME ${name} COMMAND ${name}_test)
endforeach()

add_executable(ps1_checkpoint_test ps1_checkpoint_test.cpp)
target_link_libraries(ps1_checkpoint_test PRIVATE anyps5_retro)
add_test(NAME ps1_checkpoint COMMAND ps1_checkpoint_test)

add_library(retro_fake_checkpoint_core SHARED fake_core.cpp)
target_compile_definitions(retro_fake_checkpoint_core PRIVATE TEST_CHECKPOINT=1)
set_target_properties(retro_fake_checkpoint_core PROPERTIES PREFIX "" OUTPUT_NAME "swanstation_libretro"
  LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/checkpoint"
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/checkpoint"
  ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/checkpoint")
add_executable(retro_ps1_checkpoint_test retro_ps1_checkpoint_test.cpp)
target_link_libraries(retro_ps1_checkpoint_test PRIVATE anyps5_retro)
add_library(retro_fake_plain_ps1_core SHARED fake_core.cpp)
set_target_properties(retro_fake_plain_ps1_core PROPERTIES PREFIX "" OUTPUT_NAME "swanstation_libretro"
  LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/no_checkpoint"
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/no_checkpoint"
  ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/no_checkpoint")
add_test(NAME retro_ps1_checkpoint COMMAND retro_ps1_checkpoint_test
  "$<TARGET_FILE:retro_fake_checkpoint_core>" "$<TARGET_FILE:retro_fake_plain_ps1_core>")
add_executable(retro_ps1_match_test retro_ps1_match_test.cpp)
target_link_libraries(retro_ps1_match_test PRIVATE anyps5_retro)
add_test(NAME retro_ps1_match COMMAND retro_ps1_match_test "$<TARGET_FILE:retro_fake_checkpoint_core>")

add_library(retro_fake_lan_core SHARED fake_core.cpp)
target_compile_definitions(retro_fake_lan_core PRIVATE TEST_LAN=1)
add_library(retro_fake_no_lan_core SHARED fake_core.cpp)
foreach(kind lan no_lan)
  set_target_properties(retro_fake_${kind}_core PROPERTIES
    PREFIX "" OUTPUT_NAME "pcsx2_libretro"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/${kind}"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/${kind}"
    ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/${kind}")
endforeach()
add_executable(retro_lan_test retro_lan_test.cpp)
target_link_libraries(retro_lan_test PRIVATE anyps5_retro)
add_test(NAME retro_lan_session COMMAND retro_lan_test
  "$<TARGET_FILE:retro_fake_lan_core>" "$<TARGET_FILE:retro_fake_no_lan_core>")

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  add_executable(ps1_match_probe ps1_match_probe.cpp)
  target_link_libraries(ps1_match_probe PRIVATE anyps5_retro ${CMAKE_DL_LIBS})
endif()
