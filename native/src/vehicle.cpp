#include "internal.hpp"

using namespace tsc;

namespace {

// The scalar fields shared by tsc_*_physics_control_t and rpc::*PhysicsControl,
// so reading and writing cannot drift apart.
#define TSC_VEHICLE_FLOAT_FIELDS(X)                                                        \
  X(max_torque) X(max_rpm) X(idle_rpm) X(brake_effect) X(rev_up_moi) X(rev_down_rate)     \
  X(front_rear_split) X(gear_change_time) X(final_ratio) X(change_up_rpm)                 \
  X(change_down_rpm) X(transmission_efficiency) X(mass) X(drag_coefficient)               \
  X(chassis_width) X(chassis_height) X(downforce_coefficient) X(drag_area)                \
  X(sleep_threshold) X(sleep_slope_limit)
#define TSC_VEHICLE_BOOL_FIELDS(X) X(use_automatic_gears) X(use_sweep_wheel_collision)
#define TSC_WHEEL_FLOAT_FIELDS(X)                                                          \
  X(wheel_radius) X(wheel_width) X(wheel_mass) X(cornering_stiffness)                     \
  X(friction_force_multiplier) X(side_slip_modifier) X(slip_threshold) X(skid_threshold)  \
  X(max_steer_angle) X(max_wheelspin_rotation) X(suspension_max_raise)                    \
  X(suspension_max_drop) X(suspension_damping_ratio) X(wheel_load_ratio) X(spring_rate)   \
  X(spring_preload) X(rollbar_scaling) X(max_brake_torque) X(max_hand_brake_torque)
#define TSC_WHEEL_BOOL_FIELDS(X)                                                           \
  X(affected_by_steering) X(affected_by_brake) X(affected_by_handbrake)                   \
  X(affected_by_engine) X(abs_enabled) X(traction_control_enabled)
#define TSC_WHEEL_U8_FIELDS(X) \
  X(axle_type) X(external_torque_combine_method) X(sweep_shape) X(sweep_type)
#define TSC_WHEEL_INT_FIELDS(X) X(suspension_smoothing) X(wheel_index)
#define TSC_WHEEL_VECTOR_FIELDS(X) X(offset) X(suspension_axis) X(suspension_force_offset)
#define TSC_WHEEL_LOCATION_FIELDS(X) X(location) X(old_location) X(velocity)

std::vector<tsc_vector2d_t> from_carla(const std::vector<carla::geom::Vector2D> &curve) {
  std::vector<tsc_vector2d_t> out;
  out.reserve(curve.size());
  for (const auto &p : curve) out.push_back(tsc_vector2d_t{p.x, p.y});
  return out;
}

template <typename T>
const T *require_array(const T *data, size_t size, const char *name) {
  if (data == nullptr && size != 0) fail(TSC_INVALID_ARGUMENT, std::string(name) + " is NULL");
  return data;
}

std::vector<carla::geom::Vector2D> to_curve(const tsc_vector2d_t *data, size_t size,
                                            const char *name) {
  require_array(data, size, name);
  std::vector<carla::geom::Vector2D> out;
  out.reserve(size);
  for (size_t i = 0; i < size; ++i) {
    out.emplace_back(check_finite(data[i].x, name), check_finite(data[i].y, name));
  }
  return out;
}

std::vector<float> to_ratios(const double *data, size_t size, const char *name) {
  require_array(data, size, name);
  std::vector<float> out;
  out.reserve(size);
  for (size_t i = 0; i < size; ++i) out.push_back(check_finite(data[i], name));
  return out;
}

uint8_t check_u8(int32_t v, const char *name) {
  if (v < 0 || v > 255) {
    fail(TSC_INVALID_ARGUMENT,
         std::string(name) + " must be in [0, 255], got " + std::to_string(v));
  }
  return static_cast<uint8_t>(v);
}

carla::geom::Vector3D check_vector(const tsc_vector3d_t &v, const char *name) {
  return carla::geom::Vector3D(check_finite(v.x, name), check_finite(v.y, name),
                               check_finite(v.z, name));
}

carla::geom::Location check_location(const tsc_location_t &v, const char *name) {
  return carla::geom::Location(check_finite(v.x, name), check_finite(v.y, name),
                               check_finite(v.z, name));
}

#define TSC_READ(f) r.f = src.f;
#define TSC_READ_BOOL(f) r.f = src.f ? 1 : 0;
#define TSC_READ_VECTOR(f) r.f = tsc::from_carla(src.f);
#define TSC_WRITE_FLOAT(f) dst.f = check_finite(src.f, #f);
#define TSC_WRITE_BOOL(f) dst.f = src.f != 0;
#define TSC_WRITE_U8(f) dst.f = check_u8(src.f, #f);
#define TSC_WRITE_INT(f) dst.f = src.f;
#define TSC_WRITE_VECTOR(f) dst.f = check_vector(src.f, #f);
#define TSC_WRITE_LOCATION(f) dst.f = check_location(src.f, #f);

// Overwrites every field the C ABI carries. Wheels are matched by index.
void write_physics(const tsc_vehicle_physics_control_t &c,
                   carla::rpc::VehiclePhysicsControl &pc) {
  {
    const auto &src = c;
    auto &dst = pc;
    TSC_VEHICLE_FLOAT_FIELDS(TSC_WRITE_FLOAT)
    TSC_VEHICLE_BOOL_FIELDS(TSC_WRITE_BOOL)
    dst.differential_type = check_u8(src.differential_type, "differential_type");
    dst.center_of_mass = check_location(src.center_of_mass, "center_of_mass");
    dst.inertia_tensor_scale = check_vector(src.inertia_tensor_scale, "inertia_tensor_scale");
  }
  pc.torque_curve = to_curve(c.torque_curve, c.torque_curve_size, "torque_curve");
  pc.steering_curve = to_curve(c.steering_curve, c.steering_curve_size, "steering_curve");
  pc.forward_gear_ratios =
      to_ratios(c.forward_gear_ratios, c.forward_gear_ratios_size, "forward_gear_ratios");
  pc.reverse_gear_ratios =
      to_ratios(c.reverse_gear_ratios, c.reverse_gear_ratios_size, "reverse_gear_ratios");
  require_array(c.wheels, c.wheel_count, "wheels");
  for (size_t i = 0; i < c.wheel_count; ++i) {
    const tsc_wheel_physics_control_t &src = c.wheels[i];
    carla::rpc::WheelPhysicsControl &dst = pc.wheels[i];
    TSC_WHEEL_FLOAT_FIELDS(TSC_WRITE_FLOAT)
    TSC_WHEEL_BOOL_FIELDS(TSC_WRITE_BOOL)
    TSC_WHEEL_U8_FIELDS(TSC_WRITE_U8)
    TSC_WHEEL_INT_FIELDS(TSC_WRITE_INT)
    TSC_WHEEL_VECTOR_FIELDS(TSC_WRITE_VECTOR)
    TSC_WHEEL_LOCATION_FIELDS(TSC_WRITE_LOCATION)
    dst.lateral_slip_graph =
        to_curve(src.lateral_slip_graph, src.lateral_slip_graph_size, "lateral_slip_graph");
  }
}

}  // namespace

