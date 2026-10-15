// Sensor sources (ABI 5.3): a source installed by a plugin supplies the
// measurements and the ROS 2 output of the sensors it claims, in place of the
// CARLA server (docs/design.md, "Sensor sources"). Its measurements go into
// the same per-handle queues as LibCarla's, so listen(), poll(),
// wait_for_data() and the callback dispatcher work unchanged.
#include "internal.hpp"

#include <carla/sensor/data/SemanticLidarMeasurement.h>
#ifndef TSC_MOCK_LIBCARLA
#include <carla/Buffer.h>
#include <carla/sensor/Deserializer.h>
#include <carla/sensor/SensorRegistry.h>
#include <carla/sensor/s11n/ImageSerializer.h>
#include <carla/sensor/s11n/SensorHeaderSerializer.h>
#endif

#include <dlfcn.h>

#include <cstdlib>
#include <memory>
#include <mutex>
#include <set>

using namespace tsc;
namespace data = carla::sensor::data;
using MeasurementPtr = carla::SharedPtr<carla::sensor::SensorData>;

namespace {

using SensorWeak = carla::SharedPtr<carla::client::Sensor>::weak_type;

// A handle's stream fed by the source. The handle owns both the LibCarla
// sensor object and the queue, so a stream whose handle is gone has expired.
struct Stream {
  const tsc_sensor *handle;
  uint32_t actor_id;
  SensorWeak sensor;
  std::weak_ptr<SensorQueue> queue;

