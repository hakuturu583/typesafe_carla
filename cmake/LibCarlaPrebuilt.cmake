# LibCarla prebuilt: a relocatable install prefix of LibCarla's client library
# and everything it links statically, built once (by CI) and reused by later
# builds of the shim instead of compiling LibCarla (docs/releasing.md,
# "LibCarla prebuilt").
#
# Producer (source builds):
#   tsc_add_libcarla_prebuilt_target(<dest>)
#     Adds the target `libcarla_prebuilt`, which builds carla-client and its
#     libraries and assembles <dest> from them
#     (cmake/LibCarlaPrebuiltAssemble.cmake).
#
# Consumer (TSC_CARLA_PREBUILT_DIR):
#   tsc_import_libcarla_prebuilt(<prefix> <verified SHA or "">)
#     Checks the prefix's manifest against this build (compiler ABI, CARLA
#     repository and commit) and defines the imported target carla-client.
#     Sets TSC_CARLA_RESOLVED_REF, TSC_CARLA_COMMIT, TSC_CARLA_VERSION,
#     TSC_CARLA_LICENSE_FILE and TSC_CARLA_NOTICES_COMPONENTS_TEXT in the caller.
#
# Prefix layout:
#   prebuilt.json                    manifest: CARLA repository, ref, commit and
#                                    version, ABI fingerprint, sha256 of every file
#   include/                         headers of every include directory carla-client
#                                    passes on (LibCarla, Boost, rpclib, ...), merged
#   lib/*.a                          carla-client and the static libraries it links
#   cmake/libcarla-targets.cmake     the imported targets
#   licenses/LICENSE.CARLA           CARLA's LICENSE
#   licenses/THIRD_PARTY_NOTICES.components
#                                    the notices of what lib/ contains, collected from
#                                    the sources when the prefix was built

include_guard(GLOBAL)

set(_TSC_PB_FORMAT 1)
set(_TSC_PB_GUARD "${CMAKE_CURRENT_LIST_DIR}/../tools/libcarla_cache_guard.sh")
set(_TSC_PB_RESOLVE "${CMAKE_CURRENT_LIST_DIR}/../tools/resolve_carla_ref.sh")
set(_TSC_PB_FETCH "${CMAKE_CURRENT_LIST_DIR}/../tools/fetch_libcarla_prebuilt.py")
set(_TSC_PB_ASSEMBLE "${CMAKE_CURRENT_LIST_DIR}/LibCarlaPrebuiltAssemble.cmake")

# Sets <out> to a command prefix that runs a command with this build's C and
# C++ compilers and flags as CC, CXX, CFLAGS and CXXFLAGS: the inputs of
# `tools/libcarla_cache_guard.sh --abi`.
macro(_tsc_pb_abi_env out)
  set(${out} "${CMAKE_COMMAND}" -E env
             "CC=${CMAKE_C_COMPILER}" "CXX=${CMAKE_CXX_COMPILER}"
             "CFLAGS=${CMAKE_C_FLAGS}" "CXXFLAGS=${CMAKE_CXX_FLAGS}")
endmacro()

# Sets <out> to `tools/libcarla_cache_guard.sh --abi` for this build's C and
# C++ compilers and flags. Fails if it cannot, or with SOFT sets it empty.
function(tsc_abi_fingerprint out)
  _tsc_pb_abi_env(_env)
  execute_process(
    COMMAND ${_env} bash "${_TSC_PB_GUARD}" --abi
    RESULT_VARIABLE _rc OUTPUT_VARIABLE _fp ERROR_VARIABLE _err
    OUTPUT_STRIP_TRAILING_WHITESPACE)
  if(NOT _rc EQUAL 0)
    if("SOFT" IN_LIST ARGN)
      set(${out} "" PARENT_SCOPE)
      return()
    endif()
    message(FATAL_ERROR "typesafe_carla: tools/libcarla_cache_guard.sh --abi failed:\n${_err}")
  endif()
  set(${out} "${_fp}" PARENT_SCOPE)
endfunction()

