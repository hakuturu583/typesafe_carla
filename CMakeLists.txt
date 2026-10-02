cmake_minimum_required(VERSION 3.27.2)  # CARLA UE5 requires 3.27.2

# The version lives in python/typesafe_carla/__init__.py (single source of truth).
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/python/typesafe_carla/__init__.py" _tsc_version_line
     REGEX "^__version__ = ")
string(REGEX REPLACE "^__version__ = \"([0-9]+\\.[0-9]+\\.[0-9]+)[^\"]*\"$" "\\1"
       TSC_VERSION "${_tsc_version_line}")
project(typesafe_carla VERSION ${TSC_VERSION} LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)  # LibCarla UE5 headers require C++20
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_VISIBILITY_INLINES_HIDDEN ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release)
endif()

# ---------------------------------------------------------------------------
# Backend selection
#
#   libcarla : LibCarla's client library built from CARLA UE5 sources, fetched
#              at CARLA_GIT_REF (default: ue5-dev) or taken from
#              CARLA_SOURCE_DIR. See cmake/FetchCarla.cmake.
#   mock     : in-memory stand-in for LibCarla (native/mock); for tests and
#              development without a CARLA server. Not a simulator.
#
# The selected backend is reported at runtime by tsc_backend_name() and by
# `typesafe-codon info`.
# ---------------------------------------------------------------------------
if(DEFINED ENV{TSC_BACKEND} AND NOT "$ENV{TSC_BACKEND}" STREQUAL "")
  set(_tsc_backend_default "$ENV{TSC_BACKEND}")
else()
  set(_tsc_backend_default "libcarla")
endif()
set(TSC_BACKEND "${_tsc_backend_default}" CACHE STRING "Native backend: libcarla or mock")
set_property(CACHE TSC_BACKEND PROPERTY STRINGS libcarla mock)
if(NOT TSC_BACKEND MATCHES "^(libcarla|mock)$")
  message(FATAL_ERROR "Unknown TSC_BACKEND '${TSC_BACKEND}' (expected libcarla or mock)")
endif()
message(STATUS "typesafe_carla backend: ${TSC_BACKEND}")

