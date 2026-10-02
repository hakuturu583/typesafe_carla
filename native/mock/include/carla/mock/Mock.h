// In-memory stand-in for the subset of LibCarla used by typesafe_carla.
//
// Class names, member names and signatures mirror LibCarla (CARLA UE5, ue5-dev) so that the
// shim in native/src compiles unchanged against either. The "server" is a
// process-wide in-memory episode keyed by host:port with a toy vehicle model.
// It exists for tests and for developing without a running CARLA server; it is
// not a simulator.
#pragma once

#include <chrono>
#include <functional>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

#define TSC_MOCK_LIBCARLA 1

namespace carla {

template <typename T>
using SharedPtr = std::shared_ptr<T>;

constexpr const char *version() { return "0.10.0-mock"; }

class time_duration {
 public:
  static constexpr time_duration milliseconds(size_t timeout) {
    return time_duration(std::chrono::milliseconds(timeout));
  }
  constexpr time_duration() noexcept : _milliseconds(0) {}
  template <typename Rep, typename Period>
  constexpr time_duration(std::chrono::duration<Rep, Period> d) noexcept
      : _milliseconds(std::chrono::duration_cast<std::chrono::milliseconds>(d)) {}
  constexpr size_t milliseconds() const noexcept {
    return static_cast<size_t>(_milliseconds.count());
  }