# Repository URLs compare without a trailing ".git" or "/".
function(_tsc_pb_norm_repo url out)
  string(REGEX REPLACE "/+$" "" _u "${url}/")
  string(REGEX REPLACE "\\.git$" "" _u "${_u}")
  set(${out} "${_u}" PARENT_SCOPE)
endfunction()

# ---------------------------------------------------------------------------
# Producer
# ---------------------------------------------------------------------------

# Link items that are system libraries: kept as they are in the prefix.
set(_TSC_PB_SYSTEM_LINKS "^(Threads::Threads|m|dl|rt|pthread|-pthread|-lm|-ldl|-lrt|-lpthread)$")

# Walks carla-client's INTERFACE_LINK_LIBRARIES (for a static library, every
# library it links) and sets, in the caller:
#   _tsc_pb_targets            canonical names (alias resolved), carla-client first
#   _tsc_pb_type_<i>           STATIC_LIBRARY or INTERFACE_LIBRARY
#   _tsc_pb_deps_<i>           its link items: canonical target names, system
#                              libraries, or FILE:<path> of a static library
#                              this build makes
# Anything the prefix could not reproduce (a shared or object library, an
# absolute path, an unknown generator expression) fails the configure step.
function(_tsc_pb_walk)
  # Sets _tsc_pb_error to the message (the arguments, concatenated) and returns.
  macro(_tsc_pb_fail)
    string(CONCAT _e "typesafe_carla: " ${ARGN})
    set(_tsc_pb_error "${_e}" PARENT_SCOPE)
    return()
  endmacro()

  set(_tsc_pb_error "" PARENT_SCOPE)
  set(_todo carla-client)
  set(_targets "")
  set(_i 0)
  while(_todo)
    list(POP_FRONT _todo _t)
    if(_t IN_LIST _targets)
      continue()
    endif()
    list(APPEND _targets "${_t}")
    get_target_property(_type "${_t}" TYPE)
    if(NOT _type MATCHES "^(STATIC_LIBRARY|INTERFACE_LIBRARY)$")
      _tsc_pb_fail("carla-client links ${_t}, a ${_type}; "
                   "the LibCarla prebuilt only supports static and interface libraries")
    endif()
    get_target_property(_links "${_t}" INTERFACE_LINK_LIBRARIES)
    if(NOT _links)
      set(_links "")
    endif()
    set(_deps "")
    foreach(_d IN LISTS _links)
      if(_d MATCHES "^\\$<(LINK_ONLY|BUILD_INTERFACE):([^<>$]+)>$")
        set(_d "${CMAKE_MATCH_2}")
      elseif(_d MATCHES "^\\$<INSTALL_INTERFACE:")
        continue()
      elseif(_d MATCHES "\\$<")
        _tsc_pb_fail("${_t} links '${_d}'; the LibCarla prebuilt "
                     "cannot reproduce this generator expression (update "
                     "cmake/LibCarlaPrebuilt.cmake)")
      endif()
      if(_d MATCHES "^/(usr/)?lib[^ ]*/lib(m|dl|rt|pthread)\\.(so|a)$")
        # A system library by path (libpng: .../libm.so): by name, so the
        # prefix does not depend on where this system keeps it.
        set(_d "${CMAKE_MATCH_2}")
      endif()
      if(_d MATCHES "${_TSC_PB_SYSTEM_LINKS}")
        list(APPEND _deps "${_d}")
        continue()
      endif()
      if(NOT TARGET "${_d}" AND IS_ABSOLUTE "${_d}" AND _d MATCHES "\\.a$")
        # A static library of this build named by path (libpng links zlib
        # as .../zlib-build/libz.a): shipped like the targets' libraries.
        cmake_path(IS_PREFIX CMAKE_BINARY_DIR "${_d}" NORMALIZE _ours)
        if(_ours)
          list(APPEND _deps "FILE:${_d}")
          continue()
        endif()
      endif()
      if(NOT TARGET "${_d}")
        _tsc_pb_fail("${_t} links '${_d}', which is neither a target "
                     "nor a system library; the LibCarla prebuilt cannot ship it")
      endif()
      get_target_property(_alias "${_d}" ALIASED_TARGET)
      if(_alias)
        set(_d "${_alias}")
      endif()
      get_target_property(_imported "${_d}" IMPORTED)
      if(_imported)
        # Only empty stand-ins (Boost::python, see CMakeLists.txt) are expected.
        get_target_property(_itype "${_d}" TYPE)
        get_target_property(_ilinks "${_d}" INTERFACE_LINK_LIBRARIES)
        get_target_property(_iinc "${_d}" INTERFACE_INCLUDE_DIRECTORIES)
        if(_itype STREQUAL "INTERFACE_LIBRARY" AND NOT _ilinks AND NOT _iinc)
          continue()
        endif()
        _tsc_pb_fail("${_t} links the imported target ${_d}; the "
                     "LibCarla prebuilt cannot ship it")
      endif()
      list(APPEND _deps "${_d}")
      list(APPEND _todo "${_d}")
    endforeach()
    list(REMOVE_DUPLICATES _deps)
    set(_tsc_pb_type_${_i} "${_type}" PARENT_SCOPE)
    set(_tsc_pb_deps_${_i} "${_deps}" PARENT_SCOPE)
    math(EXPR _i "${_i} + 1")
  endwhile()
  set(_tsc_pb_targets "${_targets}" PARENT_SCOPE)
