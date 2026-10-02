#include "internal.hpp"

using namespace tsc;

namespace {

carla::client::Vehicle &vehicle_of(tsc_vehicle_t *v) {
  return static_cast<carla::client::Vehicle &>(*check_handle(v, "vehicle", TSC_KIND_VEHICLE)->actor);
}

}  // namespace

extern "C" {

tsc_status_t tsc_vehicle_apply_control(tsc_vehicle_t *vehicle,
                                       const tsc_vehicle_control_t *control) {
  return TSC_GUARD({
    vehicle_of(vehicle).ApplyControl(to_carla(*require_ptr(control, "control")));
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

tsc_status_t tsc_vehicle_get_physics_control(tsc_vehicle_t *vehicle,
                                             tsc_vehicle_physics_control_t *out) {
  return TSC_GUARD({
    require_ptr(out, "out");
    const carla::rpc::VehiclePhysicsControl pc = vehicle_of(vehicle).GetPhysicsControl();
    if (pc.wheels.size() > TSC_MAX_WHEELS) {
      fail(TSC_ERROR, "vehicle has " + std::to_string(pc.wheels.size()) + " wheels; at most " +
                          std::to_string(TSC_MAX_WHEELS) + " are supported");
    }
    tsc_vehicle_physics_control_t r{};
    r.max_torque = pc.max_torque;
    r.max_rpm = pc.max_rpm;
    r.final_ratio = pc.final_ratio;
    r.gear_change_time = pc.gear_change_time;
    r.mass = pc.mass;
    r.drag_coefficient = pc.drag_coefficient;
    r.center_of_mass = from_carla(pc.center_of_mass);
    r.use_automatic_gears = pc.use_automatic_gears ? 1 : 0;
    r.wheel_count = static_cast<int32_t>(pc.wheels.size());
    for (size_t i = 0; i < pc.wheels.size(); ++i) {
      const auto &w = pc.wheels[i];
      r.wheels[i] = tsc_wheel_physics_control_t{
          w.wheel_radius, w.wheel_width, w.wheel_mass, w.max_steer_angle, w.max_brake_torque,
          w.max_hand_brake_torque, w.friction_force_multiplier, w.cornering_stiffness,
          w.affected_by_steering ? 1 : 0, w.affected_by_brake ? 1 : 0,
          w.affected_by_handbrake ? 1 : 0, w.affected_by_engine ? 1 : 0};
    }
    *out = r;
  });
}

tsc_status_t tsc_vehicle_apply_physics_control(tsc_vehicle_t *vehicle,
                                               const tsc_vehicle_physics_control_t *control) {
  return TSC_GUARD({
    const tsc_vehicle_physics_control_t &c = *require_ptr(control, "control");
    auto &v = vehicle_of(vehicle);
    // Read-modify-write: fields outside this typed subset keep their values.
    carla::rpc::VehiclePhysicsControl pc = v.GetPhysicsControl();
    if (c.wheel_count != static_cast<int32_t>(pc.wheels.size())) {
      fail(TSC_INVALID_ARGUMENT, "physics control has " + std::to_string(c.wheel_count) +
                                     " wheels but the vehicle has " +
                                     std::to_string(pc.wheels.size()));
    }
    if (!(c.mass > 0.0)) fail(TSC_INVALID_ARGUMENT, "mass must be positive");
    pc.max_torque = static_cast<float>(c.max_torque);
    pc.max_rpm = static_cast<float>(c.max_rpm);
    pc.final_ratio = static_cast<float>(c.final_ratio);
    pc.gear_change_time = static_cast<float>(c.gear_change_time);
    pc.mass = static_cast<float>(c.mass);
    pc.drag_coefficient = static_cast<float>(c.drag_coefficient);
    pc.center_of_mass = to_carla(c.center_of_mass);
    pc.use_automatic_gears = c.use_automatic_gears != 0;
    for (size_t i = 0; i < pc.wheels.size(); ++i) {
      const auto &w = c.wheels[i];
      auto &dst = pc.wheels[i];
      dst.wheel_radius = static_cast<float>(w.wheel_radius);
      dst.wheel_width = static_cast<float>(w.wheel_width);
      dst.wheel_mass = static_cast<float>(w.wheel_mass);
      dst.max_steer_angle = static_cast<float>(w.max_steer_angle);
      dst.max_brake_torque = static_cast<float>(w.max_brake_torque);
      dst.max_hand_brake_torque = static_cast<float>(w.max_hand_brake_torque);
      dst.friction_force_multiplier = static_cast<float>(w.friction_force_multiplier);
      dst.cornering_stiffness = static_cast<float>(w.cornering_stiffness);
      dst.affected_by_steering = w.affected_by_steering != 0;
      dst.affected_by_brake = w.affected_by_brake != 0;
      dst.affected_by_handbrake = w.affected_by_handbrake != 0;
      dst.affected_by_engine = w.affected_by_engine != 0;
    }
    v.ApplyPhysicsControl(pc);
  });
}

}  // extern "C"
