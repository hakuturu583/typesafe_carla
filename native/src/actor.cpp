#include "internal.hpp"

using namespace tsc;

extern "C" {

tsc_status_t tsc_actor_destroy(tsc_actor_t *actor, int32_t *out_destroyed) {
  return TSC_GUARD({
    const bool destroyed = actor_of(actor).Destroy();
    if (out_destroyed != nullptr) *out_destroyed = destroyed ? 1 : 0;
  });
}

tsc_status_t tsc_actor_as_vehicle(tsc_actor_t *actor, tsc_vehicle_t **out_vehicle) {
  return TSC_GUARD({
    require_ptr(out_vehicle, "out_vehicle");
    *out_vehicle = nullptr;
    *out_vehicle = retain_as<tsc_vehicle>(actor, TSC_KIND_VEHICLE, "a vehicle");
  });
}

}  // extern "C"
