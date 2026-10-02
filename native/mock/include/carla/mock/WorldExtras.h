// Mock mirror of the LibCarla types behind the World queries of issue #21:
// road::Map's junction lookup, CityObjectLabel, LabelledPoint,
// EnvironmentObject, VehicleLightStateList and rpc::LightState (the texture
// types are in Mock.h, issue #19).
// Included by Mock.h after its rpc namespace (signatures as in ue5-dev).
#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace carla {

namespace sensor {
namespace data {
struct Color;
}
}  // namespace sensor

namespace road {

using JuncId = int32_t;

class Junction {};

// The mock map has no junctions (see client::Map).
class Map {
 public:
  const Junction *GetJunction(JuncId) const { return nullptr; }
};

}  // namespace road

namespace rpc {

enum class CityObjectLabel : uint8_t {
  None = 0u,
  Roads = 1u,
  Sidewalks = 2u,
  Buildings = 3u,
  Walls = 4u,
  Fences = 5u,
  Poles = 6u,
  TrafficLight = 7u,
  TrafficSigns = 8u,
  Vegetation = 9u,
  Terrain = 10u,
  Sky = 11u,
  Pedestrians = 12u,
  Rider = 13u,
  Car = 14u,
  Truck = 15u,
  Bus = 16u,
  Train = 17u,
  Motorcycle = 18u,
  Bicycle = 19u,
  Static = 20u,
  Dynamic = 21u,
  Other = 22u,
  Water = 23u,
  RoadLines = 24u,
  Ground = 25u,
  Bridge = 26u,
  RailTrack = 27u,
  GuardRail = 28u,
  Rock = 29u,
  Any = 0xFF
};

struct LabelledPoint {
  LabelledPoint() {}
  LabelledPoint(geom::Location location, CityObjectLabel label)
      : _location(location), _label(label) {}
  geom::Location _location;
  CityObjectLabel _label = CityObjectLabel::None;
};

struct EnvironmentObject {
  geom::Transform transform;
  geom::BoundingBox bounding_box;
  uint64_t id = 0;
  std::string name;
  CityObjectLabel type = CityObjectLabel::None;
};

using VehicleLightStateList = std::vector<std::pair<ActorId, VehicleLightState::flag_type>>;

using LightId = uint32_t;

class LightState {
 public:
  using flag_type = uint8_t;
  enum class LightGroup : flag_type { None = 0, Vehicle, Street, Building, Other };
};

}  // namespace rpc
}  // namespace carla
