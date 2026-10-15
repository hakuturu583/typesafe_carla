/* Sensor sources (ABI 5.3): measurements built by a source, a source that
 * takes over a sensor's stream and ROS output, and plugin loading. The
 * stream checks need the mock server; the rest runs on every backend. */
#include "typesafe_carla/ffi.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_failures = 0;

#define CHECK(cond)                                                     \
  do {                                                                  \
    if (!(cond)) {                                                      \
      fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
      ++g_failures;                                                     \
    }                                                                   \
  } while (0)

#define CHECK_OK(expr)                                                  \
  do {                                                                  \
    tsc_status_t _s = (expr);                                           \
    if (_s != TSC_OK) {                                                 \
      fprintf(stderr, "%s:%d: %s returned %d: %s\n", __FILE__, __LINE__, #expr, (int)_s, \
              tsc_last_error_message());                                \
      ++g_failures;                                                     \
    }                                                                   \
  } while (0)

#define H(p) ((tsc_handle_t *)(p))

static const tsc_transform_t kAt = {{1, 2, 3}, {10, 20, 30}};

static void test_new_image(void) {
  const uint8_t bgra[2 * 1 * 4] = {1, 2, 3, 4, 5, 6, 7, 8};
  tsc_sensor_data_t *d = NULL;
  CHECK_OK(tsc_sensor_data_new_image(42, 1.5, &kAt, 2, 1, 90.0f, bgra, sizeof bgra, &d));
  tsc_sensor_data_info_t info;
  CHECK_OK(tsc_sensor_data_get_info(d, &info));
  CHECK(info.frame == 42 && info.timestamp == 1.5 && info.type == TSC_SENSOR_DATA_IMAGE);
  CHECK(info.sensor_transform.location.y == 2 && info.sensor_transform.rotation.yaw == 20);
  tsc_image_t image;
  CHECK_OK(tsc_sensor_data_as_image(d, &image));
  CHECK(image.width == 2 && image.height == 1 && image.fov == 90.0);
  CHECK(image.size == sizeof bgra && memcmp(image.data, bgra, 3) == 0);
  CHECK(image.data[3] == 255 && image.data[4] == 5 && image.data[7] == 255); /* alpha: opaque */
  tsc_handle_release(H(d));

  d = (tsc_sensor_data_t *)0x1;
  CHECK(tsc_sensor_data_new_image(0, 0, &kAt, 2, 2, 90.0f, bgra, sizeof bgra, &d) ==
            TSC_INVALID_ARGUMENT && d == NULL);
  CHECK(tsc_sensor_data_new_image(0, 0, &kAt, 0, 0, 90.0f, NULL, 0, &d) == TSC_INVALID_ARGUMENT);
  CHECK(tsc_sensor_data_new_image(0, 0, NULL, 2, 1, 90.0f, bgra, sizeof bgra, &d) ==
        TSC_INVALID_ARGUMENT);
  CHECK(tsc_sensor_data_new_image(0, 0, &kAt, 2, 1, 0.0f, bgra, sizeof bgra, &d) ==
        TSC_INVALID_ARGUMENT);
}

static void test_new_lidar(void) {
  const float points[3 * 4] = {1, 2, 3, 0.5f, 4, 5, 6, 0.25f, 7, 8, 9, 1};
  const uint32_t per_channel[2] = {2, 1};
  tsc_sensor_data_t *d = NULL;
  CHECK_OK(tsc_sensor_data_new_lidar(7, 0.1, &kAt, 1.25f, per_channel, 2, points, 3, &d));
  tsc_sensor_data_info_t info;
  CHECK_OK(tsc_sensor_data_get_info(d, &info));
  CHECK(info.type == TSC_SENSOR_DATA_LIDAR && info.frame == 7);
  tsc_lidar_t lidar;
  CHECK_OK(tsc_sensor_data_as_lidar(d, &lidar));
  CHECK(lidar.channels == 2 && lidar.point_count == 3);
  CHECK(lidar.horizontal_angle > 1.249 && lidar.horizontal_angle < 1.251);
  CHECK(memcmp(lidar.points, points, sizeof points) == 0);
  uint32_t count = 0;
  CHECK_OK(tsc_lidar_channel_point_count(d, 0, &count));
  CHECK(count == 2);
  CHECK_OK(tsc_lidar_channel_point_count(d, 1, &count));
  CHECK(count == 1);
  tsc_handle_release(H(d));
  CHECK(tsc_sensor_data_new_lidar(7, 0.1, &kAt, 0, per_channel, 2, points, 2, &d) ==
        TSC_INVALID_ARGUMENT); /* counts add up to 3 */

  const tsc_semantic_lidar_detection_t sem[2] = {{1, 2, 3, 0.5f, 11, 7}, {4, 5, 6, 1, 12, 8}};
  const uint32_t one_channel[1] = {2};
  CHECK_OK(tsc_sensor_data_new_semantic_lidar(8, 0.2, &kAt, 0.5f, one_channel, 1, sem, 2, &d));
  CHECK_OK(tsc_sensor_data_get_info(d, &info));
  CHECK(info.type == TSC_SENSOR_DATA_SEMANTIC_LIDAR);
  tsc_semantic_lidar_t sl;
  CHECK_OK(tsc_sensor_data_as_semantic_lidar(d, &sl));
  CHECK(sl.point_count == 2 && sl.channels == 1);
  CHECK(sl.points[1].object_idx == 12 && sl.points[1].object_tag == 8 && sl.points[0].z == 3);
  tsc_handle_release(H(d));
}

