// Mock of the World queries of issue #21 (see carla/mock/WorldExtras.h and
// carla/mock/Lights.h). Per-episode state lives here, keyed by the mock
// episode; StepLocked (mock.cpp) runs the OnTick callbacks via TickDelivery.
#include "carla/mock/Mock.h"

#include <algorithm>
#include <cmath>
#include <map>

namespace carla {
namespace client {
namespace mock {

namespace {

// The mock road (mock.cpp): x in [0, 200], lanes between y = -1.75 and 5.25.
constexpr float kRoadLength = 200.0f;
constexpr float kRoadMinY = -1.75f;
constexpr float kRoadMaxY = 5.25f;
constexpr float kTrafficLightS = 100.0f;  // the mock traffic light's position along the road

struct LightRecord {
  geom::Location location;
  LightState state;
};

struct Extras {
  std::mutex mutex;
  size_t next_callback_id = 1;
  // OnTick callbacks. As in LibCarla, they belong to the client's Simulator
  // (here the mock server), not to an episode: they survive load_world.
  std::map<size_t, std::function<void(WorldSnapshot)>> callbacks;
  std::map<LightId, LightRecord> lights;
  std::shared_ptr<LightManager> light_manager;
  std::vector<rpc::EnvironmentObject> objects;
  float imu_gravity = 9.81f;
};

rpc::EnvironmentObject Object(uint64_t id, std::string name, rpc::CityObjectLabel type,
                              geom::Location at, geom::Vector3D extent) {
  rpc::EnvironmentObject o;
  o.id = id;
  o.name = std::move(name);
  o.type = type;
  o.transform = geom::Transform(at);
  o.bounding_box = geom::BoundingBox(at, extent);
  return o;
}

std::shared_ptr<Extras> NewExtras() {
  auto x = std::make_shared<Extras>();
  const Color warm(255, 200, 150);
  for (LightId id = 1; id <= 3; ++id) {  // street lights along the road, on
    x->lights[id] = LightRecord{geom::Location(50.0f * static_cast<float>(id), -4.0f, 6.0f),
                                LightState(1000.0f, warm, LightState::LightGroup::Street, true)};
  }
  x->lights[4] = LightRecord{
      geom::Location(60.0f, 12.0f, 3.0f),  // a building light, off
      LightState(500.0f, Color(255, 255, 255), LightState::LightGroup::Building, false)};
  x->objects = {
      Object(101, "SM_Building_1", rpc::CityObjectLabel::Buildings,
             geom::Location(60.0f, 20.0f, 5.0f), geom::Vector3D(10.0f, 5.0f, 5.0f)),
      Object(102, "SM_StreetLight_1", rpc::CityObjectLabel::Poles,
             geom::Location(50.0f, -4.0f, 3.0f), geom::Vector3D(0.2f, 0.2f, 3.0f)),
      Object(103, "SM_Tree_1", rpc::CityObjectLabel::Vegetation, geom::Location(80.0f, -8.0f, 2.0f),
             geom::Vector3D(1.0f, 1.0f, 2.0f)),
  };
  return x;
}

std::mutex g_mutex;
std::map<const Episode *, std::shared_ptr<Extras>> g_extras;

std::shared_ptr<Extras> ExtrasOf(const Episode *episode) {
  std::lock_guard<std::mutex> lock(g_mutex);
  auto &x = g_extras[episode];
  if (x == nullptr) x = NewExtras();
  return x;
}

rpc::CityObjectLabel GroundLabel(const geom::Location &p) {
  const bool on_road = p.x >= 0.0f && p.x <= kRoadLength && p.y >= kRoadMinY && p.y <= kRoadMaxY;
  return on_road ? rpc::CityObjectLabel::Roads : rpc::CityObjectLabel::Terrain;
}

bool MatchesTag(rpc::CityObjectLabel type, uint8_t tag) {
  return tag == static_cast<uint8_t>(rpc::CityObjectLabel::Any) ||
         static_cast<uint8_t>(type) == tag;
}

}  // namespace

std::function<void()> TickDelivery(const Episode *episode, WorldSnapshot snapshot) {
  auto x = ExtrasOf(episode);
  return [x, snapshot]() {
    std::vector<std::function<void(WorldSnapshot)>> callbacks;
    {
      std::lock_guard<std::mutex> lock(x->mutex);
      for (const auto &entry : x->callbacks) callbacks.push_back(entry.second);
    }
    for (auto &cb : callbacks) cb(snapshot);
  };
}

}  // namespace mock

// ---------------------------------------------------------------------------
// World

namespace {

template <typename T>
std::vector<SharedPtr<T>> ActorsOf(const World &world, const std::string &type_id) {
  std::vector<SharedPtr<T>> result;
  auto actors = world.GetActors();
  for (size_t i = 0; i < actors->size(); ++i) {
    auto a = actors->at(i);
    if (a->GetTypeId() != type_id) continue;
    if (auto t = std::dynamic_pointer_cast<T>(a)) result.push_back(std::move(t));
  }
  return result;
}

}  // namespace

SharedPtr<Actor> World::GetSpectator() const {
  auto spectators = ActorsOf<Actor>(*this, "spectator");
  return spectators.empty() ? nullptr : spectators.front();
}

SharedPtr<Actor> World::GetTrafficLightFromOpenDRIVE(const road::SignId &sign_id) const {
  for (auto &l : ActorsOf<TrafficLight>(*this, "traffic.traffic_light"))
    if (l->GetOpenDRIVEID() == sign_id) return l;
  return nullptr;
}

// As in LibCarla: the first "traffic.*" actor (a sign or a light) whose sign id
// is the landmark's.
SharedPtr<Actor> World::GetTrafficSign(const Landmark &landmark) const {
  auto actors = GetActors();
  for (size_t i = 0; i < actors->size(); ++i) {
    auto sign = std::dynamic_pointer_cast<TrafficSign>(actors->at(i));
    if (sign != nullptr && sign->GetSignId() == landmark.GetId()) return sign;
  }
  return nullptr;
}

SharedPtr<Actor> World::GetTrafficLight(const Landmark &landmark) const {
  return GetTrafficLightFromOpenDRIVE(landmark.GetId());
}

road::SignId TrafficSign::GetSignId() const {
  // The mock's traffic light is OpenDRIVE signal "1000"; its stop sign "1001".
  if (auto light = dynamic_cast<const TrafficLight *>(this)) return light->GetOpenDRIVEID();
  return "1001";
}

std::vector<SharedPtr<Actor>> World::GetTrafficLightsFromWaypoint(const Waypoint &waypoint,
                                                                  double distance) const {
  std::vector<SharedPtr<Actor>> result;
  const double s = waypoint.GetDistance();
  if (s <= mock::kTrafficLightS && mock::kTrafficLightS <= s + distance) {
    for (auto &l : ActorsOf<TrafficLight>(*this, "traffic.traffic_light")) result.push_back(l);
  }
  return result;
}

std::vector<SharedPtr<Actor>> World::GetTrafficLightsInJunction(const road::JuncId) const {
  return {};  // the mock map has no junctions
}

void World::ResetAllTrafficLights() {
  for (auto &l : ActorsOf<TrafficLight>(*this, "traffic.traffic_light")) {
    l->Freeze(false);
    l->ResetGroup();
  }
}

void World::FreezeAllTrafficLights(bool frozen) {
  for (auto &l : ActorsOf<TrafficLight>(*this, "traffic.traffic_light")) l->Freeze(frozen);
}

rpc::VehicleLightStateList World::GetVehiclesLightStates() const {
  rpc::VehicleLightStateList result;
  auto actors = GetActors();
  for (size_t i = 0; i < actors->size(); ++i) {
    if (auto v = std::dynamic_pointer_cast<Vehicle>(actors->at(i))) {
      result.emplace_back(v->GetId(),
                          static_cast<rpc::VehicleLightState::flag_type>(v->GetLightState()));
    }
  }
  return result;
}

std::vector<geom::BoundingBox> World::GetLevelBBs(uint8_t queried_tag) const {
  std::vector<geom::BoundingBox> result;
  for (const auto &o : GetEnvironmentObjects(queried_tag)) result.push_back(o.bounding_box);
  return result;
}

std::vector<rpc::EnvironmentObject> World::GetEnvironmentObjects(uint8_t queried_tag) const {
  auto x = mock::ExtrasOf(_episode.get());
  std::lock_guard<std::mutex> lock(x->mutex);
  std::vector<rpc::EnvironmentObject> result;
  for (const auto &o : x->objects)
    if (mock::MatchesTag(o.type, queried_tag)) result.push_back(o);
  return result;
}

// The mock keeps no state for what only the server's rendering uses.
void World::EnableEnvironmentObjects(std::vector<uint64_t>, bool) const {}

std::vector<std::string> World::GetNamesOfAllObjects() const {
  auto x = mock::ExtrasOf(_episode.get());
  std::lock_guard<std::mutex> lock(x->mutex);
  std::vector<std::string> names;
  for (const auto &o : x->objects) names.push_back(o.name);
  return names;
}

// The mock world's only geometry is the ground plane z = 0.
std::optional<rpc::LabelledPoint> World::ProjectPoint(geom::Location location,
                                                      geom::Vector3D direction,
                                                      float search_distance) const {
  if (!(direction.z < 0.0f) || location.z < 0.0f) return std::nullopt;
  const float t = location.z / -direction.z;
  const float length =
      std::sqrt(direction.x * direction.x + direction.y * direction.y + direction.z * direction.z);
  if (t * length > search_distance) return std::nullopt;
  const geom::Location hit(location.x + direction.x * t, location.y + direction.y * t, 0.0f);
  return rpc::LabelledPoint(hit, mock::GroundLabel(hit));
}

std::optional<rpc::LabelledPoint> World::GroundProjection(geom::Location location,
                                                          float search_distance) const {
  return ProjectPoint(location, geom::Vector3D(0.0f, 0.0f, -1.0f), search_distance);
}

std::vector<rpc::LabelledPoint> World::CastRay(geom::Location start_location,
                                               geom::Location end_location) const {
  const float z0 = start_location.z, z1 = end_location.z;
  if ((z0 > 0.0f && z1 > 0.0f) || (z0 < 0.0f && z1 < 0.0f) || z0 == z1) return {};
  const float t = z0 / (z0 - z1);
  const geom::Location hit(start_location.x + (end_location.x - start_location.x) * t,
                           start_location.y + (end_location.y - start_location.y) * t, 0.0f);
  return {rpc::LabelledPoint(hit, mock::GroundLabel(hit))};
}

void World::LoadLevelLayer(rpc::MapLayer) const {}
void World::UnloadLevelLayer(rpc::MapLayer) const {}
void World::SetPedestriansCrossFactor(float) {}
void World::SetPedestriansSeed(unsigned int) {}

float World::GetIMUSensorGravity() const {
  auto x = mock::ExtrasOf(_episode.get());
  std::lock_guard<std::mutex> lock(x->mutex);
  return x->imu_gravity;
}

void World::SetIMUSensorGravity(float gravity) {
  auto x = mock::ExtrasOf(_episode.get());
  std::lock_guard<std::mutex> lock(x->mutex);
  x->imu_gravity = gravity;
}

SharedPtr<LightManager> World::GetLightManager() const {
  auto x = mock::ExtrasOf(_episode.get());
  std::lock_guard<std::mutex> lock(x->mutex);
  if (x->light_manager == nullptr) x->light_manager = std::make_shared<LightManager>(_episode);
  return x->light_manager;
}

size_t World::OnTick(std::function<void(WorldSnapshot)> callback) {
  auto x = mock::ExtrasOf(_episode.get());
  std::lock_guard<std::mutex> lock(x->mutex);
  const size_t id = x->next_callback_id++;
  x->callbacks[id] = std::move(callback);
  return id;
}

void World::RemoveOnTick(size_t callback_id) {
  auto x = mock::ExtrasOf(_episode.get());
  std::lock_guard<std::mutex> lock(x->mutex);
  x->callbacks.erase(callback_id);
}

void World::ApplyColorTextureToObjects(const std::vector<std::string> &,
                                       const rpc::MaterialParameter &, const rpc::TextureColor &) {}
void World::ApplyFloatColorTextureToObjects(const std::vector<std::string> &,
                                            const rpc::MaterialParameter &,
                                            const rpc::TextureFloatColor &) {}
void World::ApplyTexturesToObjects(const std::vector<std::string> &, const rpc::TextureColor &,
                                   const rpc::TextureFloatColor &, const rpc::TextureFloatColor &,
                                   const rpc::TextureFloatColor &) {}

// ---------------------------------------------------------------------------
// Light manager: LibCarla answers an unknown id with a placeholder state.

template <typename Pred>
std::vector<Light> LightManager::Select(Pred &&pred) const {
  auto x = mock::ExtrasOf(_episode.get());
  std::lock_guard<std::mutex> lock(x->mutex);
  std::vector<Light> result;
  for (const auto &entry : x->lights) {
    if (pred(entry.second.state)) {
      result.push_back(Light(const_cast<LightManager *>(this)->weak_from_this(),
                             entry.second.location, entry.first));
    }
  }
  return result;
}

std::vector<Light> LightManager::GetAllLights(LightGroup type) const {
  return Select(
      [type](const LightState &s) { return type == LightGroup::None || s._group == type; });
}

std::vector<Light> LightManager::GetTurnedOnLights(LightGroup type) const {
  return Select([type](const LightState &s) {
    return (type == LightGroup::None || s._group == type) && s._active;
  });
}

std::vector<Light> LightManager::GetTurnedOffLights(LightGroup type) const {
  return Select([type](const LightState &s) {
    return (type == LightGroup::None || s._group == type) && !s._active;
  });
}

LightState LightManager::GetLightState(LightId id) const {
  auto x = mock::ExtrasOf(_episode.get());
  std::lock_guard<std::mutex> lock(x->mutex);
  auto it = x->lights.find(id);
  if (it == x->lights.end()) return LightState();
  LightState s = it->second.state;
  s._color.a = 255;  // the server keeps no alpha (rpc::Color is r, g, b)
  return s;
}

Color LightManager::GetColor(LightId id) const { return GetLightState(id)._color; }
float LightManager::GetIntensity(LightId id) const { return GetLightState(id)._intensity; }
LightManager::LightGroup LightManager::GetLightGroup(LightId id) const {
  return GetLightState(id)._group;
}
bool LightManager::IsActive(LightId id) const { return GetLightState(id)._active; }

namespace {

template <typename F>
void UpdateLight(const std::shared_ptr<mock::Episode> &episode, LightId id, F &&update) {
  auto x = mock::ExtrasOf(episode.get());
  std::lock_guard<std::mutex> lock(x->mutex);
  auto it = x->lights.find(id);
  if (it != x->lights.end()) update(it->second.state);
}

}  // namespace

void LightManager::SetActive(LightId id, bool active) {
  UpdateLight(_episode, id, [&](LightState &s) { s._active = active; });
}
void LightManager::SetColor(LightId id, Color color) {
  UpdateLight(_episode, id, [&](LightState &s) { s._color = color; });
}
void LightManager::SetIntensity(LightId id, float intensity) {
  UpdateLight(_episode, id, [&](LightState &s) { s._intensity = intensity; });
}
void LightManager::SetLightState(LightId id, const LightState &new_state) {
  UpdateLight(_episode, id, [&](LightState &s) { s = new_state; });
}
void LightManager::SetLightGroup(LightId id, LightGroup group) {
  UpdateLight(_episode, id, [&](LightState &s) { s._group = group; });
}

void LightManager::SetDayNightCycle(const bool) {}

}  // namespace client
}  // namespace carla
