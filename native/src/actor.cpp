#include "internal.hpp"

using namespace tsc;

namespace {

carla::client::Actor &actor_of(tsc_actor_t *a) { return *check_actor(a)->actor; }

}  // namespace

extern "C" {

tsc_status_t tsc_actor_get_id(tsc_actor_t *actor, uint32_t *out_id) {
  return TSC_GUARD({ *require_ptr(out_id, "out_id") = actor_of(actor).GetId(); });
}

tsc_status_t tsc_actor_get_type_id(tsc_actor_t *actor, tsc_string_t *out) {
  return TSC_GUARD({ string_assign(out, actor_of(actor).GetTypeId()); });
}

tsc_status_t tsc_actor_is_alive(tsc_actor_t *actor, int32_t *out_alive) {
  return TSC_GUARD({ *require_ptr(out_alive, "out_alive") = actor_of(actor).IsAlive() ? 1 : 0; });
}

tsc_status_t tsc_actor_get_transform(tsc_actor_t *actor, tsc_transform_t *out) {
  return TSC_GUARD({ *require_ptr(out, "out") = from_carla(actor_of(actor).GetTransform()); });
}

tsc_status_t tsc_actor_set_transform(tsc_actor_t *actor, const tsc_transform_t *transform) {
  return TSC_GUARD({ actor_of(actor).SetTransform(to_carla(*require_ptr(transform, "transform"))); });
}

tsc_status_t tsc_actor_get_location(tsc_actor_t *actor, tsc_location_t *out) {
  return TSC_GUARD({ *require_ptr(out, "out") = from_carla(actor_of(actor).GetLocation()); });
}

tsc_status_t tsc_actor_set_location(tsc_actor_t *actor, const tsc_location_t *location) {
  return TSC_GUARD({ actor_of(actor).SetLocation(to_carla(*require_ptr(location, "location"))); });
}

tsc_status_t tsc_actor_get_velocity(tsc_actor_t *actor, tsc_vector3d_t *out) {
  return TSC_GUARD({ *require_ptr(out, "out") = from_carla(actor_of(actor).GetVelocity()); });
}

tsc_status_t tsc_actor_set_target_velocity(tsc_actor_t *actor, const tsc_vector3d_t *velocity) {
  return TSC_GUARD({
    actor_of(actor).SetTargetVelocity(to_carla_vector(*require_ptr(velocity, "velocity")));
  });
}

tsc_status_t tsc_actor_get_acceleration(tsc_actor_t *actor, tsc_vector3d_t *out) {
  return TSC_GUARD({ *require_ptr(out, "out") = from_carla(actor_of(actor).GetAcceleration()); });
}

tsc_status_t tsc_actor_get_angular_velocity(tsc_actor_t *actor, tsc_vector3d_t *out) {
  return TSC_GUARD({ *require_ptr(out, "out") = from_carla(actor_of(actor).GetAngularVelocity()); });
}

tsc_status_t tsc_actor_destroy(tsc_actor_t *actor, int32_t *out_destroyed) {
  return TSC_GUARD({
    const bool destroyed = actor_of(actor).Destroy();
    if (out_destroyed != nullptr) *out_destroyed = destroyed ? 1 : 0;
  });
}

tsc_status_t tsc_actor_get_bounding_box(tsc_actor_t *actor, tsc_bounding_box_t *out) {
  return TSC_GUARD({
    const carla::geom::BoundingBox &b = actor_of(actor).GetBoundingBox();
    *require_ptr(out, "out") =
        tsc_bounding_box_t{from_carla(b.location), from_carla(b.extent), from_carla(b.rotation)};
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
