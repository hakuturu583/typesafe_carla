// Issue #21: the light manager (World::GetLightManager). Lights cross the C
// ABI as ids plus their location; operations go through the manager by id,
// which is what LibCarla's client::Light does too.
#include "internal.hpp"

#include <unordered_set>

using namespace tsc;

namespace {

using LightGroup = carla::rpc::LightState::LightGroup;
using Manager = carla::client::LightManager;

LightGroup to_group(int32_t group) {
  if (group < TSC_LIGHT_GROUP_NONE || group > TSC_LIGHT_GROUP_OTHER) {
    fail(TSC_INVALID_ARGUMENT, "invalid light group " + std::to_string(group));
  }
  // tsc_light_group_t has rpc::LightState::LightGroup's order.
  return static_cast<LightGroup>(group);
}

carla::client::Color to_light_color(const tsc_color_t &c) {
  return carla::client::Color(c.r, c.g, c.b, c.a);
}

float to_intensity(double intensity) { return check_finite(intensity, "intensity"); }

carla::client::LightState to_client_light_state(const tsc_light_state_t &s) {
  return carla::client::LightState(to_intensity(s.intensity), to_light_color(s.color),
                                   to_group(s.group), s.active != 0);
}

tsc_light_state_t from_client_light_state(const carla::client::LightState &s) {
  tsc_light_state_t r{};
  r.intensity = s._intensity;
  r.color = tsc_color_t{s._color.r, s._color.g, s._color.b, s._color.a};
  r.group = static_cast<int32_t>(s._group);
  r.active = s._active ? 1 : 0;
  return r;
}

// LibCarla answers an unknown id with a shared placeholder state (and, for the
// setters, modifies it): reject unknown ids before touching anything.
// GetAllLights() (like the getters) reads the manager's light map without its
// lock while LibCarla's tick thread may update it: a data race inside
// LibCarla, which the official Python API has too.
void check_ids(const carla::client::LightManager &m, const uint32_t *ids, size_t count) {
  require_array(ids, count, "ids");
  std::unordered_set<uint32_t> known;
  for (const auto &light : m.GetAllLights()) known.insert(light.GetId());
  for (size_t i = 0; i < count; ++i) {
    if (known.count(ids[i]) == 0) fail(TSC_NOT_FOUND, "no light with id " + std::to_string(ids[i]));
  }
}

// Shared body of the bulk setters: convert(values[i]) goes to ids[i]. Every
// id and value is checked first, so a failing call changes no light.
template <typename T, typename Convert, typename Apply>
tsc_status_t set_each(const char *function, tsc_light_manager_t *manager, const uint32_t *ids,
                      size_t count, const T *values, Convert &&convert, Apply &&apply) noexcept {
  return guard(function, [&] {
    auto &m = light_manager_of(manager);
    check_ids(m, ids, count);
    require_array(values, count, "values");
    std::vector<decltype(convert(values[0]))> converted;
    converted.reserve(count);
    for (size_t i = 0; i < count; ++i) converted.push_back(convert(values[i]));
    for (size_t i = 0; i < count; ++i) apply(m, ids[i], converted[i]);
  });
}

}  // namespace

extern "C" {

void tsc_light_list_free(tsc_light_list_t *list) { tsc::free_list_items(list); }

tsc_status_t tsc_light_manager_get_light_states(tsc_light_manager_t *manager, const uint32_t *ids,
                                                size_t count, tsc_light_state_t *out) {
  return TSC_GUARD({
    auto &m = light_manager_of(manager);
    check_ids(m, ids, count);
    require_array(out, count, "out");
    for (size_t i = 0; i < count; ++i) out[i] = from_client_light_state(m.GetLightState(ids[i]));
  });
}

tsc_status_t tsc_light_manager_set_active(tsc_light_manager_t *manager, const uint32_t *ids,
                                          size_t count, const int32_t *active) {
  return set_each(
      __func__, manager, ids, count, active, [](int32_t on) { return on != 0; },
      [](Manager &m, uint32_t id, bool on) { m.SetActive(id, on); });
}

tsc_status_t tsc_light_manager_set_color(tsc_light_manager_t *manager, const uint32_t *ids,
                                         size_t count, const tsc_color_t *colors) {
  return set_each(
      __func__, manager, ids, count, colors, to_light_color,
      [](Manager &m, uint32_t id, const carla::client::Color &c) { m.SetColor(id, c); });
}

tsc_status_t tsc_light_manager_set_intensity(tsc_light_manager_t *manager, const uint32_t *ids,
                                             size_t count, const double *intensities) {
  return set_each(__func__, manager, ids, count, intensities, to_intensity,
                  [](Manager &m, uint32_t id, float v) { m.SetIntensity(id, v); });
}

tsc_status_t tsc_light_manager_set_light_group(tsc_light_manager_t *manager, const uint32_t *ids,
                                               size_t count, const int32_t *groups) {
  return set_each(__func__, manager, ids, count, groups, to_group,
                  [](Manager &m, uint32_t id, LightGroup g) { m.SetLightGroup(id, g); });
}

tsc_status_t tsc_light_manager_set_light_state(tsc_light_manager_t *manager, const uint32_t *ids,
                                               size_t count, const tsc_light_state_t *states) {
  return set_each(
      __func__, manager, ids, count, states, to_client_light_state,
      [](Manager &m, uint32_t id, const carla::client::LightState &s) { m.SetLightState(id, s); });
}

}  // extern "C"