 private:
  std::chrono::milliseconds _milliseconds;
};

namespace geom {

class Vector3D {
 public:
  float x = 0.0f;
  float y = 0.0f;
  float z = 0.0f;
  Vector3D() = default;
  Vector3D(float ix, float iy, float iz) : x(ix), y(iy), z(iz) {}
};

class Location : public Vector3D {
 public:
  Location() = default;
  using Vector3D::Vector3D;
  Location(const Vector3D &v) : Vector3D(v) {}
};

class Rotation {
 public:
  float pitch = 0.0f;
  float yaw = 0.0f;
  float roll = 0.0f;
  Rotation() = default;
  Rotation(float p, float y, float r) : pitch(p), yaw(y), roll(r) {}
};

class Transform {
 public:
  Location location;
  Rotation rotation;
  Transform() = default;
  Transform(const Location &l) : location(l) {}
  Transform(const Location &l, const Rotation &r) : location(l), rotation(r) {}
};

class BoundingBox {
 public:
  BoundingBox() = default;
  BoundingBox(const Location &l, const Vector3D &e, const Rotation &r = Rotation())
      : location(l), extent(e), rotation(r) {}
  Location location;
  Vector3D extent;
  Rotation rotation;
};

}  // namespace geom

namespace road {

class Lane {
 public:
  enum class LaneType : int32_t {
    None = 0x1,
    Driving = 0x1 << 1,
    Sidewalk = 0x1 << 5,
    Any = -2
  };
};

}  // namespace road

namespace rpc {

using ActorId = uint32_t;

enum class ActorAttributeType : uint8_t { Bool, Int, Float, String, RGBColor, SIZE, INVALID };

enum class AttachmentType { Rigid, SpringArm, SpringArmGhost };

enum class MapLayer : uint16_t { None = 0, All = 0xFFFF };

class VehicleControl {
 public:
  VehicleControl() = default;
  VehicleControl(float in_throttle, float in_steer, float in_brake, bool in_hand_brake,
                 bool in_reverse, bool in_manual_gear_shift, int32_t in_gear)
      : throttle(in_throttle), steer(in_steer), brake(in_brake), hand_brake(in_hand_brake),
        reverse(in_reverse), manual_gear_shift(in_manual_gear_shift), gear(in_gear) {}
  float throttle = 0.0f;
  float steer = 0.0f;
  float brake = 0.0f;
  bool hand_brake = false;
  bool reverse = false;
  bool manual_gear_shift = false;
  int32_t gear = 0;
};

class EpisodeSettings {
 public:
  bool synchronous_mode = false;
  bool no_rendering_mode = false;
  std::optional<double> fixed_delta_seconds;
  bool substepping = true;
  double max_substep_delta_time = 0.01;
  int max_substeps = 10;
  float max_culling_distance = 0.0f;
  bool deterministic_ragdolls = true;
  float tile_stream_distance = 3000.f;
  float actor_active_distance = 2000.f;
  bool spectator_as_ego = true;
};

struct WheelPhysicsControl {
  float wheel_radius = 37.0f;
  float wheel_width = 30.0f;
  float wheel_mass = 30.0f;
  float cornering_stiffness = 1000.0f;
  float friction_force_multiplier = 3.0f;
  float max_steer_angle = 70.0f;
  bool affected_by_steering = true;
  bool affected_by_brake = true;
  bool affected_by_handbrake = true;
  bool affected_by_engine = true;
  float max_brake_torque = 1500.0f;
  float max_hand_brake_torque = 3000.0f;
};

struct VehiclePhysicsControl {
  float max_torque = 300.0f;
  float max_rpm = 5000.0f;
  bool use_automatic_gears = true;
  float gear_change_time = 0.5f;
  float final_ratio = 4.0f;
  float mass = 1000.0f;
  float drag_coefficient = 0.3f;
  geom::Location center_of_mass = geom::Location(0, 0, 0);
  std::vector<WheelPhysicsControl> wheels;
};

enum class TrafficLightState : uint8_t { Red, Yellow, Green, Off, Unknown, SIZE };

class VehicleLightState {
 public:
  using flag_type = uint32_t;
  enum class LightState : flag_type {
    None = 0, Position = 0x1, LowBeam = 0x1 << 1, HighBeam = 0x1 << 2, Brake = 0x1 << 3,
    RightBlinker = 0x1 << 4, LeftBlinker = 0x1 << 5, Reverse = 0x1 << 6, Fog = 0x1 << 7,
    Interior = 0x1 << 8, Special1 = 0x1 << 9, Special2 = 0x1 << 10, All = 0xFFFFFFFF
  };
};

class WalkerControl {
 public:
  WalkerControl() = default;
  WalkerControl(geom::Vector3D in_direction, float in_speed, bool in_jump)
      : direction(in_direction), speed(in_speed), jump(in_jump) {}
  geom::Vector3D direction = {1.0f, 0.0f, 0.0f};
  float speed = 0.0f;
  bool jump = false;
};

class WeatherParameters {
 public:
  static WeatherParameters Default, ClearNoon, CloudyNoon, WetNoon, WetCloudyNoon, MidRainyNoon,
      HardRainNoon, SoftRainNoon, ClearSunset, CloudySunset, WetSunset, WetCloudySunset,
      MidRainSunset, HardRainSunset, SoftRainSunset, ClearNight, CloudyNight, WetNight,
      WetCloudyNight, SoftRainNight, MidRainyNight, HardRainNight, DustStorm;
  WeatherParameters() = default;
  WeatherParameters(float in_cloudiness, float in_precipitation, float in_precipitation_deposits,
                    float in_wind_intensity, float in_sun_azimuth_angle,
                    float in_sun_altitude_angle, float in_fog_density, float in_fog_distance,
                    float in_fog_falloff, float in_wetness, float in_scattering_intensity,
                    float in_mie_scattering_scale, float in_rayleigh_scattering_scale,
                    float in_dust_storm)
      : cloudiness(in_cloudiness), precipitation(in_precipitation),
        precipitation_deposits(in_precipitation_deposits), wind_intensity(in_wind_intensity),
        sun_azimuth_angle(in_sun_azimuth_angle), sun_altitude_angle(in_sun_altitude_angle),
        fog_density(in_fog_density), fog_distance(in_fog_distance), fog_falloff(in_fog_falloff),
        wetness(in_wetness), scattering_intensity(in_scattering_intensity),
        mie_scattering_scale(in_mie_scattering_scale),
        rayleigh_scattering_scale(in_rayleigh_scattering_scale), dust_storm(in_dust_storm) {}
  float cloudiness = 0.0f;
  float precipitation = 0.0f;
  float precipitation_deposits = 0.0f;
  float wind_intensity = 0.0f;
  float sun_azimuth_angle = 0.0f;
  float sun_altitude_angle = 0.0f;
  float fog_density = 0.0f;
  float fog_distance = 0.0f;
  float fog_falloff = 0.0f;
  float wetness = 0.0f;
  float scattering_intensity = 0.0f;
  float mie_scattering_scale = 0.0f;
  float rayleigh_scattering_scale = 0.0331f;
  float dust_storm = 0.0f;
};

class OpendriveGenerationParameters {
 public:
  OpendriveGenerationParameters() = default;
  OpendriveGenerationParameters(double v_distance, double max_road_len, double w_height,
                                double a_width, bool smooth_junc, bool e_visibility,
                                bool e_pedestrian)
      : vertex_distance(v_distance), max_road_length(max_road_len), wall_height(w_height),
        additional_width(a_width), smooth_junctions(smooth_junc),
        enable_mesh_visibility(e_visibility), enable_pedestrian_navigation(e_pedestrian) {}
  double vertex_distance = 2.0;
  double max_road_length = 50.0;
  double wall_height = 1.0;
  double additional_width = 0.6;
  bool smooth_junctions = true;
  bool enable_mesh_visibility = true;
  bool enable_pedestrian_navigation = true;
};

class ActorDescription {
 public:
  std::string id;
  std::map<std::string, std::string> attributes;
};

class ResponseError {
 public:
  ResponseError() = default;
  explicit ResponseError(std::string message) : _what(std::move(message)) {}
  const std::string &What() const { return _what; }

