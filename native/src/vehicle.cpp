#include "internal.hpp"

using namespace tsc;

namespace {

// The fields shared by tsc_*_physics_control_t and rpc::*PhysicsControl, by
// how they convert, so reading and writing cannot drift apart.
#define TSC_VEHICLE_FLOAT_FIELDS(X)                                                        \
  X(max_torque) X(max_rpm) X(idle_rpm) X(brake_effect) X(rev_up_moi) X(rev_down_rate)     \
  X(front_rear_split) X(gear_change_time) X(final_ratio) X(change_up_rpm)                 \
  X(change_down_rpm) X(transmission_efficiency) X(mass) X(drag_coefficient)               \
  X(chassis_width) X(chassis_height) X(downforce_coefficient) X(drag_area)                \
  X(sleep_threshold) X(sleep_slope_limit)
#define TSC_VEHICLE_BOOL_FIELDS(X) X(use_automatic_gears) X(use_sweep_wheel_collision)
#define TSC_VEHICLE_U8_FIELDS(X) X(differential_type)
#define TSC_VEHICLE_VECTOR_FIELDS(X) X(center_of_mass) X(inertia_tensor_scale)
// (pointer, size) in C, a std::vector in LibCarla and the snapshot.
#define TSC_VEHICLE_ARRAY_FIELDS(X) \
  X(torque_curve) X(steering_curve) X(forward_gear_ratios) X(reverse_gear_ratios)
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
#define TSC_WHEEL_VECTOR_FIELDS(X)                                                         \
  X(offset) X(suspension_axis) X(suspension_force_offset) X(location) X(old_location)     \
  X(velocity)

// Element conversions for the variable-length arrays, both directions.
double to_c(float v) { return v; }
tsc_vector2d_t to_c(const carla::geom::Vector2D &p) { return from_carla(p); }
float check_element(double v, const char *name) { return check_finite(v, name); }
carla::geom::Vector2D check_element(const tsc_vector2d_t &p, const char *name) {
  return carla::geom::Vector2D(check_finite(p.x, name), check_finite(p.y, name));
}

template <typename T>
auto to_c_vector(const std::vector<T> &values) {
  std::vector<decltype(to_c(values[0]))> out;
  out.reserve(values.size());
  for (const T &v : values) out.push_back(to_c(v));
  return out;
}

// Copies caller memory (NULL only for size 0), rejecting non-finite values.
template <typename T>
auto checked_vector(const T *data, size_t size, const char *name) {
  require_array(data, size, name);
  std::vector<decltype(check_element(*data, name))> out;
  out.reserve(size);
  for (size_t i = 0; i < size; ++i) out.push_back(check_element(data[i], name));
  return out;
}

uint8_t check_u8(int32_t v, const char *name) {
  if (v < 0 || v > 255) {
    fail(TSC_INVALID_ARGUMENT,
         std::string(name) + " must be in [0, 255], got " + std::to_string(v));
  }
  return static_cast<uint8_t>(v);
}

// geom::Location converts from geom::Vector3D, so this serves both.
carla::geom::Vector3D check_vector(const tsc_vector3d_t &v, const char *name) {
  return carla::geom::Vector3D(check_finite(v.x, name), check_finite(v.y, name),
                               check_finite(v.z, name));
}

// Plain fields (bools become 0/1), LibCarla -> C.
#define TSC_READ(f) dst.f = src.f;
#define TSC_READ_VECTOR(f) dst.f = from_carla(src.f);
// C -> LibCarla, validated.
#define TSC_WRITE_FLOAT(f) dst.f = check_finite(src.f, #f);
#define TSC_WRITE_BOOL(f) dst.f = src.f != 0;
#define TSC_WRITE_U8(f) dst.f = check_u8(src.f, #f);
#define TSC_WRITE_INT(f) dst.f = src.f;
#define TSC_WRITE_VECTOR(f) dst.f = check_vector(src.f, #f);
#define TSC_WRITE_ARRAY(f) dst.f = checked_vector(src.f, src.f##_size, #f);

// Every field except lateral_slip_graph, whose storage the caller owns.
void read_wheel(const carla::rpc::WheelPhysicsControl &src, tsc_wheel_physics_control_t &dst) {
  TSC_WHEEL_FLOAT_FIELDS(TSC_READ)
  TSC_WHEEL_BOOL_FIELDS(TSC_READ)
  TSC_WHEEL_U8_FIELDS(TSC_READ)
  TSC_WHEEL_INT_FIELDS(TSC_READ)
  TSC_WHEEL_VECTOR_FIELDS(TSC_READ_VECTOR)
}

void write_wheel(const tsc_wheel_physics_control_t &src, carla::rpc::WheelPhysicsControl &dst) {
  TSC_WHEEL_FLOAT_FIELDS(TSC_WRITE_FLOAT)
  TSC_WHEEL_BOOL_FIELDS(TSC_WRITE_BOOL)
  TSC_WHEEL_U8_FIELDS(TSC_WRITE_U8)
  TSC_WHEEL_INT_FIELDS(TSC_WRITE_INT)
  TSC_WHEEL_VECTOR_FIELDS(TSC_WRITE_VECTOR)
  dst.lateral_slip_graph =
      checked_vector(src.lateral_slip_graph, src.lateral_slip_graph_size, "lateral_slip_graph");
}

// Overwrites every field the C ABI carries. Wheels are matched by index; the
// caller has checked that the counts agree.
void write_physics(const tsc_vehicle_physics_control_t &src,
                   carla::rpc::VehiclePhysicsControl &dst) {
  TSC_VEHICLE_FLOAT_FIELDS(TSC_WRITE_FLOAT)
  TSC_VEHICLE_BOOL_FIELDS(TSC_WRITE_BOOL)
  TSC_VEHICLE_U8_FIELDS(TSC_WRITE_U8)
  TSC_VEHICLE_VECTOR_FIELDS(TSC_WRITE_VECTOR)
  TSC_VEHICLE_ARRAY_FIELDS(TSC_WRITE_ARRAY)
  require_array(src.wheels, src.wheel_count, "wheels");
  for (size_t i = 0; i < src.wheel_count; ++i) write_wheel(src.wheels[i], dst.wheels[i]);
}

}  // namespace

tsc_physics_control::tsc_physics_control(const carla::rpc::VehiclePhysicsControl &src)
    : tsc_handle(TSC_KIND_PHYSICS_CONTROL) {
  tsc_vehicle_physics_control_t &dst = view;
  TSC_VEHICLE_FLOAT_FIELDS(TSC_READ)
  TSC_VEHICLE_BOOL_FIELDS(TSC_READ)
  TSC_VEHICLE_U8_FIELDS(TSC_READ)
  TSC_VEHICLE_VECTOR_FIELDS(TSC_READ_VECTOR)
#define TSC_READ_ARRAY(f) \
  f = to_c_vector(src.f); \
  dst.f = f.data();       \
  dst.f##_size = f.size();
  TSC_VEHICLE_ARRAY_FIELDS(TSC_READ_ARRAY)
#undef TSC_READ_ARRAY
  lateral_slip_graphs.resize(src.wheels.size());
  wheels.resize(src.wheels.size());
  for (size_t i = 0; i < src.wheels.size(); ++i) {
    read_wheel(src.wheels[i], wheels[i]);
    lateral_slip_graphs[i] = to_c_vector(src.wheels[i].lateral_slip_graph);
    wheels[i].lateral_slip_graph = lateral_slip_graphs[i].data();
    wheels[i].lateral_slip_graph_size = lateral_slip_graphs[i].size();
  }
  dst.wheels = wheels.data();
  dst.wheel_count = wheels.size();
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
