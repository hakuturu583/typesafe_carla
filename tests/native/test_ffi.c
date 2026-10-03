/* Exercises the C ABI from plain C: ownership, errors, layout, threading.
 * Runs against whichever backend the library was built with; the
 * behavioural checks that depend on the mock server are skipped otherwise. */
#include "typesafe_carla/ffi.h"

#include <pthread.h>
#include <stddef.h>
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

/* Mock-only test hooks (native/mock/include/carla/mock/Mock.h): what the shim
 * last passed to LibCarla. Weak, so the libcarla build links without them. */
extern size_t tsc_mock_last_worker_threads(void) __attribute__((weak));
extern uint16_t tsc_mock_last_map_layers(void) __attribute__((weak));

static const char *kHost = "localhost";

static void test_versions(void) {
  CHECK(tsc_abi_version() == TSC_ABI_VERSION);
  CHECK(tsc_abi_version() >> 16 == TSC_ABI_VERSION_MAJOR);
  CHECK(strlen(tsc_libcarla_version()) > 0);
  CHECK(strlen(tsc_build_commit()) > 0);
  CHECK(strlen(tsc_backend_name()) > 0);
  CHECK(strlen(tsc_libcarla_git_ref()) > 0);
  CHECK(strlen(tsc_libcarla_git_commit()) > 0);
  if (strcmp(tsc_backend_name(), "libcarla") == 0) {
    /* UE5 only: LibCarla 0.10 or later. */
    CHECK(strncmp(tsc_libcarla_version(), "0.9", 3) != 0);
    printf("libcarla %s (%s %s)\n", tsc_libcarla_version(), tsc_libcarla_git_ref(),
           tsc_libcarla_git_commit());
  }
}

static void test_layout(void) {
  /* These sizes are mirrored by the Codon @tuple declarations in _ffi.codon. */
  CHECK(sizeof(tsc_vector3d_t) == 24);
  CHECK(sizeof(tsc_transform_t) == 48);
  CHECK(sizeof(tsc_vehicle_control_t) == 40);
  CHECK(sizeof(tsc_world_settings_t) == 40);
  CHECK(sizeof(tsc_string_t) == 16);
  CHECK(sizeof(tsc_actor_attribute_t) == 40);
  /* ABI 1.2 */
  CHECK(sizeof(tsc_bounding_box_t) == 72);
  CHECK(sizeof(tsc_timestamp_t) == 32);
  CHECK(sizeof(tsc_actor_snapshot_t) == 128);
  CHECK(sizeof(tsc_waypoint_info_t) == 96);
  CHECK(sizeof(tsc_command_t) == 152); /* ABI 2.0: + scalar */
  CHECK(offsetof(tsc_command_response_t, error) == 8);
  CHECK(sizeof(tsc_command_response_t) == 24);
  /* ABI 1.3 */
  CHECK(sizeof(tsc_sensor_data_info_t) == 72);
  CHECK(sizeof(tsc_image_t) == 32);
  CHECK(sizeof(tsc_lidar_t) == 32);
  CHECK(sizeof(tsc_gnss_t) == 24);
  CHECK(sizeof(tsc_imu_t) == 56);
  CHECK(sizeof(tsc_collision_t) == 32);
  /* ABI 2.0 */
  CHECK(sizeof(tsc_walker_control_t) == 40);
  CHECK(sizeof(tsc_traffic_light_info_t) == 48);
  CHECK(sizeof(tsc_weather_t) == 112);
  CHECK(sizeof(tsc_color_t) == 4);
  CHECK(sizeof(tsc_opendrive_parameters_t) == 48);
  CHECK(sizeof(tsc_landmark_t) == 224);
  /* ABI 3.0 */
  CHECK(sizeof(tsc_vector2d_t) == 16);
  CHECK(sizeof(tsc_wheel_physics_control_t) == 360);
  CHECK(offsetof(tsc_wheel_physics_control_t, wheel_radius) == 160);
  CHECK(offsetof(tsc_wheel_physics_control_t, axle_type) == 312);
  CHECK(sizeof(tsc_vehicle_physics_control_t) == 304);
  CHECK(offsetof(tsc_vehicle_physics_control_t, max_torque) == 80);
  CHECK(offsetof(tsc_vehicle_physics_control_t, center_of_mass) == 240);
  CHECK(offsetof(tsc_vehicle_physics_control_t, differential_type) == 288);
  /* Issue #24 */
  CHECK(sizeof(tsc_radar_detection_t) == 16 && sizeof(tsc_radar_t) == 16);
  CHECK(sizeof(tsc_semantic_lidar_detection_t) == 24 && sizeof(tsc_semantic_lidar_t) == 32);
  CHECK(sizeof(tsc_dvs_t) == 32 && sizeof(tsc_optical_flow_t) == 32);
  /* Issue #22 */
  CHECK(sizeof(tsc_lane_marking_t) == 24);
  CHECK(sizeof(tsc_geo_location_t) == 24);
  CHECK(sizeof(tsc_geo_projection_t) == 128);
  CHECK(offsetof(tsc_geo_projection_t, ellipsoid_a) == 16);
  CHECK(offsetof(tsc_geo_projection_t, offset_x) == 88);
  CHECK(sizeof(tsc_lane_validity_t) == 8);
  /* issue #20 */
  CHECK(sizeof(tsc_vehicle_ackermann_control_t) == 40);
  CHECK(sizeof(tsc_ackermann_controller_settings_t) == 48);
  CHECK(sizeof(tsc_wheel_telemetry_data_t) == 24);
  CHECK(sizeof(tsc_vehicle_telemetry_data_t) == 48);
  CHECK(sizeof(tsc_bone_transform_t) == 64);
  CHECK(offsetof(tsc_bone_transform_t, transform) == 16);
  CHECK(offsetof(tsc_vehicle_telemetry_data_t, gear) == 40);
  CHECK(sizeof(tsc_bone_transform_out_t) == 160);
  CHECK(sizeof(tsc_string_list_t) == 16);         /* issue #23 */
  CHECK(sizeof(tsc_world_settings_ext_t) == 32);  /* issue #23 */
  /* Issue #21 */
  CHECK(sizeof(tsc_vehicle_light_state_t) == 8);
  CHECK(sizeof(tsc_environment_object_t) == 152);
  CHECK(offsetof(tsc_environment_object_t, transform) == 24);
  CHECK(offsetof(tsc_environment_object_t, type) == 144);
  CHECK(sizeof(tsc_labelled_point_t) == 32);
  CHECK(sizeof(tsc_light_t) == 32);
  CHECK(sizeof(tsc_light_state_t) == 24);
  CHECK(offsetof(tsc_light_state_t, group) == 12);
}

/* Issue #23, on any backend: no server needed. */
static void test_issue23_offline(void) {
  tsc_string_list_t list;
  memset(&list, 0, sizeof list);
  tsc_string_list_free(&list); /* zeroed: a no-op */
  tsc_string_list_free(&list); /* twice */
  tsc_string_list_free(NULL);
  tsc_road_options_free(NULL);
  /* Transform matrices: yaw 90 degrees is the same in CARLA 0.10.0 and ue5-dev. */
  tsc_transform_t t = {{1.0, 2.0, 3.0}, {0.0, 90.0, 0.0}};
  double m[16], inv[16];
  CHECK_OK(tsc_transform_get_matrix(&t, m));
  CHECK_OK(tsc_transform_get_inverse_matrix(&t, inv));
  CHECK(m[3] == 1.0 && m[7] == 2.0 && m[11] == 3.0 && m[15] == 1.0);
  CHECK(m[1] < -0.999 && m[4] > 0.999); /* x' = -y, y' = x */
  for (int i = 0; i < 4; ++i) {
    for (int j = 0; j < 4; ++j) {
      double acc = 0.0;
      for (int k = 0; k < 4; ++k) acc += m[4 * i + k] * inv[4 * k + j];
      CHECK(acc > (i == j ? 0.9999 : -0.0001) && acc < (i == j ? 1.0001 : 0.0001));
    }
  }
  CHECK(tsc_transform_get_matrix(NULL, m) == TSC_INVALID_ARGUMENT);
  CHECK(tsc_transform_get_matrix(&t, NULL) == TSC_INVALID_ARGUMENT);
  /* Hand-written entry points reject NULL handles. */
  tsc_world_t *world = NULL;
  int32_t option = 0;
  tsc_waypoint_t *waypoint = NULL;
  CHECK(tsc_client_load_world_if_different(NULL, "Town01", 6, 1, 0xFFFF, &world) == TSC_INVALID_ARGUMENT);
  CHECK(tsc_traffic_manager_get_next_action(NULL, NULL, &option, &waypoint) == TSC_INVALID_ARGUMENT);
  CHECK(tsc_debug_clear_shapes(NULL) == TSC_INVALID_ARGUMENT);
  CHECK(tsc_world_get_settings_ext(NULL, NULL, NULL) == TSC_INVALID_ARGUMENT);
}

/* Issue #35: worker_threads and map_layers reach LibCarla (the mock). */
static void test_mock_issue35(void) {
  CHECK(tsc_mock_last_worker_threads != NULL && tsc_mock_last_map_layers != NULL);
  if (tsc_mock_last_worker_threads == NULL || tsc_mock_last_map_layers == NULL) return;
  tsc_client_t *client = NULL;
  CHECK_OK(tsc_client_create(kHost, strlen(kHost), 2035, 3, &client));
  CHECK(tsc_mock_last_worker_threads() == 3);
  tsc_world_t *world = NULL;
  CHECK_OK(tsc_client_load_world(client, "Town01", 6, 1, 0x1 | 0x100, &world));
  CHECK(tsc_mock_last_map_layers() == (0x1 | 0x100));
  tsc_handle_release(H(world));
  world = NULL;
  CHECK_OK(tsc_client_load_world_if_different(client, "Town02", 6, 1, 0, &world));
  CHECK(world != NULL && tsc_mock_last_map_layers() == 0);
  tsc_handle_release(H(world));
  tsc_handle_release(H(client));
  CHECK_OK(tsc_client_create(kHost, strlen(kHost), 2035, 0, &client));
  CHECK(tsc_mock_last_worker_threads() == 0);
  tsc_handle_release(H(client));
}

