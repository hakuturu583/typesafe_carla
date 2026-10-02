// Milestone 4: the Traffic Manager (runs inside LibCarla on the client side).
#include "internal.hpp"

#include <cstdlib>

using namespace tsc;

namespace tsc {

carla::traffic_manager::Path to_path(const tsc_location_t *path, size_t count, const char *name) {
  require_array(path, count, name);
  const std::string n = name;
  carla::traffic_manager::Path locations;
  locations.reserve(count);
  for (size_t i = 0; i < count; ++i) {
    locations.emplace_back(check_float(path[i].x, (n + " x").c_str()),
                           check_float(path[i].y, (n + " y").c_str()),
                           check_float(path[i].z, (n + " z").c_str()));
  }
  return locations;
}

carla::traffic_manager::Route to_route(const uint8_t *route, size_t count, const char *name) {
  require_array(route, count, name);
  for (size_t i = 0; i < count; ++i) {
    if (route[i] > TSC_ROAD_OPTION_ROAD_END) {
      fail(TSC_INVALID_ARGUMENT, "invalid road option " + std::to_string(route[i]));
    }
  }
  return carla::traffic_manager::Route(route, route + count);
}

}  // namespace tsc

extern "C" {

tsc_status_t tsc_client_get_traffic_manager(tsc_client_t *client, uint16_t port,
                                            tsc_traffic_manager_t **out) {
  return new_handle(__func__, out, [&] {
    return new tsc_traffic_manager(client_of(client).GetInstanceTM(port));
  });
}

tsc_status_t tsc_traffic_manager_set_vehicle_value(tsc_traffic_manager_t *tm, tsc_vehicle_t *vehicle,
                                                   int32_t setting, double value) {
  return TSC_GUARD({
    auto &t = tm_of(tm);
    auto v = vehicle_ptr(vehicle);
    const float x = check_finite(value, "value");
    auto percentage = [&]() {
      if (x < 0.0f || x > 100.0f) fail(TSC_INVALID_ARGUMENT, "percentage must be in [0, 100]");
      return x;
    };
    switch (setting) {
      case TSC_TM_PERCENTAGE_SPEED_DIFFERENCE: t.SetPercentageSpeedDifference(v, x); break;
      case TSC_TM_DISTANCE_TO_LEADING_VEHICLE:
        if (x < 0.0f) fail(TSC_INVALID_ARGUMENT, "distance must be non-negative");
        t.SetDistanceToLeadingVehicle(v, x);
        break;
      case TSC_TM_RANDOM_LEFT_LANECHANGE_PERCENTAGE: t.SetRandomLeftLaneChangePercentage(v, percentage()); break;
      case TSC_TM_RANDOM_RIGHT_LANECHANGE_PERCENTAGE: t.SetRandomRightLaneChangePercentage(v, percentage()); break;
      case TSC_TM_IGNORE_LIGHTS_PERCENTAGE: t.SetPercentageRunningLight(v, percentage()); break;
      case TSC_TM_IGNORE_SIGNS_PERCENTAGE: t.SetPercentageRunningSign(v, percentage()); break;
      case TSC_TM_IGNORE_VEHICLES_PERCENTAGE: t.SetPercentageIgnoreVehicles(v, percentage()); break;
      case TSC_TM_IGNORE_WALKERS_PERCENTAGE: t.SetPercentageIgnoreWalkers(v, percentage()); break;
      case TSC_TM_KEEP_RIGHT_PERCENTAGE: set_keep_right_percentage(t, v, percentage()); break;
      case TSC_TM_DESIRED_SPEED:
        if (x < 0.0f) fail(TSC_INVALID_ARGUMENT, "desired speed must be non-negative");
        t.SetDesiredSpeed(v, x);
        break;
      case TSC_TM_LANE_OFFSET: t.SetLaneOffset(v, x); break;
      default: fail(TSC_INVALID_ARGUMENT, "unknown Traffic Manager setting " + std::to_string(setting));
    }
  });
}

// --- Issue #23 -------------------------------------------------------------------

namespace {

// One status for "no plan" on every LibCarla: ue5-dev returns (Void, NULL)
// or an empty buffer for a vehicle it does not drive and once shut down;
// CARLA 0.10.0 throws std::out_of_range (TSC_NOT_FOUND through the guard).
constexpr const char *kNoPlan = "the vehicle is not driven by this Traffic Manager, or it is shut down";

const carla::SharedPtr<carla::client::Waypoint> &action_waypoint(
    const carla::traffic_manager::Action &action) {
  if (action.second == nullptr) fail(TSC_NOT_FOUND, kNoPlan);
  return action.second;
}

}  // namespace

tsc_status_t tsc_traffic_manager_get_next_action(tsc_traffic_manager_t *tm,
                                                 tsc_vehicle_t *vehicle,
                                                 int32_t *out_road_option,
                                                 tsc_waypoint_t **out_waypoint) {
  return TSC_GUARD({
    require_ptr(out_road_option, "out_road_option");
    require_ptr(out_waypoint, "out_waypoint");
    *out_waypoint = nullptr;
    auto &t = tm_of(tm);
    const auto id = vehicle_ptr(vehicle)->GetId();
    const carla::traffic_manager::Action action = t.GetNextAction(id);
    auto *w = new tsc_waypoint(action_waypoint(action));
    *out_road_option = static_cast<int32_t>(action.first);
    *out_waypoint = w;
  });
}

tsc_status_t tsc_traffic_manager_get_all_actions(tsc_traffic_manager_t *tm,
                                                 tsc_vehicle_t *vehicle,
                                                 uint8_t **out_road_options, size_t *out_count,
                                                 tsc_waypoint_list_t **out_waypoints) {
  return TSC_GUARD({
    require_ptr(out_road_options, "out_road_options");
    require_ptr(out_count, "out_count");
    require_ptr(out_waypoints, "out_waypoints");
    *out_road_options = nullptr;
    *out_count = 0;
    *out_waypoints = nullptr;
    auto &t = tm_of(tm);
    const auto id = vehicle_ptr(vehicle)->GetId();
    const carla::traffic_manager::ActionBuffer actions = t.GetActionBuffer(id);
    if (actions.empty()) fail(TSC_NOT_FOUND, kNoPlan);
    std::vector<carla::SharedPtr<carla::client::Waypoint>> waypoints;
    waypoints.reserve(actions.size());
    for (const auto &action : actions) waypoints.push_back(action_waypoint(action));
    auto *options = static_cast<uint8_t *>(std::malloc(actions.empty() ? 1 : actions.size()));
    if (options == nullptr) throw std::bad_alloc();
    for (size_t i = 0; i < actions.size(); ++i) options[i] = static_cast<uint8_t>(actions[i].first);
    try {
      *out_waypoints = new tsc_waypoint_list(std::move(waypoints));
    } catch (...) {
      std::free(options);
      throw;
    }
    *out_road_options = options;
    *out_count = actions.size();
  });
}

void tsc_road_options_free(uint8_t *road_options) { std::free(road_options); }

}  // extern "C"
