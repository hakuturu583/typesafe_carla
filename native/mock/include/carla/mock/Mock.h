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
  struct SetAutopilot : CommandBase<SetAutopilot> {
    SetAutopilot(ActorId id, bool value, uint16_t port) : actor(id), enabled(value), tm_port(port) {}
    ActorId actor;
    bool enabled;
    uint16_t tm_port;
  };

  using CommandType = std::variant<SpawnActor, DestroyActor, ApplyVehicleControl, ApplyTransform,
                                   ApplyTargetVelocity, SetSimulatePhysics, SetAutopilot>;
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
};

// Mock sensors produce synthetic measurements on every tick (see mock.cpp).
class Sensor : public Actor {
 public:
  using CallbackFunctionType = std::function<void(SharedPtr<sensor::SensorData>)>;
  using Actor::Actor;
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
  rpc::EpisodeSettings GetSettings() const;
  uint64_t ApplySettings(const rpc::EpisodeSettings &settings, time_duration timeout);

 private:
  std::shared_ptr<mock::Episode> _episode;
};

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
