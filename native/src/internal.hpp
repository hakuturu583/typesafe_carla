// Internal definitions shared by the C ABI implementation files.
// Nothing in here is visible to the Codon layer.
#pragma once

#include "typesafe_carla/ffi.h"
#include "carla_compat.hpp"

#include <algorithm>
#include <atomic>
#include <initializer_list>
#include <cmath>
#include <cstring>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

// ---------------------------------------------------------------------------
// Errors
// ---------------------------------------------------------------------------

namespace tsc {

void set_last_error(const std::string &message);

// Thrown inside the shim to return a specific status; never leaves the shim.
struct StatusError : std::runtime_error {
  tsc_status_t status;
  StatusError(tsc_status_t s, const std::string &m) : std::runtime_error(m), status(s) {}
};

[[noreturn]] inline void fail(tsc_status_t status, const std::string &message) {
  throw StatusError(status, message);
}

// Runs fn, converting every C++ exception into a status code plus thread-local
// message. This is the only place exceptions are allowed to stop.
template <typename F>
tsc_status_t guard(const char *function, F &&fn) noexcept {
  auto report = [function](tsc_status_t status, const char *message) {
    set_last_error(std::string(function) + ": " + message);
    return status;
  };
  try {
    fn();
    return TSC_OK;
  } catch (const StatusError &e) {
    return report(e.status, e.what());
  } catch (const carla::client::TimeoutException &e) {
    return report(TSC_TIMEOUT, e.what());
  } catch (const std::out_of_range &e) {
    return report(TSC_NOT_FOUND, e.what());
  } catch (const std::invalid_argument &e) {
    return report(TSC_INVALID_ARGUMENT, e.what());
  } catch (const std::exception &e) {
    return report(TSC_ERROR, e.what());
  } catch (...) {
    return report(TSC_ERROR, "unknown C++ exception");
  }
}

#define TSC_GUARD(...) ::tsc::guard(__func__, [&]() __VA_ARGS__)

template <typename T>
T *require_ptr(T *p, const char *name) {
  if (p == nullptr) fail(TSC_INVALID_ARGUMENT, std::string(name) + " must not be NULL");
  return p;
}

// Caller arrays and strings: NULL is allowed only for size 0.
template <typename T>
void require_array(const T *data, size_t size, const char *name) {
  if (data == nullptr && size != 0) fail(TSC_INVALID_ARGUMENT, std::string(name) + " is NULL");
}

inline std::string to_string(const char *data, size_t len, const char *name) {
  require_array(data, len, name);
  return data == nullptr ? std::string() : std::string(data, len);
}

void string_assign(tsc_string_t *out, const std::string &value);

// Shared body of the entry points that return one new handle: *out is NULL
// unless make() succeeds; make() may itself return NULL ("no such object").
template <typename H, typename F>
tsc_status_t new_handle(const char *function, H **out, F &&make) noexcept {
  return guard(function, [&]() {
    require_ptr(out, "out");
    *out = nullptr;
    *out = make();
  });
}

}  // namespace tsc

// ---------------------------------------------------------------------------
// Handles
//
// Every handle derives from tsc_handle; the base is at offset 0, so any
// handle pointer may be cast to tsc_handle_t* and back.
// ---------------------------------------------------------------------------

struct tsc_handle {
  std::atomic<uint32_t> refcount{1};
  const tsc_handle_kind_t kind;

  explicit tsc_handle(tsc_handle_kind_t k);
  virtual ~tsc_handle();
  tsc_handle(const tsc_handle &) = delete;
  tsc_handle &operator=(const tsc_handle &) = delete;
};

struct tsc_client : tsc_handle {
  carla::client::Client client;
  tsc_client(const std::string &host, uint16_t port)
      : tsc_handle(TSC_KIND_CLIENT), client(host, port) {}
};

