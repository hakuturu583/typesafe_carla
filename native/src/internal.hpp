// Internal definitions shared by the C ABI implementation files.
// Nothing in here is visible to the Codon layer.
#pragma once

#include "typesafe_carla/ffi.h"
#include "carla_compat.hpp"
#include "item_queue.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <initializer_list>
#include <limits>
#include <cmath>
#include <cstdlib>
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

// Fills *out with copies of `values` (all or nothing; *out is zeroed first).
void string_list_assign(tsc_string_list_t *out, const std::vector<std::string> &values);

// Shared body of the generated entry points with a plain output: *out is
// checked before make() runs (a NULL output fails with no side effect), and
// zeroed when make() fails, so a caller never sees (or frees) stale data.
template <typename T, typename F>
void assign_out(T *out, const char *name, F &&make) {
  require_ptr(out, name);
  try {
    *out = make();
  } catch (...) {
    *out = T{};
    throw;
  }
}

// Shared body of the entry points that return one new handle: *out is NULL
// unless make() succeeds; make() may itself return NULL ("no such object").
template <typename H, typename F>
tsc_status_t new_handle(const char *function, H **out, F &&make,
                        const char *out_name = "out") noexcept {
  return guard(function, [&]() {
    require_ptr(out, out_name);
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
using SensorQueue = ItemQueue<carla::SharedPtr<carla::sensor::SensorData>>;
}  // namespace tsc

// A sensor handle is an actor handle whose `actor` is a carla::client::Sensor.
// Listening state belongs to that client-side object (as in LibCarla): another
// handle for the same actor (e.g. from World.get_actor) is not listening, and
// releasing the last reference stops the stream (LibCarla's destructor calls
// Stop()). The queue is shared with the callback, so late measurements are safe.
struct tsc_sensor : tsc_actor {
  std::shared_ptr<tsc::SensorQueue> queue;
  // Issue #33: one queue per G-buffer texture listened to (listen_to_gbuffer).
  std::array<std::shared_ptr<tsc::SensorQueue>, TSC_GBUFFER_TEXTURE_COUNT> gbuffer_queues;
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

// A traffic sign handle: `actor` is a carla::client::TrafficSign. Traffic
// lights are traffic signs in LibCarla, so their handle derives from this one.
struct tsc_traffic_sign : tsc_actor {
  explicit tsc_traffic_sign(carla::SharedPtr<carla::client::TrafficSign> s,
                            tsc_handle_kind_t k = TSC_KIND_TRAFFIC_SIGN)
      : tsc_actor(std::move(s), k) {}
};

struct tsc_traffic_light : tsc_traffic_sign {
  explicit tsc_traffic_light(carla::SharedPtr<carla::client::TrafficLight> t)
      : tsc_traffic_sign(std::move(t), TSC_KIND_TRAFFIC_LIGHT) {}
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

// Issue #22: one landmark (the waypoint, lane validities and landmark group
// need LibCarla's object, not the copied tsc_landmark_t).
struct tsc_landmark_handle : tsc_handle {
  carla::SharedPtr<carla::client::Landmark> landmark;
  explicit tsc_landmark_handle(carla::SharedPtr<carla::client::Landmark> l)
      : tsc_handle(TSC_KIND_LANDMARK), landmark(std::move(l)) {}
};

// Issue #22: TrafficLight.get_group_traffic_lights.
struct tsc_traffic_light_list : tsc_handle {
  std::vector<carla::SharedPtr<carla::client::TrafficLight>> lights;
  explicit tsc_traffic_light_list(std::vector<carla::SharedPtr<carla::client::TrafficLight>> l)
      : tsc_handle(TSC_KIND_TRAFFIC_LIGHT_LIST), lights(std::move(l)) {}
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

// Walker::GetBonesTransform's result, one RPC (issue #20).
struct tsc_bone_list : tsc_handle {
  std::vector<carla::rpc::BoneTransformDataOut> bones;
  explicit tsc_bone_list(std::vector<carla::rpc::BoneTransformDataOut> b)
      : tsc_handle(TSC_KIND_BONE_LIST), bones(std::move(b)) {}
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

// The episode's light manager (issue #21). Lights cross the ABI as ids.
struct tsc_light_manager : tsc_handle {
  carla::SharedPtr<carla::client::LightManager> manager;
  explicit tsc_light_manager(carla::SharedPtr<carla::client::LightManager> m)
      : tsc_handle(TSC_KIND_LIGHT_MANAGER), manager(std::move(m)) {}
};


// A World::OnTick registration (issue #21): LibCarla's thread only queues the
// snapshots; the Codon side delivers them on the program's thread. Releasing
// the handle removes the registration.
struct tsc_tick_listener : tsc_handle {
  carla::client::World world;
  size_t callback_id = 0;
  bool registered = false;
  using Queue = tsc::ItemQueue<std::shared_ptr<carla::client::WorldSnapshot>>;
  std::shared_ptr<Queue> queue;
  std::shared_ptr<tsc::FrameWatch> frames;  // the newest frame queued
  explicit tsc_tick_listener(carla::client::World w)
      : tsc_handle(TSC_KIND_TICK_LISTENER), world(std::move(w)) {}
  ~tsc_tick_listener() override;  // world_queries.cpp
  void unregister() {
    if (registered) world.RemoveOnTick(callback_id);
    registered = false;
  }
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
                       TSC_KIND_WALKER_AI_CONTROLLER, TSC_KIND_TRAFFIC_LIGHT,
                       TSC_KIND_TRAFFIC_SIGN});
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

// A world's DebugHelper (a cheap value: it holds the episode).
inline carla::client::DebugHelper debug_of(tsc_world_t *w) { return world_of(w).MakeDebugHelper(); }

inline const carla::client::Map &map_of(const tsc_map_t *m) {
  return *check_handle(m, "map", TSC_KIND_MAP)->map;
}

inline const carla::client::BlueprintLibrary &blueprint_library_of(
    const tsc_blueprint_library_t *l) {
  return *check_handle(l, "library", TSC_KIND_BLUEPRINT_LIBRARY)->library;
}

inline carla::client::ActorBlueprint &blueprint_of(tsc_actor_blueprint_t *b) {
  return check_handle(b, "blueprint", TSC_KIND_ACTOR_BLUEPRINT)->blueprint;
}

inline const carla::client::ActorBlueprint &blueprint_of(const tsc_actor_blueprint_t *b) {
  return check_handle(b, "blueprint", TSC_KIND_ACTOR_BLUEPRINT)->blueprint;
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

inline carla::client::Walker &walker_of(tsc_walker_t *w) {
  return actor_as<carla::client::Walker>(w, "walker", TSC_KIND_WALKER);
}

inline carla::client::TrafficLight &light_of(tsc_traffic_light_t *t) {
  return actor_as<carla::client::TrafficLight>(t, "traffic_light", TSC_KIND_TRAFFIC_LIGHT);
}

// Traffic sign and traffic light handles (both hold a TrafficSign).
inline carla::client::TrafficSign &sign_of(tsc_traffic_sign_t *s) {
  return static_cast<carla::client::TrafficSign &>(
      *check_handle(s, "sign", {TSC_KIND_TRAFFIC_SIGN, TSC_KIND_TRAFFIC_LIGHT})->actor);
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

inline const carla::client::Landmark &landmark_of(const tsc_landmark_handle_t *l,
                                                  const char *name = "landmark") {
  return *check_handle(l, name, TSC_KIND_LANDMARK)->landmark;
}

// A new handle H of `p`, or NULL for "none" (handle outputs in bindings/types.yaml).
template <typename H, typename P>
H *new_or_null(P p) {
  return p == nullptr ? nullptr : new H(std::move(p));
}

// `p`, which LibCarla should never have returned NULL (TSC_ERROR if it did).
template <typename P>
P non_null(P p, const char *what) {
  if (p == nullptr) fail(TSC_ERROR, std::string("LibCarla returned a null ") + what);
  return p;
}

// LibCarla can put null entries in a list (GetWaypointXODR for a lane that
// does not exist, an actor it cannot find); a typed list has no place for them.
template <typename T>
std::vector<carla::SharedPtr<T>> without_nulls(std::vector<carla::SharedPtr<T>> v) {
  v.erase(std::remove(v.begin(), v.end(), nullptr), v.end());
  return v;
}

// A search distance in meters: finite and non-negative (NaN fails).
inline double check_search_distance(double distance, const char *name) {
  if (!(distance >= 0.0) || !std::isfinite(distance)) {
    fail(TSC_INVALID_ARGUMENT, std::string(name) + " must be a non-negative finite number of meters");
  }
  return distance;
}

// Traffic Manager paths and routes (traffic_manager.cpp), validated; a NULL
// array is accepted for count 0.
carla::traffic_manager::Path to_path(const tsc_location_t *path, size_t count, const char *name);
carla::traffic_manager::Route to_route(const uint8_t *route, size_t count, const char *name);

// Fails with TSC_NOT_FOUND unless index < size; `what` names the container.
inline void check_index(size_t index, size_t size, const char *what) {
  if (index >= size) {
    fail(TSC_NOT_FOUND, "index " + std::to_string(index) + " out of range for " + what +
                            " of size " + std::to_string(size));
  }
}

// Element `index` of a list handle's items (generated tsc_*_get, bindings/lists.yaml).
// The result may refer into `items`, which must outlive it: an lvalue (the
// handle's member), never a temporary (the rvalue overload is deleted).
template <typename Items>
decltype(auto) list_at(const Items &items, size_t index, const char *what) {
  check_index(index, items.size(), what);
  return items.at(index);
}
template <typename Items>
void list_at(const Items &&items, size_t index, const char *what) = delete;

// List elements (map.cpp, walker.cpp).
tsc_landmark_t from_carla(const carla::client::Landmark &lm);
tsc_bone_transform_out_t from_carla(const carla::rpc::BoneTransformDataOut &b);

// The measurement behind a sensor data handle (sensor.cpp, generated code).
inline const carla::sensor::SensorData &sensor_data_of(const tsc_sensor_data_t *d) {
  return *check_handle(d, "data", TSC_KIND_SENSOR_DATA)->data;
}

// The measurement as T, or TSC_TYPE_ERROR naming `what` ("an image").
template <typename T>
const T &sensor_data_as(const tsc_sensor_data_t *d, const char *what) {
  auto typed = dynamic_cast<const T *>(&sensor_data_of(d));
  if (typed == nullptr) fail(TSC_TYPE_ERROR, std::string("sensor data is not ") + what);
  return *typed;
}

// The event measurements bound by the generated code (bindings/*_event.yaml).
inline const carla::sensor::data::CollisionEvent &collision_event_of(const tsc_sensor_data_t *d) {
  return sensor_data_as<carla::sensor::data::CollisionEvent>(d, "a collision event");
}
inline const carla::sensor::data::ObstacleDetectionEvent &obstacle_event_of(
    const tsc_sensor_data_t *d) {
  return sensor_data_as<carla::sensor::data::ObstacleDetectionEvent>(
      d, "an obstacle-detection event");
}
inline const carla::sensor::data::LaneInvasionEvent &lane_invasion_event_of(
    const tsc_sensor_data_t *d) {
  return sensor_data_as<carla::sensor::data::LaneInvasionEvent>(d, "a lane-invasion event");
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
inline uint8_t from_carla(uint8_t v) { return v; }  // semantic tags
inline tsc_bounding_box_t from_carla(const carla::geom::BoundingBox &b) {
  return tsc_bounding_box_t{from_carla(b.location), from_carla(b.extent), from_carla(b.rotation)};
}
inline tsc_geo_location_t from_carla(const carla::geom::GeoLocation &g) {
  return tsc_geo_location_t{g.latitude, g.longitude, g.altitude};
}
inline carla::geom::GeoLocation to_carla(const tsc_geo_location_t &g) {
  return carla::geom::GeoLocation(g.latitude, g.longitude, g.altitude);
}
inline tsc_lane_marking_t from_carla(const carla::road::element::LaneMarking &m) {
  tsc_lane_marking_t r{};
  r.type = static_cast<int32_t>(m.type);
  r.color = static_cast<int32_t>(m.color);
  r.lane_change = static_cast<int32_t>(m.lane_change);
  r.width = m.width;
  return r;
}
inline tsc_lane_validity_t from_carla(const carla::road::LaneValidity &v) {
  return tsc_lane_validity_t{v._from_lane, v._to_lane};
}
inline carla::geom::BoundingBox to_carla(const tsc_bounding_box_t &b) {
  return carla::geom::BoundingBox(to_carla(b.location), to_carla_vector(b.extent),
                                  to_carla(b.rotation));
}

// Issue #21.
inline tsc_labelled_point_t from_carla(const carla::rpc::LabelledPoint &p) {
  return tsc_labelled_point_t{from_carla(p._location), static_cast<int32_t>(p._label), 0};
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

// An optional LibCarla value: *has_value = 0 and *out zeroed when it is empty.
template <typename Out, typename Optional>
void assign_optional(const Optional &value, int32_t *has_value, Out *out,
                     const char *has_value_name = "has_value") {
  require_ptr(has_value, has_value_name);
  require_ptr(out, "out");
  *has_value = value.has_value() ? 1 : 0;
  *out = value.has_value() ? from_carla(*value) : Out{};
}

// LibCarla's float parameters. A finite double above FLT_MAX would become
// +-inf in the float cast, so the float range is checked too (NaN fails).
inline float check_float(double v, const char *name) {
  if (!(std::fabs(v) <= static_cast<double>(std::numeric_limits<float>::max()))) {
    fail(TSC_INVALID_ARGUMENT, std::string(name) + " must be finite and within float range");
  }
  return static_cast<float>(v);
}

// Kept for existing callers; same check as check_float.
inline float check_finite(double v, const char *name) { return check_float(v, name); }

inline float check_non_negative(double v, const char *name) {
  if (!(v >= 0.0)) {
    fail(TSC_INVALID_ARGUMENT, std::string(name) + " must be finite and non-negative");
  }
  return check_float(v, name);
}

inline uint8_t check_u8(int32_t v, const char *name) {
  if (v < 0 || v > 255) {
    fail(TSC_INVALID_ARGUMENT,
         std::string(name) + " must be in [0, 255], got " + std::to_string(v));
  }
  return static_cast<uint8_t>(v);
}

// A C enumeration value as LibCarla's enum E; values outside [first, last] fail.
template <typename E>
E to_enum(int32_t value, int32_t first, int32_t last, const char *what) {
  if (value < first || value > last) {
    fail(TSC_INVALID_ARGUMENT, std::string("invalid ") + what + " " + std::to_string(value));
  }
  return static_cast<E>(value);
}

inline carla::rpc::TrafficLightState to_light_state(int32_t state) {
  return to_enum<carla::rpc::TrafficLightState>(state, TSC_TRAFFIC_LIGHT_RED,
                                                TSC_TRAFFIC_LIGHT_UNKNOWN, "traffic light state");
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

// Issue #20: Ackermann control and controller settings. Values must be finite
// (as everywhere else, NaN and infinities never reach the server).
inline carla::rpc::VehicleAckermannControl to_carla(const tsc_vehicle_ackermann_control_t &c) {
  return carla::rpc::VehicleAckermannControl(
      check_float(c.steer, "steer"), check_float(c.steer_speed, "steer_speed"),
      check_float(c.speed, "speed"), check_float(c.acceleration, "acceleration"),
      check_float(c.jerk, "jerk"));
}

inline carla::rpc::AckermannControllerSettings to_carla(
    const tsc_ackermann_controller_settings_t &s) {
  return carla::rpc::AckermannControllerSettings(
      check_float(s.speed_kp, "speed_kp"), check_float(s.speed_ki, "speed_ki"),
      check_float(s.speed_kd, "speed_kd"), check_float(s.accel_kp, "accel_kp"),
      check_float(s.accel_ki, "accel_ki"), check_float(s.accel_kd, "accel_kd"));
}

inline tsc_ackermann_controller_settings_t from_carla(
    const carla::rpc::AckermannControllerSettings &s) {
  return tsc_ackermann_controller_settings_t{s.speed_kp, s.speed_ki, s.speed_kd,
                                             s.accel_kp, s.accel_ki, s.accel_kd};
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

// Shared value types (#19, #21): shared_types.cpp.
void transform_list_assign(tsc_transform_list_t *out,
                           const std::vector<carla::geom::Transform> &values);
carla::rpc::TextureColor to_carla_texture(const tsc_texture_color_t *texture, const char *name);
carla::rpc::TextureFloatColor to_carla_texture(const tsc_texture_float_color_t *texture,
                                               const char *name);
carla::rpc::MaterialParameter to_material_parameter(int32_t parameter);

// Caller-owned lists of POD items (issue #21): malloc'd, freed with free().
template <typename T>
T *alloc_list_items(size_t n) {
  if (n == 0) return nullptr;
  auto *items = static_cast<T *>(std::calloc(n, sizeof(T)));
  if (items == nullptr) throw std::bad_alloc();
  return items;
}

template <typename List>
void free_list_items(List *list) {
  if (list == nullptr) return;
  std::free(list->items);
  list->items = nullptr;
  list->size = 0;
}

// *out = the items convert(values[i]).
template <typename List, typename Vec, typename Convert>
void list_assign(List *out, const Vec &values, Convert &&convert) {
  require_ptr(out, "out");
  using Item = std::remove_pointer_t<decltype(out->items)>;
  List list{alloc_list_items<Item>(values.size()), values.size()};
  for (size_t i = 0; i < values.size(); ++i) list.items[i] = convert(values[i]);
  *out = list;
}

// Issue #21: the identity of the client connection (LibCarla's Simulator) a
// World belongs to. OnTick callback ids are per Simulator, which keeps its
// callbacks across load_world.
inline uint64_t client_token(const carla::client::World &w) {
  return static_cast<uint64_t>(reinterpret_cast<uintptr_t>(w.GetEpisode().Lock().get()));
}

// Issue #21: conversions named in bindings/types.yaml (world_queries.cpp).
// A traffic light handle, or NULL when the actor is none or not a traffic light.
inline tsc_traffic_light *make_traffic_light_handle(const carla::SharedPtr<carla::client::Actor> &a) {
  return new_or_null<tsc_traffic_light>(downcast<carla::client::TrafficLight>(a));
}
inline tsc_traffic_light *make_traffic_light_handle(carla::SharedPtr<carla::client::TrafficLight> l) {
  return new_or_null<tsc_traffic_light>(std::move(l));
}
// The traffic lights among `actors` (World returns them as plain actors).
std::vector<carla::SharedPtr<carla::client::TrafficLight>> traffic_lights_of(
    const std::vector<carla::SharedPtr<carla::client::Actor>> &actors);
std::vector<std::string> to_names(const tsc_string_t *names, size_t count, const char *name);
// An input array (NULL only with count 0) as a vector.
template <typename T>
std::vector<T> to_vector(const T *values, size_t count, const char *name) {
  require_array(values, count, name);
  return values == nullptr ? std::vector<T>() : std::vector<T>(values, values + count);
}
// The `via` helper of World.get_traffic_lights_in_junction: empty for an id
// that names no junction (LibCarla would dereference NULL).
std::vector<carla::SharedPtr<carla::client::Actor>> traffic_lights_in_junction(
    const carla::client::World &world, int32_t junction_id);
void vehicle_light_state_list_assign(tsc_vehicle_light_state_list_t *out,
                                     const carla::rpc::VehicleLightStateList &states);
void bounding_box_list_assign(tsc_bounding_box_list_t *out,
                              const std::vector<carla::geom::BoundingBox> &boxes);
void labelled_point_list_assign(tsc_labelled_point_list_t *out,
                                const std::vector<carla::rpc::LabelledPoint> &points);
void environment_object_list_assign(tsc_environment_object_list_t *out,
                                    const std::vector<carla::rpc::EnvironmentObject> &objects);
void light_list_assign(tsc_light_list_t *out, const std::vector<carla::client::Light> &lights);

inline carla::client::LightManager &light_manager_of(tsc_light_manager_t *m) {
  return *check_handle(m, "light_manager", TSC_KIND_LIGHT_MANAGER)->manager;
}

// ---------------------------------------------------------------------------
// Issue #31: accessors and conversions named in bindings/*.yaml.
// ---------------------------------------------------------------------------

inline const carla::client::ActorList &actor_list_of(const tsc_actor_list_t *l) {
  return *check_handle(l, "list", TSC_KIND_ACTOR_LIST)->list;
}

inline const carla::client::WorldSnapshot &snapshot_of(const tsc_world_snapshot_t *s) {
  return check_handle(s, "snapshot", TSC_KIND_WORLD_SNAPSHOT)->snapshot;
}

inline const carla::client::Junction &junction_of(const tsc_junction_t *j) {
  return *check_handle(j, "junction", TSC_KIND_JUNCTION)->junction;
}

inline tsc_sensor &sensor_handle(tsc_sensor_t *s) {
  return *check_handle(s, "sensor", TSC_KIND_SENSOR);
}

inline carla::client::Sensor &sensor_of(tsc_sensor_t *s) {
  return static_cast<carla::client::Sensor &>(*sensor_handle(s).actor);
}

// Issue #33: the ROS2 and G-buffer methods are on ServerSideSensor; a sensor
// computed on the client (lane invasion) is TSC_TYPE_ERROR.
inline carla::client::ServerSideSensor &server_side_sensor_of(tsc_sensor_t *s) {
  auto &sensor = sensor_of(s);
  auto *server_side = dynamic_cast<carla::client::ServerSideSensor *>(&sensor);
  if (server_side == nullptr) {
    fail(TSC_TYPE_ERROR, "sensor '" + sensor.GetTypeId() +
                             "' is computed on the client, not a server-side sensor");
  }
  return *server_side;
}

// A G-buffer texture id (GBufferTextureID); LibCarla aborts on a larger one.
inline uint32_t check_gbuffer_id(uint32_t id) {
  if (id >= TSC_GBUFFER_TEXTURE_COUNT) {
    fail(TSC_INVALID_ARGUMENT, "G-buffer texture id " + std::to_string(id) + " is not below " +
                                   std::to_string(TSC_GBUFFER_TEXTURE_COUNT));
  }
  return id;
}

// The geometry behind Transform.get_matrix & co: a tsc_transform_t by value.
inline carla::geom::Transform transform_of(const tsc_transform_t *t) {
  return to_carla(*require_ptr(t, "transform"));
}

// No range checks: like the Python API, NaN and infinities just propagate.
template <typename M>
void copy_matrix(const M &m, double *out16) {
  require_ptr(out16, "out16");
  for (size_t i = 0; i < 16; ++i) out16[i] = static_cast<double>(m[i]);
}

// An output the caller may omit.
template <typename T, typename V>
void store_if(T *out, V value) {
  if (out != nullptr) *out = value;
}

inline double duration_seconds(carla::time_duration d) {
  return static_cast<double>(d.milliseconds()) / 1000.0;
}

inline double check_positive(double v, const char *name) {
  if (!(v > 0.0) || !std::isfinite(v)) {
    fail(TSC_INVALID_ARGUMENT, std::string(name) + " must be a positive finite number");
  }
  return v;
}

inline std::string to_nonempty_string(const char *data, size_t len, const char *name) {
  std::string s = to_string(data, len, name);
  if (s.empty()) fail(TSC_INVALID_ARGUMENT, std::string(name) + " must not be empty");
  return s;
}

// A spawn's parent: NULL for none.
inline carla::client::Actor *actor_or_null(tsc_actor_t *a, const char *name) {
  return a == nullptr ? nullptr : check_actor(a, name)->actor.get();
}

// LibCarla's SpawnActor throws on failure; a NULL result would be a bug there.
inline carla::SharedPtr<carla::client::Actor> spawned(carla::SharedPtr<carla::client::Actor> a) {
  if (a == nullptr) fail(TSC_ERROR, "spawn failed");
  return a;
}

// Pairs (begin, end) as one list [b0, e0, b1, e1, ...].
template <typename W>
std::vector<W> flatten(std::vector<std::pair<W, W>> pairs) {
  std::vector<W> flat;
  flat.reserve(2 * pairs.size());
  for (auto &p : pairs) {
    flat.push_back(std::move(p.first));
    flat.push_back(std::move(p.second));
  }
  return flat;
}

inline carla::rpc::OpendriveGenerationParameters to_carla(const tsc_opendrive_parameters_t &p) {
  return carla::rpc::OpendriveGenerationParameters(
      p.vertex_distance, p.max_road_length, p.wall_height, p.additional_width,
      p.smooth_junctions != 0, p.enable_mesh_visibility != 0, p.enable_pedestrian_navigation != 0);
}

inline carla::sensor::data::Color to_carla(const tsc_color_t &c) {
  return carla::sensor::data::Color(c.r, c.g, c.b, c.a);
}

inline tsc_weather_t from_carla(const carla::rpc::WeatherParameters &w) {
  return tsc_weather_t{w.cloudiness,         w.precipitation,        w.precipitation_deposits,
                       w.wind_intensity,     w.sun_azimuth_angle,    w.sun_altitude_angle,
                       w.fog_density,        w.fog_distance,         w.fog_falloff,
                       w.wetness,            w.scattering_intensity, w.mie_scattering_scale,
                       w.rayleigh_scattering_scale, w.dust_storm};
}

inline carla::rpc::WeatherParameters to_carla(const tsc_weather_t &w) {
  return carla::rpc::WeatherParameters(
      w.cloudiness, w.precipitation, w.precipitation_deposits, w.wind_intensity,
      w.sun_azimuth_angle, w.sun_altitude_angle, w.fog_density, w.fog_distance, w.fog_falloff,
      w.wetness, w.scattering_intensity, w.mie_scattering_scale, w.rayleigh_scattering_scale,
      w.dust_storm);
}

inline tsc_timestamp_t from_carla(const carla::client::Timestamp &t) {
  return tsc_timestamp_t{t.frame, t.elapsed_seconds, t.delta_seconds, t.platform_timestamp};
}

inline tsc_vehicle_control_t from_carla(const carla::rpc::VehicleControl &c) {
  return tsc_vehicle_control_t{c.throttle, c.steer, c.brake, c.hand_brake ? 1 : 0,
                               c.reverse ? 1 : 0, c.manual_gear_shift ? 1 : 0, c.gear};
}

inline carla::rpc::WalkerControl to_carla(const tsc_walker_control_t &c) {
  return to_carla_walker_control(c.direction, c.speed, c.jump != 0);
}

inline tsc_walker_control_t from_carla(const carla::rpc::WalkerControl &c) {
  return tsc_walker_control_t{from_carla(c.direction), c.speed, c.jump ? 1 : 0, 0};
}

// Walker.set_bones (walker.cpp): names and finite transforms.
carla::rpc::WalkerBoneControlIn to_bone_control(const tsc_bone_transform_t *bones, size_t count,
                                                const char *name);
}  // namespace tsc
