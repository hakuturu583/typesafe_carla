// Sensors: LibCarla callbacks feed a per-handle queue that the Codon side
// polls or dispatches to callbacks on the program's thread (design sections
// 15-16). Measurement buffers are exposed zero-copy.
#include "internal.hpp"
#include "sensor_image.hpp"

#include <span>

using namespace tsc;
namespace data = carla::sensor::data;

namespace {

SensorQueue &queue_of(tsc_sensor_t *s) {
  auto &h = sensor_handle(s);
  if (h.queue == nullptr) fail(TSC_ERROR, "sensor is not listening; call listen() first");
  return *h.queue;
}

// Calls f with the measurement as the first of Ts that it is, or fails with
// TSC_TYPE_ERROR ("sensor data is not <what>").
template <typename T, typename... Ts, typename F>
auto visit_as(const carla::sensor::SensorData &sd, const char *what, F &&f) {
  if (auto typed = dynamic_cast<const T *>(&sd)) return f(*typed);
  if constexpr (sizeof...(Ts) == 0) {
    fail(TSC_TYPE_ERROR, std::string("sensor data is not ") + what);
  } else {
    return visit_as<Ts...>(sd, what, std::forward<F>(f));
  }
}

template <typename T, typename... Ts, typename F>
auto visit_as(const tsc_sensor_data_t *d, const char *what, F &&f) {
  return visit_as<T, Ts...>(sensor_data_of(d), what, std::forward<F>(f));
}

tsc_sensor_data_type_t type_of(const carla::sensor::SensorData &d) {
  if (dynamic_cast<const data::Image *>(&d)) return TSC_SENSOR_DATA_IMAGE;
  if (dynamic_cast<const data::LidarMeasurement *>(&d)) return TSC_SENSOR_DATA_LIDAR;
  if (dynamic_cast<const data::GnssMeasurement *>(&d)) return TSC_SENSOR_DATA_GNSS;
  if (dynamic_cast<const data::IMUMeasurement *>(&d)) return TSC_SENSOR_DATA_IMU;
  if (dynamic_cast<const data::CollisionEvent *>(&d)) return TSC_SENSOR_DATA_COLLISION;
  if (dynamic_cast<const data::RadarMeasurement *>(&d)) return TSC_SENSOR_DATA_RADAR;
  if (dynamic_cast<const data::SemanticLidarMeasurement *>(&d)) return TSC_SENSOR_DATA_SEMANTIC_LIDAR;
  if (dynamic_cast<const data::LaneInvasionEvent *>(&d)) return TSC_SENSOR_DATA_LANE_INVASION;
  if (dynamic_cast<const data::ObstacleDetectionEvent *>(&d)) return TSC_SENSOR_DATA_OBSTACLE;
  if (dynamic_cast<const data::DVSEventArray *>(&d)) return TSC_SENSOR_DATA_DVS;
  if (dynamic_cast<const data::OpticalFlowImage *>(&d)) return TSC_SENSOR_DATA_OPTICAL_FLOW;
#ifdef TSC_HAS_V2X
  if (dynamic_cast<const data::CAMEvent *>(&d)) return TSC_SENSOR_DATA_CAM;
  if (dynamic_cast<const data::CustomV2XEvent *>(&d)) return TSC_SENSOR_DATA_CUSTOM_V2X;
#endif
  return TSC_SENSOR_DATA_OTHER;
}

constexpr const char *kPointCloud = "a LiDAR or semantic LiDAR measurement";

// ---------------------------------------------------------------------------
// Issue #42: V2X measurements (LibCarla ue5-dev). Without V2X, every entry
// point checks the handle and raises "... is not available in LibCarla
// <version>" instead (as geo.cpp without geo projections).

#ifdef TSC_HAS_V2X
constexpr const char *kCamEvent = "a CAM event";
constexpr const char *kCustomV2XEvent = "a custom V2X event";

const data::CAMData &cam_message(const tsc_sensor_data_t *d, size_t index) {
  return list_at(sensor_data_as<data::CAMEvent>(d, kCamEvent), index, kCamEvent);
}

tsc_its_header_t from_carla(const ITSContainer::ItsPduHeader_t &h) {
  return tsc_its_header_t{h.protocolVersion, h.messageID, h.stationID};
}

// The first `count` elements of a fixed ITS array (count clamped to it).
template <typename Array>
std::span<const typename Array::value_type> its_list(const Array &items, long count) {
  return {items.data(), std::min(items.size(), static_cast<size_t>(std::max(count, 0L)))};
}

// A CAM's two lists: empty unless their container is the one `present` names
// (the others are left as the server built them, possibly uninitialized).
std::span<const ITSContainer::ProtectedCommunicationZone_t> protected_zones(const CAM_t &cam) {
  const auto &hf = cam.cam.camParameters.highFrequencyContainer;
  if (hf.present != CAMContainer::HighFrequencyContainer_PR_rsuContainerHighFrequency) return {};
  const auto &zones = hf.rsuContainerHighFrequency.protectedCommunicationZonesRSU;
  return its_list(zones.data, zones.ProtectedCommunicationZoneCount);
}

std::span<const ITSContainer::PathPoint_t> path_history(const CAM_t &cam) {
  const auto &lf = cam.cam.camParameters.lowFrequencyContainer;
  if (lf.present != CAMContainer::LowFrequencyContainer_PR_basicVehicleContainerLowFrequency) {
    return {};
  }
  const auto &history = lf.basicVehicleContainerLowFrequency.pathHistory;
  return its_list(history.data, history.NumberOfPathPoint);
}

tsc_its_value_t its(long value, long confidence) { return tsc_its_value_t{value, confidence}; }

tsc_cam_basic_vehicle_hf_t from_carla(
    const CAMContainer::BasicVehicleContainerHighFrequency_t &b) {
  tsc_cam_basic_vehicle_hf_t r{};
  r.heading = its(b.heading.headingValue, b.heading.headingConfidence);
  r.speed = its(b.speed.speedValue, b.speed.speedConfidence);
  r.drive_direction = b.driveDirection;
  r.vehicle_length = its(b.vehicleLength.vehicleLengthValue,
                         b.vehicleLength.vehicleLengthConfidenceIndication);
  r.vehicle_width = b.vehicleWidth;
  r.longitudinal_acceleration =
      its(b.longitudinalAcceleration.longitudinalAccelerationValue,
          b.longitudinalAcceleration.longitudinalAccelerationConfidence);
  r.curvature = its(b.curvature.curvatureValue, b.curvature.curvatureConfidence);
  r.curvature_calculation_mode = b.curvatureCalculationMode;
  r.yaw_rate = its(b.yawRate.yawRateValue, b.yawRate.yawRateConfidence);
  // Optional fields: only read when present (the rest is zero).
  if (b.accelerationControlAvailable) {
    r.has_acceleration_control = 1;
    r.acceleration_control = b.accelerationControl;
  }
  if (b.lanePositionAvailable) {
    r.has_lane_position = 1;
    r.lane_position = b.lanePosition;
  }
  if (b.steeringWheelAngleAvailable) {
    r.has_steering_wheel_angle = 1;
    r.steering_wheel_angle = its(b.steeringWheelAngle.steeringWheelAngleValue,
                                 b.steeringWheelAngle.steeringWheelAngleConfidence);
  }
  if (b.lateralAccelerationAvailable) {
    r.has_lateral_acceleration = 1;
    r.lateral_acceleration = its(b.lateralAcceleration.lateralAccelerationValue,
                                 b.lateralAcceleration.lateralAccelerationConfidence);
  }
  if (b.verticalAccelerationAvailable) {
    r.has_vertical_acceleration = 1;
    r.vertical_acceleration = its(b.verticalAcceleration.verticalAccelerationValue,
                                  b.verticalAcceleration.verticalAccelerationConfidence);
  }
  if (b.performanceClassAvailable) {
    r.has_performance_class = 1;
    r.performance_class = b.performanceClass;
  }
  if (b.cenDsrcTollingZoneAvailable) {
    const auto &zone = b.cenDsrcTollingZone;
    r.has_cen_dsrc_tolling_zone = 1;
    r.cen_dsrc_tolling_zone_latitude = zone.protectedZoneLatitude;
    r.cen_dsrc_tolling_zone_longitude = zone.protectedZoneLongitude;
    if (zone.cenDsrcTollingZoneIDAvailable) {
      r.has_cen_dsrc_tolling_zone_id = 1;
      r.cen_dsrc_tolling_zone_id = zone.cenDsrcTollingZoneID;
    }
  }
  return r;
}

tsc_cam_message_t from_carla(const data::CAMData &m) {
  const CAM_t &cam = m.Message;
  const auto &params = cam.cam.camParameters;
  const auto &ref = params.basicContainer.referencePosition;
  tsc_cam_message_t r{};
  r.power = m.Power;
  r.header = from_carla(cam.header);
  r.generation_delta_time = cam.cam.generationDeltaTime;
  r.station_type = params.basicContainer.stationType;
  r.reference_position = tsc_its_reference_position_t{
      ref.latitude,
      ref.longitude,
      ref.positionConfidenceEllipse.semiMajorConfidence,
      ref.positionConfidenceEllipse.semiMinorConfidence,
      ref.positionConfidenceEllipse.semiMajorOrientation,
      its(ref.altitude.altitudeValue, ref.altitude.altitudeConfidence)};
  switch (params.highFrequencyContainer.present) {
    case CAMContainer::HighFrequencyContainer_PR_basicVehicleContainerHighFrequency:
      r.high_frequency_present = TSC_CAM_HF_BASIC_VEHICLE;
      r.basic_vehicle =
          from_carla(params.highFrequencyContainer.basicVehicleContainerHighFrequency);
      break;
    case CAMContainer::HighFrequencyContainer_PR_rsuContainerHighFrequency:
      r.high_frequency_present = TSC_CAM_HF_RSU;
      r.protected_zone_count = protected_zones(cam).size();
      break;
    default:
      r.high_frequency_present = TSC_CAM_HF_NONE;
  }
  if (params.lowFrequencyContainer.present ==
      CAMContainer::LowFrequencyContainer_PR_basicVehicleContainerLowFrequency) {
    const auto &low = params.lowFrequencyContainer.basicVehicleContainerLowFrequency;
    r.has_low_frequency = 1;
    r.vehicle_role = low.vehicleRole;
    r.exterior_lights = low.exteriorLights;
    r.path_point_count = path_history(cam).size();
  }
  return r;
}

tsc_custom_v2x_data_t from_carla(const data::CustomV2XData &m) {
  const carla::rpc::CustomV2XBytes &bytes = m.Message.data;
  tsc_custom_v2x_data_t r{};
  r.power = m.Power;
  r.header = from_carla(m.Message.header);
  r.data.data_size = std::min<uint32_t>(bytes.data_size, TSC_CUSTOM_V2X_MAX_DATA_SIZE);
  std::copy(bytes.bytes.begin(), bytes.bytes.end(), r.data.bytes);
  return r;
}
static_assert(sizeof(carla::rpc::CustomV2XBytes{}.bytes) == TSC_CUSTOM_V2X_MAX_DATA_SIZE,
              "rpc::CustomV2XBytes holds TSC_CUSTOM_V2X_MAX_DATA_SIZE bytes");
#else
[[noreturn]] void no_v2x(const tsc_sensor_data_t *d, const char *what) {
  sensor_data_of(d);
  unsupported(what);
}
#endif
uint32_t id_or_zero(const carla::SharedPtr<carla::client::Actor> &a) {
  return a == nullptr ? 0u : a->GetId();
}

}  // namespace