struct tsc_world : tsc_handle {
  carla::client::World world;
  explicit tsc_world(carla::client::World w) : tsc_handle(TSC_KIND_WORLD), world(std::move(w)) {}
};

struct tsc_actor : tsc_handle {
  carla::SharedPtr<carla::client::Actor> actor;
  explicit tsc_actor(carla::SharedPtr<carla::client::Actor> a,
                     tsc_handle_kind_t k = TSC_KIND_ACTOR)
      : tsc_handle(k), actor(std::move(a)) {}
};

// A vehicle handle is an actor handle whose `actor` is known (by its kind tag)
// to point at a carla::client::Vehicle.
struct tsc_vehicle : tsc_actor {
  explicit tsc_vehicle(carla::SharedPtr<carla::client::Vehicle> v)
      : tsc_actor(std::move(v), TSC_KIND_VEHICLE) {}
};

namespace tsc {
class SensorQueue;  // sensor.cpp
}  // namespace tsc

// A sensor handle is an actor handle whose `actor` is a carla::client::Sensor.
// Listening state belongs to that client-side object (as in LibCarla): another
// handle for the same actor (e.g. from World.get_actor) is not listening, and
// releasing the last reference stops the stream (LibCarla's destructor calls
// Stop()). The queue is shared with the callback, so late measurements are safe.
struct tsc_sensor : tsc_actor {
  std::shared_ptr<tsc::SensorQueue> queue;
  explicit tsc_sensor(carla::SharedPtr<carla::client::Sensor> s)
      : tsc_actor(std::move(s), TSC_KIND_SENSOR) {}
};

struct tsc_walker : tsc_actor {
  explicit tsc_walker(carla::SharedPtr<carla::client::Walker> w)
      : tsc_actor(std::move(w), TSC_KIND_WALKER) {}
};

struct tsc_walker_ai_controller : tsc_actor {
  explicit tsc_walker_ai_controller(carla::SharedPtr<carla::client::WalkerAIController> c)
      : tsc_actor(std::move(c), TSC_KIND_WALKER_AI_CONTROLLER) {}
};

struct tsc_traffic_light : tsc_actor {
  explicit tsc_traffic_light(carla::SharedPtr<carla::client::TrafficLight> t)
      : tsc_actor(std::move(t), TSC_KIND_TRAFFIC_LIGHT) {}
};

struct tsc_traffic_manager : tsc_handle {
  carla::traffic_manager::TrafficManager tm;
  explicit tsc_traffic_manager(carla::traffic_manager::TrafficManager t)
      : tsc_handle(TSC_KIND_TRAFFIC_MANAGER), tm(std::move(t)) {}
};

struct tsc_landmark_list : tsc_handle {
  std::vector<carla::SharedPtr<carla::client::Landmark>> landmarks;
  explicit tsc_landmark_list(std::vector<carla::SharedPtr<carla::client::Landmark>> l)
      : tsc_handle(TSC_KIND_LANDMARK_LIST), landmarks(std::move(l)) {}
};

struct tsc_junction : tsc_handle {
  carla::SharedPtr<carla::client::Junction> junction;
  explicit tsc_junction(carla::SharedPtr<carla::client::Junction> j)
      : tsc_handle(TSC_KIND_JUNCTION), junction(std::move(j)) {}
};

// A physics control snapshot, already converted to the C layout: `view` points
// into the vectors below, which never change after construction.
struct tsc_physics_control : tsc_handle {
  std::vector<tsc_vector2d_t> torque_curve, steering_curve;
  std::vector<double> forward_gear_ratios, reverse_gear_ratios;
  std::vector<std::vector<tsc_vector2d_t>> lateral_slip_graphs;  // one per wheel
  std::vector<tsc_wheel_physics_control_t> wheels;
  tsc_vehicle_physics_control_t view{};
  explicit tsc_physics_control(const carla::rpc::VehiclePhysicsControl &pc);  // vehicle.cpp
};