/* --- A source that renders sensor.camera.rgb ----------------------------------- */

typedef struct {
  uint32_t started[8];
  size_t started_count;
  uint32_t stopped;
  uint32_t destroyed;
  int32_t ros;
  int ros_calls;
  size_t last_delivered;
  int ticks;
  int fail_start;
} Recorder;

static Recorder g_rec;

static int32_t rec_claims(void *user, tsc_actor_t *sensor, uint32_t id, const char *type_id,
                          size_t len) {
  (void)user;
  (void)id;
  CHECK(sensor != NULL);
  return len == 17 && memcmp(type_id, "sensor.camera.rgb", 17) == 0;
}

static int32_t rec_start(void *user, tsc_actor_t *sensor, uint32_t id) {
  Recorder *r = (Recorder *)user;
  uint32_t handle_id = 0;
  CHECK_OK(tsc_actor_get_id(sensor, &handle_id));
  CHECK(handle_id == id);
  if (r->fail_start) return TSC_ERROR;
  if (r->started_count < 8) r->started[r->started_count++] = id;
  return TSC_OK;
}

static void rec_stop(void *user, uint32_t id) { ((Recorder *)user)->stopped = id; }

static int32_t rec_set_ros(void *user, tsc_actor_t *sensor, uint32_t id, int32_t enabled) {
  Recorder *r = (Recorder *)user;
  (void)sensor;
  (void)id;
  r->ros = enabled;
  ++r->ros_calls;
  return TSC_OK;
}

static int32_t rec_is_ros(void *user, uint32_t id) {
  (void)id;
  return ((Recorder *)user)->ros;
}

static void rec_destroyed(void *user, uint32_t id) { ((Recorder *)user)->destroyed = id; }

/* Pushes one 1x1 image per tick, its blue channel = the frame's low byte. */
static int32_t rec_ticked(void *user, tsc_world_t *world, uint64_t frame) {
  Recorder *r = (Recorder *)user;
  CHECK(world != NULL);
  ++r->ticks;
  for (size_t i = 0; i < r->started_count; ++i) {
    const uint8_t px[4] = {(uint8_t)frame, 0, 0, 255};
    tsc_sensor_data_t *d = NULL;
    CHECK_OK(tsc_sensor_data_new_image(frame, 0.0, &kAt, 1, 1, 90.0f, px, 4, &d));
    CHECK_OK(tsc_sensor_source_push(r->started[i], d, &r->last_delivered));
    tsc_handle_release(H(d));
  }
  return TSC_OK;
}

static const char *rec_last_error(void *user) {
  (void)user;
  return "start refused";
}

static tsc_sensor_source_t recorder_source(void) {
  tsc_sensor_source_t s;
  memset(&s, 0, sizeof s);
  s.abi_major = TSC_ABI_VERSION_MAJOR;
  s.struct_size = sizeof s;
  s.user = &g_rec;
  s.claims = rec_claims;
  s.start = rec_start;
  s.stop = rec_stop;
  s.set_ros_enabled = rec_set_ros;
  s.is_ros_enabled = rec_is_ros;
  s.actor_destroyed = rec_destroyed;
  s.world_ticked = rec_ticked;
  s.last_error = rec_last_error;
  return s;
}

static void test_install(void) {
  tsc_sensor_source_t s = recorder_source();
  s.abi_major = TSC_ABI_VERSION_MAJOR + 1;
  CHECK(tsc_sensor_source_install(&s) == TSC_VERSION_ERROR);
  s = recorder_source();
  s.struct_size = 8;
  CHECK(tsc_sensor_source_install(&s) == TSC_VERSION_ERROR);
  int32_t installed = 1;
  CHECK_OK(tsc_sensor_source_installed(&installed));
  CHECK(installed == 0);
  CHECK(tsc_sensor_source_load(NULL, 0) == TSC_INVALID_ARGUMENT);
  CHECK(tsc_sensor_source_load("/nonexistent/plugin.so", 22) == TSC_ERROR);
  CHECK(strstr(tsc_last_error_message(), "cannot load sensor plugin") != NULL);
  /* Nobody listens to actor 1. */
  const uint8_t px[4] = {0, 0, 0, 0};
  tsc_sensor_data_t *d = NULL;
  size_t delivered = 9;
  CHECK_OK(tsc_sensor_data_new_image(0, 0, &kAt, 1, 1, 90.0f, px, 4, &d));
  CHECK_OK(tsc_sensor_source_push(1, d, &delivered));
  CHECK(delivered == 0);
  tsc_handle_release(H(d));
}