tsc_physics_control::tsc_physics_control(const carla::rpc::VehiclePhysicsControl &src)
    : tsc_handle(TSC_KIND_PHYSICS_CONTROL),
      torque_curve(from_carla(src.torque_curve)),
      steering_curve(from_carla(src.steering_curve)),
      forward_gear_ratios(src.forward_gear_ratios.begin(), src.forward_gear_ratios.end()),
      reverse_gear_ratios(src.reverse_gear_ratios.begin(), src.reverse_gear_ratios.end()) {
  for (const carla::rpc::WheelPhysicsControl &w : src.wheels) {
    lateral_slip_graphs.push_back(from_carla(w.lateral_slip_graph));
  }
  wheels.resize(src.wheels.size());
  for (size_t i = 0; i < src.wheels.size(); ++i) {
    const carla::rpc::WheelPhysicsControl &src_wheel = src.wheels[i];
    tsc_wheel_physics_control_t &r = wheels[i];
    {
      const auto &src = src_wheel;
      TSC_WHEEL_FLOAT_FIELDS(TSC_READ)
      TSC_WHEEL_BOOL_FIELDS(TSC_READ_BOOL)
      TSC_WHEEL_U8_FIELDS(TSC_READ)
      TSC_WHEEL_INT_FIELDS(TSC_READ)
      TSC_WHEEL_VECTOR_FIELDS(TSC_READ_VECTOR)
      TSC_WHEEL_LOCATION_FIELDS(TSC_READ_VECTOR)
    }
    r.lateral_slip_graph = lateral_slip_graphs[i].data();
    r.lateral_slip_graph_size = lateral_slip_graphs[i].size();
  }
  tsc_vehicle_physics_control_t &r = view;
  TSC_VEHICLE_FLOAT_FIELDS(TSC_READ)
  TSC_VEHICLE_BOOL_FIELDS(TSC_READ_BOOL)
  r.differential_type = src.differential_type;
  r.center_of_mass = tsc::from_carla(src.center_of_mass);
  r.inertia_tensor_scale = tsc::from_carla(src.inertia_tensor_scale);
  r.torque_curve = torque_curve.data();
  r.torque_curve_size = torque_curve.size();
  r.steering_curve = steering_curve.data();
  r.steering_curve_size = steering_curve.size();
  r.forward_gear_ratios = forward_gear_ratios.data();
  r.forward_gear_ratios_size = forward_gear_ratios.size();
  r.reverse_gear_ratios = reverse_gear_ratios.data();
  r.reverse_gear_ratios_size = reverse_gear_ratios.size();
  r.wheels = wheels.data();
  r.wheel_count = wheels.size();
}

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

tsc_status_t tsc_vehicle_get_physics_control(tsc_vehicle_t *vehicle,
                                             tsc_physics_control_t **out) {
  return new_handle(__func__, out, [&] {
    return new tsc_physics_control(vehicle_of(vehicle).GetPhysicsControl());
  });
}

tsc_status_t tsc_physics_control_view(const tsc_physics_control_t *control,
                                      tsc_vehicle_physics_control_t *out) {
  return TSC_GUARD({
    check_handle(control, "control", TSC_KIND_PHYSICS_CONTROL);
    *require_ptr(out, "out") = control->view;
  });
}

tsc_status_t tsc_vehicle_apply_physics_control(tsc_vehicle_t *vehicle,
                                               const tsc_vehicle_physics_control_t *control) {
  return TSC_GUARD({
    const tsc_vehicle_physics_control_t &c = *require_ptr(control, "control");
    auto &v = vehicle_of(vehicle);
    // Read-modify-write: every field the ABI knows is overwritten below; any
    // field a newer LibCarla adds keeps the server's value.
    carla::rpc::VehiclePhysicsControl pc = v.GetPhysicsControl();
    if (c.wheel_count != pc.wheels.size()) {
      fail(TSC_INVALID_ARGUMENT, "physics control has " + std::to_string(c.wheel_count) +
                                     " wheels but the vehicle has " +
                                     std::to_string(pc.wheels.size()));
    }
    if (!(c.mass > 0.0)) fail(TSC_INVALID_ARGUMENT, "mass must be positive");
    write_physics(c, pc);
    v.ApplyPhysicsControl(pc);
  });
}

}  // extern "C"