endfunction()

function(tsc_add_libcarla_prebuilt_target dest)
  # Never fails the build it is part of: without the target, CI's publish
  # step fails instead.
  _tsc_pb_walk()
  if(_tsc_pb_error)
    message(WARNING "typesafe_carla: no libcarla_prebuilt target: ${_tsc_pb_error}")
    return()
  endif()
  tsc_abi_fingerprint(_abi SOFT)
  if(NOT _abi)
    message(WARNING "typesafe_carla: no libcarla_prebuilt target: "
                    "tools/libcarla_cache_guard.sh --abi failed")
    return()
  endif()
  set(_desc "${CMAKE_BINARY_DIR}/libcarla-prebuilt-desc.cmake")
  set(_content "# Generated by cmake/LibCarlaPrebuilt.cmake; read by LibCarlaPrebuiltAssemble.cmake.\n")
  macro(_tsc_pb_set name value)
    string(APPEND _content "set(${name} [==[${value}]==])\n")
  endmacro()
  _tsc_pb_set(PB_FORMAT "${_TSC_PB_FORMAT}")
  _tsc_pb_set(PB_DEST "${dest}")
  _tsc_pb_set(PB_ABI "${_abi}")
  _tsc_pb_set(PB_CARLA_REPOSITORY "${TSC_CARLA_GIT_REPOSITORY}")
  _tsc_pb_set(PB_CARLA_REF "${TSC_CARLA_RESOLVED_REF}")
  _tsc_pb_set(PB_CARLA_COMMIT "${TSC_CARLA_COMMIT}")
  _tsc_pb_set(PB_CARLA_VERSION "${TSC_CARLA_VERSION}")
  _tsc_pb_set(PB_BUILT_BY "typesafe_carla ${PROJECT_VERSION} (${TSC_BUILD_COMMIT})")
  _tsc_pb_set(PB_LICENSE "${TSC_CARLA_LICENSE_FILE}")
  _tsc_pb_set(PB_NOTICES "${TSC_CARLA_NOTICES_COMPONENTS}")
  # carla-client's usage requirements, transitively (what the shim compiles
  # with), evaluated at generate time.
  foreach(_p INCLUDE_DIRECTORIES COMPILE_DEFINITIONS COMPILE_OPTIONS COMPILE_FEATURES
             LINK_OPTIONS)
    _tsc_pb_set(PB_${_p} "$<TARGET_PROPERTY:carla-client,INTERFACE_${_p}>")
  endforeach()
  _tsc_pb_set(PB_TARGETS "${_tsc_pb_targets}")
  set(_i 0)
  set(_static "")
  foreach(_t IN LISTS _tsc_pb_targets)
    _tsc_pb_set(PB_TYPE_${_i} "${_tsc_pb_type_${_i}}")
    _tsc_pb_set(PB_DEPS_${_i} "${_tsc_pb_deps_${_i}}")
    if(_tsc_pb_type_${_i} STREQUAL "STATIC_LIBRARY")
      _tsc_pb_set(PB_FILE_${_i} "$<TARGET_FILE:${_t}>")
      list(APPEND _static "${_t}")
    endif()
    math(EXPR _i "${_i} + 1")
  endforeach()
  file(GENERATE OUTPUT "${_desc}" CONTENT "${_content}" TARGET carla-client)

  add_custom_target(libcarla_prebuilt
    COMMAND "${CMAKE_COMMAND}" "-DPB_DESC=${_desc}" -P "${_TSC_PB_ASSEMBLE}"
    COMMENT "Assembling the LibCarla prebuilt in ${dest}"
    VERBATIM)
  add_dependencies(libcarla_prebuilt ${_static})
  list(LENGTH _static _n)
  message(STATUS "typesafe_carla: target libcarla_prebuilt assembles a LibCarla prebuilt "
                 "(${_n} static libraries) in ${dest}")
