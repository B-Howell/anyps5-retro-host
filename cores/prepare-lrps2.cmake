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
# is a clone, so LRPS2's own build still finds its git revision. A copy that is still exactly that
# source is left as it is, so a rebuild only rebuilds what changed.

cmake_minimum_required(VERSION 3.20)

foreach(variable SUBMODULE PATCHES OUT)
  if(NOT DEFINED ${variable})
    message(FATAL_ERROR "prepare-lrps2.cmake needs -D${variable}=")
  endif()
  get_filename_component(${variable} "${${variable}}" ABSOLUTE)
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

# A copy is kept only when it is exactly the patched source: its commit, its index the tree the
# patches make of that commit (worked out again in an index of its own, so an edited index is not
# trusted), its files the index's, and no other file but the manifest. Anything else, such as a file
# edited in the copy, makes it again, so the identity always names what is built.
function(unchanged result)
  set(${result} FALSE PARENT_SCOPE)
  if(NOT EXISTS "${OUT}/anyps5-core-source.txt" OR NOT EXISTS "${OUT}/.git")
    return()
  endif()
  file(READ "${OUT}/anyps5-core-source.txt" existing)
  if(NOT existing STREQUAL manifest)
    return()
  endif()
  execute_process(COMMAND "${GIT}" -C "${OUT}" rev-parse HEAD RESULT_VARIABLE failed
                  OUTPUT_VARIABLE head OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
  if(failed OR NOT head STREQUAL commit)
    return()
  endif()
  set(index "${OUT}/.git/anyps5-expected-index")
  file(REMOVE "${index}")
  set(expected_git "${CMAKE_COMMAND}" -E env "GIT_INDEX_FILE=${index}" "${GIT}" -C "${OUT}")
  execute_process(COMMAND ${expected_git} read-tree "${commit}" RESULT_VARIABLE failed OUTPUT_QUIET ERROR_QUIET)
  if(failed)
    return()
  endif()
  foreach(patch IN LISTS patches)
    execute_process(COMMAND ${expected_git} apply --cached "${patch}" RESULT_VARIABLE failed OUTPUT_QUIET ERROR_QUIET)
    if(failed)
      return()
    endif()
  endforeach()
  execute_process(COMMAND ${expected_git} write-tree RESULT_VARIABLE failed
                  OUTPUT_VARIABLE expected OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
  file(REMOVE "${index}")
  execute_process(COMMAND "${GIT}" -C "${OUT}" write-tree RESULT_VARIABLE failed_actual
                  OUTPUT_VARIABLE actual OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
  if(failed OR failed_actual OR NOT actual STREQUAL expected)
    return()
  endif()
  execute_process(COMMAND "${GIT}" -C "${OUT}" diff --quiet RESULT_VARIABLE differs OUTPUT_QUIET ERROR_QUIET)
  execute_process(COMMAND "${GIT}" -C "${OUT}" ls-files --others RESULT_VARIABLE failed
                  OUTPUT_VARIABLE others OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
  if(differs OR failed OR NOT others STREQUAL "anyps5-core-source.txt")
    return()
  endif()
  set(${result} TRUE PARENT_SCOPE)
endfunction()

unchanged(kept)
if(kept)
  message(STATUS "LRPS2 source in ${OUT} is the patched source of ${commit}")
  return()
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
