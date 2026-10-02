// Actors: the hand-written part. Downcasts (tsc_actor_as_<kind>) are handle
// conversions, not LibCarla calls; the traffic light info and the attributes
// combine several calls. Everything else is generated (bindings/actor.yaml,
// vehicle.yaml, traffic_light.yaml, traffic_sign.yaml...).
#include "internal.hpp"

using namespace tsc;

extern "C" {

tsc_status_t tsc_actor_as_vehicle(tsc_actor_t *actor, tsc_vehicle_t **out_vehicle) {
  return TSC_GUARD({
    require_ptr(out_vehicle, "out_vehicle");
    *out_vehicle = nullptr;
    *out_vehicle = retain_as<tsc_vehicle>(actor, TSC_KIND_VEHICLE, "a vehicle");
  });
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

tsc_status_t tsc_actor_get_attributes(tsc_actor_t *actor, tsc_string_list_t *out_ids,
                                      tsc_string_list_t *out_values) {
  return TSC_GUARD({
    require_ptr(out_ids, "out_ids");
    require_ptr(out_values, "out_values");
    *out_ids = tsc_string_list_t{};
    *out_values = tsc_string_list_t{};
    std::vector<std::string> ids, values;
    for (const auto &attribute : actor_of(actor).GetAttributes()) {
      ids.push_back(attribute.GetId());
      values.push_back(attribute.GetValue());
    }
    string_list_assign(out_ids, ids);
    try {
      string_list_assign(out_values, values);
    } catch (...) {
      tsc_string_list_free(out_ids);
      throw;
    }
  });
}

tsc_status_t tsc_actor_as_traffic_sign(tsc_actor_t *actor, tsc_traffic_sign_t **out) {
  return new_handle(__func__, out, [&] {  // a traffic light is a traffic sign
    const bool light = check_actor(actor)->kind == TSC_KIND_TRAFFIC_LIGHT;
    return retain_as<tsc_traffic_sign>(actor, light ? TSC_KIND_TRAFFIC_LIGHT : TSC_KIND_TRAFFIC_SIGN,
                                       "a traffic sign");
  });
}

}  // extern "C"
