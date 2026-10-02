// Milestone 4: more actor operations, vehicle lights and traffic lights.
#include "internal.hpp"

using namespace tsc;

namespace {

carla::client::Actor &actor_of(tsc_actor_t *a) { return *check_actor(a)->actor; }

carla::client::Vehicle &vehicle_of(tsc_vehicle_t *v) {
  return static_cast<carla::client::Vehicle &>(*check_handle(v, "vehicle", TSC_KIND_VEHICLE)->actor);
}

carla::client::TrafficLight &light_of(tsc_traffic_light_t *t) {
  return static_cast<carla::client::TrafficLight &>(
      *check_handle(t, "traffic_light", TSC_KIND_TRAFFIC_LIGHT)->actor);
}

carla::rpc::TrafficLightState to_light_state(int32_t state) {
  if (state < TSC_TRAFFIC_LIGHT_RED || state > TSC_TRAFFIC_LIGHT_UNKNOWN) {
    fail(TSC_INVALID_ARGUMENT, "invalid traffic light state " + std::to_string(state));
  }
  return static_cast<carla::rpc::TrafficLightState>(state);
}

double check_duration(double seconds) {
  if (!std::isfinite(seconds) || seconds < 0.0) {
    fail(TSC_INVALID_ARGUMENT, "time must be a finite, non-negative number of seconds");
  }
  return seconds;
}

template <typename F>
tsc_status_t with_vector(const char *function, tsc_actor_t *actor, const tsc_vector3d_t *v, F &&f) {
  return guard(function, [&]() { f(actor_of(actor), to_carla_vector(*require_ptr(v, "vector"))); });
}

}  // namespace