 private:
  std::string _what;
};

template <typename T>
class Response {
 public:
  using value_type = T;
  using error_type = ResponseError;
  Response() = default;
  Response(T value) : _data(std::move(value)) {}
  Response(ResponseError error) : _data(std::move(error)) {}
  bool HasError() const { return _data.index() == 0; }
  const error_type &GetError() const { return std::get<ResponseError>(_data); }
  value_type &Get() { return std::get<T>(_data); }
  const value_type &Get() const { return std::get<T>(_data); }

 private:
  std::variant<ResponseError, T> _data;
};

using CommandResponse = Response<ActorId>;

class Command {
 private:
  template <typename T>
  struct CommandBase {
    operator Command() const { return Command{*static_cast<const T *>(this)}; }
  };

 public:
  struct SpawnActor : CommandBase<SpawnActor> {
    SpawnActor() = default;
    SpawnActor(ActorDescription d, const geom::Transform &t) : description(std::move(d)), transform(t) {}
    SpawnActor(ActorDescription d, const geom::Transform &t, ActorId p)
        : description(std::move(d)), transform(t), parent(p) {}
    ActorDescription description;
    geom::Transform transform;
    std::optional<ActorId> parent;
    std::vector<Command> do_after;
  };
  struct DestroyActor : CommandBase<DestroyActor> {
    DestroyActor(ActorId id) : actor(id) {}
    ActorId actor;
  };
  struct ApplyVehicleControl : CommandBase<ApplyVehicleControl> {
    ApplyVehicleControl(ActorId id, const VehicleControl &value) : actor(id), control(value) {}
    ActorId actor;
    VehicleControl control;
  };
  struct ApplyTransform : CommandBase<ApplyTransform> {
    ApplyTransform(ActorId id, const geom::Transform &value) : actor(id), transform(value) {}
    ActorId actor;
    geom::Transform transform;
  };
  struct ApplyTargetVelocity : CommandBase<ApplyTargetVelocity> {
    ApplyTargetVelocity(ActorId id, const geom::Vector3D &value) : actor(id), velocity(value) {}
    ActorId actor;
    geom::Vector3D velocity;
  };
  struct SetSimulatePhysics : CommandBase<SetSimulatePhysics> {
    SetSimulatePhysics(ActorId id, bool value) : actor(id), enabled(value) {}
    ActorId actor;
    bool enabled;
  };
  struct ApplyWalkerControl : CommandBase<ApplyWalkerControl> {
    ApplyWalkerControl(ActorId id, const WalkerControl &value) : actor(id), control(value) {}
    ActorId actor;
    WalkerControl control;
  };
  // Vector-valued physics commands share one shape.
  struct ApplyTargetAngularVelocity : CommandBase<ApplyTargetAngularVelocity> {
    ApplyTargetAngularVelocity(ActorId id, const geom::Vector3D &value) : actor(id), angular_velocity(value) {}
    ActorId actor;
    geom::Vector3D angular_velocity;
  };
  struct ApplyImpulse : CommandBase<ApplyImpulse> {
    ApplyImpulse(ActorId id, const geom::Vector3D &value) : actor(id), impulse(value) {}
    ActorId actor;
    geom::Vector3D impulse;
  };
  struct ApplyForce : CommandBase<ApplyForce> {
    ApplyForce(ActorId id, const geom::Vector3D &value) : actor(id), force(value) {}
    ActorId actor;
    geom::Vector3D force;
  };
  struct ApplyAngularImpulse : CommandBase<ApplyAngularImpulse> {
    ApplyAngularImpulse(ActorId id, const geom::Vector3D &value) : actor(id), impulse(value) {}
    ActorId actor;
    geom::Vector3D impulse;
  };
  struct ApplyTorque : CommandBase<ApplyTorque> {
    ApplyTorque(ActorId id, const geom::Vector3D &value) : actor(id), torque(value) {}
    ActorId actor;
    geom::Vector3D torque;
  };
  struct SetEnableGravity : CommandBase<SetEnableGravity> {
    SetEnableGravity(ActorId id, bool value) : actor(id), enabled(value) {}
    ActorId actor;
    bool enabled;
  };
  struct SetVehicleLightState : CommandBase<SetVehicleLightState> {
    SetVehicleLightState(ActorId id, VehicleLightState::flag_type value) : actor(id), light_state(value) {}
    ActorId actor;
    VehicleLightState::flag_type light_state;
  };
  struct ApplyLocation : CommandBase<ApplyLocation> {
    ApplyLocation(ActorId id, const geom::Location &value) : actor(id), location(value) {}
    ActorId actor;
    geom::Location location;
  };
  struct SetTrafficLightState : CommandBase<SetTrafficLightState> {
    SetTrafficLightState(ActorId id, TrafficLightState state) : actor(id), traffic_light_state(state) {}
    ActorId actor;
    TrafficLightState traffic_light_state;
  };
  struct SetAutopilot : CommandBase<SetAutopilot> {
    SetAutopilot(ActorId id, bool value, uint16_t port) : actor(id), enabled(value), tm_port(port) {}
    ActorId actor;
    bool enabled;
    uint16_t tm_port;
  };

