// The subset of LibCarla the shim uses, plus the few spots where the real
// library and the in-memory mock backend (native/mock) differ.
#pragma once

#include <carla/FileSystem.h>
#include <stdexcept>
#include <string>
#include <vector>

#include <carla/Memory.h>
#include <carla/Time.h>
#include <carla/Version.h>
#include <carla/client/ActorBlueprint.h>
#include <carla/client/ActorList.h>
#include <carla/client/ActorSnapshot.h>
#include <carla/client/BlueprintLibrary.h>
#include <carla/client/Client.h>
#include <carla/client/DebugHelper.h>
#include <carla/client/Junction.h>
#include <carla/client/Landmark.h>
#include <carla/client/LightManager.h>
#include <carla/client/Map.h>
#include <carla/client/Sensor.h>
#include <carla/client/ServerSideSensor.h>
#include <carla/client/Timestamp.h>
#include <carla/client/TrafficLight.h>
#include <carla/client/TrafficSign.h>
#include <carla/client/TimeoutException.h>
#include <carla/client/Vehicle.h>
#include <carla/client/Walker.h>
#include <carla/client/WalkerAIController.h>
#include <carla/client/Waypoint.h>
#include <carla/client/World.h>
#include <carla/client/WorldSnapshot.h>
#include <carla/geom/BoundingBox.h>
#include <carla/geom/GeoLocation.h>
#include <carla/geom/Transform.h>
#include <carla/road/Lane.h>
#include <carla/road/element/LaneMarking.h>
#include <carla/rpc/ActorState.h>
#include <carla/rpc/Command.h>
#include <carla/rpc/CommandResponse.h>
#include <carla/rpc/EpisodeSettings.h>
#include <carla/rpc/MaterialParameter.h>
#include <carla/rpc/OpendriveGenerationParameters.h>
#include <carla/rpc/TrafficLightState.h>
#include <carla/rpc/VehicleLightState.h>
#include <carla/rpc/WalkerControl.h>
#include <carla/rpc/WeatherParameters.h>
#include <carla/rpc/VehiclePhysicsControl.h>
#include <carla/rpc/Texture.h>
#include <carla/sensor/SensorData.h>
#include <carla/trafficmanager/TrafficManager.h>
#include <carla/sensor/data/CollisionEvent.h>
#include <carla/sensor/data/GnssMeasurement.h>
#include <carla/sensor/data/IMUMeasurement.h>
#include <carla/sensor/data/Image.h>
#include <carla/sensor/data/LidarMeasurement.h>
#include <carla/sensor/data/DVSEventArray.h>
#include <carla/sensor/data/LaneInvasionEvent.h>
#include <carla/sensor/data/ObstacleDetectionEvent.h>
#include <carla/sensor/data/RadarMeasurement.h>
#include <carla/sensor/data/SemanticLidarMeasurement.h>
#include <carla/FileSystem.h>
#include <carla/image/CityScapesPalette.h>
#include <carla/pointcloud/PointCloudIO.h>
#include <carla/rpc/VehicleControl.h>

