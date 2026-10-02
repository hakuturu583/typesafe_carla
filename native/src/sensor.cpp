// Sensors: LibCarla callbacks feed a bounded queue that Codon polls (design
// sections 15-16). Measurement buffers are exposed zero-copy.
#include "internal.hpp"

using namespace tsc;
namespace data = carla::sensor::data;

namespace tsc {

void SensorQueue::push(Item item) {
  {
    std::lock_guard<std::mutex> lock(_mutex);
    if (_capacity == 0) return;
    if (_items.size() >= _capacity) {
      _items.pop_front();
      ++_dropped;
    }
    _items.push_back(std::move(item));
  }
  _ready.notify_one();
}

SensorQueue::Item SensorQueue::pop() {
  std::lock_guard<std::mutex> lock(_mutex);
  if (_items.empty()) return nullptr;
  Item item = std::move(_items.front());
  _items.pop_front();
  return item;
}

SensorQueue::Item SensorQueue::wait(std::chrono::milliseconds timeout) {
  std::unique_lock<std::mutex> lock(_mutex);
  if (!_ready.wait_for(lock, timeout, [this] { return !_items.empty(); })) return nullptr;
  Item item = std::move(_items.front());
  _items.pop_front();
  return item;
}

uint64_t SensorQueue::dropped() const {
  std::lock_guard<std::mutex> lock(_mutex);
  return _dropped;
}

}  // namespace tsc

namespace {

tsc_sensor &sensor_handle(tsc_sensor_t *s) { return *check_handle(s, "sensor", TSC_KIND_SENSOR); }

carla::client::Sensor &sensor_of(tsc_sensor_t *s) {
  return static_cast<carla::client::Sensor &>(*sensor_handle(s).actor);
}

SensorQueue &queue_of(tsc_sensor_t *s) {
  auto &h = sensor_handle(s);
  if (h.queue == nullptr) fail(TSC_ERROR, "sensor is not listening; call listen() first");
  return *h.queue;
}

const carla::SharedPtr<carla::sensor::SensorData> &data_of(const tsc_sensor_data_t *d) {
  return check_handle(d, "data", TSC_KIND_SENSOR_DATA)->data;
}

template <typename T>
carla::SharedPtr<T> data_as(const tsc_sensor_data_t *d, const char *what) {
  auto typed = downcast<T>(data_of(d));
  if (typed == nullptr) fail(TSC_TYPE_ERROR, std::string("sensor data is not ") + what);
  return typed;
}

tsc_sensor_data_type_t type_of(const carla::SharedPtr<carla::sensor::SensorData> &d) {
  if (downcast<data::Image>(d)) return TSC_SENSOR_DATA_IMAGE;
  if (downcast<data::LidarMeasurement>(d)) return TSC_SENSOR_DATA_LIDAR;
  if (downcast<data::GnssMeasurement>(d)) return TSC_SENSOR_DATA_GNSS;
  if (downcast<data::IMUMeasurement>(d)) return TSC_SENSOR_DATA_IMU;
  if (downcast<data::CollisionEvent>(d)) return TSC_SENSOR_DATA_COLLISION;
  return TSC_SENSOR_DATA_OTHER;
}

uint32_t id_or_zero(const carla::SharedPtr<carla::client::Actor> &a) {
  return a == nullptr ? 0u : a->GetId();
}

}  // namespace

// The zero-copy views reinterpret LibCarla's element types.
static_assert(sizeof(data::Color) == 4, "Image pixels are 4 bytes (BGRA)");
static_assert(sizeof(data::LidarDetection) == 4 * sizeof(float),
              "LiDAR detections are {x, y, z, intensity} floats");