  using CommandType =
      std::variant<SpawnActor, DestroyActor, ApplyVehicleControl, ApplyTransform, ApplyTargetVelocity,
                   SetSimulatePhysics, SetAutopilot, ApplyWalkerControl, ApplyTargetAngularVelocity,
                   ApplyImpulse, ApplyForce, ApplyAngularImpulse, ApplyTorque, SetEnableGravity,
                   SetVehicleLightState, ApplyLocation, SetTrafficLightState>;
  CommandType command;
};

}  // namespace rpc

namespace sensor {
class SensorData;
}  // namespace sensor

namespace client {

class TimeoutException : public std::runtime_error {
 public:
  TimeoutException(const std::string &endpoint, time_duration timeout);
};

namespace mock {
struct Episode;
struct ActorData;
}  // namespace mock

class Junction;
class Landmark;

class Timestamp {
 public:
  std::size_t frame = 0u;
  double elapsed_seconds = 0.0;
  double delta_seconds = 0.0;
  double platform_timestamp = 0.0;
};

struct ActorSnapshot {
  rpc::ActorId id = 0u;
  geom::Transform transform;
  geom::Vector3D velocity;
  geom::Vector3D angular_velocity;
  geom::Vector3D acceleration;
};

class WorldSnapshot {
 public:
  WorldSnapshot(uint64_t id, Timestamp timestamp, std::vector<ActorSnapshot> actors)
      : _id(id), _timestamp(timestamp), _actors(std::move(actors)) {}
  uint64_t GetId() const { return _id; }
  size_t GetFrame() const { return _timestamp.frame; }
  const Timestamp &GetTimestamp() const { return _timestamp; }
  bool Contains(rpc::ActorId id) const { return Find(id).has_value(); }
  std::optional<ActorSnapshot> Find(rpc::ActorId id) const;
  size_t size() const { return _actors.size(); }
  auto begin() const { return _actors.begin(); }
  auto end() const { return _actors.end(); }

 private:
  uint64_t _id;
  Timestamp _timestamp;
  std::vector<ActorSnapshot> _actors;
};

// The mock map: one straight road (id 1) along +x from x=0 to x=200 with two
// driving lanes, lane -1 at y=0 and lane -2 at y=3.5.
class Waypoint : public std::enable_shared_from_this<Waypoint> {
 public:
  Waypoint(int32_t lane_id, double s);
  uint64_t GetId() const;
  uint32_t GetRoadId() const { return 1u; }
  uint32_t GetSectionId() const { return 0u; }
  int32_t GetLaneId() const { return _lane_id; }
  double GetDistance() const { return _s; }
  const geom::Transform &GetTransform() const { return _transform; }
  int32_t GetJunctionId() const { return -1; }
  bool IsJunction() const { return false; }
  double GetLaneWidth() const { return 3.5; }
  road::Lane::LaneType GetType() const { return road::Lane::LaneType::Driving; }
  std::vector<SharedPtr<Waypoint>> GetNext(double distance) const;
  std::vector<SharedPtr<Waypoint>> GetPrevious(double distance) const;
  std::vector<SharedPtr<Waypoint>> GetNextUntilLaneEnd(double distance) const;
  std::vector<SharedPtr<Waypoint>> GetPreviousUntilLaneStart(double distance) const;
  SharedPtr<Waypoint> GetRight() const;
  SharedPtr<Waypoint> GetLeft() const;
  SharedPtr<Junction> GetJunction() const { return nullptr; }  // the mock road has none

 private:
  int32_t _lane_id;
  double _s;
  geom::Transform _transform;
};

class Map : public std::enable_shared_from_this<Map> {
 public:
  Map();
  const std::string &GetName() const { return _name; }
  const std::string &GetOpenDrive() const { return _xodr; }
  const std::vector<geom::Transform> &GetRecommendedSpawnPoints() const { return _spawn_points; }
  SharedPtr<Waypoint> GetWaypoint(const geom::Location &location, bool project_to_road = true,
                                  int32_t lane_type = static_cast<int32_t>(road::Lane::LaneType::Driving)) const;
  std::vector<SharedPtr<Waypoint>> GenerateWaypoints(double distance) const;
  std::vector<std::pair<SharedPtr<Waypoint>, SharedPtr<Waypoint>>> GetTopology() const;
  std::vector<geom::Location> GetAllCrosswalkZones() const;
  std::vector<SharedPtr<Landmark>> GetAllLandmarks() const;
  std::vector<SharedPtr<Landmark>> GetAllLandmarksOfType(std::string type) const;

