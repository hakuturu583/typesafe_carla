// Implementation of the in-memory mock LibCarla (see carla/mock/Mock.h).
#include "carla/mock/Mock.h"
#include "carla/mock/SensorDataExt.h"
#include "carla/mock/V2X.h"
#include "carla/FileSystem.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>

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
  // Issue #20. Ackermann control replaces the VehicleControl until the next
  // ApplyControl; the settings default to the CARLA server's PID gains.
  std::optional<rpc::VehicleAckermannControl> ackermann;
  rpc::AckermannControllerSettings ackermann_settings{0.15f, 0.0f, 0.25f, 0.01f, 0.0f, 0.01f};
  uint32_t open_doors = 0u;  // bit per rpc::VehicleDoor below All
  bool debug_telemetry = false;
  std::map<int, float> wheel_steer_direction;  // visual only
  rpc::VehicleFailureState failure_state = rpc::VehicleFailureState::None;
  std::string carsim_file;
  bool carsim_road = false;
  bool chrono = false;
  float pose_blend = 0.0f;                         // walkers
  std::map<std::string, geom::Transform> custom_pose;  // walkers: SetBonesTransform
  bool ros_enabled = false;  // issue #33: ServerSideSensor::EnableForROS
  // Issue #19.
  std::optional<geom::Vector3D> constant_velocity;  // world frame in the mock

  bool is_walker() const { return type_id.rfind("walker.", 0) == 0; }
  bool is_walker_ai_controller() const { return type_id == "controller.ai.walker"; }
  bool is_traffic_light() const { return type_id == "traffic.traffic_light"; }
  // LibCarla's ActorFactory: every other "traffic." actor is a TrafficSign.
  bool is_traffic_sign() const { return !is_traffic_light() && type_id.rfind("traffic.", 0) == 0; }
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
  for (size_t i = 0; i < pc.wheels.size(); ++i) {
    auto &w = pc.wheels[i];
    w.wheel_index = static_cast<int32_t>(i);
    w.axle_type = i < 2 ? 1 : 2;  // front, rear
    w.offset = geom::Vector3D(i < 2 ? 140.0f : -140.0f, i % 2 == 0 ? -80.0f : 80.0f, 0.0f);
    w.lateral_slip_graph = {geom::Vector2D(0.0f, 0.0f), geom::Vector2D(5.0f, 1.0f)};
  }
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
    // Lane invasion: the parent's position at the previous accepted tick
    // (LibCarla's LaneInvasionCallback::_bounds); reset by Listen().
    std::optional<geom::Location> lane_previous;
  };
  std::map<rpc::ActorId, Listener> listeners;
  // Issue #33: G-buffer subscriptions, by (sensor, texture id).
  std::map<std::pair<rpc::ActorId, uint32_t>, Listener> gbuffer_listeners;
  rpc::WeatherParameters weather = rpc::WeatherParameters::ClearNoon;
  size_t debug_shapes = 0;      // DebugHelper calls (nothing is drawn)
  std::string recording;        // the active recorder file, if any
  std::map<std::string, uint64_t> recordings;  // file -> frames recorded
  float tm_global_speed_difference = 0.0f;     // percent slower than the limit
  std::map<rpc::ActorId, std::vector<uint8_t>> tm_routes;  // SetImportedRoute
  std::map<uint16_t, bool> tm_shut_down;       // by port
  // Issue #42: V2X. Custom messages sent since the last tick, delivered at the
  // next one (ACustomV2XSensor's "next frame" map), as (sender, message)
  // pairs; and, for fixed_rate CAM sensors, the time of their last CAM.
  std::vector<std::pair<rpc::ActorId, sensor::data::CustomV2XData>> v2x_outbox;
  std::map<rpc::ActorId, double> last_cam;

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
  // V2X (issue #42): CAMs and custom messages, for every listening V2X sensor.
  void SenseV2XLocked(double timestamp, std::vector<Delivery> &out);

  // Every actor removal goes through here so listeners never outlive their sensor.
  bool EraseActorLocked(rpc::ActorId id) {
    listeners.erase(id);
    last_cam.erase(id);
    // G-buffer subscriptions stay: on a real client they outlive the actor
    // unless StopGBuffer ran before the destroy (tsc_mock_gbuffer_subscriptions).
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
    // A stop sign further along (a TrafficSign: map signs are actors in CARLA).
    AddActorLocked("traffic.stop", false, geom::Transform(geom::Location(150.0f, -3.0f, 0.0f)));
  }

  // Ackermann control (issue #20): reach the target speed at `acceleration`
  // (6 m/s^2 when 0), turn with a kinematic bicycle model (2.8 m wheelbase).
  void StepAckermannLocked(ActorData &a, double speed_before, double dt) {
    const rpc::VehicleAckermannControl &k = *a.ackermann;
    const double rate = k.acceleration > 0.0f ? k.acceleration : 6.0;
    const double diff = k.speed - speed_before;
    const double speed = std::abs(diff) <= rate * dt ? k.speed
                                                     : speed_before + (diff > 0 ? rate : -rate) * dt;
    const double yaw = a.transform.rotation.yaw * M_PI / 180.0;
    const double new_yaw = yaw + speed * std::tan(k.steer) / 2.8 * dt;
    a.transform.rotation.yaw = static_cast<float>(new_yaw * 180.0 / M_PI);
    const geom::Vector3D v(static_cast<float>(speed * std::cos(new_yaw)),
                           static_cast<float>(speed * std::sin(new_yaw)), 0.0f);
    a.acceleration = geom::Vector3D(static_cast<float>((v.x - a.velocity.x) / dt),
                                    static_cast<float>((v.y - a.velocity.y) / dt), 0.0f);
    a.angular_velocity =
        geom::Vector3D(0.0f, 0.0f, static_cast<float>((new_yaw - yaw) * 180.0 / M_PI / dt));
    a.velocity = v;
    a.transform.location.x += static_cast<float>(v.x * dt);
    a.transform.location.y += static_cast<float>(v.y * dt);
  }

  // Advances one frame; returns the sensor deliveries to run after unlocking.
  std::vector<Delivery> StepLocked() {
    const double dt = DeltaSeconds();
    for (auto &entry : actors) {
      ActorData &a = entry.second;
      if (a.constant_velocity) {  // overrides physics; attached actors follow their parent
        if (a.parent) continue;
        a.velocity = *a.constant_velocity;
        a.acceleration = geom::Vector3D();
        a.transform.location.x += static_cast<float>(a.velocity.x * dt);
        a.transform.location.y += static_cast<float>(a.velocity.y * dt);
        a.transform.location.z += static_cast<float>(a.velocity.z * dt);
        continue;
      }
      if (!a.is_vehicle || !a.simulate_physics) continue;
      rpc::VehicleControl c = a.control;
      if (a.autopilot) {
        c = rpc::VehicleControl(0.5f * (1.0f - tm_global_speed_difference / 100.0f), 0.0f, 0.0f,
                                false, false, false, 0);
      }
      const double yaw = a.transform.rotation.yaw * M_PI / 180.0;
      const double speed_before = std::hypot(a.velocity.x, a.velocity.y);
      if (a.ackermann && !a.autopilot) {
        StepAckermannLocked(a, speed_before, dt);
        continue;
      }
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
    auto deliveries = SenseLocked();
    deliveries.push_back(TickDelivery(this, SnapshotLocked()));  // World::OnTick (issue #21)
    return deliveries;
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
      // Recommended values as CARLA's vehicle blueprints have them (issue #33).
      {ActorAttribute("color", rpc::ActorAttributeType::RGBColor, color, true,
                      {color, "255,255,255"}),
       ActorAttribute("role_name", rpc::ActorAttributeType::String, "autopilot", true,
                      {"autopilot", "scenario", "ego_vehicle"}),
       ActorAttribute("number_of_wheels", rpc::ActorAttributeType::Int, "4", false, {"4"}),
       ActorAttribute("sticky_control", rpc::ActorAttributeType::Bool, "true", true),
       ActorAttribute("base_mass", rpc::ActorAttributeType::Float, "1500.0", false)});
}

ActorBlueprint CameraBlueprint(const std::string &id, std::vector<std::string> tags,
                               std::vector<ActorAttribute> extra = {}) {
  std::vector<ActorAttribute> attributes{
      ActorAttribute("image_size_x", rpc::ActorAttributeType::Int, "800", true),
      ActorAttribute("image_size_y", rpc::ActorAttributeType::Int, "600", true),
      ActorAttribute("fov", rpc::ActorAttributeType::Float, "90.0", true)};
  attributes.insert(attributes.end(), extra.begin(), extra.end());
  return ActorBlueprint(id, std::move(tags), std::move(attributes));
}