extern "C" {

tsc_status_t tsc_actor_as_sensor(tsc_actor_t *actor, tsc_sensor_t **out_sensor) {
  return TSC_GUARD({
    require_ptr(out_sensor, "out_sensor");
    *out_sensor = nullptr;
    tsc_actor *a = check_actor(actor);
    if (a->kind != TSC_KIND_SENSOR) {
      fail(TSC_TYPE_ERROR, "actor " + std::to_string(a->actor->GetId()) + " (" +
                               a->actor->GetTypeId() + ") is not a sensor");
    }
    tsc_handle_retain(a);
    *out_sensor = static_cast<tsc_sensor *>(a);
  });
}

tsc_status_t tsc_sensor_listen(tsc_sensor_t *sensor, size_t queue_capacity) {
  return TSC_GUARD({
    if (queue_capacity == 0) fail(TSC_INVALID_ARGUMENT, "queue_capacity must be at least 1");
    auto &h = sensor_handle(sensor);
    auto queue = std::make_shared<SensorQueue>(queue_capacity);
    // The callback runs on LibCarla threads: it only touches the queue.
    sensor_of(sensor).Listen([queue](carla::SharedPtr<carla::sensor::SensorData> d) {
      queue->push(std::move(d));
    });
    h.queue = std::move(queue);
  });
}

tsc_status_t tsc_sensor_stop(tsc_sensor_t *sensor) {
  return TSC_GUARD({ sensor_of(sensor).Stop(); });
}

tsc_status_t tsc_sensor_is_listening(tsc_sensor_t *sensor, int32_t *out) {
  return TSC_GUARD({ *require_ptr(out, "out") = sensor_of(sensor).IsListening() ? 1 : 0; });
}

tsc_status_t tsc_sensor_dropped_count(tsc_sensor_t *sensor, uint64_t *out) {
  return TSC_GUARD({ *require_ptr(out, "out") = queue_of(sensor).dropped(); });
}

tsc_status_t tsc_sensor_poll(tsc_sensor_t *sensor, tsc_sensor_data_t **out) {
  return TSC_GUARD({
    require_ptr(out, "out");
    *out = nullptr;
    auto item = queue_of(sensor).pop();
    if (item != nullptr) *out = new tsc_sensor_data(std::move(item));
  });
}

tsc_status_t tsc_sensor_wait_for_data(tsc_sensor_t *sensor, double timeout_seconds,
                                      tsc_sensor_data_t **out) {
  return TSC_GUARD({
    require_ptr(out, "out");
    *out = nullptr;
    const auto timeout = std::chrono::milliseconds(seconds_to_duration(timeout_seconds).milliseconds());
    auto item = queue_of(sensor).wait(timeout);
    if (item == nullptr) {
      fail(TSC_TIMEOUT, "no sensor data within " + std::to_string(timeout_seconds) + " s");
    }
    *out = new tsc_sensor_data(std::move(item));
  });
}

tsc_status_t tsc_sensor_data_get_info(const tsc_sensor_data_t *d, tsc_sensor_data_info_t *out) {
  return TSC_GUARD({
    require_ptr(out, "out");
    const auto &sd = data_of(d);
    tsc_sensor_data_info_t info{};
    info.frame = sd->GetFrame();
    info.timestamp = sd->GetTimestamp();
    info.sensor_transform = from_carla(sd->GetSensorTransform());
    info.type = type_of(sd);
    *out = info;
  });
}

tsc_status_t tsc_sensor_data_as_image(const tsc_sensor_data_t *d, tsc_image_t *out) {
  return TSC_GUARD({
    require_ptr(out, "out");
    auto image = data_as<data::Image>(d, "an image");
    tsc_image_t r{};
    r.width = static_cast<uint32_t>(image->GetWidth());
    r.height = static_cast<uint32_t>(image->GetHeight());
    r.fov = image->GetFOVAngle();
    r.data = reinterpret_cast<const uint8_t *>(image->data());
    r.size = image->size() * sizeof(data::Color);
    *out = r;
  });
}

tsc_status_t tsc_sensor_data_as_lidar(const tsc_sensor_data_t *d, tsc_lidar_t *out) {
  return TSC_GUARD({
    require_ptr(out, "out");
    auto lidar = data_as<data::LidarMeasurement>(d, "a LiDAR measurement");
    tsc_lidar_t r{};
    r.channels = static_cast<uint32_t>(lidar->GetChannelCount());
    r.horizontal_angle = lidar->GetHorizontalAngle();
    r.points = reinterpret_cast<const float *>(lidar->data());
    r.point_count = lidar->size();
    *out = r;
  });
}

tsc_status_t tsc_lidar_channel_point_count(const tsc_sensor_data_t *d, uint32_t channel,
                                           uint32_t *out) {
  return TSC_GUARD({
    require_ptr(out, "out");
    auto lidar = data_as<data::LidarMeasurement>(d, "a LiDAR measurement");
    if (channel >= lidar->GetChannelCount()) {
      fail(TSC_NOT_FOUND, "channel " + std::to_string(channel) + " out of range for " +
                              std::to_string(lidar->GetChannelCount()) + " channels");
    }
    *out = static_cast<uint32_t>(lidar->GetPointCount(channel));
  });
}

tsc_status_t tsc_sensor_data_as_gnss(const tsc_sensor_data_t *d, tsc_gnss_t *out) {
  return TSC_GUARD({
    auto gnss = data_as<data::GnssMeasurement>(d, "a GNSS measurement");
    *require_ptr(out, "out") =
        tsc_gnss_t{gnss->GetLatitude(), gnss->GetLongitude(), gnss->GetAltitude()};
  });
}

tsc_status_t tsc_sensor_data_as_imu(const tsc_sensor_data_t *d, tsc_imu_t *out) {
  return TSC_GUARD({
    auto imu = data_as<data::IMUMeasurement>(d, "an IMU measurement");
    *require_ptr(out, "out") = tsc_imu_t{from_carla(imu->GetAccelerometer()),
                                         from_carla(imu->GetGyroscope()), imu->GetCompass()};
  });
}

tsc_status_t tsc_sensor_data_as_collision(const tsc_sensor_data_t *d, tsc_collision_t *out) {
  return TSC_GUARD({
    auto event = data_as<data::CollisionEvent>(d, "a collision event");
    *require_ptr(out, "out") = tsc_collision_t{id_or_zero(event->GetActor()),
                                               id_or_zero(event->GetOtherActor()),
                                               from_carla(event->GetNormalImpulse())};
  });
}

}  // extern "C"