 private:
  std::string _name;
  std::string _xodr;
  std::vector<geom::Transform> _spawn_points;
};

class ActorAttribute {
 public:
  ActorAttribute(std::string id, rpc::ActorAttributeType type, std::string value,
                 bool is_modifiable)
      : _id(std::move(id)), _type(type), _value(std::move(value)), _modifiable(is_modifiable) {}
  const std::string &GetId() const { return _id; }
  rpc::ActorAttributeType GetType() const { return _type; }
  const std::string &GetValue() const { return _value; }
  bool IsModifiable() const { return _modifiable; }
  // Throws std::invalid_argument when not modifiable or not parseable.
  void Set(std::string value);

 private:
  std::string _id;
  rpc::ActorAttributeType _type;
  std::string _value;
  bool _modifiable;
};

class ActorBlueprint {
 public:
  explicit ActorBlueprint(std::string id, std::vector<std::string> tags = {},
                          std::vector<ActorAttribute> attributes = {});
  const std::string &GetId() const { return _id; }
  bool ContainsTag(const std::string &tag) const;
  bool MatchTags(const std::string &wildcard_pattern) const;
  bool ContainsAttribute(const std::string &id) const { return _attributes.count(id) > 0; }
  // Throws std::out_of_range for an unknown attribute.
  const ActorAttribute &GetAttribute(const std::string &id) const;
  void SetAttribute(const std::string &id, std::string value);
  size_t size() const { return _attributes.size(); }
  rpc::ActorDescription MakeActorDescription() const;

 private:
  std::string _id;
  std::vector<std::string> _tags;
  std::map<std::string, ActorAttribute> _attributes;
};

class BlueprintLibrary : public std::enable_shared_from_this<BlueprintLibrary> {
 public:
  using value_type = ActorBlueprint;
  using size_type = size_t;
  using const_pointer = const ActorBlueprint *;
  using const_reference = const ActorBlueprint &;

  explicit BlueprintLibrary(std::vector<ActorBlueprint> blueprints)
      : _blueprints(std::move(blueprints)) {}
  SharedPtr<BlueprintLibrary> Filter(const std::string &wildcard_pattern) const;
  const_pointer Find(const std::string &key) const;
  const_reference at(size_type pos) const { return _blueprints.at(pos); }
  size_type size() const { return _blueprints.size(); }

 private:
  std::vector<ActorBlueprint> _blueprints;
};

class Actor : public std::enable_shared_from_this<Actor> {
 public:
  Actor(std::shared_ptr<mock::Episode> episode, rpc::ActorId id);
  virtual ~Actor() = default;
  rpc::ActorId GetId() const { return _id; }
  const std::string &GetTypeId() const { return _type_id; }
  bool IsAlive() const;
  geom::Location GetLocation() const;
  geom::Transform GetTransform() const;
  geom::Vector3D GetVelocity() const;
  geom::Vector3D GetAngularVelocity() const;
  geom::Vector3D GetAcceleration() const;
  void SetLocation(const geom::Location &location);
  void SetTransform(const geom::Transform &transform);
  void SetTargetVelocity(const geom::Vector3D &vector);
  void SetTargetAngularVelocity(const geom::Vector3D &vector);
  void AddImpulse(const geom::Vector3D &vector);
  void AddForce(const geom::Vector3D &force);
  void AddAngularImpulse(const geom::Vector3D &vector);
  void AddTorque(const geom::Vector3D &vector);
  void SetSimulatePhysics(bool enabled = true);
  void SetEnableGravity(bool enabled = true);
  const geom::BoundingBox &GetBoundingBox() const { return _bounding_box; }
  virtual bool Destroy();

 protected:
  // Locks the episode and returns the live actor record, or throws.
  template <typename F>
  auto WithData(F &&fn) const;

  std::shared_ptr<mock::Episode> _episode;
  rpc::ActorId _id;
  std::string _type_id;
  geom::BoundingBox _bounding_box;
};

class TrafficLight;

class Vehicle : public Actor {
 public:
  using Control = rpc::VehicleControl;
  using Actor::Actor;
  void SetAutopilot(bool enabled = true, uint16_t tm_port = 8000);
  void ApplyControl(const Control &control);
  Control GetControl() const;
  using PhysicsControl = rpc::VehiclePhysicsControl;
  void ApplyPhysicsControl(const PhysicsControl &physics_control);
  PhysicsControl GetPhysicsControl() const;
  using LightState = rpc::VehicleLightState::LightState;
  void SetLightState(const LightState &light_state);
  LightState GetLightState() const;
  float GetSpeedLimit() const { return 30.0f; }
  // The mock's traffic light controls vehicles within 15 m of it.
  rpc::TrafficLightState GetTrafficLightState() const;
  bool IsAtTrafficLight();
  SharedPtr<TrafficLight> GetTrafficLight() const;
};

// Mock sensors produce synthetic measurements on every tick (see mock.cpp).
class Sensor : public Actor {
 public:
  using CallbackFunctionType = std::function<void(SharedPtr<sensor::SensorData>)>;
  using Actor::Actor;
  ~Sensor() override;  // stops listening, like LibCarla's ServerSideSensor
  void Listen(CallbackFunctionType callback);
  void Stop();
  bool IsListening() const;
};

class ActorList : public std::enable_shared_from_this<ActorList> {
 public:
  explicit ActorList(std::vector<SharedPtr<Actor>> actors) : _actors(std::move(actors)) {}
  SharedPtr<Actor> Find(rpc::ActorId actor_id) const;
  SharedPtr<ActorList> Filter(const std::string &wildcard_pattern) const;
  SharedPtr<Actor> operator[](size_t pos) const { return _actors[pos]; }
  SharedPtr<Actor> at(size_t pos) const { return _actors.at(pos); }
  bool empty() const { return _actors.empty(); }
  size_t size() const { return _actors.size(); }

