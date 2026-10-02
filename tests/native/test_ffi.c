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
  CHECK(sizeof(tsc_wheel_physics_control_t) == 80);
  CHECK(offsetof(tsc_vehicle_physics_control_t, wheels) == 80);
  CHECK(sizeof(tsc_vehicle_physics_control_t) == 720);
  CHECK(sizeof(tsc_command_t) == 144);
  CHECK(offsetof(tsc_command_response_t, error) == 8);
  CHECK(sizeof(tsc_command_response_t) == 24);
}

static void test_null_arguments(void) {
  tsc_client_t *client = NULL;
  CHECK(tsc_client_create(kHost, strlen(kHost), 2000, NULL) == TSC_INVALID_ARGUMENT);
  CHECK(strstr(tsc_last_error_message(), "out_client") != NULL);
  CHECK(tsc_client_create("", 0, 2000, &client) == TSC_INVALID_ARGUMENT);
  CHECK(client == NULL);
  CHECK(tsc_client_set_timeout(NULL, 1.0) == TSC_INVALID_ARGUMENT);
  tsc_handle_release(NULL);
  tsc_string_free(NULL);
  CHECK(tsc_handle_kind(NULL) == TSC_KIND_INVALID);
  CHECK(tsc_actor_list_size(NULL) == 0);
}

static void test_wrong_handle_kind(void) {
  tsc_client_t *client = NULL;
  CHECK_OK(tsc_client_create(kHost, strlen(kHost), 2000, &client));
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
  CHECK_OK(tsc_client_create(kHost, strlen(kHost), 2000, &client));
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

  CHECK_OK(tsc_client_create(kHost, strlen(kHost), 2101, &client));
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
  CHECK(tsc_actor_list_size(actors) == 2);
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
  CHECK_OK(tsc_world_spawn_actor(world, bp, &spawn, NULL, &spawned));
  CHECK(spawned != NULL);
  tsc_actor_t *blocked = (tsc_actor_t *)0x1;
  CHECK_OK(tsc_world_try_spawn_actor(world, bp, &spawn, NULL, &blocked));
  CHECK(blocked == NULL);
  CHECK(tsc_world_spawn_actor(world, bp, &spawn, NULL, &blocked) == TSC_ERROR);
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

static void test_mock_milestone1(void) {
  uint64_t before = tsc_live_handle_count();
  tsc_client_t *client = NULL;
  tsc_world_t *world = NULL;
  tsc_map_t *map = NULL;
  CHECK_OK(tsc_client_create(kHost, strlen(kHost), 2102, &client));
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
  CHECK(tsc_world_snapshot_size(snap) == 2);
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
  tsc_vehicle_physics_control_t pc;
  CHECK_OK(tsc_vehicle_get_physics_control(vehicle, &pc));
  CHECK(pc.wheel_count == 4 && pc.mass == 1500.0);
  pc.mass = 2000.0;
  CHECK_OK(tsc_vehicle_apply_physics_control(vehicle, &pc));
  CHECK_OK(tsc_vehicle_get_physics_control(vehicle, &pc));
  CHECK(pc.mass == 2000.0);
  pc.wheel_count = 3;
  CHECK(tsc_vehicle_apply_physics_control(vehicle, &pc) == TSC_INVALID_ARGUMENT);
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

static void test_mock_timeout(void) {
  const char *host = "carla.invalid";
  tsc_client_t *client = NULL;
  tsc_world_t *world = NULL;
  CHECK_OK(tsc_client_create(host, strlen(host), 2000, &client));
  CHECK(tsc_client_get_world(client, &world) == TSC_TIMEOUT);
  CHECK(world == NULL);
  CHECK(strstr(tsc_last_error_message(), "time-out") != NULL);
  tsc_handle_release(H(client));
}

int main(void) {
  test_versions();
  test_layout();
  test_null_arguments();
  test_wrong_handle_kind();
  test_refcount();
  test_thread_local_error();
  if (strcmp(tsc_backend_name(), "mock") == 0) {
    test_mock_session();
    test_mock_milestone1();
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
