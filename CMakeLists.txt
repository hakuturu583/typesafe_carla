cmake_minimum_required(VERSION 3.20)
project(typesafe_carla VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_VISIBILITY_INLINES_HIDDEN ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

# ---------------------------------------------------------------------------
# Backend selection
#
#   libcarla : link against a LibCarla client build (set LIBCARLA_ROOT to a
#              directory with include/ and lib/, e.g. the
#              PythonAPI/carla/dependencies directory of a CARLA source build).
#   mock     : in-memory stand-in for LibCarla (native/mock); for tests and
#              development without a CARLA server. Not a simulator.
#   auto     : libcarla if LIBCARLA_ROOT is usable, otherwise mock.
#
# The selected backend is reported at runtime by tsc_backend_name() and by
# `typesafe-codon info`.
# ---------------------------------------------------------------------------
set(TSC_BACKEND "auto" CACHE STRING "Native backend: auto, libcarla or mock")
set_property(CACHE TSC_BACKEND PROPERTY STRINGS auto libcarla mock)
set(LIBCARLA_ROOT "$ENV{LIBCARLA_ROOT}" CACHE PATH "LibCarla install prefix (include/, lib/)")

set(_libcarla_usable OFF)
if(LIBCARLA_ROOT AND EXISTS "${LIBCARLA_ROOT}/include/carla/client/Client.h")
  set(_libcarla_usable ON)
endif()

if(TSC_BACKEND STREQUAL "auto")
  if(_libcarla_usable)
    set(TSC_BACKEND_RESOLVED libcarla)
  else()
    set(TSC_BACKEND_RESOLVED mock)
    message(WARNING
      "typesafe_carla: LIBCARLA_ROOT not set or not a LibCarla install; building the MOCK "
      "backend. The resulting library talks to an in-memory fake, not a CARLA server. "
      "Set LIBCARLA_ROOT or -DTSC_BACKEND=libcarla to build the real backend.")
  endif()
elseif(TSC_BACKEND STREQUAL "libcarla")
  if(NOT _libcarla_usable)
    message(FATAL_ERROR "TSC_BACKEND=libcarla but LIBCARLA_ROOT='${LIBCARLA_ROOT}' does not "
                        "contain include/carla/client/Client.h")
  endif()
  set(TSC_BACKEND_RESOLVED libcarla)
elseif(TSC_BACKEND STREQUAL "mock")
  set(TSC_BACKEND_RESOLVED mock)
else()
  message(FATAL_ERROR "Unknown TSC_BACKEND '${TSC_BACKEND}' (expected auto, libcarla or mock)")
endif()
message(STATUS "typesafe_carla backend: ${TSC_BACKEND_RESOLVED}")

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
  native/src/blueprint.cpp
  native/src/client.cpp
  native/src/error.cpp
  native/src/handle.cpp
  native/src/vehicle.cpp
  native/src/world.cpp
)
target_include_directories(typesafe_carla_ffi
  PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/native/include"
  PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/native/src")
target_compile_definitions(typesafe_carla_ffi PRIVATE TSC_BUILD_COMMIT="${TSC_BUILD_COMMIT}")
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(typesafe_carla_ffi PRIVATE -Wall -Wextra)
endif()
# Dependencies are found next to the library ($ORIGIN), never in /usr/lib.
set_target_properties(typesafe_carla_ffi PROPERTIES
  BUILD_RPATH "$ORIGIN"
  INSTALL_RPATH "$ORIGIN")

if(TSC_BACKEND_RESOLVED STREQUAL "mock")
  add_library(carla_mock STATIC native/mock/src/mock.cpp)
  target_include_directories(carla_mock PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/native/mock/include")
  target_link_libraries(typesafe_carla_ffi PRIVATE carla_mock)
else()
  # LibCarla client and the static dependencies CARLA's build places next to it.
  find_package(Threads REQUIRED)
  target_include_directories(typesafe_carla_ffi SYSTEM PRIVATE "${LIBCARLA_ROOT}/include")
  set(_libcarla_libs carla_client rpc boost_filesystem Recast Detour DetourCrowd png z)
  set(_libcarla_paths)
  foreach(_lib IN LISTS _libcarla_libs)
    find_library(_path_${_lib} NAMES ${_lib} PATHS "${LIBCARLA_ROOT}/lib" NO_DEFAULT_PATH)
    if(NOT _path_${_lib})
      find_library(_path_${_lib} NAMES ${_lib})
    endif()
    if(NOT _path_${_lib})
      message(FATAL_ERROR "LibCarla dependency '${_lib}' not found under ${LIBCARLA_ROOT}/lib")
    endif()
    list(APPEND _libcarla_paths "${_path_${_lib}}")
  endforeach()
  target_compile_definitions(typesafe_carla_ffi PRIVATE
    BOOST_ERROR_CODE_HEADER_ONLY LIBCARLA_WITH_PYTHON_SUPPORT=0)
  target_link_libraries(typesafe_carla_ffi PRIVATE ${_libcarla_paths} Threads::Threads)
endif()

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
  set_tests_properties(test_ffi PROPERTIES LABELS "${TSC_BACKEND_RESOLVED}")
endif()

# ---------------------------------------------------------------------------
# Wheel layout (scikit-build-core)
#
#   typesafe_carla/_native/libtypesafe_carla_ffi.so
#   typesafe_carla/_codon/typesafe_carla/*.codon
# ---------------------------------------------------------------------------
if(SKBUILD)
  install(TARGETS typesafe_carla_ffi LIBRARY DESTINATION typesafe_carla/_native)
  install(DIRECTORY codon/typesafe_carla DESTINATION typesafe_carla/_codon
          FILES_MATCHING PATTERN "*.codon")
endif()