 private:
  std::vector<SharedPtr<Actor>> _actors;
};

class Walker : public Actor {
 public:
  using Control = rpc::WalkerControl;
  using Actor::Actor;
  void ApplyControl(const Control &control);
  Control GetWalkerControl() const;
};

// Moves its parent walker towards the destination at up to max speed.
class WalkerAIController : public Actor {
 public:
  using Actor::Actor;
  void Start();
  void Stop();
  void GoToLocation(const geom::Location &destination);
  void SetMaxSpeed(float max_speed);
};

// Cycles Green -> Yellow -> Red with the configured times unless frozen.
class TrafficLight : public Actor {
 public:
  using Actor::Actor;
  void SetState(rpc::TrafficLightState state);
  rpc::TrafficLightState GetState() const;
  void SetGreenTime(float t);
  float GetGreenTime() const;
  void SetYellowTime(float t);
  float GetYellowTime() const;
  void SetRedTime(float t);
  float GetRedTime() const;
  float GetElapsedTime() const;
  void Freeze(bool freeze);
  bool IsFrozen() const;
  uint32_t GetPoleIndex() { return 0u; }
  void ResetGroup();
};

class Landmark {
 public:
  Landmark(std::string id, std::string name, std::string type, double s, geom::Transform t)
      : _id(std::move(id)), _name(std::move(name)), _type(std::move(type)), _s(s), _transform(t) {}
  std::string GetId() const { return _id; }
  std::string GetName() const { return _name; }
  std::string GetType() const { return _type; }
  std::string GetSubType() const { return "-1"; }
  std::string GetCountry() const { return "OpenDRIVE"; }
  std::string GetUnit() const { return ""; }
  std::string GetText() const { return ""; }
  uint32_t GetRoadId() const { return 1u; }
  int32_t GetOrientation() const { return 0; }
  double GetS() const { return _s; }
  double GetT() const { return 3.0; }
  double GetDistance() const { return 0.0; }
  double GetZOffset() const { return 0.0; }
  double GetValue() const { return -1.0; }
  double GetHeight() const { return 1.0; }
  double GetWidth() const { return 0.5; }
  const geom::Transform &GetTransform() const { return _transform; }

 private:
  std::string _id, _name, _type;
  double _s;
  geom::Transform _transform;
};

class Junction {
 public:
  int32_t GetId() const { return -1; }
  std::vector<std::pair<SharedPtr<Waypoint>, SharedPtr<Waypoint>>> GetWaypoints(
      road::Lane::LaneType = road::Lane::LaneType::Driving) const {
    return {};
  }
  geom::BoundingBox GetBoundingBox() const { return {}; }
};

}  // namespace client
namespace sensor { namespace data { struct Color; } }
namespace client {

// Debug drawing is accepted and counted (see mock::Episode::debug_shapes).
class DebugHelper {
 public:
  explicit DebugHelper(std::shared_ptr<mock::Episode> episode) : _episode(std::move(episode)) {}
  void DrawPoint(const geom::Location &location, float size, sensor::data::Color color,
                 float life_time, bool persistent_lines = true);
  void DrawLine(const geom::Location &begin, const geom::Location &end, float thickness,
                sensor::data::Color color, float life_time, bool persistent_lines = true);
  void DrawArrow(const geom::Location &begin, const geom::Location &end, float thickness,
                 float arrow_size, sensor::data::Color color, float life_time,
                 bool persistent_lines = true);
  void DrawBox(const geom::BoundingBox &box, const geom::Rotation &rotation, float thickness,
               sensor::data::Color color, float life_time, bool persistent_lines = true);
  void DrawString(const geom::Location &location, const std::string &text, bool draw_shadow,
                  sensor::data::Color color, float life_time, bool persistent_lines = true);

