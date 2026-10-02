// Milestone 4: actor downcasts, traffic light queries. The simple actor,
// vehicle and traffic light operations are generated (bindings/*.yaml).
#include "internal.hpp"

using namespace tsc;

extern "C" {

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

}  // extern "C"
