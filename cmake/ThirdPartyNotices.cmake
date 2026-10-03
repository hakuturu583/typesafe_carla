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
# by an empty stand-in; not linked) and Boost.Python (an empty target).
#
# To keep this list honest, configuring fails if carla-client links a library
# (directly or transitively), or LibCarla vendors a third-party/ entry, that is
# not accounted for here; and LibCarla's sources are scanned for copyright
# holders other than CVC, which are listed with LibCarla's notice.

include_guard(GLOBAL)
include(FetchContent)

set(_TSC_LICENSES_DIR "${CMAKE_CURRENT_LIST_DIR}/licenses")

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
  # Re-collect when a license file changes.
  set_property(DIRECTORY "${CMAKE_SOURCE_DIR}" APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${file}")
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

# Link items covered by the notices below (by target name or library file).
set(_TSC_COVERED_LINKS
  "^(Boost::.*|boost_[a-z0-9_]+|RecastNavigation::(Recast|Detour|DetourCrowd)|Recast|Detour|DetourCrowd|png_static|zlibstatic|ZLIB::ZLIB|.*/libz\\.a|rpc)$")
# Link items that are system libraries, not shipped code.
set(_TSC_SYSTEM_LINKS
  "^(Threads::Threads|m|dl|rt|pthread|-pthread|-lm|-ldl|/(usr/)?lib[^ ]*/lib(m|dl|rt|pthread)\\.(so|a))$")

# Sets <out> to every library <target> links, directly or through the
# LINK_LIBRARIES / INTERFACE_LINK_LIBRARIES of the targets it links, minus
# system libraries. $<LINK_ONLY:x> and $<BUILD_INTERFACE:x> are unwrapped;
# other generator expressions are skipped.
function(_tsc_link_closure target out)
  set(_todo "${target}")
  set(_seen "")
  set(_found "")
  while(_todo)
    list(POP_FRONT _todo _t)
    if(_t IN_LIST _seen)
      continue()
    endif()
    list(APPEND _seen "${_t}")
    get_target_property(_alias "${_t}" ALIASED_TARGET)
    if(_alias)
      list(APPEND _todo "${_alias}")
      continue()
    endif()
    set(_deps "")
    get_target_property(_type "${_t}" TYPE)
    if(NOT _type STREQUAL "INTERFACE_LIBRARY")
      get_target_property(_l "${_t}" LINK_LIBRARIES)
      if(_l)
        list(APPEND _deps ${_l})
      endif()
    endif()
    get_target_property(_l "${_t}" INTERFACE_LINK_LIBRARIES)
    if(_l)
      list(APPEND _deps ${_l})
    endif()
    foreach(_d IN LISTS _deps)
      if(_d MATCHES "^\\$<(LINK_ONLY|BUILD_INTERFACE):([^<>$]+)>$")
        set(_d "${CMAKE_MATCH_2}")
      elseif(_d MATCHES "\\$<")
        continue()
      endif()
      if(_d MATCHES "${_TSC_SYSTEM_LINKS}")
        continue()
      endif()
      if(TARGET "${_d}")
        list(APPEND _todo "${_d}")
      endif()
      list(APPEND _found "${_d}")
    endforeach()
  endwhile()
  list(REMOVE_DUPLICATES _found)
  set(${out} "${_found}" PARENT_SCOPE)
endfunction()

# Sets <out> to <path> relative to the CARLA tree ("<CARLA>/...") or the
# dependency download directory ("<deps>/..."), so no builder's paths end up in
# the shipped file.
function(_tsc_display_path path out)
  if(FETCHCONTENT_BASE_DIR)
    set(_deps "${FETCHCONTENT_BASE_DIR}")
  else()
    set(_deps "${CMAKE_BINARY_DIR}/_deps")
  endif()
  set(_bases "${TSC_CARLA_DIR}" "${_deps}")
  set(_tags "<CARLA>" "<deps>")
  foreach(_base _tag IN ZIP_LISTS _bases _tags)
    cmake_path(IS_PREFIX _base "${path}" NORMALIZE _in)
    if(_in)
      file(RELATIVE_PATH _rel "${_base}" "${path}")
      if(_rel)
        set(_tag "${_tag}/${_rel}")
      endif()
      set(${out} "${_tag}" PARENT_SCOPE)
      return()
    endif()
  endforeach()
  message(FATAL_ERROR "typesafe_carla: ${path} is neither in the CARLA tree nor in ${_deps}")
endfunction()

# Sets <out> to the first capture group of <regex> in the first line of
# <file> it matches. Fails if no line matches.
function(_tsc_version file regex out)
  _tsc_require_file("${file}" "${file}")
  file(STRINGS "${file}" _lines REGEX "${regex}")
  if(NOT _lines MATCHES "${regex}")
    message(FATAL_ERROR "typesafe_carla: no version matching '${regex}' in ${file}; "
                        "update cmake/ThirdPartyNotices.cmake")
  endif()
  set(${out} "${CMAKE_MATCH_1}" PARENT_SCOPE)
endfunction()

# Sets <out> to the leading license comment of a source file: a /* ... */
# block, or the run of // lines it starts with. Fails unless it contains
# every string in <ARGN> (e.g. "Copyright").
function(_tsc_leading_comment file out)
  _tsc_require_file("${file}" "${file}")
  file(READ "${file}" _text)
  string(REPLACE "\r" "" _text "${_text}")
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
#                  [CONTAINS text] [HEADER var] [FILES f...] [TEXT var])
# FILES are read verbatim. HEADER (before them) and TEXT (after them) name
# variables holding text to insert as is; passing names rather than values
# keeps license texts, which contain ';' and '[', out of argument lists.
function(_tsc_component)
  cmake_parse_arguments(PARSE_ARGV 0 _c ""
                        "NAME;VERSION;LICENSE;UPSTREAM;SOURCE;CONTAINS;HEADER;TEXT" "FILES")
  math(EXPR _tsc_n "${_tsc_n} + 1")
  _tsc_display_path("${_c_SOURCE}" _c_rel)
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
  if(_c_HEADER)
    string(APPEND _tsc_body "\n${${_c_HEADER}}\n")
  endif()
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
  if(_c_TEXT)
    string(APPEND _tsc_body "\n${${_c_TEXT}}\n")
  endif()
  set(_tsc_n "${_tsc_n}" PARENT_SCOPE)
  set(_tsc_index "${_tsc_index}" PARENT_SCOPE)
  set(_tsc_body "${_tsc_body}" PARENT_SCOPE)
endfunction()

# Sets <out> to a list of the copyright holders other than CVC named in
# LibCarla's own sources (LibCarla/source/carla, without ros2/ and rss/, which
# are not built), each with the files that name it. Fails if such a file does
# not say it is under the MIT license, like the rest of LibCarla.
function(_tsc_libcarla_holders out)
  set(_src "${TSC_CARLA_DIR}/LibCarla/source")
  file(GLOB_RECURSE _files RELATIVE "${_src}"
       "${_src}/carla/*.h" "${_src}/carla/*.hpp" "${_src}/carla/*.cpp" "${_src}/carla/*.inl")
  list(FILTER _files EXCLUDE REGEX "^carla/(ros2|rss)/")
  list(SORT _files)
  set(_holders "")
  foreach(_f IN LISTS _files)
    file(READ "${_src}/${_f}" _text)
    string(FIND "${_text}" "Copyright" _at)
    if(_at EQUAL -1)
      continue()
    endif()
    string(REPLACE "\r" "" _text "${_text}")
    # A copyright line and the comment lines continuing it (up to a blank one).
    string(REGEX MATCHALL "Copyright[^\n]*(\n[ \t]*//[ \t]*[^ \t\n][^\n]*)*" _lines "${_text}")
    # Consecutive copyright lines (e.g. CVC, then Intel) match as one.
    list(JOIN _lines "\n" _lines)
    string(REGEX REPLACE "\n[ \t]*//[ \t]*" " " _lines "${_lines}")
    string(REPLACE "Copyright" ";Copyright" _lines "${_lines}")
    foreach(_line IN LISTS _lines)
      string(STRIP "${_line}" _line)
      if(_line STREQUAL "")
        continue()
      endif()
      if(_line MATCHES "Computer Vision Center|\\(CVC\\)|CVC\\.")
        continue()
      endif()
      string(FIND "${_text}" "licensed under the terms of the MIT license" _mit)
      if(_mit EQUAL -1)
        message(FATAL_ERROR "typesafe_carla: ${_src}/${_f} names '${_line}' but not the MIT "
                            "license; check its license and update cmake/ThirdPartyNotices.cmake")
      endif()
      string(MD5 _key "${_line}")
      if(NOT _key IN_LIST _holders)
        list(APPEND _holders "${_key}")
        set(_holder_${_key} "${_line}")
        set(_files_${_key} "")
      endif()
      list(APPEND _files_${_key} "${_f}")
    endforeach()
  endforeach()
  set(_result "")
  foreach(_key IN LISTS _holders)
    list(REMOVE_DUPLICATES _files_${_key})
    list(JOIN _files_${_key} ", " _fl)
    string(APPEND _result "  ${_holder_${_key}}\n    (${_fl})\n")
  endforeach()
  set(${out} "${_result}" PARENT_SCOPE)
endfunction()

function(tsc_write_third_party_notices output)
  if(NOT TARGET carla-client)
    message(FATAL_ERROR "tsc_write_third_party_notices: no carla-client target")
  endif()

  # --- Guard: everything carla-client links must be listed below. ----------
  _tsc_link_closure(carla-client _links)
  message(VERBOSE "typesafe_carla: carla-client links ${_links}")
  foreach(_lib IN LISTS _links)
    if(NOT _lib MATCHES "${_TSC_COVERED_LINKS}")
      message(FATAL_ERROR "typesafe_carla: carla-client links '${_lib}' (directly or "
                          "transitively), which has no entry in THIRD_PARTY_NOTICES; add its "
                          "license to cmake/ThirdPartyNotices.cmake")
    endif()
  endforeach()
  set(_tp "${TSC_CARLA_DIR}/LibCarla/source/third-party")
  set(_known_vendored marchingcube moodycamel odrSpiral pugixml simplify)
  file(GLOB _vendored RELATIVE "${_tp}" "${_tp}/*")
  foreach(_v IN LISTS _vendored)
    if(NOT _v IN_LIST _known_vendored)
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
  _tsc_require_file("${_mit}" "MIT (template)")
  file(READ "${_mit}" _mit_terms)
  string(STRIP "${_mit_terms}" _mit_terms)
  # The MIT terms without the warranty disclaimer, for notices that carry it.
  string(FIND "${_mit_terms}" "THE SOFTWARE IS PROVIDED" _at)
  string(SUBSTRING "${_mit_terms}" 0 ${_at} _mit_grant)
  string(STRIP "${_mit_grant}" _mit_grant)

  # --- LibCarla and its vendored sources --------------------------------------
  set(_carla_ver "${TSC_CARLA_VERSION}")
  if(NOT TSC_CARLA_COMMIT STREQUAL "unknown")
    set(_carla_ver "${_carla_ver} (${TSC_CARLA_RESOLVED_REF}, ${TSC_CARLA_COMMIT})")
  endif()
  _tsc_libcarla_holders(_carla_holders)
  if(_carla_holders)
    set(_carla_holders "--- other copyright holders in LibCarla's sources, under the MIT terms above ---\n\n${_carla_holders}")
  endif()
  _tsc_component(NAME "LibCarla (CARLA)" VERSION "${_carla_ver}" LICENSE "MIT"
    UPSTREAM "https://github.com/carla-simulator/carla" SOURCE "${TSC_CARLA_DIR}"
    FILES "${TSC_CARLA_DIR}/LICENSE" TEXT _carla_holders)

  # The header credits pugxml (Kristen Wegner), which pugixml is based on.
  _tsc_version("${_tp}/pugixml/pugixml.hpp" "pugixml parser - version ([0-9.]+)" _pugiv)
  _tsc_leading_comment("${_tp}/pugixml/pugixml.hpp" _pugi "Arseny Kapoulkine" "Kristen Wegner")
  _tsc_component(NAME "pugixml" VERSION "${_pugiv} (vendored in LibCarla)" LICENSE "MIT"
    UPSTREAM "https://github.com/zeux/pugixml" SOURCE "${_tp}/pugixml"
    HEADER _pugi FILES "${_tp}/pugixml/LICENSE.md")

  _tsc_leading_comment("${_tp}/odrSpiral/odrSpiral.h" _odr "Copyright" "Apache License, Version 2.0")
  _tsc_component(NAME "odrSpiral" VERSION "(vendored in LibCarla)" LICENSE "Apache-2.0"
    UPSTREAM "https://www.asam.net/standards/detail/opendrive/" SOURCE "${_tp}/odrSpiral"
    HEADER _odr FILES "${_apache}")

  _tsc_leading_comment("${_tp}/moodycamel/ConcurrentQueue.h" _mc
    "Copyright" "Redistributions in binary form")
  _tsc_component(NAME "moodycamel::ConcurrentQueue" VERSION "(vendored in LibCarla)"
    LICENSE "BSD-2-Clause" UPSTREAM "https://github.com/cameron314/concurrentqueue"
    SOURCE "${_tp}/moodycamel" TEXT _mc)

  _tsc_leading_comment("${_tp}/simplify/Simplify.h" _simp "(C) by Sven Forstmann" "MIT")
  string(APPEND _simp "\n\nCopyright (c) 2014 Sven Forstmann\n\n${_mit_terms}")
  _tsc_component(NAME "Fast-Quadric-Mesh-Simplification" VERSION "(vendored in LibCarla)"
    LICENSE "MIT" UPSTREAM "https://github.com/sp4cerat/Fast-Quadric-Mesh-Simplification"
    SOURCE "${_tp}/simplify"
    TEXT _simp)

  # The vendored headers carry no notice; upstream states "The library can be
  # used under the terms of the MIT License" (Readme.md) without a copyright
  # line.
  _tsc_require_file("${_tp}/marchingcube/MeshReconstruction.h" "MeshReconstruction")
  set(_mr "Upstream: \"The library can be used under the terms of the MIT License.\"\n\nCopyright (c) the MeshReconstruction authors (https://github.com/Magnus2/MeshReconstruction)\n\n${_mit_terms}")
  _tsc_component(NAME "MeshReconstruction" VERSION "(vendored in LibCarla)" LICENSE "MIT"
    UPSTREAM "https://github.com/Magnus2/MeshReconstruction" SOURCE "${_tp}/marchingcube"
    TEXT _mr)

  # --- Boost ------------------------------------------------------------------
  _tsc_dep_dir(boost _boost)
  # The libraries CARLA requests (CMakeLists.txt sets BOOST_INCLUDE_LIBRARIES)
  # and those carla-client names directly; the closure adds their dependencies.
  set(_boost_libs ${BOOST_INCLUDE_LIBRARIES})
  get_target_property(_direct carla-client LINK_LIBRARIES)
  foreach(_lib IN LISTS _direct)
    if(_lib MATCHES "^Boost::(.*)$" AND NOT CMAKE_MATCH_1 STREQUAL "python")
      list(APPEND _boost_libs "${CMAKE_MATCH_1}")
    endif()
  endforeach()
  list(REMOVE_DUPLICATES _boost_libs)
  list(SORT _boost_libs)
  list(JOIN _boost_libs ", " _boost_libs)
  set(_boost_all ${_links})
  list(FILTER _boost_all INCLUDE REGEX "^Boost::")
  list(LENGTH _boost_all _boost_n)
  _tsc_version("${_boost}/libs/config/include/boost/version.hpp"
               "^#define BOOST_VERSION ([0-9]+)" _bv)
  math(EXPR _bmaj "${_bv} / 100000")
  math(EXPR _bmin "${_bv} / 100 % 1000")
  math(EXPR _bpat "${_bv} % 100")
  _tsc_component(NAME "Boost" VERSION "${_bmaj}.${_bmin}.${_bpat}" LICENSE "BSL-1.0"
    UPSTREAM "https://www.boost.org" SOURCE "${_boost}"
    CONTAINS "${_boost_libs} and their dependencies (${_boost_n} Boost libraries in all)"
    FILES "${_boost}/LICENSE_1_0.txt")

  # --- rpclib (CARLA's fork) --------------------------------------------------
  _tsc_dep_dir(rpclib _rpc)
  _tsc_version("${_rpc}/CMakeLists.txt" "^project\\(rpc VERSION ([0-9.]+)\\)" _rv)
  _tsc_leading_comment("${_rpc}/dependencies/include/format.h" _fmt
    "Copyright" "Redistributions in binary form")
  _tsc_leading_comment("${_rpc}/include/rpc/nonstd/optional.hpp" _opt "Copyright" "MIT")
  _tsc_leading_comment("${_rpc}/include/rpc/msgpack.hpp" _mp "Copyright" "Apache License")
  _tsc_leading_comment("${_rpc}/dependencies/include/asio.hpp" _asio
    "Copyright" "Boost Software License")
  string(JOIN "\n\n" _rpc_bundled
      "--- bundled asio (dependencies/include/asio.hpp; BSL-1.0, text in the Boost section) ---\n\n${_asio}"
      "--- bundled msgpack-c (include/rpc/msgpack.hpp, include/rpc/msgpack/; BSL-1.0, text in the Boost section, and Apache-2.0, text in the odrSpiral section) ---\n\n${_mp}"
      "--- bundled cppformat (dependencies/include/format.h, dependencies/src/format.cc, posix.cc) ---\n\n${_fmt}"
      "--- bundled optional-lite (include/rpc/nonstd/optional.hpp) ---\n\n${_opt}\n\n${_mit_grant}")
  _tsc_component(NAME "rpclib" VERSION "${_rv} (carla-simulator fork)" LICENSE "MIT"
    UPSTREAM "https://github.com/carla-simulator/rpclib" SOURCE "${_rpc}"
    CONTAINS "bundled asio (BSL-1.0), msgpack-c (BSL-1.0/Apache-2.0), cppformat (BSD-2-Clause), optional-lite (MIT)"
    FILES "${_rpc}/LICENSE.md" TEXT _rpc_bundled)

  # --- RecastNavigation (CARLA's fork) ----------------------------------------
  _tsc_dep_dir(recastnavigation _recast)
  _tsc_version("${_recast}/CMakeLists.txt" "^set\\(LIB_VERSION ([0-9.]+)\\)" _recv)
  _tsc_component(NAME "RecastNavigation" VERSION "${_recv} (carla-simulator fork)"
    LICENSE "Zlib" UPSTREAM "https://github.com/carla-simulator/recastnavigation"
    SOURCE "${_recast}" CONTAINS "Recast, Detour, DetourCrowd"
    FILES "${_recast}/License.txt")

  # --- libpng and zlib --------------------------------------------------------
  _tsc_dep_dir(libpng _png)
  _tsc_version("${_png}/png.h" "^#define PNG_LIBPNG_VER_STRING \"([0-9.]+)\"" _pngv)
  _tsc_component(NAME "libpng" VERSION "${_pngv}" LICENSE "libpng-2.0 AND Libpng"
    UPSTREAM "https://github.com/pnggroup/libpng" SOURCE "${_png}"
    FILES "${_png}/LICENSE")

  _tsc_dep_dir(zlib _zlib)
  _tsc_version("${_zlib}/zlib.h" "^#define ZLIB_VERSION \"([0-9.]+)\"" _zv)
  _tsc_component(NAME "zlib" VERSION "${_zv}" LICENSE "Zlib"
    UPSTREAM "https://zlib.net" SOURCE "${_zlib}"
    FILES "${_zlib}/LICENSE")

  set(_notices
"THIRD-PARTY NOTICES for typesafe_carla ${PROJECT_VERSION} (libcarla backend)

typesafe_carla/_native/libtypesafe_carla_ffi.so statically links the
components listed below. Their license notices follow, collected at build
time from the sources the library was built from.

<CARLA> stands for the CARLA source tree and <deps> for the directory CARLA
fetched its dependencies into.

Fetched by CARLA but not linked: Eigen (unused by the client library) and
SQLite (replaced by an empty stand-in; not linked). Boost.Python is not built.

${_tsc_index}${_tsc_body}")
  # Only touch <output> when it changes, so reconfiguring does not re-install it.
  file(WRITE "${output}.tmp" "${_notices}")
  file(COPY_FILE "${output}.tmp" "${output}" ONLY_IF_DIFFERENT)
  file(REMOVE "${output}.tmp")
  message(STATUS "typesafe_carla: wrote ${_tsc_n} third-party notices to ${output}")
endfunction()