  bool alive() const { return !sensor.expired() && !queue.expired(); }
};

struct State {
  std::mutex mutex;
  std::optional<tsc_sensor_source_t> source;  // the installed one
  std::vector<Stream> streams;
  std::set<uint32_t> started;  // actors whose source stream was started
  std::set<std::string> loaded;  // plugin paths
};

// Leaked: plugins' threads may push while static destructors run at exit.
State &state() {
  static auto *s = new State;
  return *s;
}

thread_local bool tl_loading = false;  // inside a plugin's init function

void *get_proc(const char *name) {
  static void *self = [] {
    Dl_info info{};
    if (dladdr(reinterpret_cast<void *>(&tsc_abi_version), &info) == 0 ||
        info.dli_fname == nullptr) {
      return static_cast<void *>(nullptr);
    }
    return dlopen(info.dli_fname, RTLD_NOW | RTLD_NOLOAD);
  }();
  if (self == nullptr || name == nullptr || std::strncmp(name, "tsc_", 4) != 0) return nullptr;
  return dlsym(self, name);
}

using PluginInit = int32_t (*)(uint32_t, void *(*)(const char *), const char **);

void load_plugin(const std::string &path) {
  {
    std::lock_guard<std::mutex> lock(state().mutex);
    if (state().loaded.count(path) != 0) return;
  }
  // Never closed: the plugin may run threads and owns the installed callbacks.
  void *library = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
  if (library == nullptr) {
    const char *error = dlerror();
    fail(TSC_ERROR, "cannot load sensor plugin '" + path + "': " + (error ? error : "dlopen failed"));
  }
  auto init = reinterpret_cast<PluginInit>(dlsym(library, "tsc_sensor_plugin_init"));
  if (init == nullptr) {
    fail(TSC_ERROR, "sensor plugin '" + path + "' does not export tsc_sensor_plugin_init");
  }
  const char *error = nullptr;
  tl_loading = true;
  const int32_t status = init(TSC_ABI_VERSION, &get_proc, &error);
  tl_loading = false;
  if (status != TSC_OK) {
    fail(TSC_ERROR, "sensor plugin '" + path + "' failed to initialize: " +
                        (error != nullptr ? error : "no message"));
  }
  std::lock_guard<std::mutex> lock(state().mutex);
  state().loaded.insert(path);
}

// Loads TYPESAFE_CARLA_SENSOR_PLUGIN once; a failure is reported every time.
void load_from_environment() {
  if (tl_loading) return;
  static std::once_flag once;
  static std::string error;
  std::call_once(once, [] {
    const char *path = std::getenv("TYPESAFE_CARLA_SENSOR_PLUGIN");
    if (path == nullptr || *path == '\0') return;
    try {
      load_plugin(path);
    } catch (const std::exception &e) {
      error = std::string(e.what()) + " (TYPESAFE_CARLA_SENSOR_PLUGIN)";
    }
  });
  if (!error.empty()) fail(TSC_ERROR, error);
}

// A copy of the installed source, or nullopt.
std::optional<tsc_sensor_source_t> installed_source() {
  std::lock_guard<std::mutex> lock(state().mutex);
  return state().source;
}

// The same, loading TYPESAFE_CARLA_SENSOR_PLUGIN first.
std::optional<tsc_sensor_source_t> current_source() {
  load_from_environment();
  return installed_source();
}

void check_callback(const tsc_sensor_source_t &source, int32_t status, const char *what) {
  if (status == TSC_OK) return;
  const char *message = source.last_error != nullptr ? source.last_error(source.user) : nullptr;
  fail(TSC_ERROR, std::string("sensor source ") + what + ": " +
                      (message != nullptr && *message != '\0' ? message : "failed"));
}

// Releases a handle made for a callback when it goes out of scope.
struct TempHandle {
  tsc_handle *handle;
  explicit TempHandle(tsc_handle *h) : handle(h) {}
  ~TempHandle() {
    if (handle != nullptr) tsc_handle_release(handle);
  }
  TempHandle(const TempHandle &) = delete;
  TempHandle &operator=(const TempHandle &) = delete;
};

bool claims(const tsc_sensor_source_t &source, tsc_actor *handle, const carla::client::Actor &a) {
  if (source.claims == nullptr) return false;
  const std::string type_id = a.GetTypeId();
  return source.claims(source.user, handle, a.GetId(), type_id.data(), type_id.size()) != 0;
}

// The source claiming `sensor` (through a temporary handle), or nullopt.
std::optional<tsc_sensor_source_t> claiming_source(carla::client::Sensor &sensor) {
  auto source = current_source();
  if (!source) return std::nullopt;
  TempHandle handle(make_actor_handle(sensor.shared_from_this()));
  if (!claims(*source, static_cast<tsc_actor *>(handle.handle), sensor)) return std::nullopt;
  return source;
}

// Drops the actor's streams; true if its source stream had been started.
bool forget_actor(uint32_t actor_id) {
  auto &s = state();
  std::lock_guard<std::mutex> lock(s.mutex);
  auto &streams = s.streams;
  streams.erase(std::remove_if(streams.begin(), streams.end(),
                               [&](const Stream &st) { return st.actor_id == actor_id; }),
                streams.end());
  return s.started.erase(actor_id) != 0;
}

// --- Measurements ------------------------------------------------------------

carla::geom::Transform transform_arg(const tsc_transform_t *t) {
  return to_carla(*require_ptr(t, "sensor_transform"));
}

void check_point_counts(const uint32_t *per_channel, uint32_t channels, size_t point_count) {
  require_array(per_channel, channels, "points_per_channel");
  size_t total = 0;
  for (uint32_t i = 0; i < channels; ++i) total += per_channel[i];
  if (total != point_count) {
    fail(TSC_INVALID_ARGUMENT, "the points per channel add up to " + std::to_string(total) +
                                   ", not point_count (" + std::to_string(point_count) + ")");
  }
}

#ifdef TSC_MOCK_LIBCARLA

MeasurementPtr make_image(uint64_t frame, double timestamp, const carla::geom::Transform &t,
                          uint32_t width, uint32_t height, float fov, const uint8_t *bgra) {
  auto image = std::make_shared<data::Image>(frame, timestamp, t, width, height, fov);
  std::memcpy(static_cast<void *>(image->data()), bgra, size_t{width} * height * 4u);
  // As LibCarla's ImageSerializer::Deserialize: every pixel opaque.
  for (size_t i = 0; i < image->size(); ++i) image->data()[i].a = 255u;
  return image;
}

MeasurementPtr make_lidar(uint64_t frame, double timestamp, const carla::geom::Transform &t,
                          float angle, const uint32_t *per_channel, uint32_t channels,
                          const float *xyzi, size_t count) {
  std::vector<data::LidarDetection> points;
  points.reserve(count);
  for (size_t i = 0; i < count; ++i) {
    const float *p = xyzi + 4 * i;
    points.emplace_back(carla::geom::Location(p[0], p[1], p[2]), p[3]);
  }
  return std::make_shared<data::LidarMeasurement>(
      frame, timestamp, t, angle, std::vector<uint32_t>(per_channel, per_channel + channels),
      std::move(points));
}

MeasurementPtr make_semantic_lidar(uint64_t frame, double timestamp,
                                   const carla::geom::Transform &t, float angle,
                                   const uint32_t *per_channel, uint32_t channels,
                                   const tsc_semantic_lidar_detection_t *in, size_t count) {
  std::vector<data::SemanticLidarDetection> points;
  points.reserve(count);
  for (size_t i = 0; i < count; ++i) {
    points.emplace_back(carla::geom::Location(in[i].x, in[i].y, in[i].z), in[i].cos_inc_angle,
                        in[i].object_idx, in[i].object_tag);
  }
  return std::make_shared<data::SemanticLidarMeasurement>(
      frame, timestamp, t, angle, std::vector<uint32_t>(per_channel, per_channel + channels),
      std::move(points));
}

#else  // LibCarla

// A measurement built the way LibCarla builds the server's: the sensor
// header, then the serializer's payload, deserialized by the serializer that
// SensorRegistry registers for SensorT.
template <typename SensorT>
MeasurementPtr deserialize(uint64_t frame, double timestamp, const carla::geom::Transform &t,
                           std::initializer_list<std::pair<const void *, size_t>> parts) {
  using carla::sensor::s11n::SensorHeaderSerializer;
  SensorHeaderSerializer::Header header{};
  header.sensor_type = carla::sensor::SensorRegistry::get<SensorT *>::index;
  header.frame = frame;
  header.timestamp = timestamp;
  header.sensor_transform = t;
  uint64_t size = sizeof(header);
  for (const auto &part : parts) size += part.second;
  carla::Buffer buffer(size);
  auto *out = buffer.data();
  std::memcpy(out, &header, sizeof(header));
  out += sizeof(header);
  for (const auto &part : parts) {
    if (part.second != 0) std::memcpy(out, part.first, part.second);
    out += part.second;
  }
  return carla::sensor::Deserializer::Deserialize(std::move(buffer));
}

MeasurementPtr make_image(uint64_t frame, double timestamp, const carla::geom::Transform &t,
                          uint32_t width, uint32_t height, float fov, const uint8_t *bgra) {
  const carla::sensor::s11n::ImageSerializer::ImageHeader header{width, height, fov};
  return deserialize<ASceneCaptureCamera>(
      frame, timestamp, t, {{&header, sizeof(header)}, {bgra, size_t{width} * height * 4u}});
}

// LidarSerializer / SemanticLidarSerializer: a uint32 header (the horizontal
// angle's bits, the channel count, the points per channel), then the points.
std::vector<uint32_t> lidar_header(float angle, const uint32_t *per_channel, uint32_t channels) {
  std::vector<uint32_t> header(2u + channels);
  std::memcpy(&header[0], &angle, sizeof(angle));
  header[1] = channels;
  std::copy(per_channel, per_channel + channels, header.begin() + 2);
  return header;
}

MeasurementPtr make_lidar(uint64_t frame, double timestamp, const carla::geom::Transform &t,
                          float angle, const uint32_t *per_channel, uint32_t channels,
                          const float *xyzi, size_t count) {
  static_assert(sizeof(data::LidarDetection) == 4 * sizeof(float));
  const auto header = lidar_header(angle, per_channel, channels);
  return deserialize<ARayCastLidar>(
      frame, timestamp, t,
      {{header.data(), header.size() * sizeof(uint32_t)}, {xyzi, count * 4 * sizeof(float)}});
}

MeasurementPtr make_semantic_lidar(uint64_t frame, double timestamp,
                                   const carla::geom::Transform &t, float angle,
                                   const uint32_t *per_channel, uint32_t channels,
                                   const tsc_semantic_lidar_detection_t *in, size_t count) {
  static_assert(sizeof(data::SemanticLidarDetection) == sizeof(tsc_semantic_lidar_detection_t));
  const auto header = lidar_header(angle, per_channel, channels);
  return deserialize<ARayCastSemanticLidar>(
      frame, timestamp, t,
      {{header.data(), header.size() * sizeof(uint32_t)},
       {in, count * sizeof(tsc_semantic_lidar_detection_t)}});
}

#endif

// A copy of an image for a second stream (Image.convert changes the pixels
// in place); other measurements are never changed, so streams share them.
MeasurementPtr copy_for_stream(const MeasurementPtr &m) {
  auto image = dynamic_cast<const data::Image *>(m.get());
  if (image == nullptr) return m;
  return make_image(m->GetFrame(), m->GetTimestamp(), m->GetSensorTransform(),
                    static_cast<uint32_t>(image->GetWidth()),
                    static_cast<uint32_t>(image->GetHeight()), image->GetFOVAngle(),
                    reinterpret_cast<const uint8_t *>(image->data()));
}

template <typename F>
tsc_status_t new_measurement(const char *function, tsc_sensor_data_t **out, F &&make) {
  return new_handle(function, out, [&] { return new tsc_sensor_data(make()); });
}

}  // namespace