endfunction()

# ---------------------------------------------------------------------------
# Consumer
# ---------------------------------------------------------------------------

function(_tsc_pb_json json out)
  string(JSON _v ERROR_VARIABLE _err GET "${json}" ${ARGN})
  if(_err)
    message(FATAL_ERROR "typesafe_carla: LibCarla prebuilt manifest: no ${ARGN} (${_err})")
  endif()
  set(${out} "${_v}" PARENT_SCOPE)
endfunction()

# Sets <out> to the commit the requested CARLA ref (TSC_CARLA_GIT_REF in
# TSC_CARLA_GIT_REPOSITORY) points at, from tools/resolve_carla_ref.sh (a full
# SHA as given, else `git ls-remote`).
function(_tsc_pb_resolve_ref out)
  execute_process(
    COMMAND bash "${_TSC_PB_RESOLVE}" "${TSC_CARLA_GIT_REF}" "${TSC_CARLA_GIT_REPOSITORY}"
    RESULT_VARIABLE _rc OUTPUT_VARIABLE _sha ERROR_VARIABLE _err
    OUTPUT_STRIP_TRAILING_WHITESPACE)
  if(NOT _rc EQUAL 0)
    message(FATAL_ERROR
      "typesafe_carla: cannot resolve CARLA ${TSC_CARLA_GIT_REF} to check the LibCarla "
      "prebuilt against it:\n${_err}\nPass the commit SHA as TSC_CARLA_GIT_REF (and the ref "
      "as TSC_CARLA_REF_NAME) to configure offline.")
  endif()
  set(${out} "${_sha}" PARENT_SCOPE)
endfunction()

