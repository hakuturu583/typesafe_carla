// World: the hand-written part (not-found errors naming the id, settings
// read-modify-write, actor list accessors, weather presets). The rest is
// generated (bindings/world.yaml, debug.yaml).
#include "internal.hpp"

#include <array>
#include <utility>

using namespace tsc;
using carla::rpc::WeatherParameters;

extern "C" {

tsc_status_t tsc_world_get_actor(tsc_world_t *world, uint32_t actor_id, tsc_actor_t **out_actor) {
  return TSC_GUARD({
    require_ptr(out_actor, "out_actor");
    *out_actor = nullptr;
    auto actor = world_of(world).GetActor(actor_id);
    if (actor == nullptr) fail(TSC_NOT_FOUND, "no actor with id " + std::to_string(actor_id));
    *out_actor = make_actor_handle(actor);
  });
}

tsc_status_t tsc_world_get_settings(tsc_world_t *world, tsc_world_settings_t *out) {
  return tsc_world_get_settings_ext(world, out, nullptr);
}

tsc_status_t tsc_world_apply_settings(tsc_world_t *world, const tsc_world_settings_t *settings,
                                      double timeout_seconds, uint64_t *out_frame) {
  return tsc_world_apply_settings_ext(world, settings, nullptr, timeout_seconds, out_frame);
}

// out_ext / ext may be NULL (tsc_world_get_settings / tsc_world_apply_settings above:
// the extended fields are then not read, or keep the server's values).
tsc_status_t tsc_world_get_settings_ext(tsc_world_t *world, tsc_world_settings_t *out,
                                        tsc_world_settings_ext_t *out_ext) {
  return TSC_GUARD({
    require_ptr(out, "out");
    const carla::rpc::EpisodeSettings s = world_of(world).GetSettings();
    *out = tsc_world_settings_t{};
    out->synchronous_mode = s.synchronous_mode ? 1 : 0;
    out->no_rendering_mode = s.no_rendering_mode ? 1 : 0;
    out->has_fixed_delta_seconds = s.fixed_delta_seconds.has_value() ? 1 : 0;
    out->fixed_delta_seconds = s.fixed_delta_seconds.has_value() ? *s.fixed_delta_seconds : 0.0;
    out->substepping = s.substepping ? 1 : 0;
    out->max_substep_delta_time = s.max_substep_delta_time;
    out->max_substeps = s.max_substeps;
    if (out_ext != nullptr) {
      *out_ext = tsc_world_settings_ext_t{};
      out_ext->max_culling_distance = s.max_culling_distance;
      out_ext->tile_stream_distance = s.tile_stream_distance;
      out_ext->actor_active_distance = s.actor_active_distance;
      out_ext->deterministic_ragdolls = s.deterministic_ragdolls ? 1 : 0;
      out_ext->spectator_as_ego = s.spectator_as_ego ? 1 : 0;
    }
  });
}

tsc_status_t tsc_world_apply_settings_ext(tsc_world_t *world, const tsc_world_settings_t *settings,
                                          const tsc_world_settings_ext_t *ext,
                                          double timeout_seconds, uint64_t *out_frame) {
  return TSC_GUARD({
    require_ptr(settings, "settings");
    require_ptr(out_frame, "out_frame");
    auto &w = world_of(world);
    if (settings->has_fixed_delta_seconds && !(settings->fixed_delta_seconds > 0.0)) {
      fail(TSC_INVALID_ARGUMENT, "fixed_delta_seconds must be positive");
    }
    if (settings->max_substeps < 1) fail(TSC_INVALID_ARGUMENT, "max_substeps must be >= 1");
    // Start from the current settings so fields this ABI does not expose
    // (or, without `ext`, the extended ones) keep their server values.
    carla::rpc::EpisodeSettings s = w.GetSettings();
    s.synchronous_mode = settings->synchronous_mode != 0;
    s.no_rendering_mode = settings->no_rendering_mode != 0;
    if (settings->has_fixed_delta_seconds) {
      s.fixed_delta_seconds = settings->fixed_delta_seconds;
    } else {
      s.fixed_delta_seconds.reset();
    }
    s.substepping = settings->substepping != 0;
    s.max_substep_delta_time = settings->max_substep_delta_time;
    s.max_substeps = settings->max_substeps;
    if (ext != nullptr) {
      s.max_culling_distance = check_non_negative(ext->max_culling_distance, "max_culling_distance");
      s.tile_stream_distance = check_non_negative(ext->tile_stream_distance, "tile_stream_distance");
      s.actor_active_distance =
          check_non_negative(ext->actor_active_distance, "actor_active_distance");
      s.deterministic_ragdolls = ext->deterministic_ragdolls != 0;
      s.spectator_as_ego = ext->spectator_as_ego != 0;
    }
    *out_frame = w.ApplySettings(s, seconds_to_duration(timeout_seconds));
  });
}

// ---------------------------------------------------------------------------
// Actor list
// ---------------------------------------------------------------------------

size_t tsc_actor_list_size(const tsc_actor_list_t *list) {
  if (list == nullptr || list->kind != TSC_KIND_ACTOR_LIST) return 0;
  return list->list->size();
}

tsc_status_t tsc_actor_list_get(const tsc_actor_list_t *list, size_t index,
                                tsc_actor_t **out_actor) {
  return TSC_GUARD({
    require_ptr(out_actor, "out_actor");
    *out_actor = nullptr;
    const auto &l = check_handle(list, "list", TSC_KIND_ACTOR_LIST)->list;
    check_index(index, l->size(), "actor list");
    *out_actor = make_actor_handle(l->at(index));
  });
}

tsc_status_t tsc_weather_preset(const char *name, size_t name_len, tsc_weather_t *out) {
  return TSC_GUARD({
    require_ptr(out, "out");
    const std::string key = to_string(name, name_len, "name");
    static const std::array<std::pair<const char *, const WeatherParameters *>, 23> presets{{
        {"Default", &WeatherParameters::Default},
        {"ClearNoon", &WeatherParameters::ClearNoon},
        {"CloudyNoon", &WeatherParameters::CloudyNoon},
        {"WetNoon", &WeatherParameters::WetNoon},
        {"WetCloudyNoon", &WeatherParameters::WetCloudyNoon},
        {"MidRainyNoon", &WeatherParameters::MidRainyNoon},
        {"HardRainNoon", &WeatherParameters::HardRainNoon},
        {"SoftRainNoon", &WeatherParameters::SoftRainNoon},
        {"ClearSunset", &WeatherParameters::ClearSunset},
        {"CloudySunset", &WeatherParameters::CloudySunset},
        {"WetSunset", &WeatherParameters::WetSunset},
        {"WetCloudySunset", &WeatherParameters::WetCloudySunset},
        {"MidRainSunset", &WeatherParameters::MidRainSunset},
        {"HardRainSunset", &WeatherParameters::HardRainSunset},
        {"SoftRainSunset", &WeatherParameters::SoftRainSunset},
        {"ClearNight", &WeatherParameters::ClearNight},
        {"CloudyNight", &WeatherParameters::CloudyNight},
        {"WetNight", &WeatherParameters::WetNight},
        {"WetCloudyNight", &WeatherParameters::WetCloudyNight},
        {"SoftRainNight", &WeatherParameters::SoftRainNight},
        {"MidRainyNight", &WeatherParameters::MidRainyNight},
        {"HardRainNight", &WeatherParameters::HardRainNight},
        {"DustStorm", &WeatherParameters::DustStorm},
    }};
    for (const auto &preset : presets) {
      if (key == preset.first) {
        *out = from_carla(*preset.second);
        return;
      }
    }
    fail(TSC_NOT_FOUND, "no weather preset named '" + key + "'");
  });
}

}  // extern "C"