// --- Hooks ---------------------------------------------------------------------

namespace tsc {

namespace sensor_source {

bool listen(tsc_sensor &handle, const std::shared_ptr<SensorQueue> &queue) {
  auto source = current_source();
  auto &sensor = static_cast<carla::client::Sensor &>(*handle.actor);
  const uint32_t id = sensor.GetId();
  if (!source || !claims(*source, &handle, sensor)) {
    stop(handle);  // a source stream of a former listen()
    return false;
  }
  auto &s = state();
  bool start = false;
  {
    std::lock_guard<std::mutex> lock(s.mutex);
    auto &streams = s.streams;
    streams.erase(std::remove_if(streams.begin(), streams.end(),
                                 [&](const Stream &st) { return st.handle == &handle; }),
                  streams.end());
    streams.push_back(Stream{&handle, id, SensorWeak(carla::SharedPtr<carla::client::Sensor>(
                                              handle.actor, &sensor)),
                             queue});
    start = s.started.insert(id).second;
  }
  if (start && source->start != nullptr) {
    const int32_t status = source->start(source->user, &handle, id);
    if (status != TSC_OK) {
      forget_actor(id);
      check_callback(*source, status, "start");
    }
  }
  return true;
}

void stop(tsc_sensor &handle) {
  auto &s = state();
  uint32_t id = 0;
  bool last = false;
  {
    std::lock_guard<std::mutex> lock(s.mutex);
    auto &streams = s.streams;
    auto it = std::find_if(streams.begin(), streams.end(),
                           [&](const Stream &st) { return st.handle == &handle; });
    if (it == streams.end()) return;
    id = it->actor_id;
    streams.erase(it);
    last = std::none_of(streams.begin(), streams.end(),
                        [&](const Stream &st) { return st.actor_id == id && st.alive(); });
    if (last) last = s.started.erase(id) != 0;
  }
  if (last) {
    const auto source = installed_source();
    if (source && source->stop != nullptr) source->stop(source->user, id);
  }
}

}  // namespace sensor_source

bool actor_destroy(carla::client::Actor &actor) {
  const uint32_t id = actor.GetId();
  const bool destroyed = actor.Destroy();
  if (destroyed) {
    forget_actor(id);
    const auto source = installed_source();
    if (source && source->actor_destroyed != nullptr) source->actor_destroyed(source->user, id);
  }
  return destroyed;
}

uint64_t world_tick(carla::client::World &world, carla::time_duration timeout) {
  const uint64_t frame = world.Tick(timeout);
  auto source = current_source();
  if (source && source->world_ticked != nullptr) {
    TempHandle handle(new tsc_world(world));
    check_callback(*source,
                   source->world_ticked(source->user, static_cast<tsc_world *>(handle.handle), frame),
                   "world_ticked");
  }
  return frame;
}

bool sensor_is_listening(carla::client::Sensor &sensor) {
  if (sensor.IsListening()) return true;
  std::lock_guard<std::mutex> lock(state().mutex);
  const auto &streams = state().streams;
  return std::any_of(streams.begin(), streams.end(), [&](const Stream &st) {
    if (!st.alive()) return false;
    const auto alive = st.sensor.lock();
    return alive.get() == &sensor;
  });
}

namespace {

void set_ros_enabled(carla::client::ServerSideSensor &sensor, bool enabled) {
  auto source = claiming_source(sensor);
  if (!source) {
    if (enabled) {
      sensor.EnableForROS();
    } else {
      sensor.DisableForROS();
    }
    return;
  }
  if (source->set_ros_enabled == nullptr) {
    fail(TSC_ERROR, "the sensor source of '" + sensor.GetTypeId() + "' has no ROS output");
  }
  TempHandle handle(make_actor_handle(sensor.shared_from_this()));
  check_callback(*source,
                 source->set_ros_enabled(source->user, static_cast<tsc_actor *>(handle.handle),
                                         sensor.GetId(), enabled ? 1 : 0),
                 enabled ? "enable_for_ros" : "disable_for_ros");
}

}  // namespace

void sensor_enable_for_ros(carla::client::ServerSideSensor &sensor) { set_ros_enabled(sensor, true); }

void sensor_disable_for_ros(carla::client::ServerSideSensor &sensor) {
  set_ros_enabled(sensor, false);
}

bool sensor_is_enabled_for_ros(carla::client::ServerSideSensor &sensor) {
  auto source = claiming_source(sensor);
  if (!source) return sensor.IsEnabledForROS();
  return source->is_ros_enabled != nullptr && source->is_ros_enabled(source->user, sensor.GetId()) != 0;
}

}  // namespace tsc