extern "C" {

tsc_status_t tsc_actor_set_target_angular_velocity(tsc_actor_t *actor, const tsc_vector3d_t *v) {
  return with_vector(__func__, actor, v, [](auto &a, const auto &x) { a.SetTargetAngularVelocity(x); });
}

tsc_status_t tsc_actor_add_impulse(tsc_actor_t *actor, const tsc_vector3d_t *impulse) {
  return with_vector(__func__, actor, impulse, [](auto &a, const auto &x) { a.AddImpulse(x); });
}

tsc_status_t tsc_actor_add_force(tsc_actor_t *actor, const tsc_vector3d_t *force) {
  return with_vector(__func__, actor, force, [](auto &a, const auto &x) { a.AddForce(x); });
}

tsc_status_t tsc_actor_add_angular_impulse(tsc_actor_t *actor, const tsc_vector3d_t *impulse) {
  return with_vector(__func__, actor, impulse, [](auto &a, const auto &x) { a.AddAngularImpulse(x); });
}

tsc_status_t tsc_actor_add_torque(tsc_actor_t *actor, const tsc_vector3d_t *torque) {
  return with_vector(__func__, actor, torque, [](auto &a, const auto &x) { a.AddTorque(x); });
}

tsc_status_t tsc_actor_set_simulate_physics(tsc_actor_t *actor, int32_t enabled) {
  return TSC_GUARD({ actor_of(actor).SetSimulatePhysics(enabled != 0); });
}

tsc_status_t tsc_actor_set_enable_gravity(tsc_actor_t *actor, int32_t enabled) {
  return TSC_GUARD({ actor_of(actor).SetEnableGravity(enabled != 0); });
}

tsc_status_t tsc_actor_as_walker(tsc_actor_t *actor, tsc_walker_t **out) {
  return new_handle(__func__, out, [&] { return retain_as<tsc_walker>(actor, TSC_KIND_WALKER, "a walker"); });
}

tsc_status_t tsc_actor_as_walker_ai_controller(tsc_actor_t *actor, tsc_walker_ai_controller_t **out) {
  return new_handle(__func__, out, [&] {
    return retain_as<tsc_walker_ai_controller>(actor, TSC_KIND_WALKER_AI_CONTROLLER,
                                               "a walker AI controller");
  });
}

tsc_status_t tsc_actor_as_traffic_light(tsc_actor_t *actor, tsc_traffic_light_t **out) {
  return new_handle(__func__, out, [&] {
    return retain_as<tsc_traffic_light>(actor, TSC_KIND_TRAFFIC_LIGHT, "a traffic light");
  });
}

tsc_status_t tsc_vehicle_set_light_state(tsc_vehicle_t *vehicle, uint32_t light_state) {
  return TSC_GUARD({
    vehicle_of(vehicle).SetLightState(
        static_cast<carla::client::Vehicle::LightState>(light_state));
  });
}

tsc_status_t tsc_vehicle_get_light_state(tsc_vehicle_t *vehicle, uint32_t *out) {
  return TSC_GUARD({
    *require_ptr(out, "out") = static_cast<uint32_t>(vehicle_of(vehicle).GetLightState());
  });
}

tsc_status_t tsc_vehicle_get_speed_limit(tsc_vehicle_t *vehicle, double *out) {
  return TSC_GUARD({ *require_ptr(out, "out") = vehicle_of(vehicle).GetSpeedLimit(); });
}

tsc_status_t tsc_vehicle_get_traffic_light_state(tsc_vehicle_t *vehicle, int32_t *out) {
  return TSC_GUARD({
    *require_ptr(out, "out") = static_cast<int32_t>(vehicle_of(vehicle).GetTrafficLightState());
  });
}

tsc_status_t tsc_vehicle_is_at_traffic_light(tsc_vehicle_t *vehicle, int32_t *out) {
  return TSC_GUARD({ *require_ptr(out, "out") = vehicle_of(vehicle).IsAtTrafficLight() ? 1 : 0; });
}

tsc_status_t tsc_vehicle_get_traffic_light(tsc_vehicle_t *vehicle, tsc_traffic_light_t **out) {
  return new_handle(__func__, out, [&]() -> tsc_traffic_light * {
    auto light = vehicle_of(vehicle).GetTrafficLight();
    return light == nullptr ? nullptr : new tsc_traffic_light(std::move(light));
  });
}

tsc_status_t tsc_traffic_light_get_info(tsc_traffic_light_t *light, tsc_traffic_light_info_t *out) {
  return TSC_GUARD({
    require_ptr(out, "out");
    auto &t = light_of(light);
    tsc_traffic_light_info_t info{};
    info.state = static_cast<int32_t>(t.GetState());
    info.is_frozen = t.IsFrozen() ? 1 : 0;
    info.green_time = t.GetGreenTime();
    info.yellow_time = t.GetYellowTime();
    info.red_time = t.GetRedTime();
    info.elapsed_time = t.GetElapsedTime();
    info.pole_index = t.GetPoleIndex();
    *out = info;
  });
}

tsc_status_t tsc_traffic_light_set_state(tsc_traffic_light_t *light, int32_t state) {
  return TSC_GUARD({ light_of(light).SetState(to_light_state(state)); });
}

tsc_status_t tsc_traffic_light_set_green_time(tsc_traffic_light_t *light, double t) {
  return TSC_GUARD({ light_of(light).SetGreenTime(static_cast<float>(check_duration(t))); });
}

tsc_status_t tsc_traffic_light_set_yellow_time(tsc_traffic_light_t *light, double t) {
  return TSC_GUARD({ light_of(light).SetYellowTime(static_cast<float>(check_duration(t))); });
}

tsc_status_t tsc_traffic_light_set_red_time(tsc_traffic_light_t *light, double t) {
  return TSC_GUARD({ light_of(light).SetRedTime(static_cast<float>(check_duration(t))); });
}

tsc_status_t tsc_traffic_light_freeze(tsc_traffic_light_t *light, int32_t freeze) {
  return TSC_GUARD({ light_of(light).Freeze(freeze != 0); });
}

tsc_status_t tsc_traffic_light_reset_group(tsc_traffic_light_t *light) {
  return TSC_GUARD({ light_of(light).ResetGroup(); });
}

}  // extern "C"
