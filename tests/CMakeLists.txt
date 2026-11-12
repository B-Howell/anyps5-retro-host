add_library(host_fake_core SHARED host_fake_core.cpp)
set_target_properties(host_fake_core PROPERTIES PREFIX "" OUTPUT_NAME "pcsx2_libretro"
  LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/frames"
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/frames"
  ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/frames")
foreach(config Debug Release RelWithDebInfo MinSizeRel)
  string(TOUPPER ${config} upper)
  set_target_properties(host_fake_core PROPERTIES RUNTIME_OUTPUT_DIRECTORY_${upper} "${CMAKE_CURRENT_BINARY_DIR}/frames"
    LIBRARY_OUTPUT_DIRECTORY_${upper} "${CMAKE_CURRENT_BINARY_DIR}/frames")
endforeach()

add_library(host_fake_ps1_core SHARED host_fake_core.cpp)
set_target_properties(host_fake_ps1_core PROPERTIES PREFIX "" OUTPUT_NAME "swanstation_libretro"
  LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/ps1"
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/ps1"
  ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/ps1")
foreach(config Debug Release RelWithDebInfo MinSizeRel)
  string(TOUPPER ${config} upper)
  set_target_properties(host_fake_ps1_core PROPERTIES RUNTIME_OUTPUT_DIRECTORY_${upper} "${CMAKE_CURRENT_BINARY_DIR}/ps1"
    LIBRARY_OUTPUT_DIRECTORY_${upper} "${CMAKE_CURRENT_BINARY_DIR}/ps1")
endforeach()

add_library(host_checkpoint_core SHARED ../src/core/test/fake_core.cpp)
target_compile_definitions(host_checkpoint_core PRIVATE TEST_CHECKPOINT=1)
set_target_properties(host_checkpoint_core PROPERTIES PREFIX "" OUTPUT_NAME "swanstation_libretro"
  LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/checkpoint"
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/checkpoint"
  ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/checkpoint")
foreach(config Debug Release RelWithDebInfo MinSizeRel)
  string(TOUPPER ${config} upper)
  set_target_properties(host_checkpoint_core PROPERTIES RUNTIME_OUTPUT_DIRECTORY_${upper} "${CMAKE_CURRENT_BINARY_DIR}/checkpoint"
    LIBRARY_OUTPUT_DIRECTORY_${upper} "${CMAKE_CURRENT_BINARY_DIR}/checkpoint")
endforeach()

add_executable(host_protocol_test host_protocol_test.cpp)
target_link_libraries(host_protocol_test PRIVATE anyps5_retro_ipc)
target_compile_features(host_protocol_test PRIVATE cxx_std_17)
add_test(NAME host_protocol COMMAND host_protocol_test "$<TARGET_FILE:anyps5-retro-host>"
  "$<TARGET_FILE:host_fake_core>" "$<TARGET_FILE:host_checkpoint_core>" "$<TARGET_FILE:host_fake_ps1_core>")

add_executable(ps1_host_probe ps1_host_probe.cpp)
target_link_libraries(ps1_host_probe PRIVATE anyps5_retro_ipc)
target_compile_features(ps1_host_probe PRIVATE cxx_std_17)

add_executable(ps3_package_batch_test ps3_package_batch_test.cpp)
target_include_directories(ps3_package_batch_test PRIVATE ../src/windows)
target_compile_features(ps3_package_batch_test PRIVATE cxx_std_17)
if(MSVC)
  target_compile_options(ps3_package_batch_test PRIVATE /UNDEBUG)
else()
  target_compile_options(ps3_package_batch_test PRIVATE -UNDEBUG)
endif()
add_test(NAME ps3_package_batch COMMAND ps3_package_batch_test)

add_executable(host_boot_check host_boot_check.cpp)
target_link_libraries(host_boot_check PRIVATE anyps5_retro_ipc)
target_compile_features(host_boot_check PRIVATE cxx_std_17)