// ue5-dev's V2X sensors (issue #42): the radio attributes the mock reads.
ActorBlueprint V2XBlueprint(const std::string &id, std::vector<std::string> tags,
                            std::vector<ActorAttribute> extra = {}) {
  std::vector<ActorAttribute> attributes{
      ActorAttribute("channel_id", rpc::ActorAttributeType::String, "Default", true),
      ActorAttribute("transmit_power", rpc::ActorAttributeType::Float, "21.5", true),
      ActorAttribute("filter_distance", rpc::ActorAttributeType::Float, "500.0", true)};
  attributes.insert(attributes.end(), extra.begin(), extra.end());
  return ActorBlueprint(id, std::move(tags), std::move(attributes));
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
      CameraBlueprint("sensor.camera.rgb", {"sensor", "camera", "rgb"},
                      {ActorAttribute("role_name", rpc::ActorAttributeType::String, "front", true)}),
      ActorBlueprint("sensor.lidar.ray_cast", {"sensor", "lidar", "ray_cast"},
                     {ActorAttribute("channels", rpc::ActorAttributeType::Int, "32", true),
                      ActorAttribute("range", rpc::ActorAttributeType::Float, "10.0", true),
                      ActorAttribute("points_per_second", rpc::ActorAttributeType::Int, "56000", true),
                      ActorAttribute("rotation_frequency", rpc::ActorAttributeType::Float, "10.0", true)}),
      ActorBlueprint("sensor.other.gnss", {"sensor", "other", "gnss"}, {}),
      ActorBlueprint("sensor.other.imu", {"sensor", "other", "imu"}, {}),
      ActorBlueprint("sensor.other.collision", {"sensor", "other", "collision"}, {}),
      // Issue #24: more sensor kinds.
      CameraBlueprint("sensor.camera.depth", {"sensor", "camera", "depth"}),
      CameraBlueprint("sensor.camera.semantic_segmentation",
                      {"sensor", "camera", "semantic_segmentation"}),
      CameraBlueprint("sensor.camera.dvs", {"sensor", "camera", "dvs"}),
      CameraBlueprint("sensor.camera.optical_flow", {"sensor", "camera", "optical_flow"}),
      ActorBlueprint("sensor.other.radar", {"sensor", "other", "radar"},
                     {ActorAttribute("horizontal_fov", rpc::ActorAttributeType::Float, "30", true),
                      ActorAttribute("vertical_fov", rpc::ActorAttributeType::Float, "30", true),
                      ActorAttribute("range", rpc::ActorAttributeType::Float, "100", true),
                      ActorAttribute("points_per_second", rpc::ActorAttributeType::Int, "1500", true)}),
      ActorBlueprint("sensor.lidar.ray_cast_semantic", {"sensor", "lidar", "ray_cast_semantic"},
                     {ActorAttribute("channels", rpc::ActorAttributeType::Int, "32", true),
                      ActorAttribute("range", rpc::ActorAttributeType::Float, "10.0", true),
                      ActorAttribute("points_per_second", rpc::ActorAttributeType::Int, "56000", true),
                      ActorAttribute("rotation_frequency", rpc::ActorAttributeType::Float, "10.0", true)}),
      ActorBlueprint("sensor.other.lane_invasion", {"sensor", "other", "lane_invasion"}, {}),
      ActorBlueprint("sensor.other.obstacle", {"sensor", "other", "obstacle"},
                     {ActorAttribute("distance", rpc::ActorAttributeType::Float, "5", true),
                      ActorAttribute("hit_radius", rpc::ActorAttributeType::Float, "0.5", true),
                      ActorAttribute("only_dynamics", rpc::ActorAttributeType::Bool, "false", true)}),
      V2XBlueprint("sensor.other.v2x", {"sensor", "other", "v2x"},
                   {ActorAttribute("gen_cam_min", rpc::ActorAttributeType::Float, "0.1", true),
                    ActorAttribute("fixed_rate", rpc::ActorAttributeType::Bool, "false", true)}),
      V2XBlueprint("sensor.other.v2x_custom", {"sensor", "other", "v2x_custom"}),
      ActorBlueprint("controller.ai.walker", {"controller", "ai", "walker"}, {}),
      ActorBlueprint("static.prop.trafficcone01", {"static", "prop", "trafficcone01"},
                     {ActorAttribute("role_name", rpc::ActorAttributeType::String, "prop", true),
                      ActorAttribute("size", rpc::ActorAttributeType::String, "small", false)}),
  };
}

SharedPtr<Actor> MakeActor(const std::shared_ptr<Episode> &episode, const ActorData &data) {
  if (data.is_vehicle) return std::make_shared<Vehicle>(episode, data.id);
  // As LibCarla's ActorFactory: lane invasion is computed on the client.
  if (data.type_id == "sensor.other.lane_invasion") {
    return std::make_shared<ClientSideSensor>(episode, data.id);
  }
  if (data.type_id.rfind("sensor.", 0) == 0) {
    return std::make_shared<ServerSideSensor>(episode, data.id);
  }
  if (data.is_walker()) return std::make_shared<Walker>(episode, data.id);
  if (data.is_walker_ai_controller()) {
    return std::make_shared<WalkerAIController>(episode, data.id);
  }
  if (data.is_traffic_light()) return std::make_shared<TrafficLight>(episode, data.id);
  if (data.is_traffic_sign()) return std::make_shared<TrafficSign>(episode, data.id);
  return std::make_shared<Actor>(episode, data.id);
}

