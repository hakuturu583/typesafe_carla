# Fetches the CARLA sources needed to build LibCarla's client library.
#
# Only CARLA UE5 (the CMake-based build: ue5-dev, 0.10.x and later) is
# supported. The checkout is shallow, blob-filtered and sparse (CMakeLists.txt,
# CMake/, LibCarla/, LICENSE), so it is a few MB instead of the multi-GB
# repository, and works for branches, tags and commit SHAs alike.
#
# Inputs (cache variables; the environment variables in brackets give defaults):
#   TSC_CARLA_SOURCE_DIR      [CARLA_SOURCE_DIR] Use this local CARLA checkout
#                             instead of fetching.
#   TSC_CARLA_GIT_REPOSITORY  Repository to fetch from.
#   TSC_CARLA_GIT_REF         [CARLA_GIT_REF] Branch, tag or commit SHA to fetch
#                             (default: ue5-dev).
#   TSC_CARLA_REFRESH         Re-fetch even if this ref was fetched before
#                             (to pick up new commits on a branch).
#   TSC_CARLA_REF_NAME        [CARLA_REF_NAME] The ref to record (BUILD_INFO,
#                             libcarla_git_ref(), bindgen's `missing_in`) when
#                             TSC_CARLA_GIT_REF is a commit SHA resolved from
#                             it, e.g. "ue5-dev" or "0.10.0"; also names a
#                             TSC_CARLA_SOURCE_DIR build. Default: the ref
#                             fetched ("local" for TSC_CARLA_SOURCE_DIR).
#
# The cache variables carry a TSC_ prefix because CARLA's own project() call
# defines CARLA_SOURCE_DIR.
#
# Outputs:
#   TSC_CARLA_DIR           Path of the CARLA source tree to add_subdirectory().
#   TSC_CARLA_RESOLVED_REF  The ref recorded: TSC_CARLA_REF_NAME, else the
#                           requested ref ("local" for TSC_CARLA_SOURCE_DIR).
#   TSC_CARLA_COMMIT        Resolved commit SHA ("unknown" if not a git tree).

include_guard(GLOBAL)

set(TSC_CARLA_DEFAULT_REF "ue5-dev")

set(TSC_CARLA_SOURCE_DIR "$ENV{CARLA_SOURCE_DIR}" CACHE PATH
    "Local CARLA (UE5) checkout to build LibCarla from; empty to fetch TSC_CARLA_GIT_REF")
set(TSC_CARLA_GIT_REPOSITORY "https://github.com/carla-simulator/carla.git" CACHE STRING
    "CARLA repository to fetch LibCarla from")
if(DEFINED ENV{CARLA_GIT_REF} AND NOT "$ENV{CARLA_GIT_REF}" STREQUAL "")
  set(_tsc_ref_default "$ENV{CARLA_GIT_REF}")
else()
  set(_tsc_ref_default "${TSC_CARLA_DEFAULT_REF}")
endif()
set(TSC_CARLA_GIT_REF "${_tsc_ref_default}" CACHE STRING
    "CARLA branch, tag or commit SHA to build LibCarla from (UE5 only)")
set(TSC_CARLA_REF_NAME "$ENV{CARLA_REF_NAME}" CACHE STRING
    "CARLA ref to record when TSC_CARLA_GIT_REF is a SHA resolved from it; empty: TSC_CARLA_GIT_REF")

# The recorded ref: TSC_CARLA_REF_NAME if given, else `fallback`.
macro(_tsc_set_resolved_ref fallback)
  if(TSC_CARLA_REF_NAME)
    set(TSC_CARLA_RESOLVED_REF "${TSC_CARLA_REF_NAME}")
  else()
    set(TSC_CARLA_RESOLVED_REF "${fallback}")
  endif()
endmacro()

function(_tsc_git)
  execute_process(
    COMMAND "${GIT_EXECUTABLE}" ${ARGN}
    WORKING_DIRECTORY "${_tsc_carla_dir}"
    RESULT_VARIABLE _result
    OUTPUT_VARIABLE _out
    ERROR_VARIABLE _err
    OUTPUT_STRIP_TRAILING_WHITESPACE)
  if(NOT _result EQUAL 0)
    message(FATAL_ERROR "git ${ARGN} failed in ${_tsc_carla_dir}:\n${_err}")
  endif()
  set(_tsc_git_out "${_out}" PARENT_SCOPE)
