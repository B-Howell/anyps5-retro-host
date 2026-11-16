# SPDX-License-Identifier: MIT
# SPDX-FileCopyrightText: 2026 AnyPS5 Desktop contributors

# Makes the LRPS2 source the host builds (docs/ps2.md "The core"): a copy of the cores/lrps2
# submodule's commit in OUT, with the host's LRPS2 patches (cores/patches/lrps2-*.patch) applied
# in the order of their names, and anyps5-core-source.txt written beside them, which the first
# patch turns into the core build identity (docs/netplay.md "Core build identity"). The submodule
# itself is never changed, so a checkout stays clean however often the host is built.
#
#   cmake -DSUBMODULE=<cores/lrps2> -DPATCHES=<cores/patches> -DOUT=<folder> -P prepare-lrps2.cmake
#
# As cores/apply-swanstation-patches.sh does for SwanStation: a submodule with any change or
# untracked file is refused, so the manifest names the source exactly; each patch must apply
# cleanly; and the patches go into git's index, whose files have the repository's LF line
# endings, before the files are checked out, so they also apply where git writes CRLF. The copy
# is a clone, so LRPS2's own build still finds its git revision. A copy whose manifest already
# matches is left as it is, so a rebuild only rebuilds what changed.

cmake_minimum_required(VERSION 3.20)

foreach(variable SUBMODULE PATCHES OUT)
  if(NOT DEFINED ${variable})
    message(FATAL_ERROR "prepare-lrps2.cmake needs -D${variable}=")
  endif()
endforeach()

find_program(GIT git REQUIRED)

function(run)
  execute_process(COMMAND ${ARGN} RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE output)
  if(NOT result EQUAL 0)
    string(REPLACE ";" " " command "${ARGN}")
    message(FATAL_ERROR "${command} failed:\n${output}")
  endif()
endfunction()

if(NOT EXISTS "${SUBMODULE}/.git")
  message(FATAL_ERROR "${SUBMODULE} is not checked out; run git submodule update --init cores/lrps2")
endif()
execute_process(COMMAND "${GIT}" -C "${SUBMODULE}" status --porcelain --untracked-files=normal
                RESULT_VARIABLE result OUTPUT_VARIABLE changes ERROR_VARIABLE changes)
if(NOT result EQUAL 0 OR NOT changes STREQUAL "")
  message(FATAL_ERROR "${SUBMODULE} differs from its commit; the patches and the core build identity "
                      "need a clean checkout:\n${changes}")
endif()
execute_process(COMMAND "${GIT}" -C "${SUBMODULE}" rev-parse HEAD
                RESULT_VARIABLE result OUTPUT_VARIABLE commit OUTPUT_STRIP_TRAILING_WHITESPACE)
if(NOT result EQUAL 0 OR NOT commit MATCHES "^[0-9a-f]+$")
  message(FATAL_ERROR "the commit of ${SUBMODULE} cannot be read")
endif()

file(GLOB patches LIST_DIRECTORIES false "${PATCHES}/lrps2-*.patch")
list(SORT patches)
if(NOT patches)
  message(FATAL_ERROR "no LRPS2 patches in ${PATCHES}")
endif()
set(manifest "anyps5-retro-host core source 1\nlrps2 ${commit}\n")
foreach(patch IN LISTS patches)
  file(SHA256 "${patch}" digest)
  get_filename_component(name "${patch}" NAME)
  string(APPEND manifest "patch ${digest} ${name}\n")
endforeach()

if(EXISTS "${OUT}/anyps5-core-source.txt")
  file(READ "${OUT}/anyps5-core-source.txt" existing)
  if(existing STREQUAL manifest)
    message(STATUS "LRPS2 source in ${OUT} is up to date")
    return()
  endif()
endif()

file(REMOVE_RECURSE "${OUT}")
run("${GIT}" clone --quiet --no-checkout "${SUBMODULE}" "${OUT}")
run("${GIT}" -C "${OUT}" -c advice.detachedHead=false checkout --quiet --detach "${commit}")
foreach(patch IN LISTS patches)
  run("${GIT}" -C "${OUT}" apply --cached --check "${patch}")
  run("${GIT}" -C "${OUT}" apply --cached "${patch}")
  get_filename_component(name "${patch}" NAME)
  message(STATUS "applied ${name}")
endforeach()
run("${GIT}" -C "${OUT}" checkout -- .)
file(WRITE "${OUT}/anyps5-core-source.txt" "${manifest}")
message(STATUS "wrote anyps5-core-source.txt:\n${manifest}")
