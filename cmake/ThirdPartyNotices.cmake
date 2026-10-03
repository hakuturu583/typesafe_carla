# Collects the license notices of everything the libcarla backend links
# statically into libtypesafe_carla_ffi.so, into one THIRD_PARTY_NOTICES file
# that ships in the wheel next to LICENSE.CARLA.
#
#   tsc_write_third_party_notices(<output file>)
#
# Call it after add_subdirectory(CARLA): it reads the license texts from the
# sources CARLA fetched (FetchContent) and from LibCarla's vendored
# third-party/ directory, so the notices always match what was built. Any
# expected license file or license comment that is missing fails the
# configure step rather than shipping an incomplete notice.
#
# What carla-client links (both 0.10.0 and ue5-dev):
#   - LibCarla itself and its vendored sources: pugixml, odrSpiral (compiled),
#     moodycamel ConcurrentQueue, Fast-Quadric-Mesh-Simplification and
#     MeshReconstruction (header-only, compiled into LibCarla);
#   - Boost: the libraries CARLA requests and their dependencies, compiled
#     (0.10.0 links atomic, chrono, container, context, coroutine, date_time,
#     exception, filesystem, random, serialization, thread) and header-only
#     (asio, geometry, gil, algorithm, ...);
#   - rpclib, with its bundled asio, msgpack-c, cppformat and optional-lite;
#   - RecastNavigation (Recast, Detour, DetourCrowd), libpng and zlib.
# Not linked: Eigen (fetched by CARLA, unused by the client), SQLite (replaced
# by an empty stand-in) and Boost.Python (an empty target).
#
# To keep this list honest, configuring fails if carla-client links a library
# or LibCarla vendors a third-party directory that is not accounted for here.

include_guard(GLOBAL)
include(FetchContent)

set(_TSC_LICENSES_DIR "${CMAKE_CURRENT_LIST_DIR}/licenses")
# Stands in for ';' in texts passed around as list elements; restored on write.
set(_TSC_SEMI "@TSC_SEMICOLON@")