/* Issue #23 against the mock server. */
static void test_mock_issue23(void) {
  tsc_client_t *client = NULL;
  CHECK_OK(tsc_client_create(kHost, strlen(kHost), 2023, 2, &client));
  tsc_string_list_t maps;
  memset(&maps, 0, sizeof maps);
  CHECK_OK(tsc_client_get_available_maps(client, &maps));
  CHECK(maps.size == 3 && strcmp(maps.items[1].data, "/Game/Carla/Maps/Town01") == 0);
  tsc_string_list_free(&maps);
  CHECK(maps.items == NULL && maps.size == 0);
  tsc_world_t *world = NULL;
  CHECK_OK(tsc_client_load_world_if_different(client, "MockTown", 8, 1, 0xFFFF, &world));
  CHECK(world == NULL);
  CHECK_OK(tsc_client_load_world_if_different(client, "Town01", 6, 1, 0xFFFF, &world));
  CHECK(world != NULL);
  tsc_world_settings_t s;
  tsc_world_settings_ext_t ext;
  CHECK_OK(tsc_world_get_settings_ext(world, &s, &ext));
  CHECK(ext.tile_stream_distance == 3000.0 && ext.deterministic_ragdolls == 1);
  ext.actor_active_distance = 750.0;
  uint64_t frame = 0;
  CHECK_OK(tsc_world_apply_settings_ext(world, &s, &ext, 1.0, &frame));
  /* The ABI 3.0 entry points leave the extended fields alone. */
  CHECK_OK(tsc_world_apply_settings(world, &s, 1.0, &frame));
  CHECK_OK(tsc_world_get_settings_ext(world, &s, &ext));
  CHECK(ext.actor_active_distance == 750.0);
  ext.max_culling_distance = 0.0 / 0.0;
  CHECK(tsc_world_apply_settings_ext(world, &s, &ext, 1.0, &frame) == TSC_INVALID_ARGUMENT);
  CHECK_OK(tsc_debug_clear_shapes(world));
  CHECK_OK(tsc_debug_clear_strings(world));
  tsc_traffic_manager_t *tm = NULL;
  CHECK_OK(tsc_client_get_traffic_manager(client, 8000, &tm));
  tsc_blueprint_library_t *library = NULL;
  tsc_actor_blueprint_t *bp = NULL;
  tsc_actor_t *actor = NULL;
  tsc_vehicle_t *vehicle = NULL;
  CHECK_OK(tsc_world_get_blueprint_library(world, &library));
  CHECK_OK(tsc_blueprint_library_find(library, "vehicle.audi.tt", 15, &bp));
  tsc_transform_t at = {{10.0, 0.0, 0.6}, {0.0, 0.0, 0.0}};
  CHECK_OK(tsc_world_spawn_actor(world, bp, &at, NULL, TSC_ATTACHMENT_RIGID, &actor));
  CHECK_OK(tsc_actor_as_vehicle(actor, &vehicle));
  const uint8_t bad_route[] = {TSC_ROAD_OPTION_LEFT, 8};
  CHECK(tsc_traffic_manager_set_route(tm, vehicle, bad_route, 2, 1) == TSC_INVALID_ARGUMENT);
  CHECK(strstr(tsc_last_error_message(), "invalid road option 8") != NULL);
  CHECK(tsc_traffic_manager_set_route(tm, vehicle, NULL, 1, 1) == TSC_INVALID_ARGUMENT);
  CHECK_OK(tsc_traffic_manager_set_route(tm, vehicle, NULL, 0, 1));
  uint8_t *options = NULL;
  size_t count = 0;
  tsc_waypoint_list_t *waypoints = NULL;
  CHECK(tsc_traffic_manager_get_all_actions(tm, vehicle, &options, &count, &waypoints) ==
        TSC_NOT_FOUND); /* not driven by the Traffic Manager: an empty plan */
  CHECK(options == NULL && waypoints == NULL);
  int32_t next_option = -1;
  tsc_waypoint_t *next_waypoint = NULL;
  CHECK(tsc_traffic_manager_get_next_action(tm, vehicle, &next_option, &next_waypoint) ==
        TSC_NOT_FOUND); /* (Void, NULL), as ue5-dev */
  CHECK(strstr(tsc_last_error_message(), "not driven by this Traffic Manager") != NULL);
  CHECK(next_waypoint == NULL);
  CHECK_OK(tsc_vehicle_set_autopilot(vehicle, 1, 8000));
  CHECK_OK(tsc_traffic_manager_get_all_actions(tm, vehicle, &options, &count, &waypoints));
  CHECK(count == 1 && options[0] == TSC_ROAD_OPTION_LANE_FOLLOW &&
        tsc_waypoint_list_size(waypoints) == 1);
  tsc_road_options_free(options);
  tsc_handle_release(H(waypoints));
  tsc_handle_release(H(vehicle));
  tsc_handle_release(H(actor));
  tsc_handle_release(H(bp));
  tsc_handle_release(H(library));
  tsc_handle_release(H(tm));
  tsc_handle_release(H(world));
  tsc_handle_release(H(client));
}

static void test_null_arguments(void) {
  tsc_client_t *client = NULL;
  CHECK(tsc_client_create(kHost, strlen(kHost), 2000, 0, NULL) == TSC_INVALID_ARGUMENT);
  CHECK(strstr(tsc_last_error_message(), "out_client") != NULL);
  CHECK(tsc_client_create("", 0, 2000, 0, &client) == TSC_INVALID_ARGUMENT);
  CHECK(client == NULL);
  CHECK(tsc_client_set_timeout(NULL, 1.0) == TSC_INVALID_ARGUMENT);
  tsc_handle_release(NULL);
  tsc_string_free(NULL);
  CHECK(tsc_handle_kind(NULL) == TSC_KIND_INVALID);
  CHECK(tsc_actor_list_size(NULL) == 0);
}

static void test_wrong_handle_kind(void) {
  tsc_client_t *client = NULL;
  CHECK_OK(tsc_client_create(kHost, strlen(kHost), 2000, 0, &client));
  tsc_transform_t t;
  /* A client handle passed where an actor is expected must be rejected. */
  CHECK(tsc_actor_get_transform((tsc_actor_t *)client, &t) == TSC_INVALID_ARGUMENT);
  CHECK(strstr(tsc_last_error_message(), "wrong handle kind") != NULL);
  CHECK(tsc_client_set_timeout(client, -1.0) == TSC_INVALID_ARGUMENT);
  /* Non-finite or absurd timeouts must not silently become 0 ms. */
  CHECK(tsc_client_set_timeout(client, 1.0 / 0.0) == TSC_INVALID_ARGUMENT);
  CHECK(tsc_client_set_timeout(client, 0.0 / 0.0) == TSC_INVALID_ARGUMENT);
  CHECK(tsc_client_set_timeout(client, 1e30) == TSC_INVALID_ARGUMENT);
  CHECK(tsc_client_set_timeout(client, 1e9) == TSC_OK);
  tsc_handle_release(H(client));
}

static void test_refcount(void) {
  uint64_t before = tsc_live_handle_count();
  tsc_client_t *client = NULL;
  CHECK_OK(tsc_client_create(kHost, strlen(kHost), 2000, 0, &client));
  CHECK(tsc_handle_kind(H(client)) == TSC_KIND_CLIENT);
  CHECK(tsc_handle_refcount(H(client)) == 1);
  tsc_handle_retain(H(client));
  CHECK(tsc_handle_refcount(H(client)) == 2);
  CHECK(tsc_live_handle_count() == before + 1);
  tsc_handle_release(H(client));
  CHECK(tsc_live_handle_count() == before + 1);
  tsc_handle_release(H(client));
  CHECK(tsc_live_handle_count() == before);
}

static void *error_thread(void *arg) {
  (void)arg;
  CHECK(tsc_client_set_timeout(NULL, 1.0) == TSC_INVALID_ARGUMENT);
  return NULL;
}

static void test_thread_local_error(void) {
  tsc_clear_last_error();
  pthread_t thread;
  pthread_create(&thread, NULL, error_thread, NULL);
  pthread_join(thread, NULL);
  /* The other thread's error must not be visible here. */
  CHECK(strcmp(tsc_last_error_message(), "") == 0);
}

static void test_mock_session(void) {
  uint64_t before = tsc_live_handle_count();
  tsc_client_t *client = NULL;
  tsc_world_t *world = NULL;
  tsc_actor_list_t *actors = NULL;
  tsc_actor_t *actor = NULL;
  tsc_actor_t *spectator = NULL;
  tsc_vehicle_t *vehicle = NULL;
  tsc_vehicle_t *not_vehicle = (tsc_vehicle_t *)0x1;
  tsc_string_t s = {0};

  CHECK_OK(tsc_client_create(kHost, strlen(kHost), 2101, 0, &client));
  CHECK_OK(tsc_client_set_timeout(client, 2.5));
  double timeout = 0.0;
  CHECK_OK(tsc_client_get_timeout(client, &timeout));
  CHECK(timeout == 2.5);
  CHECK_OK(tsc_client_get_server_version(client, &s));
  CHECK(s.data != NULL && strlen(s.data) == s.size);
  tsc_string_free(&s);
  CHECK(s.data == NULL && s.size == 0);
  tsc_string_free(&s); /* idempotent */

  CHECK_OK(tsc_client_get_world(client, &world));
  CHECK_OK(tsc_world_get_actors(world, &actors));
  CHECK(tsc_actor_list_size(actors) == 4); /* vehicle, spectator, traffic light, stop sign */
  CHECK(tsc_actor_list_get(actors, 99, &actor) == TSC_NOT_FOUND);
  CHECK(actor == NULL);

  CHECK_OK(tsc_actor_list_get(actors, 0, &actor));
  CHECK(tsc_handle_kind(H(actor)) == TSC_KIND_VEHICLE);
  CHECK_OK(tsc_actor_as_vehicle(actor, &vehicle));
  CHECK((void *)vehicle == (void *)actor);
  CHECK(tsc_handle_refcount(H(actor)) == 2);

  CHECK_OK(tsc_actor_list_get(actors, 1, &spectator));
  CHECK(tsc_handle_kind(H(spectator)) == TSC_KIND_ACTOR);
  CHECK(tsc_actor_as_vehicle(spectator, &not_vehicle) == TSC_TYPE_ERROR);
  CHECK(not_vehicle == NULL);
  CHECK(strstr(tsc_last_error_message(), "is not a vehicle") != NULL);
  /* A non-vehicle actor must be rejected by vehicle functions. */
  tsc_vehicle_control_t control = {0.5, 0.0, 0.0, 0, 0, 0, 0};
  CHECK(tsc_vehicle_apply_control((tsc_vehicle_t *)spectator, &control) == TSC_INVALID_ARGUMENT);

  /* A vehicle handle is a valid actor handle. */
  tsc_transform_t t;
  CHECK_OK(tsc_actor_get_transform((tsc_actor_t *)vehicle, &t));
  t.location.x = 10.0;
  t.rotation.yaw = 0.0;
  CHECK_OK(tsc_actor_set_transform((tsc_actor_t *)vehicle, &t));
  CHECK_OK(tsc_vehicle_apply_control(vehicle, &control));
  tsc_vehicle_control_t got;
  CHECK_OK(tsc_vehicle_get_control(vehicle, &got));
  CHECK(got.throttle == 0.5 && got.gear == 0);
  control.throttle = 1.5;
  CHECK(tsc_vehicle_apply_control(vehicle, &control) == TSC_INVALID_ARGUMENT);

  uint64_t frame = 0;
  CHECK_OK(tsc_world_tick(world, 1.0, &frame));
  CHECK(frame > 0);
  /* Outputs are checked before the call: a NULL output fails without ticking,
   * and is reported even when another argument is invalid too. */
  uint64_t frame_before = frame;
  CHECK(tsc_world_tick(world, 1.0, NULL) == TSC_INVALID_ARGUMENT);
  CHECK(strstr(tsc_last_error_message(), "out_frame must not be NULL") != NULL);
  CHECK(tsc_world_tick(world, 0.0 / 0.0, NULL) == TSC_INVALID_ARGUMENT);
  CHECK(strstr(tsc_last_error_message(), "out_frame must not be NULL") != NULL);
  CHECK_OK(tsc_world_tick(world, 1.0, &frame));
  CHECK(frame == frame_before + 1);
  tsc_location_t loc;
  CHECK_OK(tsc_actor_get_location(actor, &loc));
  CHECK(loc.x > 10.0);

  /* Blueprints, spawning and destruction. */
  tsc_blueprint_library_t *library = NULL;
  tsc_actor_blueprint_t *bp = NULL;
  tsc_actor_t *spawned = NULL;
  CHECK_OK(tsc_world_get_blueprint_library(world, &library));
  CHECK(tsc_blueprint_library_find(library, "nope", 4, &bp) == TSC_NOT_FOUND);
  const char *id = "vehicle.audi.tt";
  CHECK_OK(tsc_blueprint_library_find(library, id, strlen(id), &bp));
  CHECK(tsc_actor_blueprint_set_attribute(bp, "number_of_wheels", 16, "3", 1) ==
        TSC_INVALID_ARGUMENT);
  CHECK(tsc_actor_blueprint_set_attribute(bp, "color", 5, "1,2", 3) == TSC_INVALID_ARGUMENT);
  CHECK_OK(tsc_actor_blueprint_set_attribute(bp, "color", 5, "1,2,3", 5));
  tsc_actor_attribute_t attribute;
  CHECK_OK(tsc_actor_blueprint_get_attribute(bp, "color", 5, &attribute));
  CHECK(strcmp(attribute.value.data, "1,2,3") == 0);
  CHECK(attribute.type == TSC_ATTRIBUTE_RGB_COLOR);
  tsc_actor_attribute_free(&attribute);
  tsc_transform_t spawn = {{100.0, 0.0, 0.5}, {0.0, 90.0, 0.0}};
  CHECK_OK(tsc_world_spawn_actor(world, bp, &spawn, NULL, TSC_ATTACHMENT_RIGID, &spawned));
  CHECK(spawned != NULL);
  tsc_actor_t *blocked = (tsc_actor_t *)0x1;
  CHECK_OK(tsc_world_try_spawn_actor(world, bp, &spawn, NULL, TSC_ATTACHMENT_RIGID, &blocked));
  CHECK(blocked == NULL);
  CHECK(tsc_world_spawn_actor(world, bp, &spawn, NULL, TSC_ATTACHMENT_RIGID, &blocked) == TSC_ERROR);
  int32_t destroyed = 0;
  CHECK_OK(tsc_actor_destroy(spawned, &destroyed));
  CHECK(destroyed == 1);
  CHECK(tsc_actor_get_transform(spawned, &t) == TSC_ERROR);

  /* Settings round trip. */
  tsc_world_settings_t settings;
  CHECK_OK(tsc_world_get_settings(world, &settings));
  CHECK(settings.has_fixed_delta_seconds == 0);
  settings.synchronous_mode = 1;
  settings.has_fixed_delta_seconds = 1;
  settings.fixed_delta_seconds = 0.1;
  CHECK_OK(tsc_world_apply_settings(world, &settings, 1.0, &frame));
  tsc_world_settings_t again;
  CHECK_OK(tsc_world_get_settings(world, &again));
  CHECK(again.synchronous_mode == 1 && again.fixed_delta_seconds == 0.1);
  settings.fixed_delta_seconds = -1.0;
  CHECK(tsc_world_apply_settings(world, &settings, 1.0, &frame) == TSC_INVALID_ARGUMENT);

  tsc_handle_release(H(spawned));
  tsc_handle_release(H(bp));
  tsc_handle_release(H(library));
  tsc_handle_release(H(spectator));
  tsc_handle_release(H(vehicle));
  tsc_handle_release(H(actor));
  tsc_handle_release(H(actors));
  tsc_handle_release(H(world));
  tsc_handle_release(H(client));
  CHECK(tsc_live_handle_count() == before);
}