struct tsc_sensor_data : tsc_handle {
  carla::SharedPtr<carla::sensor::SensorData> data;
  explicit tsc_sensor_data(carla::SharedPtr<carla::sensor::SensorData> d)
      : tsc_handle(TSC_KIND_SENSOR_DATA), data(std::move(d)) {}
};

struct tsc_actor_list : tsc_handle {
  carla::SharedPtr<carla::client::ActorList> list;
  explicit tsc_actor_list(carla::SharedPtr<carla::client::ActorList> l)
      : tsc_handle(TSC_KIND_ACTOR_LIST), list(std::move(l)) {}
};

struct tsc_blueprint_library : tsc_handle {
  carla::SharedPtr<carla::client::BlueprintLibrary> library;
  explicit tsc_blueprint_library(carla::SharedPtr<carla::client::BlueprintLibrary> l)
      : tsc_handle(TSC_KIND_BLUEPRINT_LIBRARY), library(std::move(l)) {}
};

struct tsc_actor_blueprint : tsc_handle {
  // A copy, like the Python API: set_attribute does not modify the library.
  carla::client::ActorBlueprint blueprint;
  explicit tsc_actor_blueprint(carla::client::ActorBlueprint b)
      : tsc_handle(TSC_KIND_ACTOR_BLUEPRINT), blueprint(std::move(b)) {}
};

struct tsc_world_snapshot : tsc_handle {
  carla::client::WorldSnapshot snapshot;
  // The actors, converted once: LibCarla's snapshot iterator is forward-only,
  // so indexing it directly would make a full iteration O(n^2).
  std::vector<tsc_actor_snapshot_t> actors;
  explicit tsc_world_snapshot(carla::client::WorldSnapshot s);  // snapshot.cpp
};

struct tsc_map : tsc_handle {
  carla::SharedPtr<carla::client::Map> map;
  explicit tsc_map(carla::SharedPtr<carla::client::Map> m)
      : tsc_handle(TSC_KIND_MAP), map(std::move(m)) {}
};

struct tsc_waypoint : tsc_handle {
  carla::SharedPtr<carla::client::Waypoint> waypoint;
  explicit tsc_waypoint(carla::SharedPtr<carla::client::Waypoint> w)
      : tsc_handle(TSC_KIND_WAYPOINT), waypoint(std::move(w)) {}
};

struct tsc_waypoint_list : tsc_handle {
  std::vector<carla::SharedPtr<carla::client::Waypoint>> waypoints;
  explicit tsc_waypoint_list(std::vector<carla::SharedPtr<carla::client::Waypoint>> w)
      : tsc_handle(TSC_KIND_WAYPOINT_LIST), waypoints(std::move(w)) {}
};