# ---------------------------------------------------------------------------
# Build metadata
# ---------------------------------------------------------------------------
set(TSC_BUILD_COMMIT "unknown")
find_package(Git QUIET)
if(GIT_FOUND AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/.git")
  execute_process(
    COMMAND "${GIT_EXECUTABLE}" rev-parse --short=12 HEAD
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    OUTPUT_VARIABLE _commit OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET
    RESULT_VARIABLE _commit_result)
  if(_commit_result EQUAL 0 AND _commit)
    set(TSC_BUILD_COMMIT "${_commit}")
  endif()
endif()

# ---------------------------------------------------------------------------
# The C ABI shim
# ---------------------------------------------------------------------------
add_library(typesafe_carla_ffi SHARED
  native/src/actor.cpp
  native/src/actor_ext.cpp
  native/src/batch.cpp
  native/src/blueprint.cpp
  native/src/client.cpp
  native/src/client_ext.cpp
  native/src/error.cpp
  native/src/handle.cpp
  native/src/map.cpp
  native/src/map_ext.cpp
  native/src/sensor.cpp
  native/src/snapshot.cpp
  native/src/traffic_manager.cpp
  native/src/vehicle.cpp
  native/src/walker.cpp
  native/src/world.cpp
  native/src/world_ext.cpp
)
target_include_directories(typesafe_carla_ffi
  PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/native/include"
  PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/native/src")
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(typesafe_carla_ffi PRIVATE -Wall -Wextra)
  # Export only the tsc_* C ABI; keep LibCarla, Boost and libstdc++ template
  # instantiations out of the dynamic symbol table.
  target_link_options(typesafe_carla_ffi PRIVATE
    "-Wl,--version-script=${CMAKE_CURRENT_SOURCE_DIR}/native/src/exports.map"
    -Wl,--no-undefined)
  set_property(TARGET typesafe_carla_ffi APPEND PROPERTY LINK_DEPENDS
               "${CMAKE_CURRENT_SOURCE_DIR}/native/src/exports.map")
endif()
# Dependencies are found next to the library ($ORIGIN), never in /usr/lib.
set_target_properties(typesafe_carla_ffi PROPERTIES
  BUILD_RPATH "$ORIGIN"
  INSTALL_RPATH "$ORIGIN")

if(TSC_BACKEND STREQUAL "mock")
  set(TSC_CARLA_RESOLVED_REF "mock")
  set(TSC_CARLA_COMMIT "mock")
  add_library(carla_mock STATIC native/mock/src/mock.cpp native/mock/src/weather.cpp)
  target_include_directories(carla_mock PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/native/mock/include")
  target_link_libraries(typesafe_carla_ffi PRIVATE carla_mock)
else()
  include(cmake/FetchCarla.cmake)

  # Client library only: no server, Unreal, Python API, examples or tests.
  # (CARLA sets policy CMP0077, so these normal variables override its options.)
  set(BUILD_CARLA_CLIENT ON)
  set(BUILD_CARLA_SERVER OFF)
  set(BUILD_CARLA_UNREAL OFF)
  set(BUILD_PYTHON_API OFF)
  set(BUILD_EXAMPLES OFF)
  set(BUILD_LIBCARLA_TESTS OFF)
  set(BUILD_OSM_WORLD_RENDERER OFF)
  set(ENABLE_ROS2 OFF)
  set(ENABLE_ROS2_DEMO OFF)
  set(ENABLE_OSM2ODR OFF)
  set(ENABLE_RSS OFF)
  set(ENABLE_PYTORCH OFF)
  set(ENABLE_STREETMAP OFF)
  set(ENABLE_ALL_WARNINGS OFF)
  set(ENABLE_RTTI ON)        # Actor::as_vehicle relies on dynamic_pointer_cast
  set(ENABLE_EXCEPTIONS ON)  # errors are caught and converted at the C ABI

  # CARLA unconditionally downloads SQLite for its server-side tools, but
  # carla-client never links it. Point FetchContent at an empty stand-in so the
  # build does not depend on sqlite.org (override by setting
  # FETCHCONTENT_SOURCE_DIR_SQLITE3 yourself). If a future CARLA makes the
  # client use SQLite, the link fails loudly rather than silently.
  if(NOT FETCHCONTENT_SOURCE_DIR_SQLITE3)
    set(_tsc_sqlite_stub "${CMAKE_BINARY_DIR}/_deps/sqlite3-unused")
    file(MAKE_DIRECTORY "${_tsc_sqlite_stub}")
    foreach(_f sqlite3.h sqlite3.c)
      file(WRITE "${_tsc_sqlite_stub}/${_f}" "/* unused by carla-client */\n")
    endforeach()
    file(WRITE "${_tsc_sqlite_stub}/shell.c" "int main(void) { return 1; }\n")
    set(FETCHCONTENT_SOURCE_DIR_SQLITE3 "${_tsc_sqlite_stub}")
  endif()
  # Boost.Python. CARLA requests it from Boost even with BUILD_PYTHON_API=OFF,
  # which Boost rejects ("library 'python' has been explicitly requested, but
  # BOOST_ENABLE_PYTHON is OFF"), and some refs (0.10.0) link carla-client
  # against Boost::python unconditionally. The client sources only use
  # Boost.Python under LIBCARLA_WITH_PYTHON_SUPPORT, which is never defined
  # here, so: build Boost without it, and satisfy any such link with an empty
  # target (unused on refs that do not link it). No Python headers or
  # libpython are needed (manylinux has no libpython). CARLA requires
  # CMake >= 3.27 (CMP0126 NEW), so the normal variable shadows the cache
  # entry CARLA's Dependencies.cmake writes.
  file(READ "${TSC_CARLA_DIR}/CMake/Dependencies.cmake" _tsc_deps)
  if(_tsc_deps MATCHES "set \\([ \t\r\n]*BOOST_INCLUDED_PROJECTS([^)]*)\\)")
    string(REGEX REPLACE "[ \t\r\n]+" ";" _tsc_boost_libs "${CMAKE_MATCH_1}")
    list(FILTER _tsc_boost_libs EXCLUDE REGEX "^(python|)$")
    set(BOOST_INCLUDE_LIBRARIES "${_tsc_boost_libs}")
    message(STATUS "typesafe_carla: Boost libraries for LibCarla: ${_tsc_boost_libs}")
  else()
    message(WARNING "typesafe_carla: could not read CARLA's Boost library list; "
                    "configuring Boost may fail on BOOST_ENABLE_PYTHON")
  endif()
  if(NOT TARGET Boost::python)
    add_library(Boost::python INTERFACE IMPORTED)
  endif()

  # CARLA 0.10.0's carla/client/FileTransfer.h uses uint8_t without including
  # <cstdint>; newer GCC (14, in the manylinux release image) no longer
  # provides it transitively. Force-include it for CARLA and for the shim
  # (which includes that header through Client.h). Harmless for other refs.
  if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    set(_tsc_include_cstdint "$<$<COMPILE_LANGUAGE:CXX>:SHELL:-include cstdint>")
    add_compile_options("${_tsc_include_cstdint}")
    target_compile_options(typesafe_carla_ffi PRIVATE "${_tsc_include_cstdint}")
  endif()

  # CMake 4 removed compatibility with cmake_minimum_required < 3.5, which some
  # of CARLA's dependencies (e.g. libpng in 0.10.0) still declare. ue5-dev sets
  # this itself; older refs need it from us.
  if(CMAKE_VERSION VERSION_GREATER_EQUAL "4.0" AND NOT DEFINED CMAKE_POLICY_VERSION_MINIMUM)
    set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
  endif()

  add_subdirectory("${TSC_CARLA_DIR}" "${CMAKE_BINARY_DIR}/carla" EXCLUDE_FROM_ALL SYSTEM)
  if(NOT TARGET carla-client)
    message(FATAL_ERROR "CARLA at ${TSC_CARLA_DIR} did not define the carla-client target")
  endif()
  target_link_libraries(typesafe_carla_ffi PRIVATE carla-client)
endif()

target_compile_definitions(typesafe_carla_ffi PRIVATE
  TSC_BUILD_COMMIT="${TSC_BUILD_COMMIT}"
  TSC_CARLA_GIT_REF="${TSC_CARLA_RESOLVED_REF}"
  TSC_CARLA_GIT_COMMIT="${TSC_CARLA_COMMIT}")

# ---------------------------------------------------------------------------
# Tests (plain C client of the ABI)
# ---------------------------------------------------------------------------
option(TSC_BUILD_TESTS "Build native tests" ON)
if(TSC_BUILD_TESTS AND NOT SKBUILD)
  enable_testing()
  add_executable(test_ffi tests/native/test_ffi.c)
  target_link_libraries(test_ffi PRIVATE typesafe_carla_ffi)
  if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(test_ffi PRIVATE -Wall -Wextra -Werror -std=c99)
  endif()
  find_package(Threads REQUIRED)
  target_link_libraries(test_ffi PRIVATE Threads::Threads)
  add_test(NAME test_ffi COMMAND test_ffi)
  set_tests_properties(test_ffi PROPERTIES LABELS "${TSC_BACKEND}")
endif()

# ---------------------------------------------------------------------------
# Wheel layout (scikit-build-core installs only the "typesafe_carla" component,
# so CARLA's own install rules never end up in the wheel)
#
#   typesafe_carla/_native/libtypesafe_carla_ffi.so
#   typesafe_carla/_native/LICENSE.CARLA
#   typesafe_carla/_codon/typesafe_carla/*.codon
# ---------------------------------------------------------------------------
install(TARGETS typesafe_carla_ffi
        LIBRARY DESTINATION typesafe_carla/_native COMPONENT typesafe_carla)
install(DIRECTORY codon/typesafe_carla DESTINATION typesafe_carla/_codon
        COMPONENT typesafe_carla FILES_MATCHING PATTERN "*.codon")
# What this wheel was built from; read by `typesafe-codon info` and
# typesafe_carla.paths.build_info() without loading the library.
if(TSC_BACKEND STREQUAL "libcarla")
  file(STRINGS "${TSC_CARLA_DIR}/CMakeLists.txt" _tsc_carla_v REGEX "set \\(CARLA_VERSION_(MAJOR|MINOR|PATCH) [0-9]+\\)")
  string(REGEX REPLACE ".*MAJOR ([0-9]+).*MINOR ([0-9]+).*PATCH ([0-9]+).*" "\\1.\\2.\\3"
         TSC_CARLA_VERSION "${_tsc_carla_v}")
else()
  set(TSC_CARLA_VERSION "mock")
endif()
file(WRITE "${CMAKE_BINARY_DIR}/BUILD_INFO.json" "{
  \"typesafe_carla\": \"${PROJECT_VERSION}\",
  \"build_commit\": \"${TSC_BUILD_COMMIT}\",
  \"backend\": \"${TSC_BACKEND}\",
  \"carla_version\": \"${TSC_CARLA_VERSION}\",
  \"carla_git_ref\": \"${TSC_CARLA_RESOLVED_REF}\",
  \"carla_git_commit\": \"${TSC_CARLA_COMMIT}\"
}
")
install(FILES "${CMAKE_BINARY_DIR}/BUILD_INFO.json" DESTINATION typesafe_carla/_native
        COMPONENT typesafe_carla)

if(TSC_BACKEND STREQUAL "libcarla")
  # LibCarla (MIT) is linked statically; ship its license notice.
  install(FILES "${TSC_CARLA_DIR}/LICENSE" DESTINATION typesafe_carla/_native
          RENAME LICENSE.CARLA COMPONENT typesafe_carla)
endif()