/* Full physics control round trip on a mock vehicle (ABI 3.0): every kind of
 * field (float, uint8 code, bool, vector, curve, gear ratios, wheels and their
 * slip graphs) is read, modified, applied and read back. */
static void check_physics_control(tsc_vehicle_t *vehicle) {
  uint64_t before = tsc_live_handle_count();
  tsc_vehicle_physics_control_t pc;
  tsc_physics_control_t *snap = NULL;
  CHECK(tsc_vehicle_get_physics_control(vehicle, NULL) == TSC_INVALID_ARGUMENT);
  CHECK(tsc_physics_control_view(NULL, &pc) == TSC_INVALID_ARGUMENT);
  CHECK(tsc_physics_control_view((const tsc_physics_control_t *)vehicle, &pc) ==
        TSC_INVALID_ARGUMENT); /* wrong handle kind */
  CHECK(tsc_vehicle_apply_physics_control(vehicle, NULL) == TSC_INVALID_ARGUMENT);
  CHECK_OK(tsc_vehicle_get_physics_control(vehicle, &snap));
  CHECK(tsc_handle_kind(H(snap)) == TSC_KIND_PHYSICS_CONTROL);
  CHECK(tsc_live_handle_count() == before + 1);
  CHECK(tsc_physics_control_view(snap, NULL) == TSC_INVALID_ARGUMENT);
  CHECK_OK(tsc_physics_control_view(snap, &pc));
  /* LibCarla's defaults, the mock's mass and wheels. */
  CHECK(pc.mass == 1500.0 && pc.max_torque == 300.0 && pc.idle_rpm == 1.0);
  CHECK(pc.use_automatic_gears == 1 && pc.use_sweep_wheel_collision == 0);
  CHECK(pc.differential_type == 0 && pc.inertia_tensor_scale.z == 1.0);
  CHECK(pc.torque_curve_size == 2 && pc.torque_curve[1].x == 5000.0 &&
        pc.torque_curve[1].y == 500.0);
  CHECK(pc.steering_curve_size == 2 && pc.steering_curve[1].y == 0.5);
  CHECK(pc.forward_gear_ratios_size == 8 && pc.forward_gear_ratios[3] == 1.0);
  CHECK(pc.reverse_gear_ratios_size == 2);
  CHECK(pc.wheel_count == 4);
  CHECK(pc.wheels[3].wheel_index == 3 && pc.wheels[3].axle_type == 2);
  CHECK(pc.wheels[0].suspension_axis.z == -1.0 && pc.wheels[0].affected_by_steering == 1);
  CHECK(pc.wheels[0].lateral_slip_graph_size == 2 &&
        pc.wheels[0].lateral_slip_graph[1].x == 5.0);

  /* Modify a copy in caller memory; the snapshot's arrays are read-only. */
  tsc_vector2d_t torque[3] = {{0.0, 400.0}, {3000.0, 600.0}, {6000.0, 350.0}};
  double forward[3] = {3.5, 2.0, 1.25};
  tsc_vector2d_t slip[1] = {{2.0, 0.75}};
  tsc_wheel_physics_control_t wheels[4];
  memcpy(wheels, pc.wheels, sizeof wheels);
  wheels[1].cornering_stiffness = 1250.0;
  wheels[1].abs_enabled = 1;
  wheels[1].sweep_type = 2;
  wheels[1].suspension_smoothing = 3;
  wheels[1].offset.y = 90.5;
  wheels[1].lateral_slip_graph = slip;
  wheels[1].lateral_slip_graph_size = 1;
  pc.wheels = wheels;
  pc.torque_curve = torque;
  pc.torque_curve_size = 3;
  pc.forward_gear_ratios = forward;
  pc.forward_gear_ratios_size = 3;
  pc.reverse_gear_ratios = NULL; /* empty */
  pc.reverse_gear_ratios_size = 0;
  pc.mass = 2000.0;
  pc.drag_area = 2.5;
  pc.differential_type = 3;
  pc.use_sweep_wheel_collision = 1;
  pc.center_of_mass.z = -0.25;
  CHECK_OK(tsc_vehicle_apply_physics_control(vehicle, &pc));
  tsc_handle_release(H(snap));
  snap = NULL;

  CHECK_OK(tsc_vehicle_get_physics_control(vehicle, &snap));
  CHECK_OK(tsc_physics_control_view(snap, &pc));
  CHECK(pc.mass == 2000.0 && pc.drag_area == 2.5 && pc.differential_type == 3);
  CHECK(pc.use_sweep_wheel_collision == 1 && pc.center_of_mass.z == -0.25);
  CHECK(pc.max_torque == 300.0 && pc.steering_curve_size == 2); /* untouched */
  CHECK(pc.torque_curve_size == 3 && pc.torque_curve[2].x == 6000.0);
  CHECK(pc.forward_gear_ratios_size == 3 && pc.forward_gear_ratios[0] == 3.5);
  CHECK(pc.reverse_gear_ratios_size == 0);
  CHECK(pc.wheel_count == 4 && pc.wheels[1].cornering_stiffness == 1250.0);
  CHECK(pc.wheels[1].abs_enabled == 1 && pc.wheels[1].sweep_type == 2);
  CHECK(pc.wheels[1].suspension_smoothing == 3 && pc.wheels[1].offset.y == 90.5);
  CHECK(pc.wheels[1].lateral_slip_graph_size == 1 &&
        pc.wheels[1].lateral_slip_graph[0].y == 0.75);
  CHECK(pc.wheels[0].cornering_stiffness == 1000.0 && pc.wheels[0].abs_enabled == 0);

  /* Invalid input is rejected and changes nothing. */
  memcpy(wheels, pc.wheels, sizeof wheels);
  pc.wheels = wheels;
  pc.wheel_count = 3;
  CHECK(tsc_vehicle_apply_physics_control(vehicle, &pc) == TSC_INVALID_ARGUMENT);
  CHECK(strstr(tsc_last_error_message(), "wheels") != NULL);
  pc.wheel_count = 4;
  pc.differential_type = 256;
  CHECK(tsc_vehicle_apply_physics_control(vehicle, &pc) == TSC_INVALID_ARGUMENT);
  CHECK(strstr(tsc_last_error_message(), "differential_type") != NULL);
  pc.differential_type = 0;
  wheels[2].axle_type = -1;
  CHECK(tsc_vehicle_apply_physics_control(vehicle, &pc) == TSC_INVALID_ARGUMENT);
  wheels[2].axle_type = 0;
  pc.torque_curve = NULL; /* size is still 3 */
  CHECK(tsc_vehicle_apply_physics_control(vehicle, &pc) == TSC_INVALID_ARGUMENT);
  pc.torque_curve = torque;
  pc.mass = 0.0;
  CHECK(tsc_vehicle_apply_physics_control(vehicle, &pc) == TSC_INVALID_ARGUMENT);
  pc.mass = 2000.0;
  wheels[0].wheel_radius = 0.0 / 0.0; /* NaN */
  CHECK(tsc_vehicle_apply_physics_control(vehicle, &pc) == TSC_INVALID_ARGUMENT);
  CHECK(strstr(tsc_last_error_message(), "wheel_radius") != NULL);
  /* Finite doubles beyond float range would become +-inf in LibCarla's float. */
  wheels[0].wheel_radius = -1e300;
  CHECK(tsc_vehicle_apply_physics_control(vehicle, &pc) == TSC_INVALID_ARGUMENT);
  CHECK(strstr(tsc_last_error_message(), "wheel_radius") != NULL);
  wheels[0].wheel_radius = 30.0;
  pc.mass = 1e39;
  CHECK(tsc_vehicle_apply_physics_control(vehicle, &pc) == TSC_INVALID_ARGUMENT);
  CHECK(strstr(tsc_last_error_message(), "mass") != NULL);
  pc.mass = 2000.0;
  torque[1].y = 1e39;
  CHECK(tsc_vehicle_apply_physics_control(vehicle, &pc) == TSC_INVALID_ARGUMENT);
  CHECK(strstr(tsc_last_error_message(), "torque_curve") != NULL);
  torque[1].y = 600.0;
  pc.forward_gear_ratios = forward;
  forward[0] = -1e39;
  CHECK(tsc_vehicle_apply_physics_control(vehicle, &pc) == TSC_INVALID_ARGUMENT);
  CHECK(strstr(tsc_last_error_message(), "forward_gear_ratios") != NULL);
  forward[0] = 3.5;
  pc.center_of_mass.y = 1e39;
  CHECK(tsc_vehicle_apply_physics_control(vehicle, &pc) == TSC_INVALID_ARGUMENT);
  CHECK(strstr(tsc_last_error_message(), "center_of_mass") != NULL);
  pc.center_of_mass.y = 0.0;
  tsc_handle_release(H(snap));
  CHECK_OK(tsc_vehicle_get_physics_control(vehicle, &snap));
  CHECK_OK(tsc_physics_control_view(snap, &pc));
  CHECK(pc.differential_type == 3 && pc.wheels[0].wheel_radius == 30.0);
  tsc_handle_release(H(snap));
  CHECK(tsc_live_handle_count() == before);
}