// Geo projections (geom::GeoProjection, Map::GetGeoProjection) are newer than
// CARLA 0.10.0, whose GeoLocation instead has a Mercator Transform().
#if __has_include(<carla/geom/GeoProjection.h>)
#include <carla/geom/GeoProjection.h>
#define TSC_HAS_GEO_PROJECTION 1
#endif
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace tsc {

// carla::SharedPtr is std::shared_ptr in CARLA UE5 (it was boost::shared_ptr
// in UE4, which is not supported).
template <typename To, typename From>
carla::SharedPtr<To> downcast(const carla::SharedPtr<From> &p) {
  return std::dynamic_pointer_cast<To>(p);
}

// ue5-dev renamed TrafficManager::SetKeepRightPercentage (CARLA 0.10.0) to
// SetKeepSlowLanePercentage.
template <typename TM, typename A>
void set_keep_right_percentage(TM &tm, const A &actor, float percentage) {
  if constexpr (requires { tm.SetKeepSlowLanePercentage(actor, percentage); }) {
    tm.SetKeepSlowLanePercentage(actor, percentage);
  } else {
    tm.SetKeepRightPercentage(actor, percentage);
  }
}

// Waypoint::IsRHT is newer than CARLA 0.10.0, which has no left-hand traffic:
// its lane markings and lane changes always use the right-hand layout.
template <typename W>
bool waypoint_is_rht(const W &waypoint) {
  if constexpr (requires { waypoint.IsRHT(); }) {
    return waypoint.IsRHT();
  } else {
    return true;
  }
}

// Vehicle::GetTelemetryData and Vehicle::GetVehicleBoneWorldTransforms are in
// ue5-dev, not in CARLA 0.10.0 (issue #20); without them these throw (TSC_ERROR).
// rpc::VehicleTelemetryData does not exist in 0.10.0 either, so the result is
// only touched inside `use`.
inline std::runtime_error missing_in_libcarla(const char *method) {
  return std::runtime_error(std::string(method) + " is not available in LibCarla " +
                            carla::version() + " (it needs a newer CARLA, e.g. ue5-dev)");
}

template <typename V, typename Use>
void with_telemetry_data(const V &vehicle, Use &&use) {
  if constexpr (requires { vehicle.GetTelemetryData(); }) {
    use(vehicle.GetTelemetryData());
  } else {
    throw missing_in_libcarla("Vehicle::GetTelemetryData");
  }
}

template <typename V>
std::vector<carla::geom::Transform> vehicle_bone_world_transforms(const V &vehicle) {
  if constexpr (requires { vehicle.GetVehicleBoneWorldTransforms(); }) {
    return vehicle.GetVehicleBoneWorldTransforms();
  } else {
    throw missing_in_libcarla("Vehicle::GetVehicleBoneWorldTransforms");
  }
}

// Thrown (as TSC_ERROR) for a feature the linked LibCarla does not have.
[[noreturn]] inline void unsupported(const char *what) { throw missing_in_libcarla(what); }

// obj.method(args...), or unsupported(what) when the linked LibCarla has no
// such method (a generic lambda, so that the `requires` may fail). Generated
// for `optional:` entries of bindings/*.yaml.
#define TSC_CALL_OPTIONAL(obj, method, what, ...)                                \
  [](auto &&s, auto &&...a) {                                                    \
    if constexpr (requires { s.method(a...); }) s.method(a...); else unsupported(what); \
  }(obj __VA_OPT__(, ) __VA_ARGS__)
// The actor skeleton queries (bones, components, sockets, issue #19) are in
// ue5-dev, not in CARLA 0.10.0's LibCarla; there they throw (TSC_ERROR).
// LibCarla refs known to have every skeleton query (the mock mirrors
// ue5-dev): there, a query that does not resolve is a compile error, so a
// misspelled method name cannot silently become "unsupported".
#if defined(TSC_MOCK_LIBCARLA)
inline constexpr bool kSkeletonQueriesRequired = true;
#elif defined(TSC_CARLA_GIT_REF)
inline constexpr bool kSkeletonQueriesRequired = std::string_view(TSC_CARLA_GIT_REF) == "ue5-dev";
#else
inline constexpr bool kSkeletonQueriesRequired = false;
#endif

// fn(actor, args...) calls actor.Method(args...), or throws where it is missing.
#define TSC_SKELETON_QUERY(fn, Method, Result)               \
  template <typename A, typename... Args>                    \
  Result fn(const A &actor, const Args &...args) {           \
    if constexpr (requires { actor.Method(args...); }) {     \
      return actor.Method(args...);                          \
    } else {                                                 \
      static_assert(!kSkeletonQueriesRequired || sizeof(A) == 0, \
                    "Actor::" #Method " not found");         \
      throw missing_in_libcarla("Actor::" #Method);          \
    }                                                        \
  }
TSC_SKELETON_QUERY(get_bone_names, GetBoneNames, std::vector<std::string>)
TSC_SKELETON_QUERY(get_bone_world_transforms, GetBoneWorldTransforms,
                   std::vector<carla::geom::Transform>)
TSC_SKELETON_QUERY(get_bone_relative_transforms, GetBoneRelativeTransforms,
                   std::vector<carla::geom::Transform>)
TSC_SKELETON_QUERY(get_component_names, GetComponentNames, std::vector<std::string>)
TSC_SKELETON_QUERY(get_component_world_transform, GetComponentWorldTransform,
                   carla::geom::Transform)
TSC_SKELETON_QUERY(get_component_relative_transform, GetComponentRelativeTransform,
                   carla::geom::Transform)
TSC_SKELETON_QUERY(get_socket_names, GetSocketNames, std::vector<std::string>)
TSC_SKELETON_QUERY(get_socket_world_transforms, GetSocketWorldTransforms,
                   std::vector<carla::geom::Transform>)
TSC_SKELETON_QUERY(get_socket_relative_transforms, GetSocketRelativeTransforms,
                   std::vector<carla::geom::Transform>)
#undef TSC_SKELETON_QUERY

// World::GetIMUSensorGravity / SetIMUSensorGravity are in ue5-dev, not in
// LibCarla 0.10.0's source (issue #21); there both throw (TSC_ERROR).
template <typename W>
float get_imu_sensor_gravity(const W &world) {
  if constexpr (requires { world.GetIMUSensorGravity(); }) {
    return world.GetIMUSensorGravity();
  } else {
    throw missing_in_libcarla("World::GetIMUSensorGravity");
  }
}

template <typename W>
void set_imu_sensor_gravity(W &world, float gravity) {
  if constexpr (requires { world.SetIMUSensorGravity(gravity); }) {
    world.SetIMUSensorGravity(gravity);
  } else {
    throw missing_in_libcarla("World::SetIMUSensorGravity");
  }
}

#ifdef TSC_MOCK_LIBCARLA
inline constexpr const char *kBackendName = "mock";
#else
inline constexpr const char *kBackendName = "libcarla";
#endif

}  // namespace tsc