# Fails unless <file> exists and is non-empty.
function(_tsc_require_file file what)
  if(NOT EXISTS "${file}")
    message(FATAL_ERROR "typesafe_carla: license file for ${what} not found: ${file}\n"
                        "Refusing to build a wheel with incomplete THIRD_PARTY_NOTICES.")
  endif()
  file(SIZE "${file}" _size)
  if(_size EQUAL 0)
    message(FATAL_ERROR "typesafe_carla: license file for ${what} is empty: ${file}")
  endif()
  if(NOT CMAKE_SCRIPT_MODE_FILE)  # re-collect when a license file changes
    set_property(DIRECTORY "${CMAKE_SOURCE_DIR}" APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${file}")
  endif()
endfunction()

# Sets <out> to the source dir of FetchContent dependency <name>.
function(_tsc_dep_dir name out)
  FetchContent_GetProperties(${name})
  string(TOLOWER "${name}" _lc)
  if(NOT ${_lc}_POPULATED OR NOT IS_DIRECTORY "${${_lc}_SOURCE_DIR}")
    message(FATAL_ERROR "typesafe_carla: CARLA did not fetch '${name}'; cannot collect its "
                        "license notice (update cmake/ThirdPartyNotices.cmake)")
  endif()
  set(${out} "${${_lc}_SOURCE_DIR}" PARENT_SCOPE)
endfunction()

# Sets <out> to the leading license comment of a source file: a /* ... */
# block, or the run of // lines it starts with. Fails unless it contains
# every string in <ARGN> (e.g. "Copyright").
function(_tsc_leading_comment file out)
  _tsc_require_file("${file}" "${file}")
  file(READ "${file}" _text)
  string(REPLACE "\r" "" _text "${_text}")
  # Semicolons would split the text when passed on as a CMake list element.
  string(REPLACE ";" "${_TSC_SEMI}" _text "${_text}")
  if(_text MATCHES "^[ \t\n]*/\\*")
    string(FIND "${_text}" "*/" _end)
    math(EXPR _end "${_end} + 2")
    string(SUBSTRING "${_text}" 0 ${_end} _comment)
  else()
    # Blank lines may separate the // paragraphs of one comment.
    string(REGEX MATCH "^(([ \t]*//[^\n]*)?\n)+" _comment "${_text}")
  endif()
  foreach(_needle IN LISTS ARGN)
    string(FIND "${_comment}" "${_needle}" _at)
    if(_at EQUAL -1)
      message(FATAL_ERROR "typesafe_carla: the license comment of ${file} no longer "
                          "contains '${_needle}'; check its license and update "
                          "cmake/ThirdPartyNotices.cmake")
    endif()
  endforeach()
  string(STRIP "${_comment}" _comment)
  set(${out} "${_comment}" PARENT_SCOPE)
endfunction()

# Appends a component to the index (_tsc_index) and its notice to the body
# (_tsc_body), and bumps the counter (_tsc_n), in the caller's scope.
#   _tsc_component(NAME n VERSION v LICENSE spdx UPSTREAM url SOURCE dir
#                  [CONTAINS text] [HEADER t...] [FILES f...] [TEXT t...])
# FILES are read verbatim; HEADER (before them) and TEXT (after them) entries
# are inserted as given.
function(_tsc_component)
  cmake_parse_arguments(PARSE_ARGV 0 _c "" "NAME;VERSION;LICENSE;UPSTREAM;SOURCE;CONTAINS"
                        "HEADER;FILES;TEXT")
  math(EXPR _tsc_n "${_tsc_n} + 1")
  file(RELATIVE_PATH _c_rel "${CMAKE_BINARY_DIR}" "${_c_SOURCE}")
  if(_c_rel MATCHES "^\\.\\./")
    set(_c_rel "${_c_SOURCE}")
  endif()
  string(APPEND _tsc_index
    "${_tsc_n}. ${_c_NAME} ${_c_VERSION}\n"
    "   License:  ${_c_LICENSE}\n"
    "   Upstream: ${_c_UPSTREAM}\n"
    "   Source:   ${_c_rel}\n")
  if(_c_CONTAINS)
    string(APPEND _tsc_index "   Contains: ${_c_CONTAINS}\n")
  endif()
  string(APPEND _tsc_index "\n")
  string(APPEND _tsc_body
    "\n================================================================================\n"
    "${_tsc_n}. ${_c_NAME} ${_c_VERSION} (${_c_LICENSE})\n"
    "================================================================================\n")
  foreach(_t IN LISTS _c_HEADER)
    string(APPEND _tsc_body "\n${_t}\n")
  endforeach()
  foreach(_f IN LISTS _c_FILES)
    _tsc_require_file("${_f}" "${_c_NAME}")
    file(READ "${_f}" _c_text)
    string(STRIP "${_c_text}" _c_text)
    cmake_path(IS_PREFIX _TSC_LICENSES_DIR "${_f}" _c_ours)
    if(_c_ours)
      file(RELATIVE_PATH _c_frel "${CMAKE_SOURCE_DIR}" "${_f}")
      set(_c_frel "full license text, from typesafe_carla's ${_c_frel}")
    else()
      file(RELATIVE_PATH _c_frel "${_c_SOURCE}" "${_f}")
    endif()
    string(APPEND _tsc_body "\n--- ${_c_frel} ---\n\n${_c_text}\n")
  endforeach()
  foreach(_t IN LISTS _c_TEXT)
    string(APPEND _tsc_body "\n${_t}\n")
  endforeach()
  set(_tsc_n "${_tsc_n}" PARENT_SCOPE)
  set(_tsc_index "${_tsc_index}" PARENT_SCOPE)
  set(_tsc_body "${_tsc_body}" PARENT_SCOPE)
endfunction()

function(tsc_write_third_party_notices output)
  if(NOT TARGET carla-client)
    message(FATAL_ERROR "tsc_write_third_party_notices: no carla-client target")
  endif()

  # --- Guard: everything carla-client links must be listed below. ----------
  get_target_property(_links carla-client LINK_LIBRARIES)
  foreach(_lib IN LISTS _links)
    if(NOT _lib MATCHES "^(Boost::.*|RecastNavigation::(Recast|Detour|DetourCrowd)|png_static|zlibstatic|rpc)$")
      message(FATAL_ERROR "typesafe_carla: carla-client links '${_lib}', which has no entry "
                          "in THIRD_PARTY_NOTICES; add its license to cmake/ThirdPartyNotices.cmake")
    endif()
  endforeach()
  set(_tp "${TSC_CARLA_DIR}/LibCarla/source/third-party")
  set(_known_vendored marchingcube moodycamel odrSpiral pugixml simplify)
  file(GLOB _vendored RELATIVE "${_tp}" "${_tp}/*")
  foreach(_v IN LISTS _vendored)
    if(IS_DIRECTORY "${_tp}/${_v}" AND NOT _v IN_LIST _known_vendored)
      message(FATAL_ERROR "typesafe_carla: LibCarla vendors third-party/${_v}, which has no "
                          "entry in THIRD_PARTY_NOTICES; add its license to "
                          "cmake/ThirdPartyNotices.cmake")
    endif()
  endforeach()

  set(_tsc_n 0)
  set(_tsc_index "")
  set(_tsc_body "")
  set(_apache "${_TSC_LICENSES_DIR}/Apache-2.0.txt")
  set(_mit "${_TSC_LICENSES_DIR}/MIT.txt")
  _tsc_require_file("${_apache}" "Apache-2.0 (template)")
  _tsc_require_file("${_mit}" "MIT (template)")
  file(READ "${_mit}" _mit_terms)
  string(STRIP "${_mit_terms}" _mit_terms)
  string(REPLACE ";" "${_TSC_SEMI}" _mit_terms "${_mit_terms}")

  # --- LibCarla and its vendored sources --------------------------------------
  set(_carla_ver "${TSC_CARLA_VERSION}")
  if(NOT TSC_CARLA_COMMIT STREQUAL "unknown")
    set(_carla_ver "${_carla_ver} (${TSC_CARLA_RESOLVED_REF}, ${TSC_CARLA_COMMIT})")
  endif()
  _tsc_component(NAME "LibCarla (CARLA)" VERSION "${_carla_ver}" LICENSE "MIT"
    UPSTREAM "https://github.com/carla-simulator/carla" SOURCE "${TSC_CARLA_DIR}"
    FILES "${TSC_CARLA_DIR}/LICENSE")

  _tsc_component(NAME "pugixml" VERSION "(vendored in LibCarla)" LICENSE "MIT"
    UPSTREAM "https://github.com/zeux/pugixml" SOURCE "${_tp}/pugixml"
    FILES "${_tp}/pugixml/LICENSE.md")

  _tsc_leading_comment("${_tp}/odrSpiral/odrSpiral.h" _odr "Copyright" "Apache License, Version 2.0")
  _tsc_component(NAME "odrSpiral" VERSION "(vendored in LibCarla)" LICENSE "Apache-2.0"
    UPSTREAM "https://www.asam.net/standards/detail/opendrive/" SOURCE "${_tp}/odrSpiral"
    HEADER "${_odr}" FILES "${_apache}")

  _tsc_leading_comment("${_tp}/moodycamel/ConcurrentQueue.h" _mc
    "Copyright" "Redistributions in binary form")
  _tsc_component(NAME "moodycamel::ConcurrentQueue" VERSION "(vendored in LibCarla)"
    LICENSE "BSD-2-Clause" UPSTREAM "https://github.com/cameron314/concurrentqueue"
    SOURCE "${_tp}/moodycamel" TEXT "${_mc}")

  _tsc_leading_comment("${_tp}/simplify/Simplify.h" _simp "(C) by Sven Forstmann" "MIT")
  _tsc_component(NAME "Fast-Quadric-Mesh-Simplification" VERSION "(vendored in LibCarla)"
    LICENSE "MIT" UPSTREAM "https://github.com/sp4cerat/Fast-Quadric-Mesh-Simplification"
    SOURCE "${_tp}/simplify"
    TEXT "${_simp}" "Copyright (c) 2014 Sven Forstmann\n\n${_mit_terms}")

  # The vendored headers carry no notice; upstream states "The library can be
  # used under the terms of the MIT License" (Readme.md) without a copyright
  # line.
  _tsc_require_file("${_tp}/marchingcube/MeshReconstruction.h" "MeshReconstruction")
  _tsc_component(NAME "MeshReconstruction" VERSION "(vendored in LibCarla)" LICENSE "MIT"
    UPSTREAM "https://github.com/Magnus2/MeshReconstruction" SOURCE "${_tp}/marchingcube"
    TEXT "Upstream: \"The library can be used under the terms of the MIT License.\"\n\nCopyright (c) the MeshReconstruction authors (https://github.com/Magnus2/MeshReconstruction)\n\n${_mit_terms}")

  # --- Boost ------------------------------------------------------------------
  _tsc_dep_dir(boost _boost)
  # The libraries CARLA requests (CMakeLists.txt sets BOOST_INCLUDE_LIBRARIES),
  # plus those carla-client names directly; Boost builds their dependencies.
  set(_boost_libs ${BOOST_INCLUDE_LIBRARIES})
  foreach(_lib IN LISTS _links)
    if(_lib MATCHES "^Boost::(.*)$" AND NOT CMAKE_MATCH_1 STREQUAL "python")
      list(APPEND _boost_libs "${CMAKE_MATCH_1}")
    endif()
  endforeach()
  list(REMOVE_DUPLICATES _boost_libs)
  list(SORT _boost_libs)
  list(JOIN _boost_libs ", " _boost_libs)
  file(STRINGS "${_boost}/libs/config/include/boost/version.hpp" _bv
       REGEX "^#define BOOST_VERSION [0-9]+")
  string(REGEX REPLACE ".* ([0-9]+)$" "\\1" _bv "${_bv}")
  math(EXPR _bmaj "${_bv} / 100000")
  math(EXPR _bmin "${_bv} / 100 % 1000")
  math(EXPR _bpat "${_bv} % 100")
  _tsc_component(NAME "Boost" VERSION "${_bmaj}.${_bmin}.${_bpat}" LICENSE "BSL-1.0"
    UPSTREAM "https://www.boost.org" SOURCE "${_boost}"
    CONTAINS "${_boost_libs} and the Boost libraries they depend on"
    FILES "${_boost}/LICENSE_1_0.txt")

  # --- rpclib (CARLA's fork) --------------------------------------------------
  _tsc_dep_dir(rpclib _rpc)
  file(STRINGS "${_rpc}/CMakeLists.txt" _rv REGEX "^project\\(rpc VERSION [0-9.]+\\)")
  string(REGEX REPLACE ".*VERSION ([0-9.]+).*" "\\1" _rv "${_rv}")
  _tsc_leading_comment("${_rpc}/dependencies/include/format.h" _fmt
    "Copyright" "Redistributions in binary form")
  _tsc_leading_comment("${_rpc}/include/rpc/nonstd/optional.hpp" _opt "Copyright" "MIT")
  _tsc_leading_comment("${_rpc}/include/rpc/msgpack.hpp" _mp "Copyright" "Apache License")
  _tsc_leading_comment("${_rpc}/dependencies/include/asio.hpp" _asio
    "Copyright" "Boost Software License")
  _tsc_component(NAME "rpclib" VERSION "${_rv} (carla-simulator fork)" LICENSE "MIT"
    UPSTREAM "https://github.com/carla-simulator/rpclib" SOURCE "${_rpc}"
    CONTAINS "bundled asio (BSL-1.0), msgpack-c (BSL-1.0/Apache-2.0), cppformat (BSD-2-Clause), optional-lite (MIT)"
    FILES "${_rpc}/LICENSE.md"
    TEXT
      "--- bundled asio (dependencies/include/asio.hpp; BSL-1.0, text in the Boost section) ---\n\n${_asio}"
      "--- bundled msgpack-c (include/rpc/msgpack.hpp, include/rpc/msgpack/; BSL-1.0, text in the Boost section, and Apache-2.0, text in the odrSpiral section) ---\n\n${_mp}"
      "--- bundled cppformat (dependencies/include/format.h, dependencies/src/format.cc, posix.cc) ---\n\n${_fmt}"
      "--- bundled optional-lite (include/rpc/nonstd/optional.hpp) ---\n\n${_opt}\n\n${_mit_terms}")

  # --- RecastNavigation (CARLA's fork) ----------------------------------------
  _tsc_dep_dir(recastnavigation _recast)
  file(STRINGS "${_recast}/CMakeLists.txt" _recv REGEX "^set\\(LIB_VERSION [0-9.]+\\)")
  string(REGEX REPLACE ".*LIB_VERSION ([0-9.]+).*" "\\1" _recv "${_recv}")
  _tsc_component(NAME "RecastNavigation" VERSION "${_recv} (carla-simulator fork)"
    LICENSE "Zlib" UPSTREAM "https://github.com/carla-simulator/recastnavigation"
    SOURCE "${_recast}" CONTAINS "Recast, Detour, DetourCrowd"
    FILES "${_recast}/License.txt")

  # --- libpng and zlib --------------------------------------------------------
  _tsc_dep_dir(libpng _png)
  file(STRINGS "${_png}/png.h" _pngv REGEX "^#define PNG_LIBPNG_VER_STRING \"[0-9.]+\"")
  string(REGEX REPLACE ".*\"([0-9.]+)\".*" "\\1" _pngv "${_pngv}")
  _tsc_component(NAME "libpng" VERSION "${_pngv}" LICENSE "Libpng-2.0"
    UPSTREAM "https://github.com/pnggroup/libpng" SOURCE "${_png}"
    FILES "${_png}/LICENSE")

  _tsc_dep_dir(zlib _zlib)
  file(STRINGS "${_zlib}/zlib.h" _zv REGEX "^#define ZLIB_VERSION \"[0-9.]+\"")
  string(REGEX REPLACE ".*\"([0-9.]+)\".*" "\\1" _zv "${_zv}")
  _tsc_component(NAME "zlib" VERSION "${_zv}" LICENSE "Zlib"
    UPSTREAM "https://zlib.net" SOURCE "${_zlib}"
    FILES "${_zlib}/LICENSE")

  set(_notices
"THIRD-PARTY NOTICES for typesafe_carla ${PROJECT_VERSION} (libcarla backend)

typesafe_carla/_native/libtypesafe_carla_ffi.so statically links the
components listed below. Their license notices follow, collected at build
time from the sources the library was built from. \"Source\" paths are
relative to the build directory where possible.

Fetched by CARLA but not linked: Eigen (unused by the client library) and
SQLite (server-side tools only). Boost.Python is not built.

${_tsc_index}${_tsc_body}")
  string(REPLACE "${_TSC_SEMI}" ";" _notices "${_notices}")
  file(WRITE "${output}" "${_notices}")
  message(STATUS "typesafe_carla: wrote ${_tsc_n} third-party notices to ${output}")
endfunction()
