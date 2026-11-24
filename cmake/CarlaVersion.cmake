# The version a CARLA checkout declares in its top-level CMakeLists.txt.
#
#   tsc_carla_version(<CMakeLists.txt> <out var>)
#
# Reads `set (CARLA_VERSION_MAJOR <n>)`, `..._MINOR` and, when there is one,
# `..._PATCH`: CARLA 0.10.0 declares all three ("0.10.0"), CARLA 1.0 only the
# first two ("1.0", as CARLA itself spells it). Each is read on its own, so a
# missing PATCH gives MAJOR.MINOR rather than the matched lines themselves (a
# regex over the three joined left typesafe_carla 0.8.7's 1.0 ref recorded as
# "set (CARLA_VERSION_MAJOR 1);set (CARLA_VERSION_MINOR 0)"). A file declaring
# no MAJOR or no MINOR fails the configure step.
function(tsc_carla_version cmakelists out_var)
  file(STRINGS "${cmakelists}" _lines REGEX "^[ \t]*set[ \t]*\\([ \t]*CARLA_VERSION_(MAJOR|MINOR|PATCH)[ \t]+[0-9]+[ \t]*\\)")
  foreach(_part MAJOR MINOR PATCH)
    set(_${_part} "")
    foreach(_line IN LISTS _lines)
      if(_line MATCHES "CARLA_VERSION_${_part}[ \t]+([0-9]+)")
        set(_${_part} "${CMAKE_MATCH_1}")
      endif()
    endforeach()
  endforeach()
  if(_MAJOR STREQUAL "" OR _MINOR STREQUAL "")
    message(FATAL_ERROR "${cmakelists} declares no CARLA_VERSION_MAJOR and CARLA_VERSION_MINOR")
  endif()
  set(_version "${_MAJOR}.${_MINOR}")
  if(NOT _PATCH STREQUAL "")
    string(APPEND _version ".${_PATCH}")
  endif()
  set(${out_var} "${_version}" PARENT_SCOPE)
endfunction()