function(tsc_import_libcarla_prebuilt prefix verified_sha)
  get_filename_component(prefix "${prefix}" ABSOLUTE)
  set(_manifest "${prefix}/prebuilt.json")
  if(NOT EXISTS "${_manifest}")
    message(FATAL_ERROR "typesafe_carla: ${prefix} is not a LibCarla prebuilt (no prebuilt.json)")
  endif()
  file(READ "${_manifest}" _json)
  _tsc_pb_json("${_json}" _format format)
  if(NOT _format EQUAL _TSC_PB_FORMAT)
    message(FATAL_ERROR "typesafe_carla: ${prefix} has prebuilt format ${_format}; this "
                        "typesafe_carla reads format ${_TSC_PB_FORMAT}")
  endif()
  _tsc_pb_json("${_json}" _abi abi)
  _tsc_pb_json("${_json}" _repo carla_repository)
  _tsc_pb_json("${_json}" _ref carla_ref)
  _tsc_pb_json("${_json}" _commit carla_commit)
  _tsc_pb_json("${_json}" _version carla_version)

  # The compilers must produce the same ABI the prefix was built with.
  tsc_abi_fingerprint(_here)
  if(NOT _here STREQUAL _abi)
    message(FATAL_ERROR
      "typesafe_carla: the LibCarla prebuilt in ${prefix} was built with another toolchain.\n"
      "Prebuilt:\n${_abi}\nThis build (${CMAKE_C_COMPILER}, ${CMAKE_CXX_COMPILER}):\n${_here}\n"
      "Use a prebuilt made with this compiler, or unset TSC_CARLA_PREBUILT_DIR to build "
      "LibCarla from source.")
  endif()

  # ... from the CARLA commit this build asks for.
  _tsc_pb_norm_repo("${_repo}" _repo_n)
  _tsc_pb_norm_repo("${TSC_CARLA_GIT_REPOSITORY}" _want_repo)
  if(NOT _repo_n STREQUAL _want_repo)
    message(FATAL_ERROR "typesafe_carla: the LibCarla prebuilt in ${prefix} is from ${_repo}, "
                        "not TSC_CARLA_GIT_REPOSITORY (${TSC_CARLA_GIT_REPOSITORY})")
  endif()
  # Like a fetched branch, the check is not repeated until the ref changes
  # (or TSC_CARLA_REFRESH).
  set(_key "${TSC_CARLA_GIT_REPOSITORY}@${TSC_CARLA_GIT_REF}=${_commit}")
  if(verified_sha)
    set(_sha "${verified_sha}")
  elseif(_TSC_CARLA_PREBUILT_CHECKED STREQUAL _key AND NOT TSC_CARLA_REFRESH)
    set(_sha "${_commit}")
  else()
    _tsc_pb_resolve_ref(_sha)
  endif()
  if(NOT _sha STREQUAL _commit)
    message(FATAL_ERROR
      "typesafe_carla: the LibCarla prebuilt in ${prefix} is CARLA ${_ref} = ${_commit}, but "
      "CARLA ${TSC_CARLA_GIT_REF} is ${_sha}. Use a prebuilt of ${_sha} "
      "(tools/fetch_libcarla_prebuilt.py ${TSC_CARLA_GIT_REF}), or set TSC_CARLA_GIT_REF="
      "${_commit} and TSC_CARLA_REF_NAME=${_ref} to build that commit.")
  endif()
  set(_TSC_CARLA_PREBUILT_CHECKED "${_key}" CACHE INTERNAL "")
  if(TSC_CARLA_REFRESH)
    set_property(CACHE TSC_CARLA_REFRESH PROPERTY VALUE OFF)
  endif()

  foreach(_f cmake/libcarla-targets.cmake licenses/LICENSE.CARLA
             licenses/THIRD_PARTY_NOTICES.components)
    if(NOT EXISTS "${prefix}/${_f}")
      message(FATAL_ERROR "typesafe_carla: the LibCarla prebuilt in ${prefix} has no ${_f}")
    endif()
  endforeach()
  include("${prefix}/cmake/libcarla-targets.cmake")
  if(NOT TARGET carla-client)
    message(FATAL_ERROR "typesafe_carla: ${prefix}/cmake/libcarla-targets.cmake defined no carla-client")
  endif()
  set_property(DIRECTORY "${CMAKE_SOURCE_DIR}" APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_manifest}")

  # The recorded ref, as for a source build of the same ref.
  _tsc_set_resolved_ref("${TSC_CARLA_GIT_REF}")
  set(_rec "${TSC_CARLA_RESOLVED_REF}")
  set(TSC_CARLA_RESOLVED_REF "${_rec}" PARENT_SCOPE)
  set(TSC_CARLA_COMMIT "${_commit}" PARENT_SCOPE)
  set(TSC_CARLA_VERSION "${_version}" PARENT_SCOPE)
  set(TSC_CARLA_LICENSE_FILE "${prefix}/licenses/LICENSE.CARLA" PARENT_SCOPE)
  # LibCarla's entry names the ref and commit; name this build's ref.
  file(READ "${prefix}/licenses/THIRD_PARTY_NOTICES.components" _notices)
  string(REPLACE "(${_ref}, ${_commit})" "(${_rec}, ${_commit})" _notices "${_notices}")
  set(TSC_CARLA_NOTICES_COMPONENTS_TEXT "${_notices}" PARENT_SCOPE)
  message(STATUS "typesafe_carla: LibCarla prebuilt ${prefix} (CARLA ${_rec} = ${_commit})")
