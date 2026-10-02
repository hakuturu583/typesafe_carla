#include "internal.hpp"

using namespace tsc;

namespace {

carla::client::Vehicle &vehicle_of(tsc_vehicle_t *v) {
  return *check_handle(v, "vehicle", TSC_KIND_VEHICLE)->vehicle;
}

}  // namespace

extern "C" {

tsc_status_t tsc_vehicle_apply_control(tsc_vehicle_t *vehicle,
                                       const tsc_vehicle_control_t *control) {
  return TSC_GUARD({
    const tsc_vehicle_control_t &c = *require_ptr(control, "control");
    if (!(c.throttle >= 0.0 && c.throttle <= 1.0)) fail(TSC_INVALID_ARGUMENT, "throttle must be in [0, 1]");
    if (!(c.steer >= -1.0 && c.steer <= 1.0)) fail(TSC_INVALID_ARGUMENT, "steer must be in [-1, 1]");
    if (!(c.brake >= 0.0 && c.brake <= 1.0)) fail(TSC_INVALID_ARGUMENT, "brake must be in [0, 1]");
    carla::rpc::VehicleControl rc;
    rc.throttle = static_cast<float>(c.throttle);
    rc.steer = static_cast<float>(c.steer);
    rc.brake = static_cast<float>(c.brake);
    rc.hand_brake = c.hand_brake != 0;
    rc.reverse = c.reverse != 0;
    rc.manual_gear_shift = c.manual_gear_shift != 0;
    rc.gear = c.gear;
    vehicle_of(vehicle).ApplyControl(rc);
  });
}

tsc_status_t tsc_vehicle_get_control(tsc_vehicle_t *vehicle, tsc_vehicle_control_t *out) {
  return TSC_GUARD({
    require_ptr(out, "out");
    const carla::rpc::VehicleControl c = vehicle_of(vehicle).GetControl();
    out->throttle = c.throttle;
    out->steer = c.steer;
    out->brake = c.brake;
    out->hand_brake = c.hand_brake ? 1 : 0;
    out->reverse = c.reverse ? 1 : 0;
    out->manual_gear_shift = c.manual_gear_shift ? 1 : 0;
    out->gear = c.gear;
  });
}

tsc_status_t tsc_vehicle_set_autopilot(tsc_vehicle_t *vehicle, int32_t enabled, uint16_t tm_port) {
  return TSC_GUARD({ vehicle_of(vehicle).SetAutopilot(enabled != 0, tm_port); });
}

}  // extern "C"