static void test_mock_milestone1(void) {
  uint64_t before = tsc_live_handle_count();
  tsc_client_t *client = NULL;
  tsc_world_t *world = NULL;
  tsc_map_t *map = NULL;
  CHECK_OK(tsc_client_create(kHost, strlen(kHost), 2102, 0, &client));
  CHECK_OK(tsc_client_get_world(client, &world));
  CHECK_OK(tsc_world_get_map(world, &map));

  /* Spawn points: two-call pattern. */
  size_t n = 0;
  CHECK_OK(tsc_map_get_spawn_points(map, NULL, 0, &n));
  CHECK(n == 6);
  tsc_transform_t points[6];
  size_t got = 0;
  CHECK_OK(tsc_map_get_spawn_points(map, points, 2, &got));
  CHECK(got == 6); /* total count, even when capacity is smaller */
  CHECK_OK(tsc_map_get_spawn_points(map, points, 6, &got));

  /* Waypoints. */
  tsc_location_t on_road = {12.0, 0.3, 0.0};
  tsc_waypoint_t *wp = NULL;
  CHECK_OK(tsc_map_get_waypoint(map, &on_road, 1, 2 /* Driving */, &wp));
  CHECK(wp != NULL);
  tsc_waypoint_info_t info;
  CHECK_OK(tsc_waypoint_get_info(wp, &info));
  CHECK(info.lane_id == -1 && info.road_id == 1 && info.s == 12.0);
  tsc_waypoint_list_t *next = NULL;
  CHECK_OK(tsc_waypoint_next(wp, 5.0, &next));
  CHECK(tsc_waypoint_list_size(next) == 1);
  tsc_waypoint_list_t *bad = (tsc_waypoint_list_t *)0x1;
  CHECK(tsc_waypoint_next(wp, -1.0, &bad) == TSC_INVALID_ARGUMENT);
  CHECK(bad == NULL);
  tsc_waypoint_t *left = (tsc_waypoint_t *)0x1;
  CHECK_OK(tsc_waypoint_get_left_lane(wp, &left));
  CHECK(left == NULL);
  tsc_location_t off_road = {500.0, 0.0, 0.0};
  tsc_waypoint_t *none = (tsc_waypoint_t *)0x1;
  CHECK_OK(tsc_map_get_waypoint(map, &off_road, 1, 2, &none));
  CHECK(none == NULL);

  /* Snapshots. */
  tsc_world_snapshot_t *snap = NULL;
  CHECK_OK(tsc_world_get_snapshot(world, &snap));
  CHECK(tsc_world_snapshot_size(snap) == 4);
  tsc_actor_snapshot_t a;
  CHECK_OK(tsc_world_snapshot_get(snap, 0, &a));
  CHECK_OK(tsc_world_snapshot_find(snap, a.id, &a));
  CHECK(tsc_world_snapshot_find(snap, 999, &a) == TSC_NOT_FOUND);

  /* Batch: spawn + then(autopilot), a failing destroy, a dangling then_of. */
  tsc_blueprint_library_t *library = NULL;
  tsc_actor_blueprint_t *bp = NULL;
  CHECK_OK(tsc_world_get_blueprint_library(world, &library));
  CHECK_OK(tsc_blueprint_library_find(library, "vehicle.audi.tt", 15, &bp));
  tsc_command_t cmds[3];
  memset(cmds, 0, sizeof cmds);
  cmds[0].type = TSC_COMMAND_SPAWN_ACTOR;
  cmds[0].then_of = -1;
  cmds[0].blueprint = bp;
  cmds[0].transform = points[2];
  cmds[1].type = TSC_COMMAND_SET_AUTOPILOT;
  cmds[1].then_of = 0;
  cmds[1].flag = 1;
  cmds[1].tm_port = 8000;
  cmds[2].type = TSC_COMMAND_DESTROY_ACTOR;
  cmds[2].then_of = -1;
  cmds[2].actor_id = 999;
  tsc_command_response_t responses[2];
  size_t count = 0;
  CHECK(tsc_client_apply_batch_sync(client, cmds, 3, 0, responses, 1, &count) ==
        TSC_INVALID_ARGUMENT); /* not enough room: 2 top-level commands */
  CHECK_OK(tsc_client_apply_batch_sync(client, cmds, 3, 1, responses, 2, &count));
  CHECK(count == 2);
  CHECK(!responses[0].has_error && responses[0].actor_id != 0);
  CHECK(responses[1].has_error && strstr(responses[1].error.data, "not found") != NULL);
  for (size_t i = 0; i < count; ++i) tsc_string_free(&responses[i].error);
  cmds[2].then_of = 2; /* must name an earlier SpawnActor */
  CHECK(tsc_client_apply_batch_sync(client, cmds, 3, 0, responses, 2, &count) ==
        TSC_INVALID_ARGUMENT);

  /* Physics control and bounding box on the spawned vehicle. */
  tsc_actor_t *actor = NULL;
  tsc_vehicle_t *vehicle = NULL;
  CHECK_OK(tsc_world_get_actor(world, responses[0].actor_id, &actor));
  CHECK_OK(tsc_actor_as_vehicle(actor, &vehicle));
  check_physics_control(vehicle);
  tsc_bounding_box_t box;
  CHECK_OK(tsc_actor_get_bounding_box(actor, &box));
  CHECK(box.extent.x > 1.0);

  tsc_handle_release(H(vehicle));
  tsc_handle_release(H(actor));
  tsc_handle_release(H(bp));
  tsc_handle_release(H(library));
  tsc_handle_release(H(snap));
  tsc_handle_release(H(next));
  tsc_handle_release(H(wp));
  tsc_handle_release(H(map));
  tsc_handle_release(H(world));
  tsc_handle_release(H(client));
  CHECK(tsc_live_handle_count() == before);
}

static void test_mock_sensors(void) {
  uint64_t before = tsc_live_handle_count();
  tsc_client_t *client = NULL;
  tsc_world_t *world = NULL;
  tsc_actor_list_t *actors = NULL;
  tsc_actor_t *vehicle = NULL;
  tsc_blueprint_library_t *library = NULL;
  CHECK_OK(tsc_client_create(kHost, strlen(kHost), 2103, 0, &client));
  CHECK_OK(tsc_client_get_world(client, &world));
  CHECK_OK(tsc_world_get_actors(world, &actors));
  CHECK_OK(tsc_actor_list_get(actors, 0, &vehicle));
  CHECK_OK(tsc_world_get_blueprint_library(world, &library));

  tsc_actor_blueprint_t *bp = NULL;
  CHECK_OK(tsc_blueprint_library_find(library, "sensor.camera.rgb", 17, &bp));
  CHECK_OK(tsc_actor_blueprint_set_attribute(bp, "image_size_x", 12, "8", 1));
  CHECK_OK(tsc_actor_blueprint_set_attribute(bp, "image_size_y", 12, "4", 1));
  tsc_transform_t at = {{0, 0, 2}, {0, 0, 0}};
  tsc_actor_t *actor = NULL;
  tsc_sensor_t *camera = NULL;
  CHECK_OK(tsc_world_spawn_actor(world, bp, &at, vehicle, TSC_ATTACHMENT_RIGID, &actor));
  CHECK(tsc_handle_kind(H(actor)) == TSC_KIND_SENSOR);

  /* Attachment types (issue #34): SpringArm attaches; out-of-range values fail,
   * from try_spawn too, and leave the output NULL. */
  {
    tsc_actor_t *arm = NULL, *arm_parent = NULL, *bad = (tsc_actor_t *)0x1;
    uint32_t vehicle_id = 0, parent_id = 1;
    int32_t destroyed = 0;
    CHECK_OK(tsc_world_spawn_actor(world, bp, &at, vehicle, TSC_ATTACHMENT_SPRING_ARM, &arm));
    CHECK_OK(tsc_actor_get_parent(arm, &arm_parent));
    CHECK(arm_parent != NULL);
    CHECK_OK(tsc_actor_get_id(vehicle, &vehicle_id));
    CHECK_OK(tsc_actor_get_id(arm_parent, &parent_id));
    CHECK(parent_id == vehicle_id);
    CHECK(tsc_world_spawn_actor(world, bp, &at, vehicle, 3, &bad) ==
              TSC_INVALID_ARGUMENT && bad == NULL);
    bad = (tsc_actor_t *)0x1;
    CHECK(tsc_world_try_spawn_actor(world, bp, &at, vehicle, -1, &bad) ==
              TSC_INVALID_ARGUMENT && bad == NULL);
    CHECK_OK(tsc_actor_destroy(arm, &destroyed));
    CHECK(destroyed == 1);
    tsc_handle_release(H(arm_parent));
    tsc_handle_release(H(arm));
  }
  CHECK_OK(tsc_actor_as_sensor(actor, &camera));
  tsc_sensor_t *not_sensor = (tsc_sensor_t *)0x1;
  CHECK(tsc_actor_as_sensor(vehicle, &not_sensor) == TSC_TYPE_ERROR && not_sensor == NULL);
  /* A sensor handle is an actor handle. */
  tsc_transform_t t;
  CHECK_OK(tsc_actor_get_transform((tsc_actor_t *)camera, &t));

  tsc_sensor_data_t *data = (tsc_sensor_data_t *)0x1;
  CHECK(tsc_sensor_poll(camera, &data) == TSC_ERROR); /* not listening yet */
  size_t pending = 1;
  CHECK(tsc_sensor_pending_count(camera, &pending) == TSC_ERROR); /* not listening yet */
  CHECK_OK(tsc_sensor_listen(camera, 2));
  CHECK(tsc_sensor_pending_count(camera, NULL) == TSC_INVALID_ARGUMENT);
  int32_t listening = 0;
  CHECK_OK(tsc_sensor_is_listening(camera, &listening));
  CHECK(listening == 1);
  CHECK_OK(tsc_sensor_poll(camera, &data));
  CHECK(data == NULL);
  CHECK(tsc_sensor_wait_for_data(camera, 0.01, &data) == TSC_TIMEOUT);

  uint64_t frame = 0;
  for (int i = 0; i < 3; ++i) CHECK_OK(tsc_world_tick(world, 1.0, &frame));
  uint64_t dropped = 0;
  CHECK_OK(tsc_sensor_dropped_count(camera, &dropped));
  CHECK(dropped == 1); /* capacity 2, three frames */
  CHECK_OK(tsc_sensor_pending_count(camera, &pending));
  CHECK(pending == 2);
  CHECK_OK(tsc_sensor_wait_for_data(camera, 1.0, &data));
  CHECK_OK(tsc_sensor_pending_count(camera, &pending));
  CHECK(pending == 1);
  tsc_sensor_data_info_t info;
  CHECK_OK(tsc_sensor_data_get_info(data, &info));
  CHECK(info.type == TSC_SENSOR_DATA_IMAGE && info.frame == frame - 1);
  tsc_image_t image;
  CHECK_OK(tsc_sensor_data_as_image(data, &image));
  CHECK(image.width == 8 && image.height == 4 && image.size == 8 * 4 * 4);
  CHECK(image.data[4 * 3] == 3 && image.data[4 * 8 + 1] == 1); /* B = x, G = y */
  tsc_lidar_t lidar;
  CHECK(tsc_sensor_data_as_lidar(data, &lidar) == TSC_TYPE_ERROR);
  /* Issue #24: conversions are checked; an image has no event actors. */
  tsc_radar_t radar;
  CHECK(tsc_sensor_data_as_radar(data, &radar) == TSC_TYPE_ERROR);
  double distance = 0.0;
  CHECK(tsc_obstacle_detection_get_distance(data, &distance) == TSC_TYPE_ERROR);
  tsc_actor_t *event_actor = (tsc_actor_t *)0x1;
  CHECK(tsc_collision_event_get_actor(data, &event_actor) == TSC_TYPE_ERROR && event_actor == NULL);
  tsc_string_t written = {0};
  CHECK(tsc_point_cloud_save_to_disk(data, "x.ply", 5, &written) == TSC_TYPE_ERROR);
  CHECK(tsc_image_convert(data, 4) == TSC_INVALID_ARGUMENT);
  CHECK(tsc_image_convert(data, TSC_COLOR_CONVERTER_RAW) == TSC_OK);
  CHECK(image.data[4 * 3] == 3); /* Raw leaves the pixels unchanged */
  CHECK(tsc_image_save_to_disk(data, "x.png", 5, -1, &written) == TSC_INVALID_ARGUMENT);
  CHECK(written.data == NULL);

  CHECK_OK(tsc_sensor_stop(camera));
  CHECK_OK(tsc_sensor_is_listening(camera, &listening));
  CHECK(listening == 0);

  /* Capacity 0: unbounded, nothing is dropped; arrival order is kept. */
  CHECK_OK(tsc_sensor_listen(camera, 0));
  CHECK_OK(tsc_sensor_pending_count(camera, &pending));
  CHECK(pending == 0); /* listening again replaces the queue */
  uint64_t first = frame + 1;
  for (int i = 0; i < 100; ++i) CHECK_OK(tsc_world_tick(world, 1.0, &frame));
  CHECK_OK(tsc_sensor_dropped_count(camera, &dropped));
  CHECK(dropped == 0);
  CHECK_OK(tsc_sensor_pending_count(camera, &pending));
  CHECK(pending == 100);
  for (uint64_t f = first; f <= frame; ++f) {
    tsc_sensor_data_t *d = NULL;
    CHECK_OK(tsc_sensor_poll(camera, &d));
    CHECK(d != NULL);
    CHECK_OK(tsc_sensor_data_get_info(d, &info));
    CHECK(info.frame == f);
    tsc_handle_release(H(d));
  }
  CHECK_OK(tsc_sensor_pending_count(camera, &pending));
  CHECK(pending == 0);
  CHECK_OK(tsc_sensor_stop(camera));

  tsc_handle_release(H(data));
  tsc_handle_release(H(camera));
  tsc_handle_release(H(actor));
  tsc_handle_release(H(bp));
  tsc_handle_release(H(library));
  tsc_handle_release(H(vehicle));
  tsc_handle_release(H(actors));
  tsc_handle_release(H(world));
  tsc_handle_release(H(client));
  CHECK(tsc_live_handle_count() == before);
}

