# Patches to the fetched LibCarla sources (cmake/carla-patches/README.md).
#
#   tsc_carla_patch_id(<out>)
#     Sets <out> to the patch id of cmake/carla-patches/*.patch: the first 12
#     hex digits of the sha256 of "<file name> <sha256 of the file>\n" for each
#     patch in file-name order, or "none" if there is none. tools/carla_patches.py
#     computes the same (CMake strings cannot hold the NUL a byte-level
#     concatenation would need, hence the per-file digests).
#
#   tsc_apply_carla_patches(<dir>)
#     Applies them, in that order, to the CARLA checkout <dir> with
#     `git apply`, skipping one the checkout already has (upstream took the
#     fix: it applies in reverse); fails the configure step naming a patch
#     that does neither.
#
# _TSC_CARLA_PATCH_DIR (the patches' directory) may be set before including
# this file, or changed after, e.g. by tests.

include_guard(GLOBAL)

if(NOT DEFINED _TSC_CARLA_PATCH_DIR)
  set(_TSC_CARLA_PATCH_DIR "${CMAKE_CURRENT_LIST_DIR}/carla-patches")
endif()

# Sets <out> to the patch file names, sorted.
function(_tsc_carla_patch_files out)
  file(GLOB _files LIST_DIRECTORIES false RELATIVE "${_TSC_CARLA_PATCH_DIR}"
       "${_TSC_CARLA_PATCH_DIR}/*.patch")
  list(SORT _files)
  set(${out} "${_files}" PARENT_SCOPE)
endfunction()

function(tsc_carla_patch_id out)
  _tsc_carla_patch_files(_files)
  if(NOT _files)
    set(${out} "none" PARENT_SCOPE)
    return()
  endif()
  set(_text "")
  foreach(_f IN LISTS _files)
    file(SHA256 "${_TSC_CARLA_PATCH_DIR}/${_f}" _h)
    string(APPEND _text "${_f} ${_h}\n")
  endforeach()
  string(SHA256 _id "${_text}")
  string(SUBSTRING "${_id}" 0 12 _id)
  set(${out} "${_id}" PARENT_SCOPE)
endfunction()

function(tsc_apply_carla_patches dir)
  _tsc_carla_patch_files(_files)
  file(REAL_PATH "${dir}" _real)
  foreach(_f IN LISTS _files)
    execute_process(
      COMMAND "${GIT_EXECUTABLE}" -c "safe.directory=${_real}" apply
              "${_TSC_CARLA_PATCH_DIR}/${_f}"
      WORKING_DIRECTORY "${dir}"
      RESULT_VARIABLE _rc
      OUTPUT_VARIABLE _out
      ERROR_VARIABLE _err
      ERROR_STRIP_TRAILING_WHITESPACE)
    if(NOT _rc EQUAL 0)
      execute_process(
        COMMAND "${GIT_EXECUTABLE}" -c "safe.directory=${_real}" apply --reverse --check
                "${_TSC_CARLA_PATCH_DIR}/${_f}"
        WORKING_DIRECTORY "${dir}"
        RESULT_VARIABLE _rc_rev
        OUTPUT_QUIET ERROR_QUIET)
      if(_rc_rev EQUAL 0)
        message(STATUS "typesafe_carla: CARLA ${TSC_CARLA_GIT_REF} already has the LibCarla "
                       "patch ${_f}: skipped")
        continue()
      endif()
      message(FATAL_ERROR
        "typesafe_carla: the LibCarla patch cmake/carla-patches/${_f} does not apply to "
        "CARLA ${TSC_CARLA_GIT_REF} (${dir}):\n${_err}\n"
        "Every patch must apply to every CARLA ref built (cmake/carla-patches/README.md): "
        "update or drop the patch (e.g. once upstream has the fix).")
    endif()
    message(STATUS "typesafe_carla: applied the LibCarla patch ${_f}")
  endforeach()
endfunction()