static tsc_sensor_t *spawn(tsc_world_t *world, tsc_blueprint_library_t *library, const char *id,
                           tsc_actor_t **out_actor) {
  tsc_actor_blueprint_t *bp = NULL;
  tsc_actor_t *actor = NULL;
  tsc_sensor_t *sensor = NULL;
  CHECK_OK(tsc_blueprint_library_find(library, id, strlen(id), &bp));
  CHECK_OK(tsc_world_spawn_actor(world, bp, &kAt, NULL, TSC_ATTACHMENT_RIGID, &actor));
  CHECK_OK(tsc_actor_as_sensor(actor, &sensor));
  tsc_handle_release(H(bp));
  *out_actor = actor;
  return sensor;
}

static void test_mock_stream(void) {
  tsc_client_t *client = NULL;
  tsc_world_t *world = NULL;
  tsc_blueprint_library_t *library = NULL;
  CHECK_OK(tsc_client_create("localhost", 9, 2000, 0, &client));
  CHECK_OK(tsc_client_get_world(client, &world));
  CHECK_OK(tsc_world_get_blueprint_library(world, &library));

  tsc_actor_t *camera_actor = NULL, *gnss_actor = NULL;
  tsc_sensor_t *camera = spawn(world, library, "sensor.camera.rgb", &camera_actor);
  tsc_sensor_t *gnss = spawn(world, library, "sensor.other.gnss", &gnss_actor);
  uint32_t camera_id = 0;
  CHECK_OK(tsc_actor_get_id(camera_actor, &camera_id));

  tsc_sensor_source_t s = recorder_source();
  CHECK_OK(tsc_sensor_source_install(&s));
  int32_t installed = 0;
  CHECK_OK(tsc_sensor_source_installed(&installed));
  CHECK(installed == 1);

  /* A failing start fails listen() with the source's message. */
  g_rec.fail_start = 1;
  CHECK(tsc_sensor_listen(camera, 0) == TSC_ERROR);
  CHECK(strstr(tsc_last_error_message(), "start refused") != NULL);
  g_rec.fail_start = 0;

  CHECK_OK(tsc_sensor_listen(camera, 0));
  CHECK_OK(tsc_sensor_listen(gnss, 0)); /* not claimed: the server's stream */
  CHECK(g_rec.started_count == 1 && g_rec.started[0] == camera_id);
  int32_t listening = 0;
  CHECK_OK(tsc_sensor_is_listening(camera, &listening));
  CHECK(listening == 1);

  uint64_t frame = 0;
  CHECK_OK(tsc_world_tick(world, 1.0, &frame));
  CHECK(g_rec.ticks == 1 && g_rec.last_delivered == 1);
  size_t pending = 0;
  CHECK_OK(tsc_sensor_pending_count(camera, &pending));
  CHECK(pending == 1); /* only the source's image: LibCarla's stream never started */
  tsc_sensor_data_t *d = NULL;
  CHECK_OK(tsc_sensor_poll(camera, &d));
  tsc_image_t image;
  CHECK_OK(tsc_sensor_data_as_image(d, &image));
  CHECK(image.width == 1 && image.data[0] == (uint8_t)frame);
  tsc_handle_release(H(d));
  CHECK_OK(tsc_sensor_pending_count(gnss, &pending));
  CHECK(pending >= 1); /* the mock server's GNSS */

  /* Another handle of the same actor: a second stream with its own copy. */
  tsc_actor_t *again = NULL;
  tsc_sensor_t *camera2 = NULL;
  CHECK_OK(tsc_world_get_actor(world, camera_id, &again));
  CHECK_OK(tsc_actor_as_sensor(again, &camera2));
  CHECK_OK(tsc_sensor_listen(camera2, 0));
  CHECK(g_rec.started_count == 1); /* started once per actor */
  CHECK_OK(tsc_world_tick(world, 1.0, &frame));
  CHECK(g_rec.last_delivered == 2);
  tsc_sensor_data_t *d1 = NULL, *d2 = NULL;
  CHECK_OK(tsc_sensor_poll(camera, &d1));
  CHECK_OK(tsc_sensor_poll(camera2, &d2));
  CHECK(d1 != NULL && d2 != NULL);
  CHECK_OK(tsc_image_convert(d1, TSC_COLOR_CONVERTER_DEPTH));
  tsc_image_t i1, i2;
  CHECK_OK(tsc_sensor_data_as_image(d1, &i1));
  CHECK_OK(tsc_sensor_data_as_image(d2, &i2));
  CHECK(i1.data != i2.data && i2.data[0] == (uint8_t)frame);
  tsc_handle_release(H(d1));
  tsc_handle_release(H(d2));
  CHECK_OK(tsc_sensor_stop(camera2));
  CHECK(g_rec.stopped == 0); /* camera still listens */
  tsc_handle_release(H(camera2));
  tsc_handle_release(H(again));

  /* ROS goes to the source for a claimed sensor, to the server otherwise. */
  CHECK_OK(tsc_sensor_enable_for_ros(camera));
  CHECK(g_rec.ros == 1 && g_rec.ros_calls == 1);
  int32_t ros = 0;
  CHECK_OK(tsc_sensor_is_enabled_for_ros(camera, &ros));
  CHECK(ros == 1);
  CHECK_OK(tsc_sensor_disable_for_ros(camera));
  CHECK_OK(tsc_sensor_is_enabled_for_ros(camera, &ros));
  CHECK(ros == 0 && g_rec.ros_calls == 2);
  CHECK_OK(tsc_sensor_enable_for_ros(gnss));
  CHECK_OK(tsc_sensor_is_enabled_for_ros(gnss, &ros));
  CHECK(ros == 1 && g_rec.ros_calls == 2);

  /* stop() ends the last stream: the source stops. */
  CHECK_OK(tsc_sensor_stop(camera));
  CHECK(g_rec.stopped == camera_id);
  CHECK_OK(tsc_sensor_is_listening(camera, &listening));
  CHECK(listening == 0);
  g_rec.started_count = 0;
  CHECK_OK(tsc_world_tick(world, 1.0, &frame));
  size_t delivered = 9;
  {
    const uint8_t px[4] = {0, 0, 0, 0};
    CHECK_OK(tsc_sensor_data_new_image(0, 0, &kAt, 1, 1, 90.0f, px, 4, &d));
    CHECK_OK(tsc_sensor_source_push(camera_id, d, &delivered));
    CHECK(delivered == 0);
    tsc_handle_release(H(d));
  }

  /* Listening again starts it again; destroy() tells the source. */
  CHECK_OK(tsc_sensor_listen(camera, 0));
  CHECK(g_rec.started_count == 1);
  int32_t destroyed = 0;
  CHECK_OK(tsc_actor_destroy(camera_actor, &destroyed));
  CHECK(destroyed == 1 && g_rec.destroyed == camera_id);
  CHECK_OK(tsc_world_tick(world, 1.0, &frame));
  CHECK(g_rec.last_delivered == 0);

  CHECK_OK(tsc_sensor_source_install(NULL));
  CHECK_OK(tsc_sensor_source_installed(&installed));
  CHECK(installed == 0);
  CHECK_OK(tsc_sensor_stop(gnss));
  CHECK_OK(tsc_actor_destroy(gnss_actor, &destroyed));
  tsc_handle_release(H(gnss));
  tsc_handle_release(H(gnss_actor));
  tsc_handle_release(H(camera));
  tsc_handle_release(H(camera_actor));
  tsc_handle_release(H(library));
  tsc_handle_release(H(world));
  tsc_handle_release(H(client));
}

#ifdef TSC_TEST_PLUGIN_PATH
/* The test plugin (test_sensor_plugin.c) installs a source claiming
 * everything named "plugin.claimed". */
static void test_plugin(void) {
  const char *path = TSC_TEST_PLUGIN_PATH;
  CHECK_OK(tsc_sensor_source_load(path, strlen(path)));
  CHECK_OK(tsc_sensor_source_load(path, strlen(path))); /* again: nothing */
  int32_t installed = 0;
  CHECK_OK(tsc_sensor_source_installed(&installed));
  CHECK(installed == 1);
  CHECK_OK(tsc_sensor_source_install(NULL));
}
#endif

int main(void) {
  test_new_image();
  test_new_lidar();
  test_install();
  if (strcmp(tsc_backend_name(), "mock") == 0) {
    test_mock_stream();
  } else {
    printf("backend '%s': skipping mock-server checks\n", tsc_backend_name());
  }
#ifdef TSC_TEST_PLUGIN_PATH
  test_plugin();
#endif
  if (g_failures != 0) {
    fprintf(stderr, "%d check(s) failed\n", g_failures);
    return 1;
  }
  printf("test_sensor_source: all checks passed (backend %s)\n", tsc_backend_name());
  return 0;
}
