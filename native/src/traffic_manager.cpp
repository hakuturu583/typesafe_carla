// Milestone 4: the Traffic Manager (runs inside LibCarla on the client side).
#include "internal.hpp"

using namespace tsc;

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

}  // extern "C"
