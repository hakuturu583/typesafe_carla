// In-memory stand-in for the subset of LibCarla used by typesafe_carla.
//
// Class names, member names and signatures mirror LibCarla (CARLA UE5, ue5-dev) so that the
// shim in native/src compiles unchanged against either. The "server" is a
// process-wide in-memory episode keyed by host:port with a toy vehicle model.
// It exists for tests and for developing without a running CARLA server; it is
// not a simulator.
#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
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

}  // namespace geom

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

}  // namespace rpc

namespace client {

class TimeoutException : public std::runtime_error {
 public:
  TimeoutException(const std::string &endpoint, time_duration timeout);
};

namespace mock {
struct Episode;
struct ActorData;
}  // namespace mock

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
  virtual bool Destroy();

 protected:
  // Locks the episode and returns the live actor record, or throws.
  template <typename F>
  auto WithData(F &&fn) const;

  std::shared_ptr<mock::Episode> _episode;
  rpc::ActorId _id;
  std::string _type_id;
};

class Vehicle : public Actor {
 public:
  using Control = rpc::VehicleControl;
  using Actor::Actor;
  void SetAutopilot(bool enabled = true, uint16_t tm_port = 8000);
  void ApplyControl(const Control &control);
  Control GetControl() const;
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

 private:
  std::string _endpoint;
  time_duration _timeout = time_duration::milliseconds(5000);
};

}  // namespace client
}  // namespace carla