static void test_mock_milestone4(void) {
  uint64_t before = tsc_live_handle_count();
  tsc_client_t *client = NULL;
  tsc_world_t *world = NULL;
  tsc_actor_list_t *actors = NULL;
  tsc_actor_t *vehicle_actor = NULL, *light_actor = NULL, *walker_actor = NULL, *ai_actor = NULL;
  tsc_vehicle_t *vehicle = NULL;
  tsc_traffic_light_t *light = NULL;
  tsc_walker_t *walker = NULL;
  tsc_walker_ai_controller_t *ai = NULL;
  tsc_blueprint_library_t *library = NULL;
  tsc_actor_blueprint_t *walker_bp = NULL, *ai_bp = NULL;
  CHECK_OK(tsc_client_create(kHost, strlen(kHost), 2104, 0, &client));
  CHECK_OK(tsc_client_get_world(client, &world));
  CHECK_OK(tsc_world_get_actors(world, &actors));
  CHECK_OK(tsc_actor_list_get(actors, 0, &vehicle_actor));
  CHECK_OK(tsc_actor_as_vehicle(vehicle_actor, &vehicle));
  CHECK_OK(tsc_actor_list_get(actors, 2, &light_actor));
  CHECK(tsc_handle_kind(H(light_actor)) == TSC_KIND_TRAFFIC_LIGHT);
  CHECK_OK(tsc_actor_as_traffic_light(light_actor, &light));
  tsc_walker_t *not_walker = (tsc_walker_t *)0x1;
  CHECK(tsc_actor_as_walker(vehicle_actor, &not_walker) == TSC_TYPE_ERROR && not_walker == NULL);

  /* Weather */
  tsc_weather_t w;
  CHECK_OK(tsc_weather_preset("HardRainNoon", 12, &w));
  CHECK(w.precipitation == 100.0 && w.sun_altitude_angle == 45.0);
  CHECK(tsc_weather_preset("Nope", 4, &w) == TSC_NOT_FOUND);
  CHECK_OK(tsc_weather_preset("HardRainNoon", 12, &w));
  CHECK_OK(tsc_world_set_weather(world, &w));
  tsc_weather_t got;
  CHECK_OK(tsc_world_get_weather(world, &got));
  CHECK(got.precipitation == 100.0);

  /* Traffic light: frozen state, and a vehicle next to it sees it. */
  tsc_traffic_light_info_t info;
  CHECK_OK(tsc_traffic_light_set_state(light, TSC_TRAFFIC_LIGHT_RED));
  CHECK_OK(tsc_traffic_light_freeze(light, 1));
  CHECK(tsc_traffic_light_set_state(light, 9) == TSC_INVALID_ARGUMENT);
  CHECK(tsc_traffic_light_set_green_time(light, -1.0) == TSC_INVALID_ARGUMENT);
  CHECK_OK(tsc_traffic_light_get_info(light, &info));
  CHECK(info.state == TSC_TRAFFIC_LIGHT_RED && info.is_frozen == 1 && info.green_time == 10.0);
  tsc_location_t near_light = {95.0, 0.0, 0.5};
  CHECK_OK(tsc_actor_set_location(vehicle_actor, &near_light));
  int32_t at = 0, state = -1;
  CHECK_OK(tsc_vehicle_is_at_traffic_light(vehicle, &at));
  CHECK_OK(tsc_vehicle_get_traffic_light_state(vehicle, &state));
  CHECK(at == 1 && state == TSC_TRAFFIC_LIGHT_RED);
  tsc_traffic_light_t *seen = NULL;
  CHECK_OK(tsc_vehicle_get_traffic_light(vehicle, &seen));
  CHECK(seen != NULL);
  tsc_handle_release(H(seen));

  /* Vehicle lights */
  uint32_t lights = 0;
  CHECK_OK(tsc_vehicle_set_light_state(vehicle, 0x1 | 0x8));
  CHECK_OK(tsc_vehicle_get_light_state(vehicle, &lights));
  CHECK(lights == (0x1 | 0x8));

  /* Walker with an AI controller walking to a destination. */
  CHECK_OK(tsc_world_get_blueprint_library(world, &library));
  CHECK_OK(tsc_blueprint_library_find(library, "walker.pedestrian.0001", 22, &walker_bp));
  CHECK_OK(tsc_blueprint_library_find(library, "controller.ai.walker", 20, &ai_bp));
  tsc_location_t nav;
  int32_t found = 0;
  CHECK_OK(tsc_world_get_random_location_from_navigation(world, &nav, &found));
  CHECK(found == 1);
  tsc_transform_t at_nav = {nav, {0, 0, 0}};
  tsc_transform_t origin = {{0, 0, 0}, {0, 0, 0}};
  CHECK_OK(tsc_world_spawn_actor(world, walker_bp, &at_nav, NULL, TSC_ATTACHMENT_RIGID,
                                 &walker_actor));
  CHECK_OK(tsc_actor_as_walker(walker_actor, &walker));
  CHECK_OK(tsc_world_spawn_actor(world, ai_bp, &origin, walker_actor, TSC_ATTACHMENT_RIGID,
                                 &ai_actor));
  CHECK_OK(tsc_actor_as_walker_ai_controller(ai_actor, &ai));
  tsc_location_t target = {nav.x + 3.0, nav.y, nav.z};
  CHECK_OK(tsc_walker_ai_controller_start(ai));
  CHECK_OK(tsc_walker_ai_controller_go_to_location(ai, &target));
  CHECK_OK(tsc_walker_ai_controller_set_max_speed(ai, 2.0));
  uint64_t frame = 0;
  for (int i = 0; i < 60; ++i) CHECK_OK(tsc_world_tick(world, 1.0, &frame));
  tsc_location_t walked;
  CHECK_OK(tsc_actor_get_location(walker_actor, &walked));
  CHECK(walked.x > nav.x + 2.9);
  tsc_walker_control_t wc = {{0, 1, 0}, -1.0, 0, 0};
  CHECK(tsc_walker_apply_control(walker, &wc) == TSC_INVALID_ARGUMENT);

  /* Batch: the ABI 2.0 commands. */
  tsc_command_t cmds[3];
  memset(cmds, 0, sizeof cmds);
  cmds[0].type = TSC_COMMAND_APPLY_WALKER_CONTROL;
  cmds[0].then_of = -1;
  cmds[0].actor_id = 0;
  CHECK_OK(tsc_actor_get_id(walker_actor, &cmds[0].actor_id));
  cmds[0].vector = (tsc_vector3d_t){0, 1, 0};
  cmds[0].scalar = 1.5;
  cmds[1].type = TSC_COMMAND_SET_TRAFFIC_LIGHT_STATE;
  cmds[1].then_of = -1;
  CHECK_OK(tsc_actor_get_id(light_actor, &cmds[1].actor_id));
  cmds[1].flag = TSC_TRAFFIC_LIGHT_GREEN;
  cmds[2].type = TSC_COMMAND_APPLY_IMPULSE;
  cmds[2].then_of = -1;
  CHECK_OK(tsc_actor_get_id(vehicle_actor, &cmds[2].actor_id));
  cmds[2].vector = (tsc_vector3d_t){3000.0, 0, 0};
  tsc_command_response_t r[3];
  size_t n = 0;
  CHECK_OK(tsc_client_apply_batch_sync(client, cmds, 3, 0, r, 3, &n));
  CHECK(n == 3 && !r[0].has_error && !r[1].has_error && !r[2].has_error);
  for (size_t i = 0; i < n; ++i) tsc_string_free(&r[i].error);
  CHECK_OK(tsc_walker_get_control(walker, &wc));
  CHECK(wc.speed == 1.5 && wc.direction.y == 1.0);
  CHECK_OK(tsc_traffic_light_get_info(light, &info));
  CHECK(info.state == TSC_TRAFFIC_LIGHT_GREEN);
  tsc_vector3d_t v;
  CHECK_OK(tsc_actor_get_velocity(vehicle_actor, &v));
  CHECK(v.x == 2.0); /* 3000 N*s / 1500 kg */

  /* Traffic Manager */
  tsc_traffic_manager_t *tm = NULL;
  uint16_t port = 0;
  CHECK_OK(tsc_client_get_traffic_manager(client, 8123, &tm));
  CHECK_OK(tsc_traffic_manager_get_port(tm, &port));
  CHECK(port == 8123);
  CHECK_OK(tsc_traffic_manager_set_synchronous_mode(tm, 1));
  CHECK_OK(tsc_traffic_manager_set_vehicle_value(tm, vehicle, TSC_TM_IGNORE_LIGHTS_PERCENTAGE, 100.0));
  CHECK(tsc_traffic_manager_set_vehicle_value(tm, vehicle, TSC_TM_IGNORE_LIGHTS_PERCENTAGE, 101.0) ==
        TSC_INVALID_ARGUMENT);
  CHECK(tsc_traffic_manager_set_vehicle_value(tm, vehicle, 99, 1.0) == TSC_INVALID_ARGUMENT);
  CHECK(tsc_traffic_manager_set_auto_lane_change(tm, (tsc_vehicle_t *)light, 1) == TSC_INVALID_ARGUMENT);

  /* Recorder */
  tsc_string_t text = {0};
  CHECK_OK(tsc_client_start_recorder(client, "rec.log", 7, 0, &text));
  CHECK(strstr(text.data, "rec.log") != NULL);
  tsc_string_free(&text);
  for (int i = 0; i < 5; ++i) CHECK_OK(tsc_world_tick(world, 1.0, &frame));
  CHECK_OK(tsc_client_stop_recorder(client));
  CHECK_OK(tsc_client_show_recorder_file_info(client, "rec.log", 7, 0, &text));
  CHECK(strstr(text.data, "Frames: 5") != NULL);
  tsc_string_free(&text);
  CHECK(tsc_client_show_recorder_file_info(client, "nope", 4, 0, &text) == TSC_ERROR);

  /* Map queries */
  tsc_map_t *map = NULL;
  tsc_waypoint_list_t *topology = NULL;
  tsc_landmark_list_t *landmarks = NULL;
  CHECK_OK(tsc_world_get_map(world, &map));
  CHECK_OK(tsc_map_get_topology(map, &topology));
  CHECK(tsc_waypoint_list_size(topology) == 4); /* 2 lanes x (begin, end) */
  size_t crosswalk_points = 0;
  CHECK_OK(tsc_map_get_crosswalks(map, NULL, 0, &crosswalk_points));
  CHECK(crosswalk_points == 5);
  CHECK_OK(tsc_map_get_all_landmarks(map, &landmarks));
  CHECK(tsc_landmark_list_size(landmarks) == 1);
  tsc_landmark_t landmark;
  CHECK_OK(tsc_landmark_list_get(landmarks, 0, &landmark));
  CHECK(strcmp(landmark.type.data, "206") == 0 && landmark.s == 100.0);
  tsc_landmark_free(&landmark);
  /* A failure zeroes *out (nothing to free); a NULL out fails first (no copy). */
  memset(&landmark, 0xAB, sizeof landmark);
  CHECK(tsc_landmark_list_get(landmarks, 1, &landmark) == TSC_NOT_FOUND);
  CHECK(landmark.id.data == NULL && landmark.text.data == NULL && landmark.s == 0.0);
  CHECK(tsc_landmark_list_get(landmarks, 0, NULL) == TSC_INVALID_ARGUMENT);
  CHECK(tsc_landmark_list_get(landmarks, 1, NULL) == TSC_INVALID_ARGUMENT);

  /* Debug drawing is accepted; OpenDRIVE worlds need an OpenDRIVE document. */
  tsc_color_t red = {255, 0, 0, 255};
  CHECK_OK(tsc_debug_draw_point(world, &nav, 0.2, &red, 1.0, 1));
  CHECK_OK(tsc_debug_draw_point(world, &nav, 0.2, &red, 1.0, 0)); /* persistent_lines = false */
  CHECK_OK(tsc_debug_draw_line(world, &nav, &nav, 0.1, &red, 1.0, 0));
  CHECK_OK(tsc_debug_draw_arrow(world, &nav, &nav, 0.1, 0.1, &red, 1.0, 0));
  tsc_bounding_box_t dbg_box = {nav, {1.0, 1.0, 1.0}, {0.0, 0.0, 0.0}};
  tsc_rotation_t dbg_rot = {0.0, 0.0, 0.0};
  CHECK_OK(tsc_debug_draw_box(world, &dbg_box, &dbg_rot, 0.1, &red, 1.0, 0));
  CHECK_OK(tsc_debug_draw_string(world, &nav, "hi", 2, 0, &red, 1.0, 1));
  CHECK_OK(tsc_debug_draw_string(world, &nav, "hi", 2, 0, &red, 1.0, 0));
  CHECK(tsc_debug_draw_point(world, &nav, 0.2, &red, 1.0 / 0.0, 1) == TSC_INVALID_ARGUMENT);
  tsc_opendrive_parameters_t params = {2.0, 50.0, 1.0, 0.6, 1, 1, 1, 0};
  tsc_world_t *odr = NULL;
  CHECK(tsc_client_generate_opendrive_world(client, "<xml/>", 6, &params, 1, &odr) == TSC_ERROR);
  CHECK(odr == NULL);

  tsc_handle_release(H(landmarks));
  tsc_handle_release(H(topology));
  tsc_handle_release(H(map));
  tsc_handle_release(H(tm));
  tsc_handle_release(H(ai));
  tsc_handle_release(H(ai_actor));
  tsc_handle_release(H(walker));
  tsc_handle_release(H(walker_actor));
  tsc_handle_release(H(ai_bp));
  tsc_handle_release(H(walker_bp));
  tsc_handle_release(H(library));
  tsc_handle_release(H(light));
  tsc_handle_release(H(light_actor));
  tsc_handle_release(H(vehicle));
  tsc_handle_release(H(vehicle_actor));
  tsc_handle_release(H(actors));
  tsc_handle_release(H(world));
  tsc_handle_release(H(client));
  CHECK(tsc_live_handle_count() == before);
}