// CityObjectLabel values LibCarla reports for the mock's actor kinds.
std::vector<uint8_t> SemanticTags(const ActorData &data) {
  if (data.is_vehicle) return {14u};                       // Car
  if (data.is_walker()) return {12u};                      // Pedestrians
  if (data.is_traffic_light()) return {7u};                // TrafficLight
  if (data.is_traffic_sign()) return {8u};                 // TrafficSigns
  if (data.type_id.rfind("static.", 0) == 0) return {20u};  // Static
  return {};
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

SharedPtr<BlueprintLibrary> BlueprintLibrary::FilterByAttribute(const std::string &name,
                                                              const std::string &value) const {
  std::vector<ActorBlueprint> result;
  for (const auto &bp : _blueprints) {
    if (!bp.ContainsAttribute(name)) continue;
    const ActorAttribute &attribute = bp.GetAttribute(name);
    const auto &values = attribute.GetRecommendedValues();
    if (values.empty() ? attribute.GetValue() == value
                       : std::find(values.begin(), values.end(), value) != values.end()) {
      result.push_back(bp);
    }
  }
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
  _parent_id = data.parent.value_or(0u);
  _semantic_tags = mock::SemanticTags(data);
  for (const auto &[attribute, value] : data.attributes) {
    _attributes.emplace_back(attribute, rpc::ActorAttributeType::String, value);
  }
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
  WithData([&](mock::ActorData &a) {
    a.control = control;
    a.ackermann.reset();
    return 0;
  });
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

// Like LibCarla (Episode::GetActorsById over CachedActorList): in request
// order, repeated ids repeated, unknown ids left out.
SharedPtr<ActorList> World::GetActors(const std::vector<rpc::ActorId> &actor_ids) const {
  std::vector<mock::ActorData> snapshot;
  {
    std::lock_guard<std::mutex> lock(_episode->mutex);
    for (rpc::ActorId id : actor_ids) {
      if (const mock::ActorData *known = _episode->KnownLocked(id)) snapshot.push_back(*known);
    }
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
                                   rpc::AttachmentType) {
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
                                      rpc::AttachmentType attachment_type) noexcept {
  try {
    return SpawnActor(blueprint, transform, parent, attachment_type);
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

namespace {
std::atomic<size_t> g_last_worker_threads{0};
std::atomic<size_t> g_gbuffer_subscriptions{0};  // tsc_mock_gbuffer_subscriptions
std::atomic<uint16_t> g_last_map_layers{static_cast<uint16_t>(rpc::MapLayer::All)};
}  // namespace

Client::Client(const std::string &host, uint16_t port, size_t worker_threads)
    : _endpoint(host + ":" + std::to_string(port)) {
  g_last_worker_threads = worker_threads;
}

std::string Client::GetServerVersion() const {
  mock::Connect(_endpoint, _timeout);
  return carla::version();
}

World Client::GetWorld() const { return World(mock::Connect(_endpoint, _timeout)); }

// LibCarla: World{_episode}, the episode the actor was created in.
World Actor::GetWorld() const { return World(_episode); }

World Client::ReloadWorld(bool reset_settings) const {
  auto episode = mock::Connect(_endpoint, _timeout);
  {
    std::lock_guard<std::mutex> lock(episode->mutex);
    episode->ResetLocked(reset_settings, false);
  }
  return World(episode);
}

World Client::LoadWorld(std::string map_name, bool reset_settings,
                        rpc::MapLayer map_layers) const {
  g_last_map_layers = static_cast<uint16_t>(map_layers);
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
  _geo_reference = geom::GeoLocation(35.0, 139.0, 10.0);
  _geo_projection = geom::GeoProjection::Make(geom::TransverseMercatorParams(
      35.0, 139.0, 1.0, 0.0, 0.0, geom::Ellipsoid(6378137.0, 298.257223563)));
  for (double x : {10.0, 40.0, 70.0}) {
    for (int32_t lane : kLanes) {
      _spawn_points.emplace_back(
          geom::Location(static_cast<float>(x), static_cast<float>(LaneY(lane)), 0.6f));
    }
  }
}

// LibCarla parses the document client-side and, when the XML does not parse,
// throws a plain std::exception (throw_exception slices its "failed to
// generate map" runtime_error); the mock throws the same. It has no XML
// parser: it accepts a
// document with a closed <OpenDRIVE> element (<OpenDRIVE .../> or
// <OpenDRIVE>...</OpenDRIVE>) and models it as its own two-lane road, under
// the given name and with the given OpenDRIVE text. As in LibCarla, such a
// map has no recommended spawn points (they come from the server).
Map::Map(std::string name, std::string xodr_content) : Map() {
  const auto open = xodr_content.find("<OpenDRIVE");
  const auto tag_end = open == std::string::npos ? open : xodr_content.find('>', open);
  if (tag_end == std::string::npos ||
      (xodr_content[tag_end - 1] != '/' &&
       xodr_content.find("</OpenDRIVE>", tag_end) == std::string::npos)) {
    throw std::exception();
  }
  _name = std::move(name);
  _xodr = std::move(xodr_content);
  _spawn_points.clear();
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
  // target(), checked for the actor kind the command needs.
  auto not_a = [](const mock::ActorData &a, const char *kind) {
    return std::runtime_error("actor " + std::to_string(a.id) + " is not a " + kind);
  };
  auto vehicle = [&](rpc::ActorId id) -> mock::ActorData & {
    auto &a = target(id);
    if (!a.is_vehicle) throw not_a(a, "vehicle");
    return a;
  };
  auto walker = [&](rpc::ActorId id) -> mock::ActorData & {
    auto &a = target(id);
    if (!a.is_walker()) throw not_a(a, "walker");
    return a;
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
          auto &a = vehicle(c.actor);
          a.control = c.control;
          a.ackermann.reset();
          return a.id;
        } else if constexpr (std::is_same_v<T, Command::ApplyVehicleAckermannControl>) {
          auto &a = vehicle(c.actor);
          a.ackermann = c.control;
          return a.id;
        } else if constexpr (std::is_same_v<T, Command::ShowDebugTelemetry>) {
          auto &a = vehicle(c.actor);
          a.debug_telemetry = c.enabled;
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
          auto &a = vehicle(c.actor);
          a.autopilot = c.enabled;
          return a.id;
        } else if constexpr (std::is_same_v<T, Command::ApplyWalkerControl>) {
          auto &a = walker(c.actor);
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
          auto &a = vehicle(c.actor);
          a.light_state = c.light_state;
          return a.id;
        } else if constexpr (std::is_same_v<T, Command::ApplyLocation>) {
          auto &a = target(c.actor);
          a.transform.location = c.location;
          return a.id;
        } else if constexpr (std::is_same_v<T, Command::ApplyVehiclePhysicsControl>) {
          // Replaced wholesale, even when the wheel count differs from the
          // vehicle's: what the server does on such a mismatch is unverified.
          auto &a = vehicle(c.actor);
          a.physics = c.physics_control;
          return a.id;
        } else if constexpr (std::is_same_v<T, Command::ApplyWalkerState>) {
          // Moves the walker and makes it walk along the transform's forward
          // vector at `speed`. Z is taken as given: any height adjustment the
          // server makes for the walker's capsule is not modelled.
          auto &a = walker(c.actor);
          a.transform = c.transform;
          const double yaw = c.transform.rotation.yaw * M_PI / 180.0;
          const double pitch = c.transform.rotation.pitch * M_PI / 180.0;
          a.walker_control = rpc::WalkerControl(
              geom::Vector3D(static_cast<float>(std::cos(pitch) * std::cos(yaw)),
                             static_cast<float>(std::cos(pitch) * std::sin(yaw)),
                             static_cast<float>(std::sin(pitch))),
              c.speed, false);
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

}  // namespace client

void FileSystem::ValidateFilePath(std::string &filepath, const std::string &ext) {
  namespace fs = std::filesystem;
  fs::path path(filepath);
  if (!ext.empty() && path.extension() != ext) path.replace_extension(ext);
  // As ue5-dev (the default ref; 0.10.0 omits fs::absolute). With either, a
  // bare file name ("out.png") has an empty parent path, which libstdc++'s
  // absolute() / create_directories() reject, so it raises there too.
  auto parent = fs::absolute(path.parent_path());
  if (!fs::exists(parent)) fs::create_directories(parent);
  filepath = path.string();
}

namespace client {

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
  _listening_gbuffer = false;
  auto it = _episode->listeners.find(_id);
  if (it != _episode->listeners.end() && it->second.owner == this) _episode->listeners.erase(it);
}

bool Sensor::IsListening() const {
  std::lock_guard<std::mutex> lock(_episode->mutex);
  if (_listening_gbuffer) return true;
  auto it = _episode->listeners.find(_id);
  return it != _episode->listeners.end() && it->second.owner == this;
}

Sensor::~Sensor() { Stop(); }

// --- Issue #33: ServerSideSensor ---------------------------------------------

namespace {

constexpr uint32_t kGBufferTextureCount = 13;  // as LibCarla's ServerSideSensor.cpp

void CheckGBufferId(uint32_t id) {
  // LibCarla RELEASE_ASSERTs (aborts); the shim checks before calling.
  if (id >= kGBufferTextureCount) throw std::logic_error("G-buffer id out of range");
}

}  // namespace

std::vector<uint32_t> ServerSideSensor::OwnGBuffers() const {
  std::lock_guard<std::mutex> lock(_episode->mutex);
  std::vector<uint32_t> ids;
  for (const auto &entry : _episode->gbuffer_listeners) {
    if (entry.first.first == _id && entry.second.owner == this) ids.push_back(entry.first.second);
  }
  return ids;
}

// As LibCarla: StopGBuffer for each texture, errors logged (a destroyed
// actor's subscriptions then stay, see StopGBuffer).
ServerSideSensor::~ServerSideSensor() {
  for (uint32_t id : OwnGBuffers()) {
    try {
      StopGBuffer(id);
    } catch (const std::exception &) {
    }
  }
}

// As LibCarla's ServerSideSensor::Destroy: while listening (bit 0, which
// Stop() clears), stop the G-buffer streams and the measurements first.
bool ServerSideSensor::Destroy() {
  if (IsListening()) {
    for (uint32_t id : OwnGBuffers()) StopGBuffer(id);
    Stop();
  }
  return Actor::Destroy();
}

void ServerSideSensor::ListenToGBuffer(uint32_t GBufferId, CallbackFunctionType callback) {
  CheckGBufferId(GBufferId);
  // LibCarla logs a warning and does nothing for other sensors.
  if (GetTypeId() != "sensor.camera.rgb") return;
  // The server's get_gbuffer_token fails for a destroyed actor (WithData throws).
  WithData([&](mock::ActorData &) {
    auto &entry = _episode->gbuffer_listeners[{_id, GBufferId}];
    if (!entry.callback) ++g_gbuffer_subscriptions;
    entry = mock::Episode::Listener{this, std::move(callback)};
    _listening_gbuffer = true;
    return 0;
  });
}

void ServerSideSensor::StopGBuffer(uint32_t GBufferId) {
  CheckGBufferId(GBufferId);
  if (GetTypeId() != "sensor.camera.rgb") return;
  // LibCarla asks the server for the stream token first (get_gbuffer_token),
  // which fails for a destroyed actor: the subscription is then never
  // removed and the streaming client keeps reconnecting (issue #33 review).
  WithData([&](mock::ActorData &) {
    auto it = _episode->gbuffer_listeners.find({_id, GBufferId});
    if (it != _episode->gbuffer_listeners.end() && it->second.owner == this) {
      _episode->gbuffer_listeners.erase(it);
      --g_gbuffer_subscriptions;
    }
    return 0;
  });
}

bool ServerSideSensor::IsListeningGBuffer(uint32_t id) const {
  std::lock_guard<std::mutex> lock(_episode->mutex);
  auto it = _episode->gbuffer_listeners.find({_id, id});
  return it != _episode->gbuffer_listeners.end() && it->second.owner == this;
}

void ServerSideSensor::EnableForROS() {
  WithData([](mock::ActorData &a) { return a.ros_enabled = true; });
}

void ServerSideSensor::DisableForROS() {
  WithData([](mock::ActorData &a) { return a.ros_enabled = false; });
}

bool ServerSideSensor::IsEnabledForROS() {
  return WithData([](mock::ActorData &a) { return a.ros_enabled; });
}

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

std::string AttributeString(const ActorData &a, const std::string &key,
                            const std::string &fallback) {
  auto it = a.attributes.find(key);
  return it == a.attributes.end() ? fallback : it->second;
}

double Distance(const geom::Location &p, const geom::Location &q) {
  return std::sqrt((p.x - q.x) * (p.x - q.x) + (p.y - q.y) * (p.y - q.y) +
                   (p.z - q.z) * (p.z - q.z));
}

}  // namespace
}  // namespace mock

// As ServerSideSensor::Send and ACustomV2XSensor::Send (ue5-dev): LibCarla
// checks the blueprint id and only warns for any other sensor; the server
// stamps the header (protocol version 2, the custom message id, the station:
// the parent's actor id, or the sensor's own without a parent) and the
// transmit power, and queues the message for the next tick.
void ServerSideSensor::Send(const rpc::CustomV2XBytes &data) {
  WithData([&](mock::ActorData &a) {
    if (a.type_id != "sensor.other.v2x_custom") {
      std::cerr << "WARNING: Send methods are not supported on non-V2X sensors "
                   "(sensor.other.v2x_custom).\n";
      return 0;
    }
    sensor::data::CustomV2XData message{};
    message.Message.header.protocolVersion = 2;
    message.Message.header.messageID = ITSContainer::messageID_custom;
    message.Message.header.stationID = static_cast<long>(a.parent.value_or(a.id));
    message.Message.data = data;
    message.Power = static_cast<float>(mock::AttributeDouble(a, "transmit_power", 21.5));
    _episode->v2x_outbox.emplace_back(a.id, message);
    return 0;
  });
}

namespace mock {

namespace {

// A camera's image size and field of view (its blueprint's defaults).
struct CameraGeometry {
  size_t w = 0, h = 0;
  float fov = 0.0f;
};

CameraGeometry CameraGeometryOf(const ActorData &a) {
  return {static_cast<size_t>(AttributeInt(a, "image_size_x", 800)),
          static_cast<size_t>(AttributeInt(a, "image_size_y", 600)),
          static_cast<float>(AttributeDouble(a, "fov", 90.0))};
}

// A mock LiDAR sweep: `per_channel` points evenly around each channel's ring
// at the sensor's range, made by make(location, channel, elevation).
template <typename Point, typename Make>
std::vector<Point> LidarSweep(const ActorData &a, uint32_t channels, uint32_t per_channel,
                              Make make) {
  const float range = static_cast<float>(AttributeDouble(a, "range", 10.0));
  std::vector<Point> points;
  points.reserve(size_t{channels} * per_channel);
  for (uint32_t c = 0; c < channels; ++c) {
    const double elevation = (static_cast<double>(c) / std::max(1u, channels - 1) - 0.5) * 0.5;
    for (uint32_t i = 0; i < per_channel; ++i) {
      const double azimuth = 2.0 * M_PI * i / per_channel;
      points.push_back(
          make(geom::Location(static_cast<float>(range * std::cos(azimuth) * std::cos(elevation)),
                              static_cast<float>(range * std::sin(azimuth) * std::cos(elevation)),
                              static_cast<float>(range * std::sin(elevation))),
               c, elevation));
    }
  }
  return points;
}

}  // namespace

std::vector<Delivery> Episode::SenseLocked() {
  std::vector<Delivery> out;
  const double timestamp = static_cast<double>(frame) * DeltaSeconds();
  SenseV2XLocked(timestamp, out);
  auto self = shared_from_this();
  for (auto &entry : listeners) {
    auto it = actors.find(entry.first);
    if (it == actors.end()) continue;
    const ActorData &a = it->second;
    auto callback = entry.second.callback;
    SharedPtr<sensor::SensorData> data;
    namespace sd = sensor::data;
    const bool is_camera = a.type_id.rfind("sensor.camera.", 0) == 0;
    const auto [w, h, fov] = is_camera ? CameraGeometryOf(a) : CameraGeometry{};
    if (a.type_id == "sensor.camera.rgb" || a.type_id == "sensor.camera.depth" ||
        a.type_id == "sensor.camera.semantic_segmentation") {
      const bool semantic = a.type_id == "sensor.camera.semantic_segmentation";
      // The pixels are filled in the delivery, outside the episode lock.
      out.push_back([cb = std::move(callback), f = frame, timestamp, t = a.transform, w, h, fov,
                     semantic]() {
        auto image = std::make_shared<sd::Image>(f, timestamp, t, w, h, fov);
        // RGB and depth: a gradient that changes with the frame: B = x, G = y,
        // R = frame. Semantic segmentation: the tag (R) is x mod 29, G = B = 0.
        for (size_t y = 0; y < h; ++y) {
          for (size_t x = 0; x < w; ++x) {
            image->data()[y * w + x] =
                semantic ? sd::Color(static_cast<uint8_t>(x % 29), 0u, 0u, 255u)
                         : sd::Color(static_cast<uint8_t>(f), static_cast<uint8_t>(y),
                                     static_cast<uint8_t>(x), 255u);
          }
        }
        cb(std::move(image));
      });
      continue;
    } else if (a.type_id == "sensor.camera.optical_flow") {
      out.push_back([cb = std::move(callback), f = frame, timestamp, t = a.transform, w, h, fov]() {
        auto image = std::make_shared<sd::OpticalFlowImage>(f, timestamp, t, w, h, fov);
        // Flow pointing away from the image centre, scaled by the frame size.
        for (size_t y = 0; y < h; ++y) {
          for (size_t x = 0; x < w; ++x) {
            image->data()[y * w + x] = sd::OpticalFlowPixel(
                (static_cast<float>(x) - static_cast<float>(w) / 2.0f) / static_cast<float>(w),
                (static_cast<float>(y) - static_cast<float>(h) / 2.0f) / static_cast<float>(h));
          }
        }
        cb(std::move(image));
      });
      continue;
    } else if (a.type_id == "sensor.camera.dvs") {
      // One event per diagonal pixel (up to 100), alternating polarity.
      std::vector<sd::DVSEvent> events;
      const auto n = static_cast<uint32_t>(std::min<size_t>({w, h, 100u}));
      for (uint32_t i = 0; i < n; ++i) {
        events.emplace_back(static_cast<uint16_t>(i), static_cast<uint16_t>(i),
                            static_cast<int64_t>(frame) * 1000000 + i, (i + frame) % 2 == 0);
      }
      data = std::make_shared<sd::DVSEventArray>(frame, timestamp, a.transform,
                                                 static_cast<uint32_t>(w),
                                                 static_cast<uint32_t>(h), fov, std::move(events));
    } else if (a.type_id == "sensor.other.radar") {
      // Ten detections straight ahead, approaching at 0..9 m/s.
      std::vector<sd::RadarDetection> detections;
      for (int i = 0; i < 10; ++i) {
        detections.push_back(sd::RadarDetection{-1.0f * static_cast<float>(i),
                                                0.05f * static_cast<float>(i - 5),
                                                0.01f * static_cast<float>(i),
                                                5.0f + static_cast<float>(i)});
      }
      data = std::make_shared<sd::RadarMeasurement>(frame, timestamp, a.transform,
                                                    std::move(detections));
    } else if (a.type_id == "sensor.lidar.ray_cast_semantic") {
      const auto channels = static_cast<uint32_t>(AttributeInt(a, "channels", 32));
      constexpr uint32_t kPerChannel = 10;
      auto points = LidarSweep<sd::SemanticLidarDetection>(
          a, channels, kPerChannel, [&](geom::Location p, uint32_t c, double elevation) {
            return sd::SemanticLidarDetection(p, static_cast<float>(std::cos(elevation)),
                                              a.parent.value_or(0u), c % 29u);
          });
      data = std::make_shared<sd::SemanticLidarMeasurement>(
          frame, timestamp, a.transform, 0.0f, std::vector<uint32_t>(channels, kPerChannel),
          std::move(points));
    } else if (a.type_id == "sensor.other.lane_invasion" && a.parent) {
      // As LibCarla's client-side LaneInvasionCallback::Tick: the first tick
      // only records the position, a tick without motion (< 10 eps) is
      // skipped, and otherwise the markings crossed between the previous and
      // the current position are reported. The mock's markings are the lines
      // at y = -1.75 (outer edge, solid), 1.75 (between the lanes, broken)
      // and 5.25 (outer edge, solid); the parent is treated as a point.
      auto parent = actors.find(*a.parent);
      if (parent == actors.end()) continue;
      const geom::Location now = parent->second.transform.location;
      auto &previous = entry.second.lane_previous;
      if (!previous) {
        previous = now;
        continue;
      }
      const float dx = now.x - previous->x, dy = now.y - previous->y, dz = now.z - previous->z;
      if (std::sqrt(dx * dx + dy * dy + dz * dz) < 10.0f * std::numeric_limits<float>::epsilon()) {
        continue;
      }
      const double y0 = previous->y, y1 = now.y;
      previous = now;
      using LM = road::element::LaneMarking;
      std::vector<LM> crossed;
      const double centre = LaneY(-1) + kLaneWidth / 2.0;
      for (double line : {LaneY(-1) - kLaneWidth / 2.0, centre, LaneY(-2) + kLaneWidth / 2.0}) {
        if ((y0 < line) == (y1 < line)) continue;
        if (line == centre) {
          crossed.emplace_back(LM::Type::Broken, LM::Color::Standard, LM::LaneChange::Both, 0.15);
        } else {
          crossed.emplace_back(LM::Type::Solid, LM::Color::Standard, LM::LaneChange::None, 0.15);
        }
      }
      if (crossed.empty()) continue;
      const rpc::ActorId parent_id = parent->second.id;
      data = std::make_shared<sd::LaneInvasionEvent>(frame, timestamp, a.transform, parent_id,
                                                     std::move(crossed));
    } else if (a.type_id == "sensor.other.obstacle" && a.parent) {
      // The nearest other vehicle within `distance` of the parent.
      auto parent = actors.find(*a.parent);
      if (parent == actors.end()) continue;
      const double max_distance = AttributeDouble(a, "distance", 5.0);
      const auto &p = parent->second.transform.location;
      std::optional<rpc::ActorId> nearest;
      double best = max_distance;
      for (const auto &other_entry : actors) {
        const ActorData &other = other_entry.second;
        if (other.id == parent->second.id || !other.is_vehicle) continue;
        const double d = Distance(p, other.transform.location);
        if (d < best) {
          best = d;
          nearest = other.id;
        }
      }
      if (!nearest) continue;
      const rpc::ActorId self_id = parent->second.id;
      out.push_back([self, callback, self_id, other_id = *nearest, t = a.transform, f = frame,
                     timestamp, best]() {
        const World world(self);
        callback(std::make_shared<sd::ObstacleDetectionEvent>(
            f, timestamp, t, world.GetActor(self_id), world.GetActor(other_id),
            static_cast<float>(best)));
      });
      continue;
    } else if (a.type_id == "sensor.lidar.ray_cast") {
      const auto channels = static_cast<uint32_t>(AttributeInt(a, "channels", 32));
      constexpr uint32_t kPerChannel = 100;
      auto points = LidarSweep<sd::LidarDetection>(
          a, channels, kPerChannel, [&](geom::Location p, uint32_t c, double) {
            sd::LidarDetection d;
            d.point = p;
            d.intensity = 1.0f - static_cast<float>(c) / static_cast<float>(channels);
            return d;
          });
      data = std::make_shared<sd::LidarMeasurement>(frame, timestamp, a.transform, 0.0f,
                                                    std::vector<uint32_t>(channels, kPerChannel),
                                                    std::move(points));
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
  // Issue #33: one G-buffer Image per subscription and tick, every pixel
  // (id, id, id, 255), the camera's size.
  for (auto &entry : gbuffer_listeners) {
    auto it = actors.find(entry.first.first);
    if (it == actors.end()) continue;
    const ActorData &a = it->second;
    const auto [w, h, fov] = CameraGeometryOf(a);
    const auto id = static_cast<uint8_t>(entry.first.second);
    out.push_back([cb = entry.second.callback, f = frame, timestamp, t = a.transform, w, h, fov,
                   id]() {
      auto image = std::make_shared<sensor::data::Image>(f, timestamp, t, w, h, fov);
      for (size_t i = 0; i < w * h; ++i) image->data()[i] = sensor::data::Color(id, id, id, 255u);
      cb(std::move(image));
    });
  }
  return out;
}

namespace {

// Whether `to` receives what `from` sends: within the receiver's
// filter_distance. (No path-loss model: the received power is the transmit
// power.)
bool InRange(const ActorData &from, const ActorData &to) {
  return Distance(from.transform.location, to.transform.location) <=
         AttributeDouble(to, "filter_distance", 500.0);
}

// A CAM as the server's CaService builds it, from the mock's actor state.
// Positions use the mock GNSS's flat-earth reference (see the GNSS sensor).
CAM_t MakeCam(const ActorData &origin, const ActorData *vehicle, long station_id,
              long station_type, double now) {
  CAM_t cam{};
  cam.header.protocolVersion = 2;
  cam.header.messageID = ITSContainer::messageID_cam;
  cam.header.stationID = station_id;
  auto &coop = cam.cam;
  coop.generationDeltaTime = static_cast<long>(std::llround(now * 1000.0) % 65536);
  auto &basic = coop.camParameters.basicContainer;
  basic.stationType = station_type;
  const auto &p = origin.transform.location;
  basic.referencePosition.latitude = std::lround(-p.y / 111111.0 * 1e6) * 10;
  basic.referencePosition.longitude = std::lround(p.x / 111111.0 * 1e6) * 10;
  basic.referencePosition.positionConfidenceEllipse = {ITSContainer::SemiAxisLength_unavailable,
                                                       ITSContainer::SemiAxisLength_unavailable,
                                                       ITSContainer::HeadingValue_unavailable};
  basic.referencePosition.altitude = {std::lround(p.z * 100.0),
                                      ITSContainer::AltitudeConfidence_unavailable};
  auto &hfc = coop.camParameters.highFrequencyContainer;
  auto &lfc = coop.camParameters.lowFrequencyContainer;
  hfc.present = CAMContainer::HighFrequencyContainer_PR_NOTHING;
  lfc.present = CAMContainer::LowFrequencyContainer_PR_NOTHING;
  if (station_type == ITSContainer::StationType_roadSideUnit) {
    // As CaService::AddRSUContainerHighFrequency: 16 placeholder zones.
    hfc.present = CAMContainer::HighFrequencyContainer_PR_rsuContainerHighFrequency;
    auto &zones = hfc.rsuContainerHighFrequency.protectedCommunicationZonesRSU;
    zones.ProtectedCommunicationZoneCount = static_cast<long>(zones.data.size());
    for (auto &zone : zones.data) {
      zone = ITSContainer::ProtectedCommunicationZone_t{};
      zone.protectedZoneType = ITSContainer::ProtectedZoneType_cenDsrcTolling;
      zone.protectedZoneLatitude = 50;
      zone.protectedZoneLongitude = 50;
    }
    return cam;
  }
  if (vehicle == nullptr) return cam;  // e.g. a pedestrian: no container
  hfc.present = CAMContainer::HighFrequencyContainer_PR_basicVehicleContainerHighFrequency;
  auto &bvc = hfc.basicVehicleContainerHighFrequency;
  const double heading = std::fmod(vehicle->transform.rotation.yaw + 90.0 + 360.0, 360.0);
  bvc.heading = {std::lround(heading * 10.0), ITSContainer::HeadingConfidence_equalOrWithinOneDegree};
  const auto &v = vehicle->velocity;
  bvc.speed = {std::lround(std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z) * 100.0), 3};
  bvc.driveDirection = ITSContainer::DriveDirection_forward;
  bvc.vehicleLength = {46, ITSContainer::VehicleLengthConfidenceIndication_unavailable};
  bvc.vehicleWidth = 18;
  const auto &acc = vehicle->acceleration;
  bvc.longitudinalAcceleration = {std::lround(acc.x * 10.0),
                                  ITSContainer::AccelerationConfidence_unavailable};
  bvc.curvature = {ITSContainer::CurvatureValue_unavailable,
                   ITSContainer::CurvatureConfidence_unavailable};
  bvc.curvatureCalculationMode = ITSContainer::CurvatureCalculationMode_yarRateUsed;
  bvc.yawRate = {std::lround(vehicle->angular_velocity.z * 100.0),
                 ITSContainer::YawRateConfidence_unavailable};
  bvc.lateralAccelerationAvailable = true;
  bvc.lateralAcceleration = {std::lround(acc.y * 10.0),
                             ITSContainer::AccelerationConfidence_unavailable};
  bvc.verticalAccelerationAvailable = true;
  bvc.verticalAcceleration = {std::lround(acc.z * 10.0),
                              ITSContainer::AccelerationConfidence_unavailable};
  {  // the low-frequency container in every CAM (the server: at most every 0.5 s)
    lfc.present = CAMContainer::LowFrequencyContainer_PR_basicVehicleContainerLowFrequency;
    auto &low = lfc.basicVehicleContainerLowFrequency;
    low.vehicleRole = ITSContainer::VehicleRole_default;
    // CaService::AddLowFrequencyContainer: ETSI bit string, bit 0 first.
    using LS = rpc::VehicleLightState::LightState;
    const std::pair<LS, int> lights[] = {
        {LS::LowBeam, ITSContainer::ExteriorLights_lowBeamHeadlightsOn},
        {LS::HighBeam, ITSContainer::ExteriorLights_highBeamHeadlightsOn},
        {LS::LeftBlinker, ITSContainer::ExteriorLights_leftTurnSignalOn},
        {LS::RightBlinker, ITSContainer::ExteriorLights_rightTurnSignalOn},
        {LS::Reverse, ITSContainer::ExteriorLights_reverseLightOn},
        {LS::Fog, ITSContainer::ExteriorLights_fogLightOn},
        {LS::Position, ITSContainer::ExteriorLights_parkingLightsOn}};
    low.exteriorLights = 0u;
    for (const auto &[state, bit] : lights) {
      if (vehicle->light_state & static_cast<uint32_t>(state)) {
        low.exteriorLights |= static_cast<uint8_t>(1u << (7 - bit));
      }
    }
    low.pathHistory.NumberOfPathPoint = 0;  // as the server
  }
  return cam;
}

}  // namespace

void Episode::SenseV2XLocked(double timestamp, std::vector<Delivery> &out) {
  namespace sd = sensor::data;
  // CAMs: each V2X sensor (listening or not) sends one every tick, or every
  // gen_cam_min seconds with fixed_rate (the server's CaService also adapts
  // the rate to the parent's dynamics). A sensor without a vehicle or walker
  // parent is a road-side unit.
  std::vector<std::pair<rpc::ActorId, sd::CAMData>> cams;
  for (const auto &entry : actors) {
    const ActorData &a = entry.second;
    if (a.type_id != "sensor.other.v2x") continue;
    if (AttributeString(a, "fixed_rate", "false") == "true") {
      auto last = last_cam.find(a.id);
      // (+1e-6: frame * delta_seconds is not exact; 0.1 s is two 0.05 s ticks.)
      if (last != last_cam.end() &&
          timestamp - last->second + 1e-6 < AttributeDouble(a, "gen_cam_min", 0.1)) {
        continue;
      }
      last_cam[a.id] = timestamp;
    }
    const ActorData *parent = nullptr;
    if (a.parent) {
      auto p = actors.find(*a.parent);
      if (p != actors.end()) parent = &p->second;
    }
    const bool vehicle = parent != nullptr && parent->is_vehicle;
    const bool pedestrian = parent != nullptr && parent->is_walker();
    const long station_type = vehicle      ? ITSContainer::StationType_passengerCar
                              : pedestrian ? ITSContainer::StationType_pedestrian
                                           : ITSContainer::StationType_roadSideUnit;
    const ActorData &origin = parent != nullptr && (vehicle || pedestrian) ? *parent : a;
    sd::CAMData cam;
    cam.Power = static_cast<float>(AttributeDouble(a, "transmit_power", 21.5));
    cam.Message = MakeCam(origin, vehicle ? parent : nullptr,
                          static_cast<long>(parent != nullptr ? parent->id : a.id), station_type,
                          timestamp);
    cams.emplace_back(a.id, cam);
  }
  // Custom messages sent since the last tick. As ACustomV2XSensor::PrePhysTick,
  // only a sender with a parent ("owner") moves its messages on to the
  // receivers; each message is delivered on one tick only.
  // Queues for a listening receiver the (sender, message) pairs whose sender
  // still exists, passes `accept` and is in range.
  auto deliver = [&](const ActorData &receiver, const auto &callback, const auto &sent,
                     auto accept) {
    std::vector<typename std::decay_t<decltype(sent)>::value_type::second_type> received;
    for (const auto &[sender_id, message] : sent) {
      auto sender = actors.find(sender_id);
      if (sender_id == receiver.id || sender == actors.end() || !accept(sender->second) ||
          !InRange(sender->second, receiver)) {
        continue;
      }
      received.push_back(message);
    }
    if (received.empty()) return;
    using Event = sd::V2XArray<typename decltype(received)::value_type>;
    out.push_back([cb = callback, f = frame, timestamp, t = receiver.transform,
                   data = std::move(received)]() mutable {
      cb(std::make_shared<Event>(f, timestamp, t, std::move(data)));
    });
  };
  std::vector<std::pair<rpc::ActorId, sd::CustomV2XData>> sent;
  sent.swap(v2x_outbox);
  for (auto &entry : listeners) {
    auto it = actors.find(entry.first);
    if (it == actors.end()) continue;
    const ActorData &receiver = it->second;
    if (receiver.type_id == "sensor.other.v2x") {
      deliver(receiver, entry.second.callback, cams, [](const ActorData &) { return true; });
    } else if (receiver.type_id == "sensor.other.v2x_custom") {
      const std::string channel = AttributeString(receiver, "channel_id", "Default");
      deliver(receiver, entry.second.callback, sent, [&](const ActorData &sender) {
        return sender.parent && AttributeString(sender, "channel_id", "Default") == channel;
      });
    }
  }
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

// --- Issue #20 -----------------------------------------------------------------

void Vehicle::ShowDebugTelemetry(bool enabled) {
  WithData([&](mock::ActorData &a) { a.debug_telemetry = enabled; return 0; });
}

void Vehicle::ApplyAckermannControl(const AckermannControl &control) {
  WithData([&](mock::ActorData &a) { a.ackermann = control; return 0; });
}

rpc::AckermannControllerSettings Vehicle::GetAckermannControllerSettings() const {
  return WithData([](mock::ActorData &a) { return a.ackermann_settings; });
}

void Vehicle::ApplyAckermannControllerSettings(const rpc::AckermannControllerSettings &settings) {
  WithData([&](mock::ActorData &a) { a.ackermann_settings = settings; return 0; });
}

namespace {

uint32_t DoorBits(rpc::VehicleDoor door) {
  return door == rpc::VehicleDoor::All ? 0x3Fu : 1u << static_cast<uint32_t>(door);
}

// The server answers an unknown wheel with an error, which CallAndWait throws.
const rpc::WheelPhysicsControl &WheelAt(const mock::ActorData &a, rpc::VehicleWheelLocation w) {
  const auto i = static_cast<size_t>(w);
  if (i >= a.physics.wheels.size()) {
    throw std::runtime_error("vehicle " + std::to_string(a.id) + " has no wheel " +
                             std::to_string(i));
  }
  return a.physics.wheels[i];
}

double ForwardSpeed(const mock::ActorData &a) {
  const double yaw = a.transform.rotation.yaw * M_PI / 180.0;
  return a.velocity.x * std::cos(yaw) + a.velocity.y * std::sin(yaw);
}

}  // namespace

void Vehicle::OpenDoor(const VehicleDoor door_idx) {
  WithData([&](mock::ActorData &a) { a.open_doors |= DoorBits(door_idx); return 0; });
}

void Vehicle::CloseDoor(const VehicleDoor door_idx) {
  WithData([&](mock::ActorData &a) { a.open_doors &= ~DoorBits(door_idx); return 0; });
}

void Vehicle::SetWheelSteerDirection(WheelLocation wheel_location, float angle_in_deg) {
  WithData([&](mock::ActorData &a) {
    WheelAt(a, wheel_location);
    a.wheel_steer_direction[static_cast<int>(wheel_location)] = angle_in_deg;
    return 0;
  });
}

float Vehicle::GetWheelSteerAngle(WheelLocation wheel_location) {
  return WithData([&](mock::ActorData &a) {
    const auto &w = WheelAt(a, wheel_location);
    if (!w.affected_by_steering) return 0.0f;
    const float steer = a.ackermann ? static_cast<float>(a.ackermann->steer * 180.0 / M_PI) /
                                          std::max(w.max_steer_angle, 1.0f)
                                    : a.control.steer;
    return std::clamp(steer, -1.0f, 1.0f) * w.max_steer_angle;
  });
}

Vehicle::TelemetryData Vehicle::GetTelemetryData() const {
  return WithData([](mock::ActorData &a) {
    TelemetryData t;
    const double speed = ForwardSpeed(a);
    t.speed = static_cast<float>(speed);
    t.steer = a.control.steer;
    t.throttle = a.control.throttle;
    t.brake = a.control.brake;
    t.gear = speed > 0.1 ? 1 : speed < -0.1 ? -1 : 0;
    t.engine_rpm = static_cast<float>(800.0 + 120.0 * std::abs(speed));
    for (const auto &w : a.physics.wheels) {
      const double radius_m = std::max(w.wheel_radius, 1.0f) / 100.0;
      t.wheels.emplace_back(0.0f, 0.0f, static_cast<float>(speed / radius_m));
    }
    return t;
  });
}

void Vehicle::EnableCarSim(std::string simfile_path) {
  WithData([&](mock::ActorData &a) { a.carsim_file = simfile_path; return 0; });
}

void Vehicle::UseCarSimRoad(bool enabled) {
  WithData([&](mock::ActorData &a) { a.carsim_road = enabled; return 0; });
}

void Vehicle::EnableChronoPhysics(uint64_t, float, std::string, std::string, std::string,
                                  std::string) {
  WithData([&](mock::ActorData &a) { a.chrono = true; return 0; });
}

rpc::VehicleFailureState Vehicle::GetFailureState() const {
  return WithData([](mock::ActorData &a) { return a.failure_state; });
}

std::vector<geom::Transform> Vehicle::GetVehicleBoneWorldTransforms() const {
  return WithData([](mock::ActorData &a) {
    // The root bone, then one per wheel at its offset (cm) from the center.
    std::vector<geom::Transform> out{a.transform};
    const double yaw = a.transform.rotation.yaw * M_PI / 180.0;
    for (const auto &w : a.physics.wheels) {
      const double x = w.offset.x / 100.0, y = w.offset.y / 100.0;
      geom::Transform t = a.transform;
      t.location.x += static_cast<float>(x * std::cos(yaw) - y * std::sin(yaw));
      t.location.y += static_cast<float>(x * std::sin(yaw) + y * std::cos(yaw));
      t.location.z += static_cast<float>(w.offset.z / 100.0);
      out.push_back(t);
    }
    return out;
  });
}

namespace {

// A small fixed skeleton with CARLA's pedestrian bone names; each bone sits
// 0.1 m above its predecessor in the component space.
const std::vector<std::string> &MockBoneNames() {
  static const std::vector<std::string> names{
      "crl_root",       "crl_hips__C",    "crl_spine__C",   "crl_spine01__C", "crl_neck__C",
      "crl_Head__C",    "crl_arm__L",     "crl_foreArm__L", "crl_hand__L",    "crl_arm__R",
      "crl_foreArm__R", "crl_hand__R",    "crl_thigh__L",   "crl_leg__L",     "crl_foot__L",
      "crl_thigh__R",   "crl_leg__R",     "crl_foot__R"};
  return names;
}

}  // namespace

Walker::BoneControlOut Walker::GetBonesTransform() {
  return WithData([](mock::ActorData &a) {
    BoneControlOut out;
    const auto &names = MockBoneNames();
    for (size_t i = 0; i < names.size(); ++i) {
      rpc::BoneTransformDataOut b;
      b.bone_name = names[i];
      b.relative = geom::Transform(geom::Location(0.0f, 0.0f, i == 0 ? 0.0f : 0.1f));
      auto custom = a.custom_pose.find(names[i]);
      if (a.pose_blend > 0.0f && custom != a.custom_pose.end()) b.relative = custom->second;
      b.component = geom::Transform(geom::Location(0.0f, 0.0f, 0.1f * static_cast<float>(i)),
                                    b.relative.rotation);
      b.world = a.transform;
      b.world.location.z += b.component.location.z;
      out.bone_transforms.push_back(b);
    }
    return out;
  });
}

void Walker::SetBonesTransform(const BoneControlIn &bones) {
  WithData([&](mock::ActorData &a) {
    for (const auto &bone : bones.bone_transforms) a.custom_pose[bone.first] = bone.second;
    return 0;
  });
}

void Walker::BlendPose(float blend) {
  WithData([&](mock::ActorData &a) { a.pose_blend = blend; return 0; });
}

// The custom pose becomes the current animation pose (the mock's rest pose).
void Walker::GetPoseFromAnimation() {
  WithData([](mock::ActorData &a) { a.custom_pose.clear(); return 0; });
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
void DebugHelper::ClearDebugShape() {}
void DebugHelper::ClearDebugString() {}

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

// ---------------------------------------------------------------------------
// Issue #22: XODR waypoints, landmarks, lane markings, traffic light geometry

namespace {

constexpr double kSignalS = 100.0;  // the stop sign / traffic light signal "1000"

using Marking = road::element::LaneMarking;

}  // namespace

SharedPtr<Waypoint> Map::GetWaypointXODR(road::RoadId road_id, road::LaneId lane_id,
                                         float s) const {
  if (road_id != 1u || (lane_id != -1 && lane_id != -2) || s < 0.0f || s > kRoadLength) {
    return nullptr;
  }
  return MakeWaypoint(lane_id, s);
}

std::vector<SharedPtr<Landmark>> Map::GetLandmarksFromId(std::string id) const {
  std::vector<SharedPtr<Landmark>> result;
  for (auto &l : GetAllLandmarks())
    if (l->GetId() == id) result.push_back(l);
  return result;
}

std::vector<SharedPtr<Landmark>> Map::GetLandmarkGroup(const Landmark &) const {
  // LibCarla returns the landmarks of the signal's controllers. The mock's
  // only signal is a stop sign, which (as in CARLA's towns) has none.
  return {};
}

void Map::CookInMemoryMap(const std::string &path) const {
  // LibCarla logs (and otherwise ignores) a file it cannot open.
  std::ofstream out(path.empty() ? std::string("MockTown.bin") : path, std::ios::binary);
  const uint32_t records = 0u;
  out.write(reinterpret_cast<const char *>(&records), sizeof records);
}

std::optional<road::element::LaneMarking> Waypoint::GetLeftLaneMarking() const {
  if (_lane_id == -1) {
    return Marking(Marking::Type::Solid, Marking::Color::Yellow, Marking::LaneChange::None, 0.15);
  }
  return Marking(Marking::Type::Broken, Marking::Color::Standard, Marking::LaneChange::Both, 0.15);
}

std::optional<road::element::LaneMarking> Waypoint::GetRightLaneMarking() const {
  if (_lane_id == -1) {
    return Marking(Marking::Type::Broken, Marking::Color::Standard, Marking::LaneChange::Both, 0.15);
  }
  return std::nullopt;  // the road edge has no marking record
}

road::element::LaneMarking::LaneChange Waypoint::GetLaneChange() const {
  return _lane_id == -1 ? Marking::LaneChange::Right : Marking::LaneChange::Left;
}

std::vector<SharedPtr<Landmark>> Waypoint::GetAllLandmarksInDistance(double distance,
                                                                     bool) const {
  if (_s > kSignalS || kSignalS - _s > distance) return {};
  return {std::make_shared<Landmark>("1000", "Stop", "206", kSignalS,
                                     geom::Transform(geom::Location(100.0f, -3.0f, 1.0f)),
                                     MakeWaypoint(_lane_id, kSignalS), kSignalS - _s)};
}

std::vector<SharedPtr<Landmark>> Waypoint::GetLandmarksOfTypeInDistance(
    double distance, std::string filter_type, bool stop_at_junction) const {
  std::vector<SharedPtr<Landmark>> result;
  for (auto &l : GetAllLandmarksInDistance(distance, stop_at_junction))
    if (l->GetType() == filter_type) result.push_back(l);
  return result;
}

std::vector<SharedPtr<TrafficLight>> TrafficLight::GetGroupTrafficLights() {
  WithData([](mock::ActorData &) { return 0; });  // throws if destroyed
  return {std::static_pointer_cast<TrafficLight>(shared_from_this())};
}

std::vector<SharedPtr<Waypoint>> TrafficLight::GetAffectedLaneWaypoints() const {
  WithData([](mock::ActorData &) { return 0; });
  return {MakeWaypoint(-1, kSignalS), MakeWaypoint(-2, kSignalS)};
}

std::vector<geom::BoundingBox> TrafficLight::GetLightBoxes() const {
  const auto t = GetTransform();
  return {geom::BoundingBox(geom::Location(t.location.x, t.location.y, t.location.z + 4.0f),
                            geom::Vector3D(0.3f, 0.3f, 0.9f), t.rotation)};
}

std::vector<SharedPtr<Waypoint>> TrafficLight::GetStopWaypoints() const {
  WithData([](mock::ActorData &) { return 0; });
  return {MakeWaypoint(-1, kSignalS - 5.0), MakeWaypoint(-2, kSignalS - 5.0)};
}

traffic_manager::TrafficManager Client::GetInstanceTM(uint16_t port) const {
  return traffic_manager::TrafficManager(mock::Connect(_endpoint, _timeout), port);
}

std::string Client::StartRecorder(std::string name, bool, bool stop_replayer) {
  auto e = mock::Connect(_endpoint, _timeout);
  std::lock_guard<std::mutex> lock(e->mutex);
  e->recording = name;
  e->recordings[name] = e->frame;
  return "Recording on file: " + name + (stop_replayer ? "" : " (replayer kept)");
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

std::string Client::ReplayFile(std::string name, double, double, uint32_t, bool,
                               bool replay_weather, const geom::Transform &offset,
                               std::string map_override) {
  // The text echoes the ue5-dev arguments, so tests can see them arrive.
  std::string text = "Replaying " +
                     std::to_string(RecordedFrames(mock::Connect(_endpoint, _timeout), name)) +
                     " frames of " + name;
  if (replay_weather) text += " with weather";
  const auto &l = offset.location;
  const auto &r = offset.rotation;
  if (l.x != 0.0f || l.y != 0.0f || l.z != 0.0f || r.pitch != 0.0f || r.yaw != 0.0f ||
      r.roll != 0.0f) {
    text += " offset by (" + std::to_string(l.x) + ", " + std::to_string(l.y) + ", " +
            std::to_string(l.z) + ") rotated (" + std::to_string(r.pitch) + ", " +
            std::to_string(r.yaw) + ", " + std::to_string(r.roll) + ")";
  }
  if (!map_override.empty()) text += " on " + map_override;
  return text;
}

void Client::StopReplayer(bool) {}

void Client::SetReplayerTimeFactor(double) {}

void Client::SetReplayerIgnoreHero(bool) {}

void Client::SetReplayerIgnoreSpectator(bool) {}

std::vector<std::string> Client::GetAvailableMaps() const {
  mock::Connect(_endpoint, _timeout);
  return {"/Game/Carla/Maps/MockTown", "/Game/Carla/Maps/Town01", "/Game/Carla/Maps/Town10HD_Opt"};
}

bool Client::SetFilesBaseFolder(const std::string &path) { return !path.empty(); }

// The mock map needs one file; downloading is not simulated.
std::vector<std::string> Client::GetRequiredFiles(const std::string &folder, const bool) const {
  mock::Connect(_endpoint, _timeout);
  const std::string file = "MockTown/OpenDrive/MockTown.xodr";
  if (file.rfind(folder, 0) != 0) return {};
  return {file};
}

void Client::RequestFile(const std::string &name) const {
  mock::Connect(_endpoint, _timeout);
  if (name != "MockTown/OpenDrive/MockTown.xodr") {
    throw std::runtime_error("file '" + name + "' not found on the server");
  }
}

// As LibCarla: loads unless the current map is `map_name`, with or without
// the "Carla/Maps/" prefix.
void Client::LoadWorldIfDifferent(std::string map_name, bool reset_settings,
                                  rpc::MapLayer map_layers) const {
  g_last_map_layers = static_cast<uint16_t>(map_layers);
  const std::string current = GetWorld().GetMap()->GetName();
  if (map_name != current && "Carla/Maps/" + map_name != current) {
    LoadWorld(std::move(map_name), reset_settings, map_layers);
  }
}

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

// ---------------------------------------------------------------------------
// Issue #19: actor state, physics at a location, textures, skeleton queries.

namespace {

// The mock's "skeletons": vehicles and walkers have a skinned mesh (bones),
// every actor a root component, and vehicles one socket. Bones and sockets
// sit at the actor's origin.
bool HasSkinnedMesh(const mock::ActorData &a) { return a.is_vehicle || a.is_walker(); }

std::vector<std::string> MockBoneNames(const mock::ActorData &a, const char *rpc) {
  if (!HasSkinnedMesh(a)) {
    throw std::runtime_error(std::string(rpc) +
                             ": component not found. Component Name: SkinnedMeshComponent");
  }
  if (a.is_vehicle) return {"Root", "Body", "Wheel_Front_Left", "Wheel_Front_Right",
                            "Wheel_Rear_Left", "Wheel_Rear_Right"};
  return {"crl_root", "crl_hips__C", "crl_spine__C", "crl_Head__C"};
}

std::vector<std::string> MockComponentNames(const mock::ActorData &a) {
  if (HasSkinnedMesh(a)) return {"RootComponent", "Mesh"};
  return {"RootComponent"};
}

std::vector<std::string> MockSocketNames(const mock::ActorData &a) {
  if (a.is_vehicle) return {"Socket_Driver"};
  return {};
}

void CheckComponent(const mock::ActorData &a, const std::string &component, const char *rpc) {
  const auto names = MockComponentNames(a);
  if (std::find(names.begin(), names.end(), component) == names.end()) {
    throw std::runtime_error(std::string(rpc) + ": component not found. Component Name: " + component);
  }
}

std::string Sanitized(const std::string &type_id) {
  std::string out;
  for (char c : type_id) out += (c == '.') ? '_' : c;
  return out;
}

}  // namespace

SharedPtr<Actor> Actor::GetParent() const {
  return _parent_id != 0u ? World(_episode).GetActor(_parent_id) : nullptr;
}

std::string Actor::GetActorName() const {
  return WithData([](mock::ActorData &a) {
    return "Mock_" + Sanitized(a.type_id) + "_" + std::to_string(a.id);
  });
}

std::string Actor::GetActorClassName() const {
  return WithData([](mock::ActorData &a) -> std::string {
    if (a.is_vehicle) return "MockVehicle_C";
    if (a.is_walker()) return "MockWalker_C";
    if (a.is_traffic_light()) return "MockTrafficLight_C";
    if (a.is_traffic_sign()) return "MockTrafficSign_C";
    if (a.type_id.rfind("sensor.", 0) == 0) return "MockSensor_C";
    return "MockActor_C";
  });
}

// Like LibCarla, whose Destroy() leaves the actor without an episode: a
// destroyed actor's state cannot be queried (IsActive / IsDormant are false).
rpc::ActorState Actor::GetActorState() const {
  return WithData([](mock::ActorData &) { return rpc::ActorState::Active; });
}

bool Actor::IsDormant() const { return false; }  // the mock has no dormant actors

bool Actor::IsActive() const {
  std::lock_guard<std::mutex> lock(_episode->mutex);
  return _episode->actors.count(_id) > 0;
}

void Actor::EnableConstantVelocity(const geom::Vector3D &vector) {
  WithData([&](mock::ActorData &a) { a.constant_velocity = vector; return 0; });
}

void Actor::DisableConstantVelocity() {
  WithData([](mock::ActorData &a) { a.constant_velocity.reset(); return 0; });
}

// The mock has no rotational dynamics: the point of application is ignored.
void Actor::AddImpulse(const geom::Vector3D &impulse, const geom::Vector3D &) { AddImpulse(impulse); }

void Actor::AddForce(const geom::Vector3D &force, const geom::Vector3D &) { AddForce(force); }

// No collision or rendering model: these only check that the actor exists.
void Actor::SetCollisions(bool) { WithData([](mock::ActorData &) { return 0; }); }

void Actor::ApplyTexture(const rpc::MaterialParameter &, const rpc::TextureColor &) {
  WithData([](mock::ActorData &) { return 0; });
}

void Actor::ApplyTexture(const rpc::MaterialParameter &, const rpc::TextureFloatColor &) {
  WithData([](mock::ActorData &) { return 0; });
}

geom::Transform Actor::GetComponentWorldTransform(const std::string &component_name) const {
  return WithData([&](mock::ActorData &a) {
    CheckComponent(a, component_name, "get_actor_component_world_transform");
    return a.transform;
  });
}

geom::Transform Actor::GetComponentRelativeTransform(const std::string &component_name) const {
  return WithData([&](mock::ActorData &a) {
    CheckComponent(a, component_name, "get_actor_component_relative_transform");
    return geom::Transform();
  });
}

std::vector<geom::Transform> Actor::GetBoneWorldTransforms() const {
  return WithData([](mock::ActorData &a) {
    return std::vector<geom::Transform>(MockBoneNames(a, "get_actor_bone_world_transforms").size(),
                                        a.transform);
  });
}

std::vector<geom::Transform> Actor::GetBoneRelativeTransforms() const {
  return WithData([](mock::ActorData &a) {
    return std::vector<geom::Transform>(
        MockBoneNames(a, "get_actor_bone_relative_transforms").size(), geom::Transform());
  });
}

std::vector<std::string> Actor::GetComponentNames() const {
  return WithData([](mock::ActorData &a) { return MockComponentNames(a); });
}

std::vector<std::string> Actor::GetBoneNames() const {
  return WithData([](mock::ActorData &a) { return MockBoneNames(a, "get_actor_bone_names"); });
}

std::vector<geom::Transform> Actor::GetSocketWorldTransforms() const {
  return WithData([](mock::ActorData &a) {
    return std::vector<geom::Transform>(MockSocketNames(a).size(), a.transform);
  });
}

std::vector<geom::Transform> Actor::GetSocketRelativeTransforms() const {
  return WithData([](mock::ActorData &a) {
    return std::vector<geom::Transform>(MockSocketNames(a).size(), geom::Transform());
  });
}

std::vector<std::string> Actor::GetSocketNames() const {
  return WithData([](mock::ActorData &a) { return MockSocketNames(a); });
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
void TrafficManager::SetOSMMode(const bool) {}
void TrafficManager::SetRespawnDormantVehicles(const bool) {}
void TrafficManager::SetBoundariesRespawnDormantVehicles(const float, const float) {}
void TrafficManager::SetHybridPhysicsRadius(const float) {}
void TrafficManager::SetGlobalLaneOffset(float const) {}
void TrafficManager::SetCollisionDetection(const ActorPtr &, const ActorPtr &, const bool) {}
void TrafficManager::SetLargeVehicleWideTurn(const ActorPtr &, const bool) {}
void TrafficManager::SetGlobalLargeVehicleWideTurn(const bool) {}

void TrafficManager::SetCustomPath(const ActorPtr &, const Path, const bool) {}

void TrafficManager::SetImportedRoute(const ActorPtr &actor, const Route route, const bool) {
  std::lock_guard<std::mutex> lock(_episode->mutex);
  _episode->tm_routes[actor->GetId()] = route;
}

void TrafficManager::ShutDown() {
  std::lock_guard<std::mutex> lock(_episode->mutex);
  _episode->tm_shut_down[_port] = true;
}

ActionBuffer TrafficManager::GetActionBuffer(const ActorId &actor_id) {
  std::lock_guard<std::mutex> lock(_episode->mutex);
  if (_episode->tm_shut_down[_port]) return {};
  auto it = _episode->actors.find(actor_id);
  // As ue5-dev: an empty buffer for a vehicle the Traffic Manager does not
  // drive (CARLA 0.10.0 throws std::out_of_range instead).
  if (it == _episode->actors.end() || !it->second.autopilot) return {};
  std::vector<uint8_t> route{static_cast<uint8_t>(RoadOption::LaneFollow)};
  auto r = _episode->tm_routes.find(actor_id);
  if (r != _episode->tm_routes.end() && !r->second.empty()) route = r->second;
  const geom::Location here = it->second.transform.location;
  const auto start = client::Map().GetWaypoint(here);
  ActionBuffer actions;
  for (size_t i = 0; i < route.size(); ++i) {
    const auto next = start->GetNext(10.0 * static_cast<double>(i + 1));
    actions.emplace_back(static_cast<RoadOption>(route[i]), next.empty() ? start : next.front());
  }
  return actions;
}

Action TrafficManager::GetNextAction(const ActorId &actor_id) {
  const ActionBuffer actions = GetActionBuffer(actor_id);
  if (actions.empty()) return Action{RoadOption::Void, nullptr};
  for (const auto &action : actions)
    if (action.first != RoadOption::LaneFollow) return action;
  return actions.back();
}

}  // namespace traffic_manager

// ---------------------------------------------------------------------------
// Issue #22: geo projections and file paths

namespace geom {

namespace {

// The mock's projections: an equirectangular approximation around each
// projection's origin (not the real projection formulas).
struct Origin {
  double lat_0, lon_0, x_0, y_0, a;
};

Origin OriginOf(const ProjectionParams &params) {
  return std::visit(
      [](const auto &p) -> Origin {
        using T = std::decay_t<decltype(p)>;
        if constexpr (std::is_same_v<T, TransverseMercatorParams> ||
                      std::is_same_v<T, LambertConformalConicParams>) {
          return {p.lat_0, p.lon_0, p.x_0, p.y_0, p.ellps.a};
        } else if constexpr (std::is_same_v<T, UniversalTransverseMercatorParams>) {
          return {0.0, p.zone * 6.0 - 183.0, 0.0, 0.0, p.ellps.a};
        } else {
          return {0.0, 0.0, 0.0, 0.0, p.ellps.a};
        }
      },
      params);
}

constexpr double kDegrees = 180.0 / 3.14159265358979323846;

}  // namespace

GeoLocation GeoProjection::TransformToGeoLocation(const Location &location) const {
  const Origin o = OriginOf(params);
  // Like ue5-dev's projections: y is northing as given, the altitude is z.
  const double east = location.x - o.x_0;
  const double north = location.y - o.y_0;
  return GeoLocation(o.lat_0 + north / o.a * kDegrees,
                     o.lon_0 + east / (o.a * std::cos(o.lat_0 / kDegrees)) * kDegrees,
                     location.z);
}

Location GeoProjection::GeoLocationToTransform(const GeoLocation &geolocation) const {
  const Origin o = OriginOf(params);
  const double north = (geolocation.latitude - o.lat_0) / kDegrees * o.a;
  const double east =
      (geolocation.longitude - o.lon_0) / kDegrees * o.a * std::cos(o.lat_0 / kDegrees);
  return Location(static_cast<float>(east + o.x_0), static_cast<float>(north + o.y_0),
                  static_cast<float>(geolocation.altitude));
}

}  // namespace geom

}  // namespace carla

extern "C" size_t tsc_mock_last_worker_threads(void) {
  return carla::client::g_last_worker_threads;
}
extern "C" uint16_t tsc_mock_last_map_layers(void) { return carla::client::g_last_map_layers; }
extern "C" size_t tsc_mock_gbuffer_subscriptions(void) {
  return carla::client::g_gbuffer_subscriptions;
}