// --- C ABI -----------------------------------------------------------------------

extern "C" {

tsc_status_t tsc_sensor_source_install(const tsc_sensor_source_t *source) {
  return TSC_GUARD({
    if (source != nullptr) {
      if (source->abi_major != TSC_ABI_VERSION_MAJOR) {
        fail(TSC_VERSION_ERROR, "the sensor source was built for ABI major " +
                                    std::to_string(source->abi_major) + ", this library has " +
                                    std::to_string(TSC_ABI_VERSION_MAJOR));
      }
      if (source->struct_size < sizeof(tsc_sensor_source_t)) {
        fail(TSC_VERSION_ERROR, "tsc_sensor_source_t.struct_size is " +
                                    std::to_string(source->struct_size) + ", expected at least " +
                                    std::to_string(sizeof(tsc_sensor_source_t)));
      }
    }
    std::lock_guard<std::mutex> lock(state().mutex);
    state().source = source != nullptr ? std::optional(*source) : std::nullopt;
  });
}

tsc_status_t tsc_sensor_source_installed(int32_t *out) {
  return TSC_GUARD({
    require_ptr(out, "out");
    *out = current_source() ? 1 : 0;
  });
}

tsc_status_t tsc_sensor_source_load(const char *path, size_t path_len) {
  return TSC_GUARD({
    const std::string p = to_string(path, path_len, "path");
    if (p.empty()) fail(TSC_INVALID_ARGUMENT, "path is empty");
    load_plugin(p);
  });
}

tsc_status_t tsc_sensor_source_push(uint32_t actor_id, const tsc_sensor_data_t *data,
                                    size_t *out_delivered) {
  return TSC_GUARD({
    sensor_data_of(data);  // checks the handle
    const MeasurementPtr measurement = data->data;
    std::vector<std::shared_ptr<SensorQueue>> queues;
    {
      auto &s = state();
      std::lock_guard<std::mutex> lock(s.mutex);
      auto &streams = s.streams;
      for (auto it = streams.begin(); it != streams.end();) {
        if (it->actor_id != actor_id) {
          ++it;
        } else if (auto queue = it->queue.lock(); queue != nullptr && !it->sensor.expired()) {
          queues.push_back(std::move(queue));
          ++it;
        } else {
          it = streams.erase(it);
        }
      }
      if (queues.empty()) s.started.erase(actor_id);
    }
    for (size_t i = 0; i < queues.size(); ++i) {
      queues[i]->push(i == 0 ? measurement : copy_for_stream(measurement));
    }
    store_if(out_delivered, queues.size());
  });
}

tsc_status_t tsc_sensor_data_new_image(uint64_t frame, double timestamp,
                                       const tsc_transform_t *sensor_transform, uint32_t width,
                                       uint32_t height, float fov, const uint8_t *bgra,
                                       size_t size, tsc_sensor_data_t **out) {
  return new_measurement(__func__, out, [&] {
    const auto t = transform_arg(sensor_transform);
    if (width == 0 || height == 0) fail(TSC_INVALID_ARGUMENT, "the image is empty");
    if (!std::isfinite(fov) || fov <= 0.0f) fail(TSC_INVALID_ARGUMENT, "fov must be positive");
    if (size != size_t{width} * height * 4u) {
      fail(TSC_INVALID_ARGUMENT, "size is " + std::to_string(size) + ", not width * height * 4 (" +
                                     std::to_string(size_t{width} * height * 4u) + ")");
    }
    require_array(bgra, size, "bgra");
    return make_image(frame, timestamp, t, width, height, fov, bgra);
  });
}

tsc_status_t tsc_sensor_data_new_lidar(uint64_t frame, double timestamp,
                                       const tsc_transform_t *sensor_transform,
                                       float horizontal_angle, const uint32_t *points_per_channel,
                                       uint32_t channel_count, const float *points,
                                       size_t point_count, tsc_sensor_data_t **out) {
  return new_measurement(__func__, out, [&] {
    const auto t = transform_arg(sensor_transform);
    check_float(horizontal_angle, "horizontal_angle");
    check_point_counts(points_per_channel, channel_count, point_count);
    require_array(points, point_count, "points");
    return make_lidar(frame, timestamp, t, horizontal_angle, points_per_channel, channel_count,
                      points, point_count);
  });
}

tsc_status_t tsc_sensor_data_new_semantic_lidar(
    uint64_t frame, double timestamp, const tsc_transform_t *sensor_transform,
    float horizontal_angle, const uint32_t *points_per_channel, uint32_t channel_count,
    const tsc_semantic_lidar_detection_t *points, size_t point_count, tsc_sensor_data_t **out) {
  return new_measurement(__func__, out, [&] {
    const auto t = transform_arg(sensor_transform);
    check_float(horizontal_angle, "horizontal_angle");
    check_point_counts(points_per_channel, channel_count, point_count);
    require_array(points, point_count, "points");
    return make_semantic_lidar(frame, timestamp, t, horizontal_angle, points_per_channel,
                               channel_count, points, point_count);
  });
}

}  // extern "C"
