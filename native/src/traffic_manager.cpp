// Milestone 4: the Traffic Manager (runs inside LibCarla on the client side).
#include "internal.hpp"

using namespace tsc;

namespace {

carla::traffic_manager::TrafficManager &tm_of(tsc_traffic_manager_t *t) {
  return check_handle(t, "traffic_manager", TSC_KIND_TRAFFIC_MANAGER)->tm;
}

carla::SharedPtr<carla::client::Actor> vehicle_ptr(tsc_vehicle_t *v) {
  return check_handle(v, "vehicle", TSC_KIND_VEHICLE)->actor;
}

float finite(double v, const char *what) {
  if (!std::isfinite(v)) fail(TSC_INVALID_ARGUMENT, std::string(what) + " must be finite");
  return static_cast<float>(v);
}

}  // namespace

extern "C" {

tsc_status_t tsc_client_get_traffic_manager(tsc_client_t *client, uint16_t port,
                                            tsc_traffic_manager_t **out) {
  return new_handle(__func__, out, [&] {
    return new tsc_traffic_manager(client_of(client).GetInstanceTM(port));
  });
}

tsc_status_t tsc_traffic_manager_get_port(tsc_traffic_manager_t *tm, uint16_t *out) {
  return TSC_GUARD({ *require_ptr(out, "out") = tm_of(tm).Port(); });
}

tsc_status_t tsc_traffic_manager_set_synchronous_mode(tsc_traffic_manager_t *tm, int32_t enabled) {
  return TSC_GUARD({ tm_of(tm).SetSynchronousMode(enabled != 0); });
}

tsc_status_t tsc_traffic_manager_set_random_device_seed(tsc_traffic_manager_t *tm, uint64_t seed) {
  return TSC_GUARD({ tm_of(tm).SetRandomDeviceSeed(seed); });
}

tsc_status_t tsc_traffic_manager_set_hybrid_physics_mode(tsc_traffic_manager_t *tm, int32_t enabled) {
  return TSC_GUARD({ tm_of(tm).SetHybridPhysicsMode(enabled != 0); });
}

tsc_status_t tsc_traffic_manager_set_global_percentage_speed_difference(tsc_traffic_manager_t *tm,
                                                                       double percentage) {
  return TSC_GUARD({ tm_of(tm).SetGlobalPercentageSpeedDifference(finite(percentage, "percentage")); });
}

tsc_status_t tsc_traffic_manager_set_global_distance_to_leading_vehicle(tsc_traffic_manager_t *tm,
                                                                       double distance) {
  return TSC_GUARD({
    if (!(distance >= 0.0)) fail(TSC_INVALID_ARGUMENT, "distance must be non-negative");
    tm_of(tm).SetGlobalDistanceToLeadingVehicle(finite(distance, "distance"));
  });
}

tsc_status_t tsc_traffic_manager_set_vehicle_value(tsc_traffic_manager_t *tm, tsc_vehicle_t *vehicle,
                                                   int32_t setting, double value) {
  return TSC_GUARD({
    auto &t = tm_of(tm);
    auto v = vehicle_ptr(vehicle);
    const float x = finite(value, "value");
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

tsc_status_t tsc_traffic_manager_set_auto_lane_change(tsc_traffic_manager_t *tm, tsc_vehicle_t *vehicle,
                                                      int32_t enabled) {
  return TSC_GUARD({ tm_of(tm).SetAutoLaneChange(vehicle_ptr(vehicle), enabled != 0); });
}

tsc_status_t tsc_traffic_manager_force_lane_change(tsc_traffic_manager_t *tm, tsc_vehicle_t *vehicle,
                                                   int32_t to_left) {
  return TSC_GUARD({ tm_of(tm).SetForceLaneChange(vehicle_ptr(vehicle), to_left != 0); });
}

tsc_status_t tsc_traffic_manager_set_update_vehicle_lights(tsc_traffic_manager_t *tm,
                                                           tsc_vehicle_t *vehicle, int32_t enabled) {
  return TSC_GUARD({ tm_of(tm).SetUpdateVehicleLights(vehicle_ptr(vehicle), enabled != 0); });
}

}  // extern "C"