/* Issue #22: geo-reference, XODR waypoints, landmark handles, lane markings,
 * traffic light geometry. */
static void test_mock_issue22(void) {
  uint64_t before = tsc_live_handle_count();
  tsc_client_t *client = NULL;
  tsc_world_t *world = NULL;
  tsc_map_t *map = NULL;
  CHECK_OK(tsc_client_create(kHost, strlen(kHost), 2122, 0, &client));
  CHECK_OK(tsc_client_get_world(client, &world));
  CHECK_OK(tsc_world_get_map(world, &map));

  /* Geo-reference and a round trip through the map's projection. */
  tsc_geo_location_t ref, geo;
  CHECK_OK(tsc_map_get_georeference(map, &ref));
  CHECK(ref.latitude == 35.0 && ref.longitude == 139.0 && ref.altitude == 10.0);
  tsc_geo_projection_t proj;
  CHECK_OK(tsc_map_get_geoprojection(map, &proj));
  CHECK(proj.type == TSC_GEO_PROJECTION_TM && proj.lat_0 == 35.0 && proj.k == 1.0);
  tsc_location_t p = {100.0, -50.0, 2.0}, back;
  CHECK_OK(tsc_map_transform_to_geolocation(map, &p, NULL, &geo));
  CHECK(geo.latitude < 35.0 && geo.altitude == 2.0);
  CHECK_OK(tsc_map_geolocation_to_transform(map, &geo, &proj, &back));
  CHECK(back.x > 99.99 && back.x < 100.01 && back.y < -49.99 && back.y > -50.01);
  proj.type = 7;
  CHECK(tsc_map_transform_to_geolocation(map, &p, &proj, &geo) == TSC_INVALID_ARGUMENT);
  CHECK(tsc_map_transform_to_geolocation(map, NULL, NULL, &geo) == TSC_INVALID_ARGUMENT);

  /* XODR waypoints: NULL when the lane does not exist. */
  tsc_waypoint_t *wp = NULL, *none = (tsc_waypoint_t *)0x1;
  CHECK_OK(tsc_map_get_waypoint_xodr(map, 1, -1, 20.0, &wp));
  CHECK(wp != NULL);
  CHECK_OK(tsc_map_get_waypoint_xodr(map, 1, 3, 20.0, &none));
  CHECK(none == NULL);

  /* Lane markings and lane change. */
  int32_t has = -1, lane_change = -1, rht = -1;
  tsc_lane_marking_t marking;
  CHECK_OK(tsc_waypoint_get_left_lane_marking(wp, &has, &marking));
  CHECK(has == 1 && marking.type == 2 /* Solid */ && marking.color == 4 /* Yellow */);
  CHECK_OK(tsc_waypoint_get_right_lane_marking(wp, &has, &marking));
  CHECK(has == 1 && marking.type == 1 /* Broken */ && marking.lane_change == 3 /* Both */);
  CHECK_OK(tsc_waypoint_get_lane_change(wp, &lane_change));
  CHECK(lane_change == 1 /* Right */);
  CHECK_OK(tsc_waypoint_is_rht(wp, &rht));
  CHECK(rht == 1);
  tsc_waypoint_t *far = NULL; /* the mock drives on the left beyond s = 150 */
  CHECK_OK(tsc_map_get_waypoint_xodr(map, 1, -1, 180.0, &far));
  CHECK_OK(tsc_waypoint_is_rht(far, &rht));
  CHECK(rht == 0);
  tsc_handle_release(H(far));
  CHECK(tsc_waypoint_get_left_lane_marking(wp, NULL, &marking) == TSC_INVALID_ARGUMENT);

  /* Landmarks ahead, as handles. */
  tsc_landmark_list_t *ahead = NULL, *group = NULL;
  tsc_landmark_handle_t *landmark = NULL;
  CHECK_OK(tsc_waypoint_get_landmarks(wp, 100.0, 0, &ahead));
  CHECK(tsc_landmark_list_size(ahead) == 1);
  CHECK_OK(tsc_landmark_list_get_landmark(ahead, 0, &landmark));
  CHECK(tsc_handle_kind(H(landmark)) == TSC_KIND_LANDMARK);
  tsc_waypoint_t *at = NULL;
  CHECK_OK(tsc_landmark_get_waypoint(landmark, &at));
  CHECK(at != NULL);
  tsc_lane_validity_t validity[2];
  size_t count = 0;
  CHECK_OK(tsc_landmark_get_lane_validities(landmark, validity, 2, &count));
  CHECK(count == 1 && validity[0].from_lane == -1 && validity[0].to_lane == -2);
  double h_offset = 0.0;
  CHECK_OK(tsc_landmark_get_h_offset(landmark, &h_offset));
  CHECK(h_offset == 0.25);
  CHECK_OK(tsc_map_get_landmark_group(map, landmark, &group));
  CHECK(tsc_landmark_list_size(group) == 0); /* a stop sign has no controllers */
  tsc_landmark_handle_t *missing = NULL;
  tsc_landmark_list_t *rejected = NULL;
  CHECK(tsc_landmark_list_get_landmark(ahead, 1, &missing) == TSC_NOT_FOUND && missing == NULL);
  CHECK(tsc_waypoint_get_landmarks(wp, -1.0, 0, &rejected) == TSC_INVALID_ARGUMENT &&
        rejected == NULL);
  /* A landmark handle is not a waypoint handle. */
  CHECK(tsc_waypoint_get_lane_change((const tsc_waypoint_t *)landmark, &lane_change) ==
        TSC_INVALID_ARGUMENT);

  /* Traffic light geometry. */
  tsc_actor_list_t *actors = NULL;
  tsc_actor_t *light_actor = NULL;
  tsc_traffic_light_t *light = NULL, *member = NULL;
  tsc_traffic_light_list_t *lights = NULL;
  tsc_waypoint_list_t *stops = NULL;
  tsc_string_t id = {0};
  tsc_bounding_box_t boxes[2];
  CHECK_OK(tsc_world_get_actors(world, &actors));
  CHECK_OK(tsc_actor_list_get(actors, 2, &light_actor));
  CHECK_OK(tsc_actor_as_traffic_light(light_actor, &light));
  CHECK_OK(tsc_traffic_light_get_opendrive_id(light, &id));
  CHECK(strcmp(id.data, "1000") == 0);
  tsc_string_free(&id);
  CHECK_OK(tsc_traffic_light_get_stop_waypoints(light, &stops));
  CHECK(tsc_waypoint_list_size(stops) == 2);
  CHECK_OK(tsc_traffic_light_get_group_traffic_lights(light, &lights));
  CHECK(tsc_traffic_light_list_size(lights) == 1);
  CHECK_OK(tsc_traffic_light_list_get(lights, 0, &member));
  CHECK(tsc_handle_kind(H(member)) == TSC_KIND_TRAFFIC_LIGHT);
  CHECK_OK(tsc_traffic_light_get_light_boxes(light, NULL, 0, &count));
  CHECK(count == 1);
  CHECK_OK(tsc_traffic_light_get_light_boxes(light, boxes, 2, &count));
  CHECK(count == 1 && boxes[0].extent.z > 0.0);

  tsc_handle_release(H(member));
  tsc_handle_release(H(lights));
  tsc_handle_release(H(stops));
  tsc_handle_release(H(light));
  tsc_handle_release(H(light_actor));
  tsc_handle_release(H(actors));
  tsc_handle_release(H(group));
  tsc_handle_release(H(at));
  tsc_handle_release(H(landmark));
  tsc_handle_release(H(ahead));
  tsc_handle_release(H(wp));
  tsc_handle_release(H(map));
  tsc_handle_release(H(world));
  tsc_handle_release(H(client));
  CHECK(tsc_live_handle_count() == before);
}

/* Issue #20: Ackermann control, doors, failure state, telemetry, wheel steer,
 * walker bones and poses. */
