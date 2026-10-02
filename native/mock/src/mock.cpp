// Implementation of the in-memory mock LibCarla (see carla/mock/Mock.h).
#include "carla/mock/Mock.h"

#include <cmath>
#include <cstdlib>

namespace carla {
namespace client {
namespace mock {

constexpr double kDefaultDeltaSeconds = 0.05;
constexpr double kSpawnClearance = 2.0;  // meters between spawned vehicles

struct ActorData {
  rpc::ActorId id = 0;
  std::string type_id;
  bool is_vehicle = false;
  geom::Transform transform;
  geom::Vector3D velocity;
  geom::Vector3D acceleration;
  geom::Vector3D angular_velocity;
  rpc::VehicleControl control;
  rpc::VehiclePhysicsControl physics;
  bool autopilot = false;
  bool simulate_physics = true;
  bool gravity = true;
  uint32_t light_state = 0u;
  // Walkers and their AI controllers.
  rpc::WalkerControl walker_control{geom::Vector3D(1.0f, 0.0f, 0.0f), 0.0f, false};
  bool ai_running = false;
  std::optional<geom::Location> ai_target;
  float ai_max_speed = 1.4f;
  // Traffic lights (CARLA's default times).
  rpc::TrafficLightState light = rpc::TrafficLightState::Green;
  float green_time = 10.0f, yellow_time = 3.0f, red_time = 2.0f, elapsed = 0.0f;
  bool frozen = false;
  // Attached actors (sensors) follow their parent at a fixed offset.
  std::optional<rpc::ActorId> parent;
  geom::Transform offset;
  std::map<std::string, std::string> attributes;  // blueprint attributes at spawn

  bool is_walker() const { return type_id.rfind("walker.", 0) == 0; }
  bool is_walker_ai_controller() const { return type_id == "controller.ai.walker"; }
  bool is_traffic_light() const { return type_id == "traffic.traffic_light"; }
  void set_light(rpc::TrafficLightState state) {  // restarts the phase timer
    light = state;
    elapsed = 0.0f;
  }
};

using Delivery = std::function<void()>;  // run after the episode lock is released

rpc::VehiclePhysicsControl DefaultPhysics() {
  rpc::VehiclePhysicsControl pc;
  pc.mass = 1500.0f;
  pc.wheels.resize(4);
  pc.wheels[2].affected_by_steering = pc.wheels[3].affected_by_steering = false;
  pc.wheels[2].max_steer_angle = pc.wheels[3].max_steer_angle = 0.0f;
  return pc;
}

struct Episode : std::enable_shared_from_this<Episode> {
  std::mutex mutex;
  uint64_t id = 1;
  uint64_t frame = 0;
  rpc::ActorId next_actor_id = 1;
  rpc::EpisodeSettings settings;
  std::map<rpc::ActorId, ActorData> actors;  // ordered: GetActors() is deterministic
  // Actors destroyed in this episode. LibCarla's Episode::GetActorById reads a
  // client-side cache (CachedActorList) that destroying an actor never clears,
  // so World::GetActor keeps returning them until the episode changes.
  std::map<rpc::ActorId, ActorData> destroyed;
  // Listening is per client-side Sensor object, like LibCarla's ServerSideSensor.
  struct Listener {
    const Sensor *owner;
    std::function<void(SharedPtr<sensor::SensorData>)> callback;
  };
  std::map<rpc::ActorId, Listener> listeners;
  rpc::WeatherParameters weather = rpc::WeatherParameters::ClearNoon;
  size_t debug_shapes = 0;      // DebugHelper calls (nothing is drawn)
  std::string recording;        // the active recorder file, if any
  std::map<std::string, uint64_t> recordings;  // file -> frames recorded
  float tm_global_speed_difference = 0.0f;     // percent slower than the limit

  double Mass(const ActorData &a) const { return a.is_vehicle ? a.physics.mass : 80.0; }

  rpc::ActorId AddActorLocked(const std::string &type_id, bool is_vehicle,
                              const geom::Transform &transform) {
    ActorData data;
    data.id = next_actor_id++;
    data.type_id = type_id;
    data.is_vehicle = is_vehicle;
    data.transform = transform;
    if (is_vehicle) data.physics = DefaultPhysics();
    actors.emplace(data.id, data);
    return data.id;
  }

  // World::SpawnActor and the SpawnActor batch command. Throws on failure.
  rpc::ActorId SpawnLocked(const rpc::ActorDescription &description, geom::Transform t,
                           std::optional<rpc::ActorId> parent) {
    const std::string &type_id = description.id;
    const geom::Transform offset = t;
    if (parent) {
      auto p = actors.find(*parent);
      if (p == actors.end()) throw std::runtime_error("parent actor is destroyed");
      t.location.x += p->second.transform.location.x;
      t.location.y += p->second.transform.location.y;
      t.location.z += p->second.transform.location.z;
      t.rotation.yaw += p->second.transform.rotation.yaw;
    }
    const bool is_vehicle = type_id.rfind("vehicle.", 0) == 0;
    if (is_vehicle) {
      for (const auto &entry : actors) {
        const auto &other = entry.second;
        if (!other.is_vehicle) continue;
        const double d = std::hypot(other.transform.location.x - t.location.x,
                                    other.transform.location.y - t.location.y,
                                    other.transform.location.z - t.location.z);
        if (d < kSpawnClearance) {
          throw std::runtime_error("Spawn failed because of collision at spawn position");
        }
      }
    }
    const rpc::ActorId id = AddActorLocked(type_id, is_vehicle, t);
    ActorData &data = actors.at(id);
    data.parent = parent;
    data.offset = offset;
    data.attributes = description.attributes;
    return id;
  }

  // Synthetic measurements for every listening sensor (see the class comment
  // on client::Sensor). Collision events need actor objects, whose
  // constructors take the episode lock, so they are built in the delivery.
  std::vector<Delivery> SenseLocked();

  // Every actor removal goes through here so listeners never outlive their sensor.
  bool EraseActorLocked(rpc::ActorId id) {
    listeners.erase(id);
    auto it = actors.find(id);
    if (it == actors.end()) return false;
    destroyed.insert_or_assign(id, it->second);
    actors.erase(it);
    return true;
  }

  // A live actor, or one destroyed in this episode (see `destroyed`).
  const ActorData *KnownLocked(rpc::ActorId id) const {
    auto it = actors.find(id);
    if (it != actors.end()) return &it->second;
    auto gone = destroyed.find(id);
    return gone == destroyed.end() ? nullptr : &gone->second;
  }

  ActorData &LiveLocked(rpc::ActorId id) {
    auto it = actors.find(id);
    if (it == actors.end()) throw std::runtime_error("actor " + std::to_string(id) + " not found");
    return it->second;
  }

  double DeltaSeconds() const {
    return settings.fixed_delta_seconds.value_or(kDefaultDeltaSeconds);
  }

  WorldSnapshot SnapshotLocked() const {
    Timestamp t;
    t.frame = frame;
    t.delta_seconds = DeltaSeconds();
    t.elapsed_seconds = static_cast<double>(frame) * t.delta_seconds;
    t.platform_timestamp = t.elapsed_seconds;
    std::vector<ActorSnapshot> snapshot;
    for (const auto &entry : actors) {
      const ActorData &a = entry.second;
      snapshot.push_back(ActorSnapshot{a.id, a.transform, a.velocity, a.angular_velocity,
                                       a.acceleration});
    }
    return WorldSnapshot(id, t, std::move(snapshot));
  }