#ifdef TSC_HAS_V2X
// The elements of the CAM lists (declared in internal.hpp for copy_out).
tsc_its_protected_zone_t tsc::from_carla(const ITSContainer::ProtectedCommunicationZone_t &z) {
  tsc_its_protected_zone_t r{};
  r.protected_zone_type = z.protectedZoneType;
  r.has_expiry_time = z.expiryTimeAvailable ? 1 : 0;
  r.has_protected_zone_radius = z.protectedZoneRadiusAvailable ? 1 : 0;
  r.has_protected_zone_id = z.protectedZoneIDAvailable ? 1 : 0;
  r.expiry_time = z.expiryTimeAvailable ? z.expiryTime : 0;
  r.protected_zone_latitude = z.protectedZoneLatitude;
  r.protected_zone_longitude = z.protectedZoneLongitude;
  r.protected_zone_radius = z.protectedZoneRadiusAvailable ? z.protectedZoneRadius : 0;
  r.protected_zone_id = z.protectedZoneIDAvailable ? z.protectedZoneID : 0;
  return r;
}

tsc_its_path_point_t tsc::from_carla(const ITSContainer::PathPoint_t &p) {
  tsc_its_path_point_t r{};
  r.delta_latitude = p.pathPosition.deltaLatitude;
  r.delta_longitude = p.pathPosition.deltaLongitude;
  r.delta_altitude = p.pathPosition.deltaAltitude;
  r.has_path_delta_time = p.pathDeltaTimeAvailable ? 1 : 0;
  r.path_delta_time = p.pathDeltaTimeAvailable ? p.pathDeltaTime : 0;
  return r;
}
#endif