 private:
  std::shared_ptr<mock::Episode> _episode;
};

class World {
 public:
  explicit World(std::shared_ptr<mock::Episode> episode) : _episode(std::move(episode)) {}
  uint64_t GetId() const;
  SharedPtr<BlueprintLibrary> GetBlueprintLibrary() const;
  SharedPtr<ActorList> GetActors() const;
  SharedPtr<Actor> GetActor(rpc::ActorId id) const;
  SharedPtr<Actor> SpawnActor(const ActorBlueprint &blueprint, const geom::Transform &transform,
                              Actor *parent = nullptr,
                              rpc::AttachmentType attachment_type = rpc::AttachmentType::Rigid,
                              const std::string &socket_name = "");
  SharedPtr<Actor> TrySpawnActor(const ActorBlueprint &blueprint,
                                 const geom::Transform &transform, Actor *parent = nullptr,
                                 rpc::AttachmentType attachment_type = rpc::AttachmentType::Rigid,
                                 const std::string &socket_name = "") noexcept;
  uint64_t Tick(time_duration timeout);
  SharedPtr<Map> GetMap() const;
  WorldSnapshot GetSnapshot() const;
  // Asynchronous mode: the mock server "ticks on its own", so this steps once.
  // Synchronous mode: nobody else ticks, so this times out.
  WorldSnapshot WaitForTick(time_duration timeout) const;
  std::optional<geom::Location> GetRandomLocationFromNavigation() const;
  rpc::WeatherParameters GetWeather() const;
  void SetWeather(const rpc::WeatherParameters &weather);
  bool IsWeatherEnabled() const { return true; }
  DebugHelper MakeDebugHelper() const { return DebugHelper(_episode); }
  rpc::EpisodeSettings GetSettings() const;
  uint64_t ApplySettings(const rpc::EpisodeSettings &settings, time_duration timeout);

 private:
  std::shared_ptr<mock::Episode> _episode;
};

}  // namespace client

namespace traffic_manager {

// Records settings; the mock's autopilot drives straight at 0.5 throttle,
// scaled by the global percentage speed difference.
class TrafficManager {
 public:
  using ActorPtr = SharedPtr<client::Actor>;
  TrafficManager(std::shared_ptr<client::mock::Episode> episode, uint16_t port)
      : _episode(std::move(episode)), _port(port) {}
  uint16_t Port() const { return _port; }
  void SetSynchronousMode(bool mode);
  void SetRandomDeviceSeed(uint64_t seed);
  void SetHybridPhysicsMode(bool mode);
  void SetGlobalPercentageSpeedDifference(float percentage);
  void SetGlobalDistanceToLeadingVehicle(float distance);
  void SetPercentageSpeedDifference(const ActorPtr &actor, float percentage);
  void SetDistanceToLeadingVehicle(const ActorPtr &actor, float distance);
  void SetRandomLeftLaneChangePercentage(const ActorPtr &actor, float percentage);
  void SetRandomRightLaneChangePercentage(const ActorPtr &actor, float percentage);
  void SetPercentageRunningLight(const ActorPtr &actor, float percentage);
  void SetPercentageRunningSign(const ActorPtr &actor, float percentage);
  void SetPercentageIgnoreVehicles(const ActorPtr &actor, float percentage);
  void SetPercentageIgnoreWalkers(const ActorPtr &actor, float percentage);
  void SetKeepRightPercentage(const ActorPtr &actor, float percentage);
  void SetDesiredSpeed(const ActorPtr &actor, float value);
  void SetLaneOffset(const ActorPtr &actor, float offset);
  void SetAutoLaneChange(const ActorPtr &actor, bool enable);
  void SetForceLaneChange(const ActorPtr &actor, bool direction);
  void SetUpdateVehicleLights(const ActorPtr &actor, bool do_update);

 private:
  std::shared_ptr<client::mock::Episode> _episode;
  uint16_t _port;
};

}  // namespace traffic_manager

namespace client {

class Client {
 public:
  explicit Client(const std::string &host, uint16_t port, size_t worker_threads = 0u);
  void SetTimeout(time_duration timeout) { _timeout = timeout; }
  time_duration GetTimeout() { return _timeout; }
  std::string GetClientVersion() const { return carla::version(); }
  std::string GetServerVersion() const;
  World GetWorld() const;
  World ReloadWorld(bool reset_settings = true) const;
  World LoadWorld(std::string map_name, bool reset_settings = true,
                  rpc::MapLayer map_layers = rpc::MapLayer::All) const;
  void ApplyBatch(std::vector<rpc::Command> commands, bool do_tick_cue = false) const;
  traffic_manager::TrafficManager GetInstanceTM(uint16_t port = 8000) const;
  std::string StartRecorder(std::string name, bool additional_data = false);
  void StopRecorder();
  std::string ShowRecorderFileInfo(std::string name, bool show_all);
  std::string ShowRecorderCollisions(std::string name, char type1, char type2);
  std::string ShowRecorderActorsBlocked(std::string name, double min_time, double min_distance);
  std::string ReplayFile(std::string name, double start, double duration, uint32_t follow_id,
                         bool replay_sensors);
  void StopReplayer(bool keep_actors);
  void SetReplayerTimeFactor(double time_factor);
  World GenerateOpenDriveWorld(std::string opendrive,
                               const rpc::OpendriveGenerationParameters &params,
                               bool reset_settings = true) const;
  std::vector<rpc::CommandResponse> ApplyBatchSync(std::vector<rpc::Command> commands,
                                                   bool do_tick_cue = false) const;

