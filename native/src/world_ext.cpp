// Milestone 4: weather and debug drawing.
#include "internal.hpp"

#include <array>
#include <utility>

using namespace tsc;
using carla::rpc::WeatherParameters;

namespace {

tsc_weather_t to_c(const WeatherParameters &w) {
  return tsc_weather_t{w.cloudiness,         w.precipitation,        w.precipitation_deposits,
                       w.wind_intensity,     w.sun_azimuth_angle,    w.sun_altitude_angle,
                       w.fog_density,        w.fog_distance,         w.fog_falloff,
                       w.wetness,            w.scattering_intensity, w.mie_scattering_scale,
                       w.rayleigh_scattering_scale, w.dust_storm};
}

WeatherParameters to_carla_weather(const tsc_weather_t &w) {
  auto f = [](double v) { return static_cast<float>(v); };
  return WeatherParameters(f(w.cloudiness), f(w.precipitation), f(w.precipitation_deposits),
                           f(w.wind_intensity), f(w.sun_azimuth_angle), f(w.sun_altitude_angle),
                           f(w.fog_density), f(w.fog_distance), f(w.fog_falloff), f(w.wetness),
                           f(w.scattering_intensity), f(w.mie_scattering_scale),
                           f(w.rayleigh_scattering_scale), f(w.dust_storm));
}

carla::sensor::data::Color to_color(tsc_color_t c) {
  return carla::sensor::data::Color(c.r, c.g, c.b, c.a);
}

float life(double life_time) {
  if (!std::isfinite(life_time)) fail(TSC_INVALID_ARGUMENT, "life_time must be finite");
  return static_cast<float>(life_time);
}

}  // namespace

extern "C" {

tsc_status_t tsc_world_get_weather(tsc_world_t *world, tsc_weather_t *out) {
  return TSC_GUARD({ *require_ptr(out, "out") = to_c(world_of(world).GetWeather()); });
}

tsc_status_t tsc_world_set_weather(tsc_world_t *world, const tsc_weather_t *weather) {
  return TSC_GUARD({ world_of(world).SetWeather(to_carla_weather(*require_ptr(weather, "weather"))); });
}

tsc_status_t tsc_world_is_weather_enabled(tsc_world_t *world, int32_t *out) {
  return TSC_GUARD({ *require_ptr(out, "out") = world_of(world).IsWeatherEnabled() ? 1 : 0; });
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
        *out = to_c(*preset.second);
        return;
      }
    }
    fail(TSC_NOT_FOUND, "no weather preset named '" + key + "'");
  });
}

tsc_status_t tsc_debug_draw_point(tsc_world_t *world, const tsc_location_t *location, double size,
                                  tsc_color_t color, double life_time) {
  return TSC_GUARD({
    world_of(world).MakeDebugHelper().DrawPoint(to_carla(*require_ptr(location, "location")),
                                                static_cast<float>(size), to_color(color),
                                                life(life_time));
  });
}

tsc_status_t tsc_debug_draw_line(tsc_world_t *world, const tsc_location_t *begin,
                                 const tsc_location_t *end, double thickness, tsc_color_t color,
                                 double life_time) {
  return TSC_GUARD({
    world_of(world).MakeDebugHelper().DrawLine(to_carla(*require_ptr(begin, "begin")),
                                               to_carla(*require_ptr(end, "end")),
                                               static_cast<float>(thickness), to_color(color),
                                               life(life_time));
  });
}

tsc_status_t tsc_debug_draw_arrow(tsc_world_t *world, const tsc_location_t *begin,
                                  const tsc_location_t *end, double thickness, double arrow_size,
                                  tsc_color_t color, double life_time) {
  return TSC_GUARD({
    world_of(world).MakeDebugHelper().DrawArrow(
        to_carla(*require_ptr(begin, "begin")), to_carla(*require_ptr(end, "end")),
        static_cast<float>(thickness), static_cast<float>(arrow_size), to_color(color),
        life(life_time));
  });
}

tsc_status_t tsc_debug_draw_box(tsc_world_t *world, const tsc_bounding_box_t *box,
                                const tsc_rotation_t *rotation, double thickness,
                                tsc_color_t color, double life_time) {
  return TSC_GUARD({
    world_of(world).MakeDebugHelper().DrawBox(
        to_carla(*require_ptr(box, "box")),
        to_carla(*require_ptr(rotation, "rotation")), static_cast<float>(thickness),
        to_color(color), life(life_time));
  });
}

tsc_status_t tsc_debug_draw_string(tsc_world_t *world, const tsc_location_t *location,
                                   const char *text, size_t text_len, int32_t draw_shadow,
                                   tsc_color_t color, double life_time) {
  return TSC_GUARD({
    world_of(world).MakeDebugHelper().DrawString(to_carla(*require_ptr(location, "location")),
                                                 to_string(text, text_len, "text"),
                                                 draw_shadow != 0, to_color(color),
                                                 life(life_time));
  });
}

}  // extern "C"
