// Issue #19, the hand-written part: attributes (two lists from one loop) and
// the traffic sign downcast. Everything else of #19 is generated
// (bindings/actor.yaml, bindings/traffic_sign.yaml).
#include "internal.hpp"

using namespace tsc;

extern "C" {

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
