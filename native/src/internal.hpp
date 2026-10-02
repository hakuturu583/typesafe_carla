// Internal definitions shared by the C ABI implementation files.
// Nothing in here is visible to the Codon layer.
#pragma once

#include "typesafe_carla/ffi.h"
#include "carla_compat.hpp"

#include <atomic>
#include <cmath>
#include <cstring>
#include <stdexcept>
#include <string>
#include <utility>

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

#define TSC_GUARD(body) ::tsc::guard(__func__, [&]() body)

template <typename T>
T *require_ptr(T *p, const char *name) {
  if (p == nullptr) fail(TSC_INVALID_ARGUMENT, std::string(name) + " must not be NULL");
  return p;
}

inline std::string to_string(const char *data, size_t len, const char *name) {
  if (data == nullptr && len != 0) fail(TSC_INVALID_ARGUMENT, std::string(name) + " is NULL");
  return data == nullptr ? std::string() : std::string(data, len);
}

void string_assign(tsc_string_t *out, const std::string &value);

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

namespace tsc {

// Validates a handle argument: non-NULL and of an accepted kind.
template <typename T>
T *check_handle(T *h, const char *name, tsc_handle_kind_t kind,
                tsc_handle_kind_t alt = TSC_KIND_INVALID) {
  if (h == nullptr) fail(TSC_INVALID_ARGUMENT, std::string(name) + " must not be NULL");
  if (h->kind != kind && (alt == TSC_KIND_INVALID || h->kind != alt)) {
    fail(TSC_INVALID_ARGUMENT, std::string(name) + " has the wrong handle kind");
  }
  return h;
}

template <typename T>
const T *check_handle(const T *h, const char *name, tsc_handle_kind_t kind,
                      tsc_handle_kind_t alt = TSC_KIND_INVALID) {
  return check_handle(const_cast<T *>(h), name, kind, alt);
}

inline tsc_actor *check_actor(tsc_actor *a, const char *name = "actor") {
  return check_handle(a, name, TSC_KIND_ACTOR, TSC_KIND_VEHICLE);
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
inline tsc_rotation_t from_carla(const carla::geom::Rotation &r) {
  return tsc_rotation_t{r.pitch, r.yaw, r.roll};
}
inline tsc_transform_t from_carla(const carla::geom::Transform &t) {
  return tsc_transform_t{from_carla(t.location), from_carla(t.rotation)};
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