static void test_mock_issue20(void) {
  uint64_t before = tsc_live_handle_count();
  tsc_client_t *client = NULL;
  tsc_world_t *world = NULL;
  tsc_actor_list_t *actors = NULL;
  tsc_actor_t *vehicle_actor = NULL, *walker_actor = NULL;
  tsc_vehicle_t *vehicle = NULL;
  tsc_walker_t *walker = NULL;
  tsc_blueprint_library_t *library = NULL;
  tsc_actor_blueprint_t *walker_bp = NULL;
  CHECK_OK(tsc_client_create(kHost, strlen(kHost), 2120, 0, &client));
  CHECK_OK(tsc_client_get_world(client, &world));
  CHECK_OK(tsc_world_get_actors(world, &actors));
  CHECK_OK(tsc_actor_list_get(actors, 0, &vehicle_actor));
  CHECK_OK(tsc_actor_as_vehicle(vehicle_actor, &vehicle));

  /* Ackermann controller settings: server defaults, then a round trip. */
  tsc_ackermann_controller_settings_t s;
  CHECK_OK(tsc_vehicle_get_ackermann_controller_settings(vehicle, &s));
  CHECK(s.speed_kp > 0.149 && s.speed_kp < 0.151);
  tsc_ackermann_controller_settings_t s2 = {0.5, 0.1, 0.2, 0.3, 0.0, 0.05};
  CHECK_OK(tsc_vehicle_apply_ackermann_controller_settings(vehicle, &s2));
  CHECK_OK(tsc_vehicle_get_ackermann_controller_settings(vehicle, &s));
  CHECK(s.speed_kp == 0.5 && s.accel_kd > 0.0499 && s.accel_kd < 0.0501);
  s2.accel_ki = 0.0 / 0.0;
  CHECK(tsc_vehicle_apply_ackermann_controller_settings(vehicle, &s2) == TSC_INVALID_ARGUMENT);
  CHECK(tsc_vehicle_apply_ackermann_controller_settings(vehicle, NULL) == TSC_INVALID_ARGUMENT);

  /* Ackermann control drives the vehicle towards the target speed. */
  tsc_vehicle_ackermann_control_t ack = {0.0, 0.0, 5.0, 2.0, 0.0};
  CHECK_OK(tsc_vehicle_apply_ackermann_control(vehicle, &ack));
  uint64_t frame = 0;
  for (int i = 0; i < 100; ++i) CHECK_OK(tsc_world_tick(world, 1.0, &frame));
  tsc_vector3d_t v;
  CHECK_OK(tsc_actor_get_velocity(vehicle_actor, &v));
  CHECK(v.x > 4.9 && v.x < 5.1);
  ack.jerk = 1.0 / 0.0;
  CHECK(tsc_vehicle_apply_ackermann_control(vehicle, &ack) == TSC_INVALID_ARGUMENT);

  /* Telemetry: one entry per wheel; a smaller buffer still reports the count. */
  tsc_vehicle_telemetry_data_t tel;
  tsc_wheel_telemetry_data_t wheels[8];
  size_t wheel_count = 0;
  CHECK_OK(tsc_vehicle_get_telemetry_data(vehicle, &tel, wheels, 8, &wheel_count));
  CHECK(wheel_count == 4 && tel.speed > 4.9 && tel.gear == 1 && wheels[0].omega > 0.0);
  CHECK_OK(tsc_vehicle_get_telemetry_data(vehicle, &tel, NULL, 0, &wheel_count));
  CHECK(wheel_count == 4);
  CHECK(tsc_vehicle_get_telemetry_data(vehicle, &tel, NULL, 2, &wheel_count) ==
        TSC_INVALID_ARGUMENT);

  /* Bones of the vehicle: the root and one per wheel on the mock. */
  tsc_transform_t bones[8];
  size_t bone_count = 0;
  CHECK_OK(tsc_vehicle_get_vehicle_bone_world_transforms(vehicle, bones, 8, &bone_count));
  CHECK(bone_count == 5);

  /* Doors, failure state, debug telemetry, CarSim / Chrono (no-ops here). */
  CHECK_OK(tsc_vehicle_open_door(vehicle, TSC_VEHICLE_DOOR_ALL));
  CHECK_OK(tsc_vehicle_close_door(vehicle, TSC_VEHICLE_DOOR_FL));
  CHECK(tsc_vehicle_open_door(vehicle, 7) == TSC_INVALID_ARGUMENT);
  CHECK(tsc_vehicle_open_door(vehicle, -1) == TSC_INVALID_ARGUMENT);
  int32_t failure = -1;
  CHECK_OK(tsc_vehicle_get_failure_state(vehicle, &failure));
  CHECK(failure == TSC_VEHICLE_FAILURE_NONE);
  CHECK_OK(tsc_vehicle_show_debug_telemetry(vehicle, 1));
  CHECK_OK(tsc_vehicle_enable_carsim(vehicle, "sim.par", 7));
  CHECK_OK(tsc_vehicle_enable_carsim(vehicle, NULL, 0));
  CHECK(tsc_vehicle_enable_carsim(vehicle, NULL, 3) == TSC_INVALID_ARGUMENT);
  CHECK_OK(tsc_vehicle_use_carsim_road(vehicle, 1));
  CHECK_OK(tsc_vehicle_enable_chrono_physics(vehicle, 30, 0.002, "", 0, NULL, 0, NULL, 0,
                                             "/tmp/", 5));
  CHECK(tsc_vehicle_enable_chrono_physics(vehicle, 30, -1.0, NULL, 0, NULL, 0, NULL, 0, NULL,
                                          0) == TSC_INVALID_ARGUMENT);

  /* Wheel steer: the physics angle follows the control; the direction is visual. */
  tsc_vehicle_control_t control = {0.0, 0.5, 0.0, 0, 0, 0, 0};
  CHECK_OK(tsc_vehicle_apply_control(vehicle, &control));
  double angle = 0.0;
  CHECK_OK(tsc_vehicle_get_wheel_steer_angle(vehicle, TSC_WHEEL_FL, &angle));
  CHECK(angle > 34.9 && angle < 35.1);
  CHECK_OK(tsc_vehicle_get_wheel_steer_angle(vehicle, TSC_WHEEL_BL, &angle));
  CHECK(angle == 0.0);
  CHECK_OK(tsc_vehicle_set_wheel_steer_direction(vehicle, TSC_WHEEL_FR, 10.0));
  CHECK(tsc_vehicle_set_wheel_steer_direction(vehicle, 4, 10.0) == TSC_INVALID_ARGUMENT);
  CHECK(tsc_vehicle_set_wheel_steer_direction(vehicle, TSC_WHEEL_FR, 0.0 / 0.0) ==
        TSC_INVALID_ARGUMENT);

  /* Batch: Ackermann control and debug telemetry. */
  tsc_command_t cmds[2];
  memset(cmds, 0, sizeof cmds);
  cmds[0].type = TSC_COMMAND_APPLY_VEHICLE_ACKERMANN_CONTROL;
  cmds[0].then_of = -1;
  CHECK_OK(tsc_actor_get_id(vehicle_actor, &cmds[0].actor_id));
  cmds[0].vector = (tsc_vector3d_t){0.0, 0.0, 0.0};
  cmds[0].transform.location = (tsc_location_t){3.0, 0.0, 0.0};
  cmds[1].type = TSC_COMMAND_SHOW_DEBUG_TELEMETRY;
  cmds[1].then_of = -1;
  cmds[1].actor_id = cmds[0].actor_id;
  cmds[1].flag = 0;
  tsc_command_response_t r[2];
  size_t n = 0;
  CHECK_OK(tsc_client_apply_batch_sync(client, cmds, 2, 0, r, 2, &n));
  CHECK(n == 2 && !r[0].has_error && !r[1].has_error);
  for (size_t i = 0; i < n; ++i) tsc_string_free(&r[i].error);
  for (int i = 0; i < 100; ++i) CHECK_OK(tsc_world_tick(world, 1.0, &frame));
  CHECK_OK(tsc_actor_get_velocity(vehicle_actor, &v));
  CHECK(v.x * v.x + v.y * v.y < 0.01);
  cmds[0].transform.location.y = 0.0 / 0.0; /* jerk */
  CHECK(tsc_client_apply_batch(client, cmds, 1, 0) == TSC_INVALID_ARGUMENT);

  /* Walker bones and poses. */
  CHECK_OK(tsc_world_get_blueprint_library(world, &library));
  CHECK_OK(tsc_blueprint_library_find(library, "walker.pedestrian.0001", 22, &walker_bp));
  tsc_transform_t at = {{20.0, 20.0, 1.0}, {0, 0, 0}};
  CHECK_OK(tsc_world_spawn_actor(world, walker_bp, &at, NULL, TSC_ATTACHMENT_RIGID,
                                 &walker_actor));
  CHECK_OK(tsc_actor_as_walker(walker_actor, &walker));
  tsc_bone_list_t *list = NULL;
  CHECK_OK(tsc_walker_get_bones(walker, &list));
  CHECK(tsc_handle_kind(H(list)) == TSC_KIND_BONE_LIST);
  size_t nbones = tsc_bone_list_size(list);
  CHECK(nbones > 2);
  tsc_bone_transform_out_t bone;
  CHECK_OK(tsc_bone_list_get(list, 1, &bone));
  CHECK(strcmp(bone.name.data, "crl_hips__C") == 0 && bone.world.location.x == 20.0);
  tsc_string_free(&bone.name);
  CHECK(tsc_bone_list_get(list, nbones, &bone) == TSC_NOT_FOUND);
  CHECK(tsc_bone_list_size((tsc_bone_list_t *)world) == 0);
  tsc_handle_release(H(list));
  tsc_bone_transform_t in[1] = {{"crl_hips__C", 11, {{0.0, 0.0, 0.5}, {0.0, 90.0, 0.0}}}};
  CHECK_OK(tsc_walker_set_bones(walker, in, 1));
  CHECK_OK(tsc_walker_show_pose(walker));
  CHECK_OK(tsc_walker_get_bones(walker, &list));
  CHECK_OK(tsc_bone_list_get(list, 1, &bone));
  CHECK(bone.relative.location.z == 0.5 && bone.relative.rotation.yaw == 90.0);
  tsc_string_free(&bone.name);
  tsc_handle_release(H(list));
  CHECK_OK(tsc_walker_hide_pose(walker));
  CHECK_OK(tsc_walker_blend_pose(walker, 0.5));
  CHECK(tsc_walker_blend_pose(walker, 0.0 / 0.0) == TSC_INVALID_ARGUMENT);
  CHECK_OK(tsc_walker_get_pose_from_animation(walker));
  CHECK(tsc_walker_set_bones(walker, NULL, 1) == TSC_INVALID_ARGUMENT);
  in[0].transform.location.x = 1.0 / 0.0;
  CHECK(tsc_walker_set_bones(walker, in, 1) == TSC_INVALID_ARGUMENT);
  CHECK(tsc_walker_set_bones((tsc_walker_t *)vehicle, in, 0) == TSC_INVALID_ARGUMENT);

  tsc_handle_release(H(walker));
  tsc_handle_release(H(walker_actor));
  tsc_handle_release(H(walker_bp));
  tsc_handle_release(H(library));
  tsc_handle_release(H(vehicle));
  tsc_handle_release(H(vehicle_actor));
  tsc_handle_release(H(actors));
  tsc_handle_release(H(world));
  tsc_handle_release(H(client));
  CHECK(tsc_live_handle_count() == before);
}

/* Issue #21: world queries, caller-owned lists, the light manager, on_tick. */
static void test_mock_issue21(void) {
  uint64_t before = tsc_live_handle_count();
  tsc_client_t *client = NULL;
  tsc_world_t *world = NULL;
  CHECK_OK(tsc_client_create(kHost, strlen(kHost), 2121, 0, &client));
  CHECK_OK(tsc_client_get_world(client, &world));

  tsc_actor_t *spectator = NULL;
  CHECK_OK(tsc_world_get_spectator(world, &spectator));
  CHECK(spectator != NULL && tsc_handle_kind(H(spectator)) == TSC_KIND_ACTOR);
  tsc_handle_release(H(spectator));

  tsc_traffic_light_t *light = (tsc_traffic_light_t *)0x1;
  CHECK_OK(tsc_world_get_traffic_light_from_opendrive_id(world, "nope", 4, &light));
  CHECK(light == NULL);
  CHECK_OK(tsc_world_get_traffic_light_from_opendrive_id(world, "1000", 4, &light));
  CHECK(light != NULL && tsc_handle_kind(H(light)) == TSC_KIND_TRAFFIC_LIGHT);
  tsc_handle_release(H(light));

  tsc_traffic_light_list_t *lights = NULL;
  CHECK_OK(tsc_world_get_traffic_lights_in_junction(world, 12345, &lights));
  CHECK(lights != NULL && tsc_traffic_light_list_size(lights) == 0);
  tsc_handle_release(H(lights));
  tsc_waypoint_t *before_light = NULL;
  tsc_map_t *map = NULL;
  tsc_location_t at = {90.0, 0.0, 0.0};
  CHECK_OK(tsc_world_get_map(world, &map));
  CHECK_OK(tsc_map_get_waypoint(map, &at, 1, 2, &before_light));
  CHECK_OK(tsc_world_get_traffic_lights_from_waypoint(world, before_light, 20.0, &lights));
  CHECK(tsc_traffic_light_list_size(lights) == 1);
  CHECK_OK(tsc_traffic_light_list_get(lights, 0, &light));
  CHECK(tsc_handle_kind(H(light)) == TSC_KIND_TRAFFIC_LIGHT);
  tsc_handle_release(H(light));
  CHECK(tsc_traffic_light_list_get(lights, 1, &light) == TSC_NOT_FOUND && light == NULL);
  tsc_handle_release(H(lights));
  tsc_handle_release(H(before_light));
  tsc_handle_release(H(map));

  tsc_bounding_box_list_t boxes = {NULL, 0};
  CHECK(tsc_world_get_level_bbs(world, 256, &boxes) == TSC_INVALID_ARGUMENT);
  CHECK_OK(tsc_world_get_level_bbs(world, 255, &boxes));
  CHECK(boxes.size == 3);
  tsc_bounding_box_list_free(&boxes);
  CHECK(boxes.items == NULL && boxes.size == 0);
  tsc_bounding_box_list_free(&boxes); /* idempotent */

  tsc_environment_object_list_t objects = {NULL, 0};
  CHECK_OK(tsc_world_get_environment_objects(world, 3 /* Buildings */, &objects));
  CHECK(objects.size == 1 && strcmp(objects.items[0].name.data, "SM_Building_1") == 0);
  tsc_environment_object_list_free(&objects);

  tsc_string_list_t names = {NULL, 0};
  CHECK_OK(tsc_world_get_names_of_all_objects(world, &names));
  CHECK(names.size == 3);
  tsc_string_list_free(&names);

  tsc_labelled_point_t point;
  int32_t found = -1;
  tsc_location_t above = {10.0, 0.0, 5.0};
  tsc_vector3d_t up = {0.0, 0.0, 1.0};
  CHECK_OK(tsc_world_project_point(world, &above, &up, 100.0, &found, &point));
  CHECK(found == 0);
  CHECK_OK(tsc_world_ground_projection(world, &above, 100.0, &found, &point));
  CHECK(found == 1 && point.label == 1 /* Roads */ && point.location.z == 0.0);
  CHECK(tsc_world_ground_projection(world, &above, -1.0, &found, &point) == TSC_INVALID_ARGUMENT);

  tsc_string_t object = {(char *)"SM_Tree_1", 9};
  tsc_color_t pixels[2] = {{1, 2, 3, 4}, {5, 6, 7, 8}};
  tsc_texture_color_t texture = {2, 1, pixels};
  CHECK_OK(tsc_world_apply_color_texture_to_objects(world, &object, 1, TSC_MATERIAL_DIFFUSE,
                                                    &texture));
  CHECK(tsc_world_apply_color_texture_to_objects(world, &object, 1, 7, &texture) ==
        TSC_INVALID_ARGUMENT);
  tsc_texture_color_t missing = {2, 1, NULL};
  CHECK(tsc_world_apply_color_texture_to_objects(world, &object, 1, TSC_MATERIAL_DIFFUSE,
                                                 &missing) == TSC_INVALID_ARGUMENT);
  CHECK(tsc_world_apply_color_texture_to_objects(world, NULL, 0, TSC_MATERIAL_DIFFUSE,
                                                 &texture) == TSC_INVALID_ARGUMENT);

  /* Light manager: bulk setters check every id first. */
  tsc_light_manager_t *manager = NULL;
  CHECK_OK(tsc_world_get_light_manager(world, &manager));
  CHECK(tsc_handle_kind(H(manager)) == TSC_KIND_LIGHT_MANAGER);
  tsc_light_list_t all = {NULL, 0}, on = {NULL, 0};
  CHECK_OK(tsc_light_manager_get_all_lights(manager, TSC_LIGHT_GROUP_NONE, &all));
  CHECK(all.size == 4);
  CHECK_OK(tsc_light_manager_get_turned_on_lights(manager, TSC_LIGHT_GROUP_NONE, &on));
  CHECK(on.size == 3);
  uint32_t ids[2] = {all.items[0].id, 999};
  int32_t active[2] = {0, 0};
  CHECK(tsc_light_manager_set_active(manager, ids, 2, active) == TSC_NOT_FOUND);
  tsc_light_state_t state;
  CHECK_OK(tsc_light_manager_get_light_states(manager, ids, 1, &state));
  CHECK(state.active == 1); /* unchanged */
  double intensities[1] = {0.0 / 0.0};
  CHECK(tsc_light_manager_set_intensity(manager, ids, 1, intensities) == TSC_INVALID_ARGUMENT);
  CHECK_OK(tsc_light_manager_set_active(manager, ids, 1, active));
  CHECK_OK(tsc_light_manager_get_light_states(manager, ids, 1, &state));
  CHECK(state.active == 0 && state.color.a == 255);
  CHECK_OK(tsc_light_manager_set_day_night_cycle(manager, 0));
  tsc_light_list_free(&all);
  tsc_light_list_free(&on);
  tsc_handle_release(H(manager));

  /* on_tick: LibCarla's thread only queues snapshots. */
  tsc_tick_listener_t *listener = NULL;
  CHECK_OK(tsc_world_on_tick(world, 0, &listener));
  CHECK(tsc_handle_kind(H(listener)) == TSC_KIND_TICK_LISTENER);
  uint64_t frame = 0;
  size_t pending = 99;
  CHECK_OK(tsc_world_tick(world, 1.0, &frame));
  CHECK_OK(tsc_world_tick(world, 1.0, &frame));
  CHECK_OK(tsc_tick_listener_pending_count(listener, &pending));
  CHECK(pending == 2);
  int32_t reached = -1;
  CHECK_OK(tsc_tick_listener_wait_for_frame(listener, frame, 1.0, &reached));
  CHECK(reached == 1);
  CHECK_OK(tsc_tick_listener_wait_for_frame(listener, frame + 100, 0.01, &reached));
  CHECK(reached == 0);
  uint64_t token = 0, same = 1;
  tsc_world_t *again = NULL;
  CHECK_OK(tsc_client_get_world(client, &again));
  CHECK_OK(tsc_world_get_client_token(world, &token));
  CHECK_OK(tsc_world_get_client_token(again, &same));
  CHECK(token != 0 && token == same);
  tsc_handle_release(H(again));
  tsc_world_snapshot_t *snapshot = NULL;
  CHECK_OK(tsc_tick_listener_poll(listener, &snapshot));
  CHECK(snapshot != NULL);
  tsc_handle_release(H(snapshot));
  CHECK_OK(tsc_tick_listener_stop(listener));
  CHECK_OK(tsc_tick_listener_stop(listener)); /* idempotent */
  CHECK_OK(tsc_world_tick(world, 1.0, &frame));
  CHECK_OK(tsc_tick_listener_pending_count(listener, &pending));
  CHECK(pending == 0);
  tsc_handle_release(H(listener));

  tsc_handle_release(H(world));
  tsc_handle_release(H(client));
  CHECK(tsc_live_handle_count() == before);
}

