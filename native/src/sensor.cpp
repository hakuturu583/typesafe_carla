// Sensors: LibCarla callbacks feed a per-handle queue that the Codon side
// polls or dispatches to callbacks on the program's thread (design sections
// 15-16). Measurement buffers are exposed zero-copy.
#include "internal.hpp"
#include "sensor_image.hpp"

using namespace tsc;
namespace data = carla::sensor::data;

namespace {

SensorQueue &queue_of(tsc_sensor_t *s) {
  auto &h = sensor_handle(s);
  if (h.queue == nullptr) fail(TSC_ERROR, "sensor is not listening; call listen() first");
  return *h.queue;
}

// A new queue that listen(callback) feeds. The callback runs on LibCarla
// threads: it only touches the queue, which it shares, so late data is safe.
template <typename Listen>
std::shared_ptr<SensorQueue> listen_into_queue(size_t capacity, Listen &&listen) {
  auto queue = std::make_shared<SensorQueue>(capacity);
  listen([queue](carla::SharedPtr<carla::sensor::SensorData> d) { queue->push(std::move(d)); });
  return queue;
}

// The oldest queued item as a new handle, or nullptr (also for no queue).
tsc_sensor_data *pop_handle(SensorQueue *queue) {
  auto item = queue == nullptr ? nullptr : queue->pop();
  return item == nullptr ? nullptr : new tsc_sensor_data(std::move(item));
}

SensorQueue *gbuffer_queue_of(tsc_sensor_t *s, uint32_t gbuffer_id) {
  return sensor_handle(s).gbuffer_queues[check_gbuffer_id(gbuffer_id)].get();
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
  return TSC_SENSOR_DATA_OTHER;
}

constexpr const char *kPointCloud = "a LiDAR or semantic LiDAR measurement";

uint32_t id_or_zero(const carla::SharedPtr<carla::client::Actor> &a) {
  return a == nullptr ? 0u : a->GetId();
}

}  // namespace

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

namespace {

// ServerSideSensor::ListenToGBuffer. A server that does not stream G-buffers
// (CARLA 0.10.0) rejects the get_gbuffer_token call; LibCarla's
// throw_exception rethrows that error by value as a plain std::exception
// ("std::exception"; the official Python module aborts on it). That one
// error gets a readable message, as new_map_from_opendrive does; any other
// error passes through unchanged.
void listen_to_gbuffer(carla::client::ServerSideSensor &s, uint32_t id,
                       carla::client::Sensor::CallbackFunctionType callback) {
  try {
    s.ListenToGBuffer(id, std::move(callback));
  } catch (const std::exception &e) {
    if (typeid(e) != typeid(std::exception)) throw;
    throw std::runtime_error(
        "the server rejected the G-buffer subscription (CARLA 0.10.0 servers do not stream "
        "G-buffers)");
  }
}

}  // namespace

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
    h.queue = listen_into_queue(queue_capacity, [&](auto cb) { s.Listen(std::move(cb)); });
  });
}

tsc_status_t tsc_sensor_stop(tsc_sensor_t *sensor) {
  return TSC_GUARD({
    // Idempotent (LibCarla logs a warning when stopping a stopped sensor).
    auto &s = sensor_of(sensor);
    if (s.IsListening()) s.Stop();
  });
}

// --- Issue #33: G-buffer textures --------------------------------------------------

tsc_status_t tsc_sensor_listen_to_gbuffer(tsc_sensor_t *sensor, uint32_t gbuffer_id,
                                          size_t queue_capacity) {
  return TSC_GUARD({
    auto &h = sensor_handle(sensor);
    auto &s = server_side_sensor_of(sensor);
    const uint32_t id = check_gbuffer_id(gbuffer_id);
    // LibCarla only logs a warning (and delivers nothing) for other sensors.
    if (s.GetTypeId() != "sensor.camera.rgb") {
      fail(TSC_INVALID_ARGUMENT, "G-buffer textures come from RGB cameras (sensor.camera.rgb), not '" +
                                     s.GetTypeId() + "'");
    }
    // As tsc_sensor_listen: never leave an orphaned subscription behind.
    if (s.IsListeningGBuffer(id)) s.StopGBuffer(id);
    h.gbuffer_queues[id] =
        listen_into_queue(queue_capacity, [&](auto cb) { listen_to_gbuffer(s, id, std::move(cb)); });
  });
}

tsc_status_t tsc_sensor_gbuffer_pending_count(tsc_sensor_t *sensor, uint32_t gbuffer_id,
                                              size_t *out) {
  return TSC_GUARD({
    require_ptr(out, "out");
    const SensorQueue *queue = gbuffer_queue_of(sensor, gbuffer_id);
    *out = queue == nullptr ? 0 : queue->size();
  });
}

tsc_status_t tsc_sensor_gbuffer_poll(tsc_sensor_t *sensor, uint32_t gbuffer_id,
                                     tsc_sensor_data_t **out) {
  return new_handle(__func__, out, [&] { return pop_handle(gbuffer_queue_of(sensor, gbuffer_id)); });
}

tsc_status_t tsc_sensor_dropped_count(tsc_sensor_t *sensor, uint64_t *out) {
  return TSC_GUARD({ *require_ptr(out, "out") = queue_of(sensor).dropped(); });
}

tsc_status_t tsc_sensor_pending_count(tsc_sensor_t *sensor, size_t *out) {
  return TSC_GUARD({ *require_ptr(out, "out") = queue_of(sensor).size(); });
}

tsc_status_t tsc_sensor_poll(tsc_sensor_t *sensor, tsc_sensor_data_t **out) {
  return new_handle(__func__, out, [&] { return pop_handle(&queue_of(sensor)); });
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

}  // extern "C"
