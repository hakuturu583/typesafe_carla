// Mock measurement types added for issue #24. Each mirrors the LibCarla UE5
// class of the same name (the members the shim uses); the data is synthetic
// (see Episode::SenseLocked in mock.cpp).
#pragma once
#include "carla/mock/Mock.h"
#include "carla/road/element/LaneMarking.h"

#include <ostream>

namespace carla {
namespace sensor {
namespace data {

struct RadarDetection {
  float velocity;  // m/s
  float azimuth;   // rad
  float altitude;  // rad
  float depth;     // m
};

class RadarMeasurement : public SensorData {
 public:
  RadarMeasurement(size_t frame, double timestamp, const geom::Transform &t,
                   std::vector<RadarDetection> detections)
      : SensorData(frame, timestamp, t), _detections(std::move(detections)) {}
  size_t GetDetectionAmount() const { return _detections.size(); }
  const RadarDetection *data() const { return _detections.data(); }
  size_t size() const { return _detections.size(); }

 private:
  std::vector<RadarDetection> _detections;
};

#pragma pack(push, 1)
class SemanticLidarDetection {
 public:
  geom::Location point{};
  float cos_inc_angle{};
  uint32_t object_idx{};
  uint32_t object_tag{};

  SemanticLidarDetection() = default;
  SemanticLidarDetection(geom::Location p, float cos_th, uint32_t idx, uint32_t tag)
      : point(p), cos_inc_angle{cos_th}, object_idx{idx}, object_tag{tag} {}

  void WritePlyHeaderInfo(std::ostream &out) const {
    out << "property float32 x\n"
           "property float32 y\n"
           "property float32 z\n"
           "property float32 CosAngle\n"
           "property uint32 ObjIdx\n"
           "property uint32 ObjTag";
  }

  void WriteDetection(std::ostream &out) const {
    out << point.x << ' ' << point.y << ' ' << point.z << ' ' << cos_inc_angle << ' '
        << object_idx << ' ' << object_tag;
  }
};
#pragma pack(pop)

class SemanticLidarMeasurement : public SensorData {
 public:
  SemanticLidarMeasurement(size_t frame, double timestamp, const geom::Transform &t, float angle,
                           std::vector<uint32_t> per_channel,
                           std::vector<SemanticLidarDetection> points)
      : SensorData(frame, timestamp, t), _angle(angle), _per_channel(std::move(per_channel)),
        _points(std::move(points)) {}
  float GetHorizontalAngle() const { return _angle; }
  uint32_t GetChannelCount() const { return static_cast<uint32_t>(_per_channel.size()); }
  uint32_t GetPointCount(size_t channel) const { return _per_channel.at(channel); }
  const SemanticLidarDetection *data() const { return _points.data(); }
  size_t size() const { return _points.size(); }
  auto begin() const { return _points.begin(); }
  auto end() const { return _points.end(); }

 private:
  float _angle;
  std::vector<uint32_t> _per_channel;
  std::vector<SemanticLidarDetection> _points;
};

class LaneInvasionEvent : public SensorData {
 public:
  using LaneMarking = road::element::LaneMarking;

  LaneInvasionEvent(size_t frame, double timestamp, const geom::Transform &t, rpc::ActorId parent,
                    std::vector<LaneMarking> crossed)
      : SensorData(frame, timestamp, t), _parent(parent),
        _crossed_lane_markings(std::move(crossed)) {}

  // LibCarla's LaneInvasionSensor is client-side: its events never get an
  // episode, so GetActor() always throws there (CARLA 0.10.0 and ue5-dev).
  // The mock does the same.
  SharedPtr<client::Actor> GetActor() const {
    throw std::runtime_error(
        "trying to operate on a destroyed actor; an actor's function was called, but the actor "
        "is already destroyed.");
  }

  const std::vector<LaneMarking> &GetCrossedLaneMarkings() const { return _crossed_lane_markings; }

 private:
  rpc::ActorId _parent;  // kept as in LibCarla, which cannot resolve it either
  std::vector<LaneMarking> _crossed_lane_markings;
};

class ObstacleDetectionEvent : public SensorData {
 public:
  ObstacleDetectionEvent(size_t frame, double timestamp, const geom::Transform &t,
                         SharedPtr<client::Actor> self, SharedPtr<client::Actor> other,
                         float distance)
      : SensorData(frame, timestamp, t), _self(std::move(self)), _other(std::move(other)),
        _distance(distance) {}
  SharedPtr<client::Actor> GetActor() const { return _self; }
  SharedPtr<client::Actor> GetOtherActor() const { return _other; }
  float GetDistance() const { return _distance; }

 private:
  SharedPtr<client::Actor> _self, _other;
  float _distance;
};

#pragma pack(push, 1)
struct DVSEvent {
  DVSEvent() = default;
  DVSEvent(std::uint16_t in_x, std::uint16_t in_y, std::int64_t in_t, bool in_pol)
      : x(in_x), y(in_y), t(in_t), pol(in_pol) {}
  std::uint16_t x;
  std::uint16_t y;
  std::int64_t t;
  bool pol;
};
#pragma pack(pop)

class DVSEventArray : public SensorData {
 public:
  DVSEventArray(size_t frame, double timestamp, const geom::Transform &t, uint32_t width,
                uint32_t height, float fov, std::vector<DVSEvent> events)
      : SensorData(frame, timestamp, t), _width(width), _height(height), _fov(fov),
        _events(std::move(events)) {}
  uint32_t GetWidth() const { return _width; }
  uint32_t GetHeight() const { return _height; }
  float GetFOVAngle() const { return _fov; }
  const DVSEvent *data() const { return _events.data(); }
  size_t size() const { return _events.size(); }

 private:
  uint32_t _width, _height;
  float _fov;
  std::vector<DVSEvent> _events;
};

}  // namespace data
}  // namespace sensor
}  // namespace carla