static void test_mock_timeout(void) {
  const char *host = "carla.invalid";
  tsc_client_t *client = NULL;
  tsc_world_t *world = NULL;
  CHECK_OK(tsc_client_create(host, strlen(host), 2000, 0, &client));
  CHECK(tsc_client_get_world(client, &world) == TSC_TIMEOUT);
  CHECK(world == NULL);
  CHECK(strstr(tsc_last_error_message(), "time-out") != NULL);
  tsc_handle_release(H(client));
}

/* Issue #19: parent, tags, attributes, skeleton, textures, traffic signs. */
static void test_mock_issue19(void) {
  uint64_t before = tsc_live_handle_count();
  tsc_client_t *client = NULL;
  tsc_world_t *world = NULL;
  tsc_actor_list_t *actors = NULL;
  tsc_actor_t *vehicle = NULL, *light = NULL, *sign_actor = NULL, *parent = NULL;
  tsc_traffic_sign_t *sign = NULL, *light_sign = NULL;
  CHECK(sizeof(tsc_string_list_t) == 16 && sizeof(tsc_transform_list_t) == 16);
  CHECK(sizeof(tsc_float_color_t) == 16 && sizeof(tsc_texture_color_t) == 16);
  CHECK_OK(tsc_client_create(kHost, strlen(kHost), 2119, 0, &client));
  CHECK_OK(tsc_client_get_world(client, &world));
  CHECK_OK(tsc_world_get_actors(world, &actors));
  CHECK_OK(tsc_actor_list_get(actors, 0, &vehicle));
  CHECK_OK(tsc_actor_list_get(actors, 2, &light));
  CHECK_OK(tsc_actor_list_get(actors, 3, &sign_actor));
  CHECK(tsc_handle_kind(H(sign_actor)) == TSC_KIND_TRAFFIC_SIGN);

  /* State, parent (generated) */
  int32_t state = -1;
  CHECK_OK(tsc_actor_get_actor_state(vehicle, &state));
  CHECK(state == TSC_ACTOR_STATE_ACTIVE);
  parent = (tsc_actor_t *)0x1;
  CHECK_OK(tsc_actor_get_parent(vehicle, &parent));
  CHECK(parent == NULL);

  /* Semantic tags: two-call pattern */
  size_t n = 0;
  uint8_t tags[4] = {0, 0, 0, 0};
  CHECK_OK(tsc_actor_get_semantic_tags(vehicle, NULL, 0, &n));
  CHECK(n == 1);
  CHECK_OK(tsc_actor_get_semantic_tags(vehicle, tags, 4, &n));
  CHECK(n == 1 && tags[0] == 14);
  CHECK(tsc_actor_get_semantic_tags(vehicle, tags, 4, NULL) == TSC_INVALID_ARGUMENT);

  /* Attributes: the pre-existing vehicle has none */
  tsc_string_list_t ids = {(tsc_string_t *)0x1, 7}, values = {(tsc_string_t *)0x1, 7};
  CHECK_OK(tsc_actor_get_attributes(vehicle, &ids, &values));
  CHECK(ids.size == 0 && values.size == 0);
  tsc_string_list_free(&ids);
  tsc_string_list_free(&values);
  CHECK(tsc_actor_get_attributes(vehicle, &ids, NULL) == TSC_INVALID_ARGUMENT);

  /* Skeleton */
  tsc_string_list_t names = {NULL, 0};
  CHECK_OK(tsc_actor_get_bone_names(vehicle, &names));
  CHECK(names.size == 6 && strcmp(names.items[0].data, "Root") == 0);
  tsc_string_list_free(&names);
  CHECK(names.items == NULL && names.size == 0);
  tsc_string_list_free(&names); /* idempotent */
  tsc_string_list_free(NULL);
  tsc_transform_list_t transforms = {NULL, 0};
  CHECK_OK(tsc_actor_get_bone_world_transforms(vehicle, &transforms));
  CHECK(transforms.size == 6);
  tsc_transform_list_free(&transforms);
  CHECK(transforms.items == NULL && transforms.size == 0);
  /* No skinned mesh: an error, and nothing is allocated. */
  CHECK(tsc_actor_get_bone_names(light, &names) == TSC_ERROR);
  CHECK(names.items == NULL && names.size == 0);
  CHECK(tsc_actor_get_bone_world_transforms(light, &transforms) == TSC_ERROR);
  CHECK(transforms.items == NULL && transforms.size == 0);
  tsc_transform_t t;
  CHECK_OK(tsc_actor_get_component_relative_transform(vehicle, "Mesh", 4, &t));
  CHECK(tsc_actor_get_component_world_transform(vehicle, "Nope", 4, &t) == TSC_ERROR);
  CHECK(tsc_actor_get_component_world_transform(vehicle, NULL, 4, &t) == TSC_INVALID_ARGUMENT);
  CHECK_OK(tsc_actor_get_socket_names(light, &names));
  CHECK(names.size == 0);
  tsc_string_list_free(&names);

  /* Textures (generated; conversions in shared_types.cpp) */
  tsc_color_t pixels[2] = {{255, 0, 0, 255}, {0, 255, 0, 255}};
  tsc_texture_color_t tex = {2, 1, pixels};
  CHECK_OK(tsc_actor_apply_texture_color(vehicle, TSC_MATERIAL_DIFFUSE, &tex));
  CHECK(tsc_actor_apply_texture_color(vehicle, 4, &tex) == TSC_INVALID_ARGUMENT);
  tsc_texture_color_t bad = {2, 1, NULL};
  CHECK(tsc_actor_apply_texture_color(vehicle, TSC_MATERIAL_DIFFUSE, &bad) == TSC_INVALID_ARGUMENT);
  tsc_float_color_t fpixels[1] = {{0.5f, 0.5f, 0.5f, 1.0f}};
  tsc_texture_float_color_t ftex = {1, 1, fpixels};
  CHECK_OK(tsc_actor_apply_texture_float_color(vehicle, TSC_MATERIAL_NORMAL, &ftex));

  /* Traffic signs: lights are signs too */
  tsc_bounding_box_t box;
  CHECK_OK(tsc_actor_as_traffic_sign(sign_actor, &sign));
  CHECK_OK(tsc_traffic_sign_get_trigger_volume(sign, &box));
  CHECK(box.extent.x == 0.5);
  CHECK_OK(tsc_actor_as_traffic_sign(light, &light_sign));
  CHECK(tsc_handle_kind(H(light_sign)) == TSC_KIND_TRAFFIC_LIGHT);
  CHECK_OK(tsc_traffic_sign_get_trigger_volume(light_sign, &box));
  tsc_traffic_sign_t *none = (tsc_traffic_sign_t *)0x1;
  CHECK(tsc_actor_as_traffic_sign(vehicle, &none) == TSC_TYPE_ERROR && none == NULL);
  CHECK(tsc_traffic_sign_get_trigger_volume((tsc_traffic_sign_t *)vehicle, &box) ==
        TSC_INVALID_ARGUMENT);

  tsc_handle_release(H(light_sign));
  tsc_handle_release(H(sign));
  tsc_handle_release(H(sign_actor));
  tsc_handle_release(H(light));
  tsc_handle_release(H(vehicle));
  tsc_handle_release(H(actors));
  tsc_handle_release(H(world));
  tsc_handle_release(H(client));
  CHECK(tsc_live_handle_count() == before);
}

int main(void) {
  test_versions();
  test_layout();
  test_null_arguments();
  test_wrong_handle_kind();
  test_refcount();
  test_thread_local_error();
  test_issue23_offline();
  if (strcmp(tsc_backend_name(), "mock") == 0) {
    test_mock_issue23();
    test_mock_issue35();
    test_mock_session();
    test_mock_milestone1();
    test_mock_sensors();
    test_mock_milestone4();
    test_mock_issue22();
    test_mock_issue20();
    test_mock_issue19();
    test_mock_issue21();
    test_mock_timeout();
  } else {
    printf("backend '%s': skipping mock-server checks\n", tsc_backend_name());
  }
  if (g_failures != 0) {
    fprintf(stderr, "%d check(s) failed\n", g_failures);
    return 1;
  }
  printf("test_ffi: all checks passed (backend %s)\n", tsc_backend_name());
  return 0;
}