namespace tsc {

// Validates a handle argument: non-NULL and of one of the accepted kinds.
template <typename T>
T *check_handle(T *h, const char *name, std::initializer_list<tsc_handle_kind_t> kinds) {
  if (h == nullptr) fail(TSC_INVALID_ARGUMENT, std::string(name) + " must not be NULL");
  if (std::find(kinds.begin(), kinds.end(), h->kind) == kinds.end()) {
    fail(TSC_INVALID_ARGUMENT, std::string(name) + " has the wrong handle kind");
  }
  return h;
}

template <typename T>
T *check_handle(T *h, const char *name, tsc_handle_kind_t kind) {
  return check_handle(h, name, {kind});
}

// Any actor handle: plain actors and every derived actor kind.
inline tsc_actor *check_actor(tsc_actor *a, const char *name = "actor") {
  return check_handle(a, name,
                      {TSC_KIND_ACTOR, TSC_KIND_VEHICLE, TSC_KIND_SENSOR, TSC_KIND_WALKER,
                       TSC_KIND_WALKER_AI_CONTROLLER, TSC_KIND_TRAFFIC_LIGHT});
}

// Shared body of tsc_actor_as_<kind>: a new reference to `actor` as the
// derived handle H, or TSC_TYPE_ERROR naming `what` ("a vehicle").
template <typename H>
H *retain_as(tsc_actor_t *actor, tsc_handle_kind_t kind, const char *what) {
  tsc_actor *a = check_actor(actor);
  if (a->kind != kind) {
    fail(TSC_TYPE_ERROR, "actor " + std::to_string(a->actor->GetId()) + " (" +
                             a->actor->GetTypeId() + ") is not " + what);
  }
  tsc_handle_retain(a);
  return static_cast<H *>(a);
}

// Unwrapping accessors shared by the entry-point files.
inline carla::client::Client &client_of(tsc_client_t *c) {
  return check_handle(c, "client", TSC_KIND_CLIENT)->client;
}

inline carla::client::World &world_of(tsc_world_t *w) {
  return check_handle(w, "world", TSC_KIND_WORLD)->world;
}

inline const carla::client::Map &map_of(const tsc_map_t *m) {
  return *check_handle(m, "map", TSC_KIND_MAP)->map;
}

inline const carla::client::Waypoint &waypoint_of(const tsc_waypoint_t *w) {
  return *check_handle(w, "waypoint", TSC_KIND_WAYPOINT)->waypoint;
}

inline carla::client::Actor &actor_of(tsc_actor_t *a) { return *check_actor(a)->actor; }

// The LibCarla object behind a derived actor handle: the kind tag guarantees
// that `actor` points at a C.
template <typename C, typename H>
C &actor_as(H *h, const char *name, tsc_handle_kind_t kind) {
  return static_cast<C &>(*check_handle(h, name, kind)->actor);
}

inline carla::client::Vehicle &vehicle_of(tsc_vehicle_t *v) {
  return actor_as<carla::client::Vehicle>(v, "vehicle", TSC_KIND_VEHICLE);
}

inline carla::client::TrafficLight &light_of(tsc_traffic_light_t *t) {
  return actor_as<carla::client::TrafficLight>(t, "traffic_light", TSC_KIND_TRAFFIC_LIGHT);
}

inline carla::client::WalkerAIController &controller_of(tsc_walker_ai_controller_t *c) {
  return actor_as<carla::client::WalkerAIController>(c, "controller",
                                                     TSC_KIND_WALKER_AI_CONTROLLER);
}

inline carla::traffic_manager::TrafficManager &tm_of(tsc_traffic_manager_t *t) {
  return check_handle(t, "traffic_manager", TSC_KIND_TRAFFIC_MANAGER)->tm;
}

// A vehicle handle as the Traffic Manager takes it.
inline carla::SharedPtr<carla::client::Actor> vehicle_ptr(tsc_vehicle_t *v,
                                                          const char *name = "vehicle") {
  return check_handle(v, name, TSC_KIND_VEHICLE)->actor;
}

// Fails with TSC_NOT_FOUND unless index < size; `what` names the container.
inline void check_index(size_t index, size_t size, const char *what) {
  if (index >= size) {
    fail(TSC_NOT_FOUND, "index " + std::to_string(index) + " out of range for " + what +
                            " of size " + std::to_string(size));
  }
}

// Wraps a LibCarla actor in the most derived handle kind we support.
tsc_actor *make_actor_handle(const carla::SharedPtr<carla::client::Actor> &actor);

// Value conversions (LibCarla geometry uses float).
inline carla::geom::Location to_carla(const tsc_location_t &v) {
  return carla::geom::Location(static_cast<float>(v.x), static_cast<float>(v.y),
                               static_cast<float>(v.z));
}
inline carla::geom::Vector3D to_carla_vector(const tsc_vector3d_t &v) {
  return carla::geom::Vector3D(static_cast<float>(v.x), static_cast<float>(v.y),
                               static_cast<float>(v.z));
}
inline carla::geom::Rotation to_carla(const tsc_rotation_t &r) {
  return carla::geom::Rotation(static_cast<float>(r.pitch), static_cast<float>(r.yaw),
                               static_cast<float>(r.roll));
}
inline carla::geom::Transform to_carla(const tsc_transform_t &t) {
  return carla::geom::Transform(to_carla(t.location), to_carla(t.rotation));
}
inline tsc_vector3d_t from_carla(const carla::geom::Vector3D &v) {
  return tsc_vector3d_t{v.x, v.y, v.z};
}
inline tsc_vector2d_t from_carla(const carla::geom::Vector2D &v) {
  return tsc_vector2d_t{v.x, v.y};
}
inline tsc_rotation_t from_carla(const carla::geom::Rotation &r) {
  return tsc_rotation_t{r.pitch, r.yaw, r.roll};
}
inline tsc_transform_t from_carla(const carla::geom::Transform &t) {
  return tsc_transform_t{from_carla(t.location), from_carla(t.rotation)};
}
inline tsc_bounding_box_t from_carla(const carla::geom::BoundingBox &b) {
  return tsc_bounding_box_t{from_carla(b.location), from_carla(b.extent), from_carla(b.rotation)};
}
inline carla::geom::BoundingBox to_carla(const tsc_bounding_box_t &b) {
  return carla::geom::BoundingBox(to_carla(b.location), to_carla_vector(b.extent),
                                  to_carla(b.rotation));
}

// Shared body of the "fill a caller buffer" entry points: reports the full
// count and converts up to `capacity` elements (out may be NULL to query size).
template <typename Out, typename Vec>
void copy_out(const Vec &values, Out *out, size_t capacity, size_t *out_count) {
  require_ptr(out_count, "out_count");
  *out_count = values.size();
  if (out == nullptr) return;
  for (size_t i = 0; i < values.size() && i < capacity; ++i) out[i] = from_carla(values[i]);
}

// LibCarla's float parameters: `name` must be finite (and non-negative; NaN
// fails too).
inline float check_finite(double v, const char *name) {
  if (!std::isfinite(v)) fail(TSC_INVALID_ARGUMENT, std::string(name) + " must be finite");
  return static_cast<float>(v);
}

inline float check_non_negative(double v, const char *name) {
  if (!(v >= 0.0) || !std::isfinite(v)) {
    fail(TSC_INVALID_ARGUMENT, std::string(name) + " must be finite and non-negative");
  }
  return static_cast<float>(v);
}

inline carla::rpc::TrafficLightState to_light_state(int32_t state) {
  if (state < TSC_TRAFFIC_LIGHT_RED || state > TSC_TRAFFIC_LIGHT_UNKNOWN) {
    fail(TSC_INVALID_ARGUMENT, "invalid traffic light state " + std::to_string(state));
  }
  return static_cast<carla::rpc::TrafficLightState>(state);
}

// Direct and batch walker control go through here.
inline carla::rpc::WalkerControl to_carla_walker_control(const tsc_vector3d_t &direction,
                                                         double speed, bool jump) {
  return carla::rpc::WalkerControl(to_carla_vector(direction),
                                   check_non_negative(speed, "walker speed"), jump);
}

// Validates ranges (NaN fails too): direct and batch control go through here.
inline carla::rpc::VehicleControl to_carla(const tsc_vehicle_control_t &c) {
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
  return rc;
}

// Longest accepted timeout (~31 years): keeps the millisecond count far inside
// size_t and LibCarla's signed boost::posix_time milliseconds.
inline constexpr double kMaxTimeoutSeconds = 1e9;

inline carla::time_duration seconds_to_duration(double seconds) {
  if (!std::isfinite(seconds) || seconds < 0.0 || seconds > kMaxTimeoutSeconds) {
    fail(TSC_INVALID_ARGUMENT, "timeout must be a finite number of seconds in [0, 1e9]");
  }
  return carla::time_duration::milliseconds(static_cast<size_t>(seconds * 1000.0 + 0.5));
}

}  // namespace tsc
