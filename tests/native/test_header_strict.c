/* Compile-only: ffi.h must stay valid under strict C99, C11 and C++17
 * (-pedantic-errors), including tsc_command_t's anonymous union. Built by the
 * header_strict_* CTest entries (CMakeLists.txt). */
#include "typesafe_carla/ffi.h"

int tsc_header_strict_check(const tsc_actor_blueprint_t *bp,
                            const tsc_vehicle_physics_control_t *pc);

int tsc_header_strict_check(const tsc_actor_blueprint_t *bp,
                            const tsc_vehicle_physics_control_t *pc) {
  tsc_command_t cmd;
  cmd.blueprint = bp;
  cmd.physics_control = pc;
  return cmd.physics_control != 0 && sizeof(cmd) == 152;
}
