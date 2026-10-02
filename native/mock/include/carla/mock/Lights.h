// Mock mirror of LibCarla's client::LightManager, client::Light and
// client::LightState (issue #21). The mock episode has a few street and
// building lights; changes apply to the client-side state at once.
// Included at the end of Mock.h (needs sensor::data::Color).
#pragma once

#include <functional>
#include <memory>
#include <vector>

namespace carla {
namespace client {

using Color = sensor::data::Color;
using LightId = uint32_t;

struct LightState {
  using LightGroup = rpc::LightState::LightGroup;
  LightState() {}
  LightState(float intensity, Color color, LightGroup group, bool active)
      : _intensity(intensity), _color(color), _group(group), _active(active) {}
  float _intensity = 0.0f;
  Color _color;
  LightGroup _group = LightGroup::None;
  bool _active = false;
};

class LightManager;

class Light {
 public:
  Light() {}
  LightId GetId() const { return _id; }
  const geom::Location GetLocation() const { return _location; }

 private:
  friend class LightManager;
  Light(std::weak_ptr<LightManager> manager, geom::Location location, LightId id)
      : _light_manager(std::move(manager)), _location(location), _id(id) {}
  std::weak_ptr<LightManager> _light_manager;
  geom::Location _location;
  LightId _id = 0;
};

class LightManager : public std::enable_shared_from_this<LightManager> {
  using LightGroup = rpc::LightState::LightGroup;

 public:
  explicit LightManager(std::shared_ptr<mock::Episode> episode) : _episode(std::move(episode)) {}
  std::vector<Light> GetAllLights(LightGroup type = LightGroup::None) const;
  std::vector<Light> GetTurnedOnLights(LightGroup type = LightGroup::None) const;
  std::vector<Light> GetTurnedOffLights(LightGroup type = LightGroup::None) const;
  Color GetColor(LightId id) const;
  float GetIntensity(LightId id) const;
  LightState GetLightState(LightId id) const;
  LightGroup GetLightGroup(LightId id) const;
  bool IsActive(LightId id) const;
  void SetActive(LightId id, bool active);
  void SetColor(LightId id, Color color);
  void SetIntensity(LightId id, float intensity);
  void SetLightState(LightId id, const LightState &new_state);
  void SetLightGroup(LightId id, LightGroup group);
  void SetDayNightCycle(const bool active);

 private:
  template <typename Pred>
  std::vector<Light> Select(Pred &&pred) const;
  std::shared_ptr<mock::Episode> _episode;
};

namespace mock {
// The World::OnTick callbacks of the episode, to run after a step (world_extras.cpp).
std::function<void()> TickDelivery(const Episode *episode, WorldSnapshot snapshot);
}  // namespace mock

}  // namespace client
}  // namespace carla