 private:
  std::string _endpoint;
  time_duration _timeout = time_duration::milliseconds(5000);
};

}  // namespace client

namespace sensor {

class SensorData : public std::enable_shared_from_this<SensorData> {
 public:
  SensorData(size_t frame, double timestamp, const geom::Transform &transform)
      : _frame(frame), _timestamp(timestamp), _transform(transform) {}
  virtual ~SensorData() = default;
  size_t GetFrame() const { return _frame; }
  double GetTimestamp() const { return _timestamp; }
  const geom::Transform &GetSensorTransform() const { return _transform; }

 private:
  size_t _frame;
  double _timestamp;
  geom::Transform _transform;
};

namespace data {

struct Color {
  Color() = default;
  Color(uint8_t in_r, uint8_t in_g, uint8_t in_b, uint8_t in_a = 255u)
      : b(in_b), g(in_g), r(in_r), a(in_a) {}
  uint8_t b = 0u;
  uint8_t g = 0u;
  uint8_t r = 0u;
  uint8_t a = 0u;
};

class Image : public SensorData {
 public:
  Image(size_t frame, double timestamp, const geom::Transform &t, size_t width, size_t height,
        float fov)
      : SensorData(frame, timestamp, t), _width(width), _height(height), _fov(fov),
        _pixels(width * height) {}
  size_t GetWidth() const { return _width; }
  size_t GetHeight() const { return _height; }
  float GetFOVAngle() const { return _fov; }
  const Color *data() const { return _pixels.data(); }
  Color *data() { return _pixels.data(); }
  size_t size() const { return _pixels.size(); }

 private:
  size_t _width, _height;
  float _fov;
  std::vector<Color> _pixels;
};

struct LidarDetection {
  geom::Location point;
  float intensity = 0.0f;
};

class LidarMeasurement : public SensorData {
 public:
  LidarMeasurement(size_t frame, double timestamp, const geom::Transform &t, float angle,
                   std::vector<uint32_t> per_channel, std::vector<LidarDetection> points)
      : SensorData(frame, timestamp, t), _angle(angle), _per_channel(std::move(per_channel)),
        _points(std::move(points)) {}
  float GetHorizontalAngle() const { return _angle; }
  uint32_t GetChannelCount() const { return static_cast<uint32_t>(_per_channel.size()); }
  uint32_t GetPointCount(size_t channel) const { return _per_channel.at(channel); }
  const LidarDetection *data() const { return _points.data(); }
  size_t size() const { return _points.size(); }

 private:
  float _angle;
  std::vector<uint32_t> _per_channel;
  std::vector<LidarDetection> _points;
};

class GnssMeasurement : public SensorData {
 public:
  GnssMeasurement(size_t frame, double timestamp, const geom::Transform &t, double lat,
                  double lon, double alt)
      : SensorData(frame, timestamp, t), _lat(lat), _lon(lon), _alt(alt) {}
  double GetLatitude() const { return _lat; }
  double GetLongitude() const { return _lon; }
  double GetAltitude() const { return _alt; }

 private:
  double _lat, _lon, _alt;
};

class IMUMeasurement : public SensorData {
 public:
  IMUMeasurement(size_t frame, double timestamp, const geom::Transform &t,
                 const geom::Vector3D &accel, const geom::Vector3D &gyro, float compass)
      : SensorData(frame, timestamp, t), _accel(accel), _gyro(gyro), _compass(compass) {}
  geom::Vector3D GetAccelerometer() const { return _accel; }
  geom::Vector3D GetGyroscope() const { return _gyro; }
  float GetCompass() const { return _compass; }

 private:
  geom::Vector3D _accel, _gyro;
  float _compass;
};

class CollisionEvent : public SensorData {
 public:
  CollisionEvent(size_t frame, double timestamp, const geom::Transform &t,
                 SharedPtr<client::Actor> self, SharedPtr<client::Actor> other,
                 const geom::Vector3D &impulse)
      : SensorData(frame, timestamp, t), _self(std::move(self)), _other(std::move(other)),
        _impulse(impulse) {}
  SharedPtr<client::Actor> GetActor() const { return _self; }
  SharedPtr<client::Actor> GetOtherActor() const { return _other; }
  const geom::Vector3D &GetNormalImpulse() const { return _impulse; }

 private:
  SharedPtr<client::Actor> _self, _other;
  geom::Vector3D _impulse;
};

}  // namespace data
}  // namespace sensor
}  // namespace carla