endfunction()

# ---------------------------------------------------------------------------
# TSC_CARLA_PREBUILT=auto
# ---------------------------------------------------------------------------

# Sets <out_prefix> to a LibCarla prebuilt for TSC_CARLA_GIT_REF and this
# compiler, downloaded from CI by tools/fetch_libcarla_prebuilt.py (or found in
# its cache), and <out_sha> to the commit it was checked against; both empty
# if there is none, so that LibCarla is built from source. The answer is kept
# until the ref, repository or compiler changes, or TSC_CARLA_REFRESH.
function(tsc_fetch_libcarla_prebuilt out_prefix out_sha)
  set(${out_prefix} "" PARENT_SCOPE)
  set(${out_sha} "" PARENT_SCOPE)
  set(_key "${TSC_CARLA_GIT_REPOSITORY}@${TSC_CARLA_GIT_REF}|${CMAKE_C_COMPILER}|${CMAKE_CXX_COMPILER}|${CMAKE_C_FLAGS}|${CMAKE_CXX_FLAGS}")
  if(_TSC_CARLA_PREBUILT_AUTO_KEY STREQUAL _key AND NOT TSC_CARLA_REFRESH)
    if(_TSC_CARLA_PREBUILT_AUTO_DIR STREQUAL "")
      message(STATUS "typesafe_carla: no LibCarla prebuilt (as before; "
                     "-DTSC_CARLA_REFRESH=ON looks again)")
      return()
    elseif(EXISTS "${_TSC_CARLA_PREBUILT_AUTO_DIR}/prebuilt.json")
      set(${out_prefix} "${_TSC_CARLA_PREBUILT_AUTO_DIR}" PARENT_SCOPE)
      set(${out_sha} "${_TSC_CARLA_PREBUILT_AUTO_SHA}" PARENT_SCOPE)
      return()
    endif()
  endif()

  find_package(Python3 COMPONENTS Interpreter QUIET)
  if(NOT Python3_Interpreter_FOUND)
    message(STATUS "typesafe_carla: no Python 3 to fetch a LibCarla prebuilt; building from source")
    return()
  endif()
  message(STATUS "typesafe_carla: looking for a LibCarla prebuilt of CARLA ${TSC_CARLA_GIT_REF}")
  _tsc_pb_abi_env(_env)
  execute_process(
    COMMAND ${_env} "${Python3_EXECUTABLE}" "${_TSC_PB_FETCH}"
            --repository "${TSC_CARLA_GIT_REPOSITORY}" "${TSC_CARLA_GIT_REF}"
    RESULT_VARIABLE _rc OUTPUT_VARIABLE _out ERROR_VARIABLE _err
    OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_STRIP_TRAILING_WHITESPACE)
  set(_dir "")
  set(_sha "")
  if(_rc EQUAL 0 AND EXISTS "${_out}/prebuilt.json")
    set(_dir "${_out}")
    file(READ "${_dir}/prebuilt.json" _json)
    _tsc_pb_json("${_json}" _sha carla_commit)
    message(STATUS "typesafe_carla: using the LibCarla prebuilt ${_dir}")
  elseif(_rc EQUAL 2)
    # Expected: no gh, not logged in, or CI has no prebuilt for this.
    message(STATUS "typesafe_carla: ${_err}\n   building LibCarla from source")
  else()
    message(WARNING "typesafe_carla: fetching a LibCarla prebuilt failed (${_rc}):\n${_err}\n"
                    "Building LibCarla from source.")
  endif()
  set(_TSC_CARLA_PREBUILT_AUTO_KEY "${_key}" CACHE INTERNAL "")
  set(_TSC_CARLA_PREBUILT_AUTO_DIR "${_dir}" CACHE INTERNAL "")
  set(_TSC_CARLA_PREBUILT_AUTO_SHA "${_sha}" CACHE INTERNAL "")
  set(${out_prefix} "${_dir}" PARENT_SCOPE)
  set(${out_sha} "${_sha}" PARENT_SCOPE)
endfunction()