  void ResetLocked(bool reset_settings, bool with_parked_vehicle) {
    ++id;
    actors.clear();
    destroyed.clear();
    listeners.clear();
    if (reset_settings) settings = rpc::EpisodeSettings{};
    if (with_parked_vehicle) {
      // A pre-existing vehicle so that `world.get_actors()[0].as_vehicle()`
      // works on a fresh server, as in the Milestone 0 success criterion.
      AddActorLocked("vehicle.tesla.model3", true,
                     geom::Transform(geom::Location(0.0f, 0.0f, 0.5f)));
    }
    AddActorLocked("spectator", false, geom::Transform(geom::Location(0.0f, 0.0f, 50.0f)));
    // One traffic light beside the road at x = 100, controlling lane traffic.
    AddActorLocked("traffic.traffic_light", false,
                   geom::Transform(geom::Location(100.0f, -3.0f, 0.0f)));
  }

  // Advances one frame; returns the sensor deliveries to run after unlocking.
  std::vector<Delivery> StepLocked() {
    const double dt = DeltaSeconds();
    for (auto &entry : actors) {
      ActorData &a = entry.second;
      if (!a.is_vehicle || !a.simulate_physics) continue;
      rpc::VehicleControl c = a.control;
      if (a.autopilot) {
        c = rpc::VehicleControl(0.5f * (1.0f - tm_global_speed_difference / 100.0f), 0.0f, 0.0f,
                                false, false, false, 0);
      }
      const double yaw = a.transform.rotation.yaw * M_PI / 180.0;
      const double speed_before = std::hypot(a.velocity.x, a.velocity.y);
      double accel = 6.0 * c.throttle * (c.reverse ? -1.0 : 1.0) - 0.2 * speed_before;
      if (c.brake > 0.0f || c.hand_brake) {
        const double braking = 9.0 * (c.hand_brake ? 1.0 : c.brake);
        accel = speed_before > braking * dt ? -braking : -speed_before / dt;
      }
      const double speed = speed_before + accel * dt;
      a.transform.rotation.yaw += static_cast<float>(c.steer * 40.0 * dt * std::min(speed, 10.0) / 10.0);
      const double new_yaw = a.transform.rotation.yaw * M_PI / 180.0;
      const geom::Vector3D new_velocity(static_cast<float>(speed * std::cos(new_yaw)),
                                        static_cast<float>(speed * std::sin(new_yaw)), 0.0f);
      a.acceleration = geom::Vector3D(static_cast<float>((new_velocity.x - a.velocity.x) / dt),
                                      static_cast<float>((new_velocity.y - a.velocity.y) / dt),
                                      0.0f);
      a.angular_velocity = geom::Vector3D(
          0.0f, 0.0f, static_cast<float>((new_yaw - yaw) * 180.0 / M_PI / dt));
      a.velocity = new_velocity;
      a.transform.location.x += static_cast<float>(new_velocity.x * dt);
      a.transform.location.y += static_cast<float>(new_velocity.y * dt);
    }
    for (auto &entry : actors) {
      ActorData &a = entry.second;
      if (a.is_walker()) {
        const auto &d = a.walker_control.direction;
        const double n = std::hypot(d.x, d.y, d.z);
        const double v = n > 0.0 ? a.walker_control.speed / n : 0.0;
        a.velocity = geom::Vector3D(static_cast<float>(d.x * v), static_cast<float>(d.y * v),
                                    static_cast<float>(d.z * v));
        a.transform.location.x += static_cast<float>(a.velocity.x * dt);
        a.transform.location.y += static_cast<float>(a.velocity.y * dt);
      } else if (a.is_walker_ai_controller() && a.ai_running && a.ai_target && a.parent) {
        auto walker = actors.find(*a.parent);
        if (walker == actors.end()) continue;
        auto &w = walker->second;
        const double dx = a.ai_target->x - w.transform.location.x;
        const double dy = a.ai_target->y - w.transform.location.y;
        const double dist = std::hypot(dx, dy);
        const double step = std::min(dist, static_cast<double>(a.ai_max_speed) * dt);
        if (dist > 1e-6) {
          w.walker_control = rpc::WalkerControl(
              geom::Vector3D(static_cast<float>(dx / dist), static_cast<float>(dy / dist), 0.0f),
              static_cast<float>(step / dt), false);
        } else {
          w.walker_control.speed = 0.0f;
        }
      } else if (a.is_traffic_light() && !a.frozen) {
        a.elapsed += static_cast<float>(dt);
        const float limit = a.light == rpc::TrafficLightState::Green    ? a.green_time
                            : a.light == rpc::TrafficLightState::Yellow ? a.yellow_time
                                                                        : a.red_time;
        if (a.light <= rpc::TrafficLightState::Green && a.elapsed >= limit) {
          a.elapsed = 0.0f;
          a.light = a.light == rpc::TrafficLightState::Green    ? rpc::TrafficLightState::Yellow
                    : a.light == rpc::TrafficLightState::Yellow ? rpc::TrafficLightState::Red
                                                                : rpc::TrafficLightState::Green;
        }
      }
    }
    for (auto &entry : actors) {
      ActorData &a = entry.second;
      if (!a.parent) continue;
      auto p = actors.find(*a.parent);
      if (p == actors.end()) continue;
      const geom::Transform &pt = p->second.transform;
      a.transform = geom::Transform(
          geom::Location(pt.location.x + a.offset.location.x, pt.location.y + a.offset.location.y,
                         pt.location.z + a.offset.location.z),
          geom::Rotation(pt.rotation.pitch + a.offset.rotation.pitch,
                         pt.rotation.yaw + a.offset.rotation.yaw,
                         pt.rotation.roll + a.offset.rotation.roll));
      a.velocity = p->second.velocity;
      a.acceleration = p->second.acceleration;
      a.angular_velocity = p->second.angular_velocity;
    }
    ++frame;
    return SenseLocked();
  }
};

namespace {

std::mutex g_servers_mutex;
std::map<std::string, std::shared_ptr<Episode>> g_servers;

// Hosts ending in ".invalid" behave as unreachable servers (RFC 2606).
bool IsUnreachable(const std::string &endpoint) {
  const std::string host = endpoint.substr(0, endpoint.rfind(':'));
  const std::string suffix = ".invalid";
  return host.size() >= suffix.size() &&
         host.compare(host.size() - suffix.size(), suffix.size(), suffix) == 0;
}

std::shared_ptr<Episode> Connect(const std::string &endpoint, time_duration timeout) {
  if (IsUnreachable(endpoint)) throw TimeoutException(endpoint, timeout);
  std::lock_guard<std::mutex> lock(g_servers_mutex);
  auto &episode = g_servers[endpoint];
  if (episode == nullptr) {
    episode = std::make_shared<Episode>();
    std::lock_guard<std::mutex> episode_lock(episode->mutex);
    episode->ResetLocked(true, true);
  }
  return episode;
}

// Glob match with '*' and '?', like carla::StringUtil::Match.
bool Match(const char *str, const char *pattern) {
  if (*pattern == '\0') return *str == '\0';
  if (*pattern == '*') return Match(str, pattern + 1) || (*str != '\0' && Match(str + 1, pattern));
  if (*str == '\0') return false;
  return (*pattern == '?' || *pattern == *str) && Match(str + 1, pattern + 1);
}

bool Match(const std::string &str, const std::string &pattern) {
  return Match(str.c_str(), pattern.c_str());
}

std::vector<std::string> SplitTags(const std::string &id) {
  std::vector<std::string> tags;
  size_t start = 0;
  while (start <= id.size()) {
    const size_t dot = id.find('.', start);
    const size_t end = dot == std::string::npos ? id.size() : dot;
    if (end > start) tags.push_back(id.substr(start, end - start));
    if (dot == std::string::npos) break;
    start = dot + 1;
  }
  return tags;
}

ActorBlueprint VehicleBlueprint(const std::string &id, const std::string &color) {
  return ActorBlueprint(
      id, SplitTags(id),
      {ActorAttribute("color", rpc::ActorAttributeType::RGBColor, color, true),
       ActorAttribute("role_name", rpc::ActorAttributeType::String, "autopilot", true),
       ActorAttribute("number_of_wheels", rpc::ActorAttributeType::Int, "4", false),
       ActorAttribute("sticky_control", rpc::ActorAttributeType::Bool, "true", true),
       ActorAttribute("base_mass", rpc::ActorAttributeType::Float, "1500.0", false)});
}

std::vector<ActorBlueprint> DefaultBlueprints() {
  return {
      VehicleBlueprint("vehicle.tesla.model3", "17,37,103"),
      VehicleBlueprint("vehicle.lincoln.mkz_2020", "0,0,0"),
      VehicleBlueprint("vehicle.audi.tt", "255,0,0"),
      ActorBlueprint("walker.pedestrian.0001", {"walker", "pedestrian", "0001"},
                     {ActorAttribute("role_name", rpc::ActorAttributeType::String, "pedestrian", true),
                      ActorAttribute("speed", rpc::ActorAttributeType::Float, "1.4", true),
                      ActorAttribute("is_invincible", rpc::ActorAttributeType::Bool, "true", true)}),
      ActorBlueprint("sensor.camera.rgb", {"sensor", "camera", "rgb"},
                     {ActorAttribute("image_size_x", rpc::ActorAttributeType::Int, "800", true),
                      ActorAttribute("image_size_y", rpc::ActorAttributeType::Int, "600", true),
                      ActorAttribute("fov", rpc::ActorAttributeType::Float, "90.0", true),
                      ActorAttribute("role_name", rpc::ActorAttributeType::String, "front", true)}),
      ActorBlueprint("sensor.lidar.ray_cast", {"sensor", "lidar", "ray_cast"},
                     {ActorAttribute("channels", rpc::ActorAttributeType::Int, "32", true),
                      ActorAttribute("range", rpc::ActorAttributeType::Float, "10.0", true),
                      ActorAttribute("points_per_second", rpc::ActorAttributeType::Int, "56000", true),
                      ActorAttribute("rotation_frequency", rpc::ActorAttributeType::Float, "10.0", true)}),
      ActorBlueprint("sensor.other.gnss", {"sensor", "other", "gnss"}, {}),
      ActorBlueprint("sensor.other.imu", {"sensor", "other", "imu"}, {}),
      ActorBlueprint("sensor.other.collision", {"sensor", "other", "collision"}, {}),
      ActorBlueprint("controller.ai.walker", {"controller", "ai", "walker"}, {}),
      ActorBlueprint("static.prop.trafficcone01", {"static", "prop", "trafficcone01"},
                     {ActorAttribute("role_name", rpc::ActorAttributeType::String, "prop", true),
                      ActorAttribute("size", rpc::ActorAttributeType::String, "small", false)}),
  };
}

SharedPtr<Actor> MakeActor(const std::shared_ptr<Episode> &episode, const ActorData &data) {
  if (data.is_vehicle) return std::make_shared<Vehicle>(episode, data.id);
  if (data.type_id.rfind("sensor.", 0) == 0) return std::make_shared<Sensor>(episode, data.id);
  if (data.is_walker()) return std::make_shared<Walker>(episode, data.id);
  if (data.is_walker_ai_controller()) {
    return std::make_shared<WalkerAIController>(episode, data.id);
  }
  if (data.is_traffic_light()) return std::make_shared<TrafficLight>(episode, data.id);
  return std::make_shared<Actor>(episode, data.id);
}

void CheckValue(rpc::ActorAttributeType type, const std::string &value) {
  auto bad = [&]() { throw std::invalid_argument("invalid value '" + value + "' for attribute"); };
  char *end = nullptr;
  switch (type) {
    case rpc::ActorAttributeType::Bool:
      if (value != "true" && value != "false" && value != "True" && value != "False") bad();
      break;
    case rpc::ActorAttributeType::Int:
      std::strtol(value.c_str(), &end, 10);
      if (value.empty() || *end != '\0') bad();
      break;
    case rpc::ActorAttributeType::Float:
      std::strtod(value.c_str(), &end);
      if (value.empty() || *end != '\0') bad();
      break;
    case rpc::ActorAttributeType::RGBColor: {
      int r, g, b;
      char tail;
      if (std::sscanf(value.c_str(), "%d,%d,%d%c", &r, &g, &b, &tail) != 3 || r < 0 || r > 255 ||
          g < 0 || g > 255 || b < 0 || b > 255)
        bad();
      break;
    }
    default:
      break;
  }
}

}  // namespace
}  // namespace mock

// ---------------------------------------------------------------------------

TimeoutException::TimeoutException(const std::string &endpoint, time_duration timeout)
    : std::runtime_error(std::string("time-out of ") + std::to_string(timeout.milliseconds()) +
                         "ms while waiting for the simulator, make sure the simulator is "
                         "ready and connected to " +
                         endpoint) {}

void ActorAttribute::Set(std::string value) {
  if (!_modifiable) throw std::invalid_argument("attribute '" + _id + "' is not modifiable");
  mock::CheckValue(_type, value);
  _value = std::move(value);
}

ActorBlueprint::ActorBlueprint(std::string id, std::vector<std::string> tags,
                               std::vector<ActorAttribute> attributes)
    : _id(std::move(id)), _tags(std::move(tags)) {
  for (auto &attribute : attributes) _attributes.emplace(attribute.GetId(), attribute);
}

bool ActorBlueprint::ContainsTag(const std::string &tag) const {
  for (const auto &t : _tags)
    if (t == tag) return true;
  return false;
}

bool ActorBlueprint::MatchTags(const std::string &wildcard_pattern) const {
  if (mock::Match(_id, wildcard_pattern)) return true;
  for (const auto &t : _tags)
    if (mock::Match(t, wildcard_pattern)) return true;
  return false;
}

const ActorAttribute &ActorBlueprint::GetAttribute(const std::string &id) const {
  auto it = _attributes.find(id);
  if (it == _attributes.end()) throw std::out_of_range("attribute '" + id + "' not found");
  return it->second;
}

void ActorBlueprint::SetAttribute(const std::string &id, std::string value) {
  auto it = _attributes.find(id);
  if (it == _attributes.end()) throw std::out_of_range("attribute '" + id + "' not found");
  it->second.Set(std::move(value));
}

SharedPtr<BlueprintLibrary> BlueprintLibrary::Filter(const std::string &wildcard_pattern) const {
  std::vector<ActorBlueprint> result;
  for (const auto &bp : _blueprints)
    if (bp.MatchTags(wildcard_pattern)) result.push_back(bp);
  return std::make_shared<BlueprintLibrary>(std::move(result));
}

BlueprintLibrary::const_pointer BlueprintLibrary::Find(const std::string &key) const {
  for (const auto &bp : _blueprints)
    if (bp.GetId() == key) return &bp;
  return nullptr;
}

// ---------------------------------------------------------------------------

Actor::Actor(std::shared_ptr<mock::Episode> episode, rpc::ActorId id)
    : _episode(std::move(episode)), _id(id) {
  std::lock_guard<std::mutex> lock(_episode->mutex);
  const mock::ActorData *known = _episode->KnownLocked(id);
  if (known == nullptr) throw std::out_of_range("unknown actor " + std::to_string(id));
  const mock::ActorData &data = *known;
  _type_id = data.type_id;
  _bounding_box = data.is_vehicle
                      ? geom::BoundingBox(geom::Location(0.0f, 0.0f, 0.7f), geom::Vector3D(2.4f, 1.0f, 0.75f))
                      : geom::BoundingBox(geom::Location(), geom::Vector3D(0.5f, 0.5f, 0.5f));
}

template <typename F>
auto Actor::WithData(F &&fn) const {
  std::lock_guard<std::mutex> lock(_episode->mutex);
  auto it = _episode->actors.find(_id);
  if (it == _episode->actors.end()) {
    throw std::runtime_error(
        "trying to operate on a destroyed actor; an actor's function was called, but the actor "
        "is already destroyed.");
  }
  return fn(it->second);
}

bool Actor::IsAlive() const {
  std::lock_guard<std::mutex> lock(_episode->mutex);
  auto it = _episode->actors.find(_id);
  return it != _episode->actors.end();
}

geom::Location Actor::GetLocation() const {
  return WithData([](mock::ActorData &a) { return a.transform.location; });
}

geom::Transform Actor::GetTransform() const {
  return WithData([](mock::ActorData &a) { return a.transform; });
}

geom::Vector3D Actor::GetVelocity() const {
  return WithData([](mock::ActorData &a) { return a.velocity; });
}

geom::Vector3D Actor::GetAngularVelocity() const {
  return WithData([](mock::ActorData &a) { return a.angular_velocity; });
}

geom::Vector3D Actor::GetAcceleration() const {
  return WithData([](mock::ActorData &a) { return a.acceleration; });
}

void Actor::SetLocation(const geom::Location &location) {
  WithData([&](mock::ActorData &a) { a.transform.location = location; return 0; });
}

void Actor::SetTransform(const geom::Transform &transform) {
  WithData([&](mock::ActorData &a) { a.transform = transform; return 0; });
}

void Actor::SetTargetVelocity(const geom::Vector3D &vector) {
  WithData([&](mock::ActorData &a) { a.velocity = vector; return 0; });
}

bool Actor::Destroy() {
  std::lock_guard<std::mutex> lock(_episode->mutex);
  return _episode->EraseActorLocked(_id);
}

void Vehicle::SetAutopilot(bool enabled, uint16_t) {
  WithData([&](mock::ActorData &a) { a.autopilot = enabled; return 0; });
}

void Vehicle::ApplyControl(const Control &control) {
  WithData([&](mock::ActorData &a) { a.control = control; return 0; });
}

void Vehicle::ApplyPhysicsControl(const PhysicsControl &physics_control) {
  WithData([&](mock::ActorData &a) { a.physics = physics_control; return 0; });
}

Vehicle::PhysicsControl Vehicle::GetPhysicsControl() const {
  return WithData([](mock::ActorData &a) { return a.physics; });
}

Vehicle::Control Vehicle::GetControl() const {
  return WithData([](mock::ActorData &a) { return a.control; });
}

// ---------------------------------------------------------------------------

SharedPtr<Actor> ActorList::Find(rpc::ActorId actor_id) const {
  for (const auto &a : _actors)
    if (a->GetId() == actor_id) return a;
  return nullptr;
}

SharedPtr<ActorList> ActorList::Filter(const std::string &wildcard_pattern) const {
  std::vector<SharedPtr<Actor>> result;
  for (const auto &a : _actors)
    if (mock::Match(a->GetTypeId(), wildcard_pattern)) result.push_back(a);
  return std::make_shared<ActorList>(std::move(result));
}

// ---------------------------------------------------------------------------

uint64_t World::GetId() const {
  std::lock_guard<std::mutex> lock(_episode->mutex);
  return _episode->id;
}

SharedPtr<BlueprintLibrary> World::GetBlueprintLibrary() const {
  return std::make_shared<BlueprintLibrary>(mock::DefaultBlueprints());
}

SharedPtr<ActorList> World::GetActors() const {
  std::vector<mock::ActorData> snapshot;
  {
    std::lock_guard<std::mutex> lock(_episode->mutex);
    for (const auto &entry : _episode->actors) snapshot.push_back(entry.second);
  }
  std::vector<SharedPtr<Actor>> actors;
  for (const auto &data : snapshot) actors.push_back(mock::MakeActor(_episode, data));
  return std::make_shared<ActorList>(std::move(actors));
}

SharedPtr<Actor> World::GetActor(rpc::ActorId id) const {
  mock::ActorData data;
  {
    std::lock_guard<std::mutex> lock(_episode->mutex);
    const mock::ActorData *known = _episode->KnownLocked(id);  // includes destroyed actors
    if (known == nullptr) return nullptr;
    data = *known;
  }
  return mock::MakeActor(_episode, data);
}

SharedPtr<Actor> World::SpawnActor(const ActorBlueprint &blueprint,
                                   const geom::Transform &transform, Actor *parent,
                                   rpc::AttachmentType, const std::string &) {
  mock::ActorData data;
  {
    std::lock_guard<std::mutex> lock(_episode->mutex);
    std::optional<rpc::ActorId> parent_id;
    if (parent != nullptr) parent_id = parent->GetId();
    data = _episode->actors.at(
        _episode->SpawnLocked(blueprint.MakeActorDescription(), transform, parent_id));
  }
  return mock::MakeActor(_episode, data);
}

SharedPtr<Actor> World::TrySpawnActor(const ActorBlueprint &blueprint,
                                      const geom::Transform &transform, Actor *parent,
                                      rpc::AttachmentType attachment_type,
                                      const std::string &socket_name) noexcept {
  try {
    return SpawnActor(blueprint, transform, parent, attachment_type, socket_name);
  } catch (const std::exception &) {
    return nullptr;
  }
}

uint64_t World::Tick(time_duration) {
  std::vector<mock::Delivery> deliveries;
  uint64_t frame;
  {
    std::lock_guard<std::mutex> lock(_episode->mutex);
    deliveries = _episode->StepLocked();
    frame = _episode->frame;
  }
  for (auto &d : deliveries) d();
  return frame;
}

SharedPtr<Map> World::GetMap() const { return std::make_shared<Map>(); }

WorldSnapshot World::GetSnapshot() const {
  std::lock_guard<std::mutex> lock(_episode->mutex);
  return _episode->SnapshotLocked();
}

WorldSnapshot World::WaitForTick(time_duration timeout) const {
  std::vector<mock::Delivery> deliveries;
  std::optional<WorldSnapshot> snapshot;
  {
    std::lock_guard<std::mutex> lock(_episode->mutex);
    if (_episode->settings.synchronous_mode) {
      throw TimeoutException("mock (synchronous mode: no tick will come)", timeout);
    }
    deliveries = _episode->StepLocked();
    snapshot = _episode->SnapshotLocked();
  }
  for (auto &d : deliveries) d();
  return *snapshot;
}

rpc::EpisodeSettings World::GetSettings() const {
  std::lock_guard<std::mutex> lock(_episode->mutex);
  return _episode->settings;
}

uint64_t World::ApplySettings(const rpc::EpisodeSettings &settings, time_duration) {
  std::lock_guard<std::mutex> lock(_episode->mutex);
  _episode->settings = settings;
  return _episode->frame;
}

// ---------------------------------------------------------------------------

Client::Client(const std::string &host, uint16_t port, size_t)
    : _endpoint(host + ":" + std::to_string(port)) {}

std::string Client::GetServerVersion() const {
  mock::Connect(_endpoint, _timeout);
  return carla::version();
}

World Client::GetWorld() const { return World(mock::Connect(_endpoint, _timeout)); }

World Client::ReloadWorld(bool reset_settings) const {
  auto episode = mock::Connect(_endpoint, _timeout);
  {
    std::lock_guard<std::mutex> lock(episode->mutex);
    episode->ResetLocked(reset_settings, false);
  }
  return World(episode);
}

World Client::LoadWorld(std::string map_name, bool reset_settings, rpc::MapLayer) const {
  if (map_name.empty()) throw std::invalid_argument("map name must not be empty");
  if (map_name.rfind("Town", 0) != 0 && map_name.rfind("/Game/", 0) != 0) {
    throw std::runtime_error("map '" + map_name + "' not found");
  }
  auto episode = mock::Connect(_endpoint, _timeout);
  {
    std::lock_guard<std::mutex> lock(episode->mutex);
    episode->ResetLocked(reset_settings, false);
  }
  return World(episode);
}

// ---------------------------------------------------------------------------
// Snapshots, map and waypoints

std::optional<ActorSnapshot> WorldSnapshot::Find(rpc::ActorId id) const {
  for (const auto &a : _actors)
    if (a.id == id) return a;
  return std::nullopt;
}

namespace {

constexpr double kRoadLength = 200.0;
constexpr double kLaneWidth = 3.5;
constexpr int32_t kLanes[] = {-1, -2};

double LaneY(int32_t lane_id) { return (-lane_id - 1) * kLaneWidth; }

SharedPtr<Waypoint> MakeWaypoint(int32_t lane, double s) { return std::make_shared<Waypoint>(lane, s); }

}  // namespace

uint64_t Waypoint::GetId() const {
  return (static_cast<uint64_t>(static_cast<uint32_t>(-_lane_id)) << 32) |
         static_cast<uint64_t>(std::llround(_s * 100.0));
}

Waypoint::Waypoint(int32_t lane_id, double s)
    : _lane_id(lane_id),
      _s(s),
      _transform(geom::Location(static_cast<float>(s), static_cast<float>(LaneY(lane_id)), 0.0f)) {}

std::vector<SharedPtr<Waypoint>> Waypoint::GetNext(double distance) const {
  if (_s + distance > kRoadLength) return {};
  return {MakeWaypoint(_lane_id, _s + distance)};
}

std::vector<SharedPtr<Waypoint>> Waypoint::GetPrevious(double distance) const {
  if (_s - distance < 0.0) return {};
  return {MakeWaypoint(_lane_id, _s - distance)};
}

std::vector<SharedPtr<Waypoint>> Waypoint::GetNextUntilLaneEnd(double distance) const {
  std::vector<SharedPtr<Waypoint>> result;
  for (double s = _s + distance; s <= kRoadLength; s += distance) result.push_back(MakeWaypoint(_lane_id, s));
  return result;
}

std::vector<SharedPtr<Waypoint>> Waypoint::GetPreviousUntilLaneStart(double distance) const {
  std::vector<SharedPtr<Waypoint>> result;
  for (double s = _s - distance; s >= 0.0; s -= distance) result.push_back(MakeWaypoint(_lane_id, s));
  return result;
}

SharedPtr<Waypoint> Waypoint::GetRight() const {
  return _lane_id == -1 ? MakeWaypoint(-2, _s) : nullptr;
}

SharedPtr<Waypoint> Waypoint::GetLeft() const {
  return _lane_id == -2 ? MakeWaypoint(-1, _s) : nullptr;
}

Map::Map() : _name("Carla/Maps/MockTown") {
  _xodr = "<?xml version=\"1.0\"?><OpenDRIVE><header name=\"MockTown\"/>"
          "<road id=\"1\" length=\"200\"/></OpenDRIVE>";
  for (double x : {10.0, 40.0, 70.0}) {
    for (int32_t lane : kLanes) {
      _spawn_points.emplace_back(
          geom::Location(static_cast<float>(x), static_cast<float>(LaneY(lane)), 0.6f));
    }
  }
}

SharedPtr<Waypoint> Map::GetWaypoint(const geom::Location &location, bool project_to_road,
                                     int32_t lane_type) const {
  if ((lane_type & static_cast<int32_t>(road::Lane::LaneType::Driving)) == 0) return nullptr;
  if (location.x < 0.0f || location.x > kRoadLength) return nullptr;
  int32_t best = kLanes[0];
  for (int32_t lane : kLanes) {
    if (std::abs(location.y - LaneY(lane)) < std::abs(location.y - LaneY(best))) best = lane;
  }
  if (!project_to_road && std::abs(location.y - LaneY(best)) > kLaneWidth / 2.0) return nullptr;
  return MakeWaypoint(best, location.x);
}

std::vector<SharedPtr<Waypoint>> Map::GenerateWaypoints(double distance) const {
  std::vector<SharedPtr<Waypoint>> result;
  for (int32_t lane : kLanes) {
    for (double s = 0.0; s <= kRoadLength; s += distance) result.push_back(MakeWaypoint(lane, s));
  }
  return result;
}

// ---------------------------------------------------------------------------
// Batch commands

namespace {

using Command = rpc::Command;

geom::Vector3D Scaled(const geom::Vector3D &v, float k) {
  return geom::Vector3D(v.x * k, v.y * k, v.z * k);
}

void ApplyImpulseLocked(mock::Episode &e, mock::ActorData &a, const geom::Vector3D &impulse) {
  const float inv = static_cast<float>(1.0 / e.Mass(a));
  a.velocity = geom::Vector3D(a.velocity.x + impulse.x * inv, a.velocity.y + impulse.y * inv,
                              a.velocity.z + impulse.z * inv);
}

// Executes one command; `future` replaces actor id 0 in do_after commands.
rpc::ActorId Execute(mock::Episode &e, const Command &cmd, rpc::ActorId future) {
  auto target = [&](rpc::ActorId id) -> mock::ActorData & {
    // Like the server: a then-command always acts on the spawned actor.
    return e.LiveLocked(future != 0 ? future : id);
  };
  return std::visit(
      [&](const auto &c) -> rpc::ActorId {
        using T = std::decay_t<decltype(c)>;
        if constexpr (std::is_same_v<T, Command::SpawnActor>) {
          const rpc::ActorId id = e.SpawnLocked(c.description, c.transform, c.parent);
          // Like the server: a failing then-command does not fail the spawn.
          for (const auto &after : c.do_after) {
            try {
              Execute(e, after, id);
            } catch (const std::exception &) {
            }
          }
          return id;
        } else if constexpr (std::is_same_v<T, Command::DestroyActor>) {
          const rpc::ActorId id = target(c.actor).id;
          e.EraseActorLocked(id);
          return id;
        } else if constexpr (std::is_same_v<T, Command::ApplyVehicleControl>) {
          auto &a = target(c.actor);
          if (!a.is_vehicle) throw std::runtime_error("actor " + std::to_string(a.id) + " is not a vehicle");
          a.control = c.control;
          return a.id;
        } else if constexpr (std::is_same_v<T, Command::ApplyTransform>) {
          auto &a = target(c.actor);
          a.transform = c.transform;
          return a.id;
        } else if constexpr (std::is_same_v<T, Command::ApplyTargetVelocity>) {
          auto &a = target(c.actor);
          a.velocity = c.velocity;
          return a.id;
        } else if constexpr (std::is_same_v<T, Command::SetSimulatePhysics>) {
          auto &a = target(c.actor);
          a.simulate_physics = c.enabled;
          return a.id;
        } else if constexpr (std::is_same_v<T, Command::SetAutopilot>) {
          auto &a = target(c.actor);
          if (!a.is_vehicle) throw std::runtime_error("actor " + std::to_string(a.id) + " is not a vehicle");
          a.autopilot = c.enabled;
          return a.id;
        } else if constexpr (std::is_same_v<T, Command::ApplyWalkerControl>) {
          auto &a = target(c.actor);
          if (!a.is_walker()) {
            throw std::runtime_error("actor " + std::to_string(a.id) + " is not a walker");
          }
          a.walker_control = c.control;
          return a.id;
        } else if constexpr (std::is_same_v<T, Command::ApplyTargetAngularVelocity>) {
          auto &a = target(c.actor);
          a.angular_velocity = c.angular_velocity;
          return a.id;
        } else if constexpr (std::is_same_v<T, Command::ApplyImpulse>) {
          auto &a = target(c.actor);
          ApplyImpulseLocked(e, a, c.impulse);
          return a.id;
        } else if constexpr (std::is_same_v<T, Command::ApplyForce>) {
          auto &a = target(c.actor);
          ApplyImpulseLocked(e, a, Scaled(c.force, static_cast<float>(e.DeltaSeconds())));
          return a.id;
        } else if constexpr (std::is_same_v<T, Command::ApplyAngularImpulse> ||
                             std::is_same_v<T, Command::ApplyTorque>) {
          return target(c.actor).id;  // no rotational dynamics in the mock
        } else if constexpr (std::is_same_v<T, Command::SetEnableGravity>) {
          auto &a = target(c.actor);
          a.gravity = c.enabled;
          return a.id;
        } else if constexpr (std::is_same_v<T, Command::SetVehicleLightState>) {
          auto &a = target(c.actor);
          if (!a.is_vehicle) throw std::runtime_error("actor " + std::to_string(a.id) + " is not a vehicle");
          a.light_state = c.light_state;
          return a.id;
        } else if constexpr (std::is_same_v<T, Command::ApplyLocation>) {
          auto &a = target(c.actor);
          a.transform.location = c.location;
          return a.id;
        } else {
          static_assert(std::is_same_v<T, Command::SetTrafficLightState>);
          auto &a = target(c.actor);
          if (!a.is_traffic_light()) {
            throw std::runtime_error("actor " + std::to_string(a.id) + " is not a traffic light");
          }
          a.set_light(c.traffic_light_state);
          return a.id;
        }
      },
      cmd.command);
}

}  // namespace

std::vector<rpc::CommandResponse> Client::ApplyBatchSync(std::vector<rpc::Command> commands,
                                                         bool do_tick_cue) const {
  auto episode = mock::Connect(_endpoint, _timeout);
  std::vector<rpc::CommandResponse> responses;
  std::vector<mock::Delivery> deliveries;
  {
    std::lock_guard<std::mutex> lock(episode->mutex);
    for (const auto &cmd : commands) {
      try {
        responses.emplace_back(Execute(*episode, cmd, 0));
      } catch (const std::exception &e) {
        responses.emplace_back(rpc::ResponseError(e.what()));
      }
    }
    if (do_tick_cue) deliveries = episode->StepLocked();
  }
  for (auto &d : deliveries) d();
  return responses;
}

void Client::ApplyBatch(std::vector<rpc::Command> commands, bool do_tick_cue) const {
  ApplyBatchSync(std::move(commands), do_tick_cue);
}

// ---------------------------------------------------------------------------
// Sensors

rpc::ActorDescription ActorBlueprint::MakeActorDescription() const {
  rpc::ActorDescription d;
  d.id = _id;
  for (const auto &entry : _attributes) d.attributes[entry.first] = entry.second.GetValue();
  return d;
}

void Sensor::Listen(CallbackFunctionType callback) {
  WithData([&](mock::ActorData &) {
    _episode->listeners[_id] = mock::Episode::Listener{this, std::move(callback)};
    return 0;
  });
}

void Sensor::Stop() {
  std::lock_guard<std::mutex> lock(_episode->mutex);
  auto it = _episode->listeners.find(_id);
  if (it != _episode->listeners.end() && it->second.owner == this) _episode->listeners.erase(it);
}

bool Sensor::IsListening() const {
  std::lock_guard<std::mutex> lock(_episode->mutex);
  auto it = _episode->listeners.find(_id);
  return it != _episode->listeners.end() && it->second.owner == this;
}

Sensor::~Sensor() { Stop(); }

namespace mock {

namespace {

long AttributeInt(const ActorData &a, const std::string &key, long fallback) {
  auto it = a.attributes.find(key);
  return it == a.attributes.end() ? fallback : std::strtol(it->second.c_str(), nullptr, 10);
}

double AttributeDouble(const ActorData &a, const std::string &key, double fallback) {
  auto it = a.attributes.find(key);
  return it == a.attributes.end() ? fallback : std::strtod(it->second.c_str(), nullptr);
}

}  // namespace

std::vector<Delivery> Episode::SenseLocked() {
  std::vector<Delivery> out;
  const double timestamp = static_cast<double>(frame) * DeltaSeconds();
  auto self = shared_from_this();
  for (const auto &entry : listeners) {
    auto it = actors.find(entry.first);
    if (it == actors.end()) continue;
    const ActorData &a = it->second;
    auto callback = entry.second.callback;
    SharedPtr<sensor::SensorData> data;
    namespace sd = sensor::data;
    if (a.type_id == "sensor.camera.rgb") {
      const auto w = static_cast<size_t>(AttributeInt(a, "image_size_x", 800));
      const auto h = static_cast<size_t>(AttributeInt(a, "image_size_y", 600));
      const auto fov = static_cast<float>(AttributeDouble(a, "fov", 90.0));
      // The pixels are filled in the delivery, outside the episode lock.
      out.push_back([cb = std::move(callback), f = frame, timestamp, t = a.transform, w, h, fov]() {
        auto image = std::make_shared<sd::Image>(f, timestamp, t, w, h, fov);
        // A gradient that changes with the frame: B = x, G = y, R = frame.
        for (size_t y = 0; y < h; ++y) {
          for (size_t x = 0; x < w; ++x) {
            image->data()[y * w + x] = sd::Color(static_cast<uint8_t>(f), static_cast<uint8_t>(y),
                                                 static_cast<uint8_t>(x), 255u);
          }
        }
        cb(std::move(image));
      });
      continue;
    } else if (a.type_id == "sensor.lidar.ray_cast") {
      const auto channels = static_cast<uint32_t>(AttributeInt(a, "channels", 32));
      const float range = static_cast<float>(AttributeDouble(a, "range", 10.0));
      constexpr uint32_t kPerChannel = 100;
      std::vector<uint32_t> per_channel(channels, kPerChannel);
      std::vector<sd::LidarDetection> points;
      points.reserve(size_t{channels} * kPerChannel);
      for (uint32_t c = 0; c < channels; ++c) {
        const double elevation = (static_cast<double>(c) / std::max(1u, channels - 1) - 0.5) * 0.5;
        for (uint32_t i = 0; i < kPerChannel; ++i) {
          const double azimuth = 2.0 * M_PI * i / kPerChannel;
          sd::LidarDetection d;
          d.point = geom::Location(static_cast<float>(range * std::cos(azimuth) * std::cos(elevation)),
                                   static_cast<float>(range * std::sin(azimuth) * std::cos(elevation)),
                                   static_cast<float>(range * std::sin(elevation)));
          d.intensity = 1.0f - static_cast<float>(c) / static_cast<float>(channels);
          points.push_back(d);
        }
      }
      data = std::make_shared<sd::LidarMeasurement>(frame, timestamp, a.transform, 0.0f,
                                                    std::move(per_channel), std::move(points));
    } else if (a.type_id == "sensor.other.gnss") {
      // Flat-earth reference at (0, 0): ~111 km per degree, CARLA's y points south.
      data = std::make_shared<sd::GnssMeasurement>(frame, timestamp, a.transform,
                                                   -a.transform.location.y / 111111.0,
                                                   a.transform.location.x / 111111.0,
                                                   a.transform.location.z);
    } else if (a.type_id == "sensor.other.imu") {
      geom::Vector3D accel = a.acceleration;
      accel.z += 9.81f;
      const geom::Vector3D gyro(a.angular_velocity.x * static_cast<float>(M_PI / 180.0),
                                a.angular_velocity.y * static_cast<float>(M_PI / 180.0),
                                a.angular_velocity.z * static_cast<float>(M_PI / 180.0));
      data = std::make_shared<sd::IMUMeasurement>(
          frame, timestamp, a.transform, accel, gyro,
          static_cast<float>(std::fmod(a.transform.rotation.yaw + 90.0, 360.0) * M_PI / 180.0));
    } else if (a.type_id == "sensor.other.collision" && a.parent) {
      // A collision while the parent vehicle is within 2 m of another vehicle.
      auto parent = actors.find(*a.parent);
      if (parent == actors.end()) continue;
      for (const auto &other_entry : actors) {
        const ActorData &other = other_entry.second;
        if (other.id == parent->second.id || !other.is_vehicle) continue;
        const auto &p = parent->second.transform.location;
        const auto &o = other.transform.location;
        if (std::hypot(p.x - o.x, p.y - o.y) >= 2.0) continue;
        const rpc::ActorId self_id = parent->second.id;
        const rpc::ActorId other_id = other.id;
        const geom::Transform t = a.transform;
        const size_t f = frame;
        const geom::Vector3D impulse(1000.0f * (p.x - o.x), 1000.0f * (p.y - o.y), 0.0f);
        out.push_back([self, callback, self_id, other_id, t, f, timestamp, impulse]() {
          const World world(self);  // GetActor: also finds actors destroyed since
          callback(std::make_shared<sensor::data::CollisionEvent>(
              f, timestamp, t, world.GetActor(self_id), world.GetActor(other_id), impulse));
        });
      }
      continue;
    } else {
      continue;
    }
    out.push_back([cb = std::move(callback), data = std::move(data)]() { cb(data); });
  }
  return out;
}

}  // namespace mock

// ---------------------------------------------------------------------------
// Milestone 4: more actor operations, walkers, traffic lights, weather, debug,
// recorder, OpenDRIVE worlds, map queries and the Traffic Manager.

void Actor::SetTargetAngularVelocity(const geom::Vector3D &vector) {
  WithData([&](mock::ActorData &a) { a.angular_velocity = vector; return 0; });
}

void Actor::AddImpulse(const geom::Vector3D &vector) {
  WithData([&](mock::ActorData &a) { ApplyImpulseLocked(*_episode, a, vector); return 0; });
}

void Actor::AddForce(const geom::Vector3D &force) {
  WithData([&](mock::ActorData &a) {
    ApplyImpulseLocked(*_episode, a, Scaled(force, static_cast<float>(_episode->DeltaSeconds())));
    return 0;
  });
}

void Actor::AddAngularImpulse(const geom::Vector3D &) {
  WithData([](mock::ActorData &) { return 0; });  // no rotational dynamics in the mock
}

void Actor::AddTorque(const geom::Vector3D &) {
  WithData([](mock::ActorData &) { return 0; });
}

void Actor::SetSimulatePhysics(bool enabled) {
  WithData([&](mock::ActorData &a) { a.simulate_physics = enabled; return 0; });
}

void Actor::SetEnableGravity(bool enabled) {
  WithData([&](mock::ActorData &a) { a.gravity = enabled; return 0; });
}

void Vehicle::SetLightState(const LightState &light_state) {
  WithData([&](mock::ActorData &a) { a.light_state = static_cast<uint32_t>(light_state); return 0; });
}

Vehicle::LightState Vehicle::GetLightState() const {
  return WithData([](mock::ActorData &a) { return static_cast<LightState>(a.light_state); });
}

namespace {

constexpr double kTrafficLightReach = 15.0;

// The traffic light within reach of a vehicle, if any (episode lock held).
const mock::ActorData *LightNear(const mock::Episode &e, const mock::ActorData &vehicle) {
  for (const auto &entry : e.actors) {
    const auto &l = entry.second;
    if (!l.is_traffic_light()) continue;
    const auto &p = vehicle.transform.location;
    if (std::hypot(p.x - l.transform.location.x, p.y - l.transform.location.y) < kTrafficLightReach) {
      return &l;
    }
  }
  return nullptr;
}

}  // namespace

rpc::TrafficLightState Vehicle::GetTrafficLightState() const {
  return WithData([&](mock::ActorData &a) {
    const auto *l = LightNear(*_episode, a);
    return l == nullptr ? rpc::TrafficLightState::Green : l->light;
  });
}

bool Vehicle::IsAtTrafficLight() {
  return WithData([&](mock::ActorData &a) { return LightNear(*_episode, a) != nullptr; });
}

SharedPtr<TrafficLight> Vehicle::GetTrafficLight() const {
  const rpc::ActorId id = WithData([&](mock::ActorData &a) -> rpc::ActorId {
    const auto *l = LightNear(*_episode, a);
    return l == nullptr ? 0u : l->id;
  });
  return id == 0 ? nullptr : std::make_shared<TrafficLight>(_episode, id);
}

void Walker::ApplyControl(const Control &control) {
  WithData([&](mock::ActorData &a) { a.walker_control = control; return 0; });
}

Walker::Control Walker::GetWalkerControl() const {
  return WithData([](mock::ActorData &a) { return a.walker_control; });
}

void WalkerAIController::Start() {
  WithData([](mock::ActorData &a) {
    if (!a.parent) throw std::runtime_error("walker AI controller must be attached to a walker");
    a.ai_running = true;
    return 0;
  });
}

void WalkerAIController::Stop() {
  WithData([&](mock::ActorData &a) {
    a.ai_running = false;
    if (a.parent) {
      auto w = _episode->actors.find(*a.parent);
      if (w != _episode->actors.end()) w->second.walker_control.speed = 0.0f;
    }
    return 0;
  });
}

void WalkerAIController::GoToLocation(const geom::Location &destination) {
  WithData([&](mock::ActorData &a) { a.ai_target = destination; return 0; });
}

void WalkerAIController::SetMaxSpeed(float max_speed) {
  WithData([&](mock::ActorData &a) { a.ai_max_speed = max_speed; return 0; });
}

void TrafficLight::SetState(rpc::TrafficLightState state) {
  WithData([&](mock::ActorData &a) { a.set_light(state); return 0; });
}

rpc::TrafficLightState TrafficLight::GetState() const {
  return WithData([](mock::ActorData &a) { return a.light; });
}

void TrafficLight::SetGreenTime(float t) { WithData([&](mock::ActorData &a) { a.green_time = t; return 0; }); }
float TrafficLight::GetGreenTime() const { return WithData([](mock::ActorData &a) { return a.green_time; }); }
void TrafficLight::SetYellowTime(float t) { WithData([&](mock::ActorData &a) { a.yellow_time = t; return 0; }); }
float TrafficLight::GetYellowTime() const { return WithData([](mock::ActorData &a) { return a.yellow_time; }); }
void TrafficLight::SetRedTime(float t) { WithData([&](mock::ActorData &a) { a.red_time = t; return 0; }); }
float TrafficLight::GetRedTime() const { return WithData([](mock::ActorData &a) { return a.red_time; }); }
float TrafficLight::GetElapsedTime() const { return WithData([](mock::ActorData &a) { return a.elapsed; }); }
void TrafficLight::Freeze(bool freeze) { WithData([&](mock::ActorData &a) { a.frozen = freeze; return 0; }); }
bool TrafficLight::IsFrozen() const { return WithData([](mock::ActorData &a) { return a.frozen; }); }
void TrafficLight::ResetGroup() {
  WithData([](mock::ActorData &a) { a.set_light(rpc::TrafficLightState::Green); return 0; });
}

std::optional<geom::Location> World::GetRandomLocationFromNavigation() const {
  // The mock's "sidewalk": along the road edge at y = -2.5.
  std::lock_guard<std::mutex> lock(_episode->mutex);
  const float x = static_cast<float>((_episode->frame * 37 + 11) % 200);
  return geom::Location(x, -2.5f, 0.5f);
}

rpc::WeatherParameters World::GetWeather() const {
  std::lock_guard<std::mutex> lock(_episode->mutex);
  return _episode->weather;
}

void World::SetWeather(const rpc::WeatherParameters &weather) {
  std::lock_guard<std::mutex> lock(_episode->mutex);
  _episode->weather = weather;
}

namespace {
void CountShape(const std::shared_ptr<mock::Episode> &e) {
  std::lock_guard<std::mutex> lock(e->mutex);
  ++e->debug_shapes;
}
}  // namespace

void DebugHelper::DrawPoint(const geom::Location &, float, sensor::data::Color, float, bool) { CountShape(_episode); }
void DebugHelper::DrawLine(const geom::Location &, const geom::Location &, float, sensor::data::Color, float, bool) { CountShape(_episode); }
void DebugHelper::DrawArrow(const geom::Location &, const geom::Location &, float, float, sensor::data::Color, float, bool) { CountShape(_episode); }
void DebugHelper::DrawBox(const geom::BoundingBox &, const geom::Rotation &, float, sensor::data::Color, float, bool) { CountShape(_episode); }
void DebugHelper::DrawString(const geom::Location &, const std::string &, bool, sensor::data::Color, float, bool) { CountShape(_episode); }

std::vector<std::pair<SharedPtr<Waypoint>, SharedPtr<Waypoint>>> Map::GetTopology() const {
  std::vector<std::pair<SharedPtr<Waypoint>, SharedPtr<Waypoint>>> topology;
  for (int32_t lane : kLanes) topology.emplace_back(MakeWaypoint(lane, 0.0), MakeWaypoint(lane, kRoadLength));
  return topology;
}

std::vector<geom::Location> Map::GetAllCrosswalkZones() const {
  // One crosswalk across both lanes at x = 50 (a closed polygon, like CARLA's).
  return {geom::Location(49.0f, -1.75f, 0.0f), geom::Location(51.0f, -1.75f, 0.0f),
          geom::Location(51.0f, 5.25f, 0.0f), geom::Location(49.0f, 5.25f, 0.0f),
          geom::Location(49.0f, -1.75f, 0.0f)};
}

std::vector<SharedPtr<Landmark>> Map::GetAllLandmarks() const {
  return {std::make_shared<Landmark>("1000", "Stop", "206", 100.0,
                                     geom::Transform(geom::Location(100.0f, -3.0f, 1.0f)))};
}

std::vector<SharedPtr<Landmark>> Map::GetAllLandmarksOfType(std::string type) const {
  std::vector<SharedPtr<Landmark>> result;
  for (auto &l : GetAllLandmarks())
    if (l->GetType() == type) result.push_back(l);
  return result;
}

traffic_manager::TrafficManager Client::GetInstanceTM(uint16_t port) const {
  return traffic_manager::TrafficManager(mock::Connect(_endpoint, _timeout), port);
}

std::string Client::StartRecorder(std::string name, bool) {
  auto e = mock::Connect(_endpoint, _timeout);
  std::lock_guard<std::mutex> lock(e->mutex);
  e->recording = name;
  e->recordings[name] = e->frame;
  return "Recording on file: " + name;
}

void Client::StopRecorder() {
  auto e = mock::Connect(_endpoint, _timeout);
  std::lock_guard<std::mutex> lock(e->mutex);
  if (!e->recording.empty()) e->recordings[e->recording] = e->frame - e->recordings[e->recording];
  e->recording.clear();
}

namespace {
uint64_t RecordedFrames(const std::shared_ptr<mock::Episode> &e, const std::string &name) {
  std::lock_guard<std::mutex> lock(e->mutex);
  auto it = e->recordings.find(name);
  if (it == e->recordings.end()) throw std::runtime_error("file " + name + " not found");
  return it->second;
}
}  // namespace

std::string Client::ShowRecorderFileInfo(std::string name, bool) {
  return "File: " + name + "\nFrames: " + std::to_string(RecordedFrames(mock::Connect(_endpoint, _timeout), name)) + "\n";
}

std::string Client::ShowRecorderCollisions(std::string name, char, char) {
  RecordedFrames(mock::Connect(_endpoint, _timeout), name);
  return "Collisions in " + name + ": 0\n";
}

std::string Client::ShowRecorderActorsBlocked(std::string name, double, double) {
  RecordedFrames(mock::Connect(_endpoint, _timeout), name);
  return "Blocked actors in " + name + ": 0\n";
}

std::string Client::ReplayFile(std::string name, double, double, uint32_t, bool) {
  return "Replaying " + std::to_string(RecordedFrames(mock::Connect(_endpoint, _timeout), name)) +
         " frames of " + name;
}

void Client::StopReplayer(bool) {}

void Client::SetReplayerTimeFactor(double) {}

World Client::GenerateOpenDriveWorld(std::string opendrive, const rpc::OpendriveGenerationParameters &,
                                     bool reset_settings) const {
  if (opendrive.find("<OpenDRIVE") == std::string::npos) {
    throw std::runtime_error("not an OpenDRIVE document");
  }
  auto episode = mock::Connect(_endpoint, _timeout);
  {
    std::lock_guard<std::mutex> lock(episode->mutex);
    episode->ResetLocked(reset_settings, false);
  }
  return World(episode);
}

}  // namespace client

namespace traffic_manager {

void TrafficManager::SetSynchronousMode(bool) {}
void TrafficManager::SetRandomDeviceSeed(uint64_t) {}
void TrafficManager::SetHybridPhysicsMode(bool) {}
void TrafficManager::SetGlobalPercentageSpeedDifference(float percentage) {
  std::lock_guard<std::mutex> lock(_episode->mutex);
  _episode->tm_global_speed_difference = percentage;
}
void TrafficManager::SetGlobalDistanceToLeadingVehicle(float) {}
void TrafficManager::SetPercentageSpeedDifference(const ActorPtr &, float) {}
void TrafficManager::SetDistanceToLeadingVehicle(const ActorPtr &, float) {}
void TrafficManager::SetRandomLeftLaneChangePercentage(const ActorPtr &, float) {}
void TrafficManager::SetRandomRightLaneChangePercentage(const ActorPtr &, float) {}
void TrafficManager::SetPercentageRunningLight(const ActorPtr &, float) {}
void TrafficManager::SetPercentageRunningSign(const ActorPtr &, float) {}
void TrafficManager::SetPercentageIgnoreVehicles(const ActorPtr &, float) {}
void TrafficManager::SetPercentageIgnoreWalkers(const ActorPtr &, float) {}
void TrafficManager::SetKeepRightPercentage(const ActorPtr &, float) {}
void TrafficManager::SetDesiredSpeed(const ActorPtr &, float) {}
void TrafficManager::SetLaneOffset(const ActorPtr &, float) {}
void TrafficManager::SetAutoLaneChange(const ActorPtr &, bool) {}
void TrafficManager::SetForceLaneChange(const ActorPtr &, bool) {}
void TrafficManager::SetUpdateVehicleLights(const ActorPtr &, bool) {}

}  // namespace traffic_manager
}  // namespace carla