// The zero-copy views reinterpret LibCarla's element types.
static_assert(sizeof(data::Color) == 4, "Image pixels are 4 bytes (BGRA)");
static_assert(sizeof(data::LidarDetection) == 4 * sizeof(float),
              "LiDAR detections are {x, y, z, intensity} floats");
static_assert(sizeof(data::RadarDetection) == sizeof(tsc_radar_detection_t),
              "radar detections are {velocity, azimuth, altitude, depth} floats");
static_assert(sizeof(data::SemanticLidarDetection) == sizeof(tsc_semantic_lidar_detection_t),
              "semantic LiDAR detections are {x, y, z, cos_inc_angle, object_idx, object_tag}");
static_assert(sizeof(data::DVSEvent) == TSC_DVS_EVENT_SIZE, "DVS events are packed, 13 bytes");
static_assert(sizeof(data::OpticalFlowPixel) == 2 * sizeof(float), "optical flow pixels are {x, y}");

extern "C" {

tsc_status_t tsc_actor_as_sensor(tsc_actor_t *actor, tsc_sensor_t **out_sensor) {
  return TSC_GUARD({
    require_ptr(out_sensor, "out_sensor");
    *out_sensor = nullptr;
    *out_sensor = retain_as<tsc_sensor>(actor, TSC_KIND_SENSOR, "a sensor");
  });
}

tsc_status_t tsc_sensor_listen(tsc_sensor_t *sensor, size_t queue_capacity) {
  return TSC_GUARD({
    auto &h = sensor_handle(sensor);
    auto &s = sensor_of(sensor);
    // LibCarla does not replace an existing subscription: a second Listen()
    // would leave an orphaned stream that Stop() cannot reach. Stop first.
    if (s.IsListening()) s.Stop();
    auto queue = std::make_shared<SensorQueue>(queue_capacity);
    // The callback runs on LibCarla threads: it only touches the queue.
    s.Listen([queue](carla::SharedPtr<carla::sensor::SensorData> d) {
      queue->push(std::move(d));
    });
    h.queue = std::move(queue);
  });
}

tsc_status_t tsc_sensor_stop(tsc_sensor_t *sensor) {
  return TSC_GUARD({
    // Idempotent (LibCarla logs a warning when stopping a stopped sensor).
    auto &s = sensor_of(sensor);
    if (s.IsListening()) s.Stop();
  });
}

tsc_status_t tsc_sensor_dropped_count(tsc_sensor_t *sensor, uint64_t *out) {
  return TSC_GUARD({ *require_ptr(out, "out") = queue_of(sensor).dropped(); });
}

tsc_status_t tsc_sensor_pending_count(tsc_sensor_t *sensor, size_t *out) {
  return TSC_GUARD({ *require_ptr(out, "out") = queue_of(sensor).size(); });
}

tsc_status_t tsc_sensor_poll(tsc_sensor_t *sensor, tsc_sensor_data_t **out) {
  return new_handle(__func__, out, [&]() -> tsc_sensor_data * {
    auto item = queue_of(sensor).pop();
    return item == nullptr ? nullptr : new tsc_sensor_data(std::move(item));
  });
}

tsc_status_t tsc_sensor_wait_for_data(tsc_sensor_t *sensor, double timeout_seconds,
                                      tsc_sensor_data_t **out) {
  return new_handle(__func__, out, [&]() {
    const auto timeout = std::chrono::milliseconds(seconds_to_duration(timeout_seconds).milliseconds());
    auto item = queue_of(sensor).wait(timeout);
    if (item == nullptr) {
      fail(TSC_TIMEOUT, "no sensor data within " + std::to_string(timeout_seconds) + " s");
    }
    return new tsc_sensor_data(std::move(item));
  });
}

tsc_status_t tsc_sensor_data_get_info(const tsc_sensor_data_t *d, tsc_sensor_data_info_t *out) {
  return TSC_GUARD({
    require_ptr(out, "out");
    const auto &sd = sensor_data_of(d);
    tsc_sensor_data_info_t info{};
    info.frame = sd.GetFrame();
    info.timestamp = sd.GetTimestamp();
    info.sensor_transform = from_carla(sd.GetSensorTransform());
    info.type = type_of(sd);
    *out = info;
  });
}

tsc_status_t tsc_sensor_data_as_image(const tsc_sensor_data_t *d, tsc_image_t *out) {
  return TSC_GUARD({
    require_ptr(out, "out");
    const auto &image = sensor_data_as<data::Image>(d, "an image");
    tsc_image_t r{};
    r.width = static_cast<uint32_t>(image.GetWidth());
    r.height = static_cast<uint32_t>(image.GetHeight());
    r.fov = image.GetFOVAngle();
    r.data = reinterpret_cast<const uint8_t *>(image.data());
    r.size = image.size() * sizeof(data::Color);
    *out = r;
  });
}

tsc_status_t tsc_sensor_data_as_lidar(const tsc_sensor_data_t *d, tsc_lidar_t *out) {
  return TSC_GUARD({
    require_ptr(out, "out");
    const auto &lidar = sensor_data_as<data::LidarMeasurement>(d, "a LiDAR measurement");
    tsc_lidar_t r{};
    r.channels = static_cast<uint32_t>(lidar.GetChannelCount());
    r.horizontal_angle = lidar.GetHorizontalAngle();
    r.points = reinterpret_cast<const float *>(lidar.data());
    r.point_count = lidar.size();
    *out = r;
  });
}

tsc_status_t tsc_lidar_channel_point_count(const tsc_sensor_data_t *d, uint32_t channel,
                                           uint32_t *out) {
  return TSC_GUARD({
    require_ptr(out, "out");
    *out = visit_as<data::LidarMeasurement, data::SemanticLidarMeasurement>(
        d, kPointCloud, [&](const auto &lidar) {
          if (channel >= lidar.GetChannelCount()) {
            fail(TSC_NOT_FOUND, "channel " + std::to_string(channel) + " out of range for " +
                                    std::to_string(lidar.GetChannelCount()) + " channels");
          }
          return static_cast<uint32_t>(lidar.GetPointCount(channel));
        });
  });
}

tsc_status_t tsc_sensor_data_as_gnss(const tsc_sensor_data_t *d, tsc_gnss_t *out) {
  return TSC_GUARD({
    const auto &gnss = sensor_data_as<data::GnssMeasurement>(d, "a GNSS measurement");
    *require_ptr(out, "out") =
        tsc_gnss_t{gnss.GetLatitude(), gnss.GetLongitude(), gnss.GetAltitude()};
  });
}

tsc_status_t tsc_sensor_data_as_imu(const tsc_sensor_data_t *d, tsc_imu_t *out) {
  return TSC_GUARD({
    const auto &imu = sensor_data_as<data::IMUMeasurement>(d, "an IMU measurement");
    *require_ptr(out, "out") = tsc_imu_t{from_carla(imu.GetAccelerometer()),
                                         from_carla(imu.GetGyroscope()), imu.GetCompass()};
  });
}

tsc_status_t tsc_sensor_data_as_collision(const tsc_sensor_data_t *d, tsc_collision_t *out) {
  return TSC_GUARD({
    const auto &event = sensor_data_as<data::CollisionEvent>(d, "a collision event");
    *require_ptr(out, "out") = tsc_collision_t{id_or_zero(event.GetActor()),
                                               id_or_zero(event.GetOtherActor()),
                                               from_carla(event.GetNormalImpulse())};
  });
}

// ---------------------------------------------------------------------------
// Issue #24: image conversion and files, more measurement types. The event
// actors and lane markings are generated (bindings/*_event.yaml).

tsc_status_t tsc_image_convert(tsc_sensor_data_t *d, int32_t color_converter) {
  return TSC_GUARD({
    // The handle owns the (non-const) measurement: convert changes it in place.
    auto &image = const_cast<data::Image &>(sensor_data_as<data::Image>(d, "an image"));
    tsc::image::convert_bgra(reinterpret_cast<uint8_t *>(image.data()), image.size(),
                             color_converter);
  });
}

tsc_status_t tsc_image_save_to_disk(const tsc_sensor_data_t *d, const char *path, size_t path_len,
                                    int32_t color_converter, tsc_string_t *out_path) {
  return TSC_GUARD({
    require_ptr(out_path, "out_path");
    const auto &image = sensor_data_as<data::Image>(d, "an image");
    std::string file = to_string(path, path_len, "path");
    // Checked before ValidateFilePath creates directories.
    tsc::image::check_png(image.GetWidth(), image.GetHeight(), image.size(), color_converter);
    carla::FileSystem::ValidateFilePath(file, ".png");
    tsc::image::write_png(file, image.GetWidth(), image.GetHeight(),
                          reinterpret_cast<const uint8_t *>(image.data()), image.size(),
                          color_converter);
    string_assign(out_path, file);
  });
}

tsc_status_t tsc_point_cloud_save_to_disk(const tsc_sensor_data_t *d, const char *path,
                                          size_t path_len, tsc_string_t *out_path) {
  return TSC_GUARD({
    require_ptr(out_path, "out_path");
    std::string file = to_string(path, path_len, "path");
    // PointCloudIO writes the header from the first point: an empty cloud
    // would read past the end in LibCarla.
    string_assign(out_path, visit_as<data::LidarMeasurement, data::SemanticLidarMeasurement>(
                                d, kPointCloud, [&](const auto &cloud) {
                                  if (cloud.size() == 0) {
                                    fail(TSC_ERROR, "cannot save a measurement without points");
                                  }
                                  return carla::pointcloud::PointCloudIO::SaveToDisk(
                                      std::move(file), cloud.begin(), cloud.end());
                                }));
  });
}

tsc_status_t tsc_sensor_data_as_radar(const tsc_sensor_data_t *d, tsc_radar_t *out) {
  return TSC_GUARD({
    require_ptr(out, "out");
    const auto &radar = sensor_data_as<data::RadarMeasurement>(d, "a radar measurement");
    *out = tsc_radar_t{reinterpret_cast<const tsc_radar_detection_t *>(radar.data()),
                       radar.GetDetectionAmount()};
  });
}

tsc_status_t tsc_sensor_data_as_semantic_lidar(const tsc_sensor_data_t *d,
                                               tsc_semantic_lidar_t *out) {
  return TSC_GUARD({
    require_ptr(out, "out");
    const auto &lidar =
        sensor_data_as<data::SemanticLidarMeasurement>(d, "a semantic LiDAR measurement");
    *out = tsc_semantic_lidar_t{static_cast<uint32_t>(lidar.GetChannelCount()), 0,
                                lidar.GetHorizontalAngle(),
                                reinterpret_cast<const tsc_semantic_lidar_detection_t *>(lidar.data()),
                                lidar.size()};
  });
}

tsc_status_t tsc_sensor_data_as_dvs(const tsc_sensor_data_t *d, tsc_dvs_t *out) {
  return TSC_GUARD({
    require_ptr(out, "out");
    const auto &events = sensor_data_as<data::DVSEventArray>(d, "a DVS event array");
    *out = tsc_dvs_t{static_cast<uint32_t>(events.GetWidth()),
                     static_cast<uint32_t>(events.GetHeight()), events.GetFOVAngle(),
                     reinterpret_cast<const uint8_t *>(events.data()), events.size()};
  });
}

tsc_status_t tsc_sensor_data_as_optical_flow(const tsc_sensor_data_t *d, tsc_optical_flow_t *out) {
  return TSC_GUARD({
    require_ptr(out, "out");
    const auto &image = sensor_data_as<data::OpticalFlowImage>(d, "an optical flow image");
    *out = tsc_optical_flow_t{static_cast<uint32_t>(image.GetWidth()),
                              static_cast<uint32_t>(image.GetHeight()), image.GetFOVAngle(),
                              reinterpret_cast<const float *>(image.data()), image.size()};
  });
}

tsc_status_t tsc_optical_flow_color_coded(const tsc_sensor_data_t *d, uint8_t *out,
                                          size_t capacity) {
  return TSC_GUARD({
    const auto &image = sensor_data_as<data::OpticalFlowImage>(d, "an optical flow image");
    const size_t needed = 4 * image.size();
    require_array(out, needed, "out");
    if (capacity < needed) {
      fail(TSC_INVALID_ARGUMENT, "out holds " + std::to_string(capacity) + " bytes, " +
                                     std::to_string(needed) + " needed");
    }
    tsc::image::color_coded_flow(reinterpret_cast<const float *>(image.data()), image.size(), out);
  });
}

// ---------------------------------------------------------------------------
// Issue #42: V2X events. tsc_sensor_send is generated
// (bindings/server_side_sensor.yaml).

tsc_status_t tsc_cam_event_get_message_count(const tsc_sensor_data_t *d, size_t *out) {
  return TSC_GUARD({
    require_ptr(out, "out");
#ifdef TSC_HAS_V2X
    *out = sensor_data_as<data::CAMEvent>(d, kCamEvent).GetMessageCount();
#else
    no_v2x(d, "CAMEvent");
#endif
  });
}

tsc_status_t tsc_cam_event_get_message(const tsc_sensor_data_t *d, size_t index,
                                       tsc_cam_message_t *out) {
  return TSC_GUARD({
    require_ptr(out, "out");
#ifdef TSC_HAS_V2X
    *out = from_carla(cam_message(d, index));
#else
    no_v2x(d, "CAMEvent");
#endif
  });
}

tsc_status_t tsc_cam_event_get_protected_zones(const tsc_sensor_data_t *d, size_t index,
                                               tsc_its_protected_zone_t *out, size_t capacity,
                                               size_t *out_count) {
  return TSC_GUARD({
    require_ptr(out_count, "out_count");
#ifdef TSC_HAS_V2X
    copy_out(protected_zones(cam_message(d, index).Message), out, capacity, out_count);
#else
    no_v2x(d, "CAMEvent");
#endif
  });
}

tsc_status_t tsc_cam_event_get_path_history(const tsc_sensor_data_t *d, size_t index,
                                            tsc_its_path_point_t *out, size_t capacity,
                                            size_t *out_count) {
  return TSC_GUARD({
    require_ptr(out_count, "out_count");
#ifdef TSC_HAS_V2X
    copy_out(path_history(cam_message(d, index).Message), out, capacity, out_count);
#else
    no_v2x(d, "CAMEvent");
#endif
  });
}

tsc_status_t tsc_custom_v2x_event_get_message_count(const tsc_sensor_data_t *d, size_t *out) {
  return TSC_GUARD({
    require_ptr(out, "out");
#ifdef TSC_HAS_V2X
    *out = sensor_data_as<data::CustomV2XEvent>(d, kCustomV2XEvent).GetMessageCount();
#else
    no_v2x(d, "CustomV2XEvent");
#endif
  });
}

tsc_status_t tsc_custom_v2x_event_get_message(const tsc_sensor_data_t *d, size_t index,
                                              tsc_custom_v2x_data_t *out) {
  return TSC_GUARD({
    require_ptr(out, "out");
#ifdef TSC_HAS_V2X
    const auto &event = sensor_data_as<data::CustomV2XEvent>(d, kCustomV2XEvent);
    *out = from_carla(list_at(event, index, kCustomV2XEvent));
#else
    no_v2x(d, "CustomV2XEvent");
#endif
  });
}
}  // extern "C"