endfunction()

function(_tsc_check_ue5 dir)
  if(NOT EXISTS "${dir}/LibCarla/CMakeLists.txt" OR NOT EXISTS "${dir}/CMake/Options.cmake")
    message(FATAL_ERROR
      "${dir} is not a CARLA UE5 source tree (no LibCarla/CMakeLists.txt or CMake/Options.cmake). "
      "typesafe_carla supports only CARLA UE5 (ue5-dev, 0.10.x and later); UE4 refs such as "
      "0.9.x or ue4-dev cannot be used.")
  endif()
endfunction()

find_package(Git QUIET)

if(TSC_CARLA_SOURCE_DIR)
  get_filename_component(TSC_CARLA_DIR "${TSC_CARLA_SOURCE_DIR}" ABSOLUTE)
  _tsc_check_ue5("${TSC_CARLA_DIR}")
  _tsc_set_resolved_ref("local")
  set(TSC_CARLA_COMMIT "unknown")
  if(GIT_FOUND AND EXISTS "${TSC_CARLA_DIR}/.git")
    execute_process(
      COMMAND "${GIT_EXECUTABLE}" rev-parse HEAD
      WORKING_DIRECTORY "${TSC_CARLA_DIR}"
      OUTPUT_VARIABLE TSC_CARLA_COMMIT OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
  endif()
  message(STATUS "typesafe_carla: LibCarla from local tree ${TSC_CARLA_DIR}")
  return()
endif()

if(NOT GIT_FOUND)
  message(FATAL_ERROR "git is required to fetch CARLA (or set TSC_CARLA_SOURCE_DIR)")
endif()

set(_tsc_carla_dir "${CMAKE_BINARY_DIR}/_deps/carla-src")
set(_tsc_stamp "${CMAKE_BINARY_DIR}/_deps/carla-src.ref")
set(_tsc_want "${TSC_CARLA_GIT_REPOSITORY}@${TSC_CARLA_GIT_REF}")

set(_tsc_have "")
if(EXISTS "${_tsc_stamp}" AND EXISTS "${_tsc_carla_dir}/.git")
  file(READ "${_tsc_stamp}" _tsc_have)
endif()

# A ref that is a full SHA is immutable; branches and tags are re-fetched only
# when the requested ref changes (pass -DTSC_CARLA_REFRESH=ON to update a branch).
option(TSC_CARLA_REFRESH "Re-fetch TSC_CARLA_GIT_REF even if it was fetched before" OFF)

if(NOT _tsc_have STREQUAL _tsc_want OR TSC_CARLA_REFRESH)
  message(STATUS "typesafe_carla: fetching CARLA ${TSC_CARLA_GIT_REF} from ${TSC_CARLA_GIT_REPOSITORY}")
  file(REMOVE_RECURSE "${_tsc_carla_dir}")
  file(MAKE_DIRECTORY "${_tsc_carla_dir}")
  _tsc_git(init -q)
  _tsc_git(remote add origin "${TSC_CARLA_GIT_REPOSITORY}")
  _tsc_git(sparse-checkout set --no-cone /CMakeLists.txt /CMake/ /LibCarla/ /LICENSE /.clangd.in)
  _tsc_git(fetch -q --depth 1 --filter=blob:none origin "${TSC_CARLA_GIT_REF}")
  _tsc_git(checkout -q FETCH_HEAD)
  file(WRITE "${_tsc_stamp}" "${_tsc_want}")
  set_property(CACHE TSC_CARLA_REFRESH PROPERTY VALUE OFF)
endif()

_tsc_git(rev-parse HEAD)
set(TSC_CARLA_DIR "${_tsc_carla_dir}")
_tsc_set_resolved_ref("${TSC_CARLA_GIT_REF}")
set(TSC_CARLA_COMMIT "${_tsc_git_out}")
_tsc_check_ue5("${TSC_CARLA_DIR}")
message(STATUS "typesafe_carla: CARLA ${TSC_CARLA_RESOLVED_REF} = ${TSC_CARLA_COMMIT}")
