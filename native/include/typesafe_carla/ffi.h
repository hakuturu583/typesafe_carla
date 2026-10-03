/*
 * typesafe_carla C ABI.
 *
 * This is the only interface between the Codon layer and LibCarla.
 *
 * Rules (see docs/design.md, sections 7-14):
 *   - Complex objects are opaque, reference-counted handles. Every handle
 *     returned through an out-parameter is owned by the caller (refcount 1)
 *     and must be released with tsc_handle_release().
 *   - Value types are POD structs passed through pointers. Struct returns are
 *     never used, because C struct-return conventions differ between ABIs.
 *   - No C++ exception crosses this boundary. Every fallible function returns
 *     a tsc_status_t; the message is in tsc_last_error_message(), which is
 *     thread-local.
 *   - Input strings are (pointer, length) UTF-8 pairs and need not be NUL
 *     terminated. Output strings are tsc_string_t, freed with tsc_string_free().
 */
#ifndef TYPESAFE_CARLA_FFI_H
#define TYPESAFE_CARLA_FFI_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(__GNUC__)
#define TSC_API __attribute__((visibility("default")))
#else
#define TSC_API
#endif

/* ABI version. Bump MAJOR on any incompatible change to this header.
 * 2.0: tsc_command_t gained `scalar` (and new command types), Milestone 4.
 * 2.1: tsc_sensor_pending_count; tsc_sensor_listen queue_capacity 0 = unbounded.
 * 3.0: VehiclePhysicsControl with every LibCarla field: physics-control
 *      snapshot handles, new vehicle/wheel structs (issue #12).
 * 3.1: map geo-reference, XODR waypoints, landmarks as handles, lane markings,
 *      traffic light geometry (#22).
 * 3.2: vehicle Ackermann/doors/failure state/telemetry/wheel steer, walker bones
 *      and poses (#20).
 * 3.3: client map list/files/replayer flags, Traffic Manager actions and settings,
 *      blueprint tags, debug clear, extended WorldSettings, transform matrices (#23).
 * 3.4: actor state/attributes/parent/tags, physics at a location, skeleton queries,
 *      textures, TrafficSign (#19).
 * 3.5: world spectator, traffic light/sign queries, environment objects, ray casts,
 *      map layers, IMU gravity, textures, on_tick, light manager (#21).
 * 3.6: sensor data frame_number, image convert/save, point cloud save, collision
 *      actors, radar, semantic LiDAR, lane invasion, obstacle, DVS, optical flow (#24).
 * 3.7: tsc_debug_draw_* take a trailing persistent_lines flag (#37).
 * 3.8: tsc_world_get_actors_by_id (#38).
 * 4.0: tsc_world_spawn_actor / try_spawn_actor take a tsc_attachment_type_t (#34).
 * 4.1: tsc_client_create worker_threads; map_layers on load_world and
 *      load_world_if_different (#35).
 * 4.2: tsc_map_new_from_opendrive, a client-side Map from an OpenDRIVE string (#39).
 * 4.3: tsc_client_replay_file_ex, tsc_client_start_recorder_ex (#36).
 * 4.4: actor world, blueprint attribute ids and recommended values, sensor ROS
 *      and G-buffer streams (#33). */
#define TSC_ABI_VERSION_MAJOR 4
#define TSC_ABI_VERSION_MINOR 4
#define TSC_ABI_VERSION ((TSC_ABI_VERSION_MAJOR << 16) | TSC_ABI_VERSION_MINOR)

/* ------------------------------------------------------------------------ */
/* Status / errors                                                          */
/* ------------------------------------------------------------------------ */

typedef enum {
  TSC_OK = 0,
  TSC_ERROR = 1,
  TSC_INVALID_ARGUMENT = 2,
  TSC_TIMEOUT = 3,
  TSC_NOT_FOUND = 4,
  TSC_TYPE_ERROR = 5,
  TSC_VERSION_ERROR = 6
} tsc_status_t;

/* Message describing the last non-OK status returned on this thread.
 * Valid until the next tsc_* call on the same thread. Never NULL. */
TSC_API const char *tsc_last_error_message(void);
TSC_API void tsc_clear_last_error(void);

/* ------------------------------------------------------------------------ */
/* Versioning                                                               */
/* ------------------------------------------------------------------------ */

/* (TSC_ABI_VERSION_MAJOR << 16) | TSC_ABI_VERSION_MINOR of the built library. */
TSC_API uint32_t tsc_abi_version(void);
/* LibCarla version the library was built against, e.g. "0.10.0". */
TSC_API const char *tsc_libcarla_version(void);
/* CARLA git ref LibCarla was built from (e.g. "ue5-dev", a tag or a SHA),
 * "local" for a local source tree, "mock" for the mock backend. (ABI 1.1) */
TSC_API const char *tsc_libcarla_git_ref(void);
/* Resolved CARLA commit SHA, or "unknown" / "mock". (ABI 1.1) */
TSC_API const char *tsc_libcarla_git_commit(void);
/* Source revision of typesafe_carla the library was built from. */
TSC_API const char *tsc_build_commit(void);
/* "libcarla" for the real backend, "mock" for the in-memory test backend. */
TSC_API const char *tsc_backend_name(void);

/* ------------------------------------------------------------------------ */
/* Strings                                                                  */
/* ------------------------------------------------------------------------ */

typedef struct {
  char *data; /* NUL terminated for convenience; size excludes the NUL. */
  size_t size;
} tsc_string_t;

/* Frees data and resets the struct. Safe on a zeroed or already freed struct. */
TSC_API void tsc_string_free(tsc_string_t *string);

/* A list of output strings (e.g. map names, blueprint tags). */
typedef struct {
  tsc_string_t *items;
  size_t size;
} tsc_string_list_t;

/* Frees every item and the array, and resets the struct. Safe on a zeroed or
 * already freed struct. */
TSC_API void tsc_string_list_free(tsc_string_list_t *list);

/* ------------------------------------------------------------------------ */
/* Handles                                                                  */
/* ------------------------------------------------------------------------ */

typedef enum {
  TSC_KIND_INVALID = 0,
  TSC_KIND_CLIENT = 1,
  TSC_KIND_WORLD = 2,
  TSC_KIND_ACTOR = 3,
  TSC_KIND_VEHICLE = 4,
  TSC_KIND_ACTOR_LIST = 5,
  TSC_KIND_BLUEPRINT_LIBRARY = 6,
  TSC_KIND_ACTOR_BLUEPRINT = 7,
  TSC_KIND_WORLD_SNAPSHOT = 8, /* ABI 1.2 */
  TSC_KIND_MAP = 9,            /* ABI 1.2 */
  TSC_KIND_WAYPOINT = 10,      /* ABI 1.2 */
  TSC_KIND_WAYPOINT_LIST = 11, /* ABI 1.2 */
  TSC_KIND_SENSOR = 12,        /* ABI 1.3; also an actor */
  TSC_KIND_SENSOR_DATA = 13,   /* ABI 1.3 */
  TSC_KIND_WALKER = 14,        /* also an actor */
  TSC_KIND_WALKER_AI_CONTROLLER = 15, /* also an actor */
  TSC_KIND_TRAFFIC_LIGHT = 16, /* also an actor */
  TSC_KIND_TRAFFIC_MANAGER = 17,
  TSC_KIND_LANDMARK_LIST = 18,
  TSC_KIND_JUNCTION = 19,
  TSC_KIND_PHYSICS_CONTROL = 20, /* ABI 3.0 */
  TSC_KIND_LANDMARK = 21,           /* issue #22 */
  TSC_KIND_TRAFFIC_LIGHT_LIST = 22, /* issue #22 */
  TSC_KIND_BONE_LIST = 23,          /* ABI 3.2 (#20): Walker.get_bones() result */
  TSC_KIND_TRAFFIC_SIGN = 24,       /* ABI 3.4 (#19); also an actor (a traffic light is also a sign) */
  TSC_KIND_LIGHT_MANAGER = 25,      /* ABI 3.5 (#21) */
  TSC_KIND_TICK_LISTENER = 26       /* ABI 3.5 (#21): a World.on_tick registration */
} tsc_handle_kind_t;

typedef struct tsc_handle tsc_handle_t;
typedef struct tsc_client tsc_client_t;
typedef struct tsc_world tsc_world_t;
typedef struct tsc_actor tsc_actor_t;
/* A vehicle handle is also a valid actor handle. */
typedef struct tsc_vehicle tsc_vehicle_t;
typedef struct tsc_actor_list tsc_actor_list_t;
typedef struct tsc_blueprint_library tsc_blueprint_library_t;
typedef struct tsc_actor_blueprint tsc_actor_blueprint_t;

/* Every handle type above may be passed (cast) as a tsc_handle_t*.
 * retain/release are thread-safe; release on NULL is a no-op. */
TSC_API void tsc_handle_retain(tsc_handle_t *handle);
TSC_API void tsc_handle_release(tsc_handle_t *handle);
TSC_API tsc_handle_kind_t tsc_handle_kind(const tsc_handle_t *handle);
/* Current reference count; for tests and diagnostics only. */
TSC_API uint32_t tsc_handle_refcount(const tsc_handle_t *handle);
/* Number of live handles in the process; for leak tests only. */
TSC_API uint64_t tsc_live_handle_count(void);

/* ------------------------------------------------------------------------ */
/* Value types                                                              */
/* ------------------------------------------------------------------------ */

typedef struct {
  double x, y, z;
} tsc_vector3d_t;

typedef tsc_vector3d_t tsc_location_t;

typedef struct {
  double pitch, yaw, roll;
} tsc_rotation_t;

typedef struct {
  tsc_location_t location;
  tsc_rotation_t rotation;
} tsc_transform_t;

/* Booleans are int32_t (0/1) so the layout is unambiguous on every side. */
typedef struct {
  double throttle;
  double steer;
  double brake;
  int32_t hand_brake;
  int32_t reverse;
  int32_t manual_gear_shift;
  int32_t gear;
} tsc_vehicle_control_t;

typedef struct {
  int32_t synchronous_mode;
  int32_t no_rendering_mode;
  int32_t has_fixed_delta_seconds;
  int32_t substepping;
  double fixed_delta_seconds; /* ignored unless has_fixed_delta_seconds */
  double max_substep_delta_time;
  int32_t max_substeps;
  int32_t reserved0;
} tsc_world_settings_t;

typedef enum {
  TSC_ATTRIBUTE_BOOL = 0,
  TSC_ATTRIBUTE_INT = 1,
  TSC_ATTRIBUTE_FLOAT = 2,
  TSC_ATTRIBUTE_STRING = 3,
  TSC_ATTRIBUTE_RGB_COLOR = 4
} tsc_attribute_type_t;

typedef struct {
  tsc_string_t id;
  tsc_string_t value;
  int32_t type; /* tsc_attribute_type_t */
  int32_t is_modifiable;
} tsc_actor_attribute_t;

/* Frees both strings. */
TSC_API void tsc_actor_attribute_free(tsc_actor_attribute_t *attribute);

/* ------------------------------------------------------------------------ */
/* Client                                                                   */
/* ------------------------------------------------------------------------ */

/* worker_threads: LibCarla's asynchronous worker threads, 0 for all cores. */
TSC_API tsc_status_t tsc_client_create(const char *host, size_t host_len, uint16_t port,
                                       size_t worker_threads, tsc_client_t **out_client);
/* BEGIN GENERATED client from bindings/client.yaml, do not edit */
TSC_API tsc_status_t tsc_client_set_timeout(tsc_client_t *client, double seconds);
TSC_API tsc_status_t tsc_client_get_timeout(tsc_client_t *client, double *out_seconds);
TSC_API tsc_status_t tsc_client_get_client_version(tsc_client_t *client, tsc_string_t *out);
TSC_API tsc_status_t tsc_client_get_server_version(tsc_client_t *client, tsc_string_t *out);
TSC_API tsc_status_t tsc_client_get_world(tsc_client_t *client, tsc_world_t **out_world);
TSC_API tsc_status_t tsc_client_load_world(tsc_client_t *client,
                                           const char *map_name, size_t map_name_len,
                                           int32_t reset_settings, uint16_t map_layers,
                                           tsc_world_t **out_world);
TSC_API tsc_status_t tsc_client_reload_world(tsc_client_t *client, int32_t reset_settings,
                                             tsc_world_t **out_world);
/* END GENERATED client */

/* ------------------------------------------------------------------------ */
/* World                                                                    */
/* ------------------------------------------------------------------------ */

/* carla.AttachmentType (LibCarla rpc::AttachmentType), how a spawned actor
 * attaches to its parent; other values are rejected. */
typedef enum {
  TSC_ATTACHMENT_RIGID = 0,
  TSC_ATTACHMENT_SPRING_ARM = 1,
  TSC_ATTACHMENT_SPRING_ARM_GHOST = 2
} tsc_attachment_type_t;

/* BEGIN GENERATED world_core from bindings/world.yaml, do not edit */
TSC_API tsc_status_t tsc_world_get_id(tsc_world_t *world, uint64_t *out_id);
TSC_API tsc_status_t tsc_world_get_actors(tsc_world_t *world, tsc_actor_list_t **out_list);
/* In request order; ids that name no actor are left out (as in LibCarla). Destroyed actors may still be listed (LibCarla's actor cache is only cleared when an episode starts). */
TSC_API tsc_status_t tsc_world_get_actors_by_id(tsc_world_t *world,
                                                const uint32_t *actor_ids, size_t count,
                                                tsc_actor_list_t **out_list);
TSC_API tsc_status_t tsc_world_get_blueprint_library(tsc_world_t *world,
                                                     tsc_blueprint_library_t **out_library);
/* attachment_type: tsc_attachment_type_t, always checked; used only with a parent (may be NULL). */
TSC_API tsc_status_t tsc_world_spawn_actor(tsc_world_t *world,
                                           const tsc_actor_blueprint_t *blueprint,
                                           const tsc_transform_t *transform, tsc_actor_t *parent,
                                           int32_t attachment_type, tsc_actor_t **out_actor);
/* TSC_OK with *out_actor == NULL if the spawn fails; bad attachment_type: TSC_INVALID_ARGUMENT. */
TSC_API tsc_status_t tsc_world_try_spawn_actor(tsc_world_t *world,
                                               const tsc_actor_blueprint_t *blueprint,
                                               const tsc_transform_t *transform,
                                               tsc_actor_t *parent, int32_t attachment_type,
                                               tsc_actor_t **out_actor);
TSC_API tsc_status_t tsc_world_tick(tsc_world_t *world, double timeout_seconds,
                                    uint64_t *out_frame);
/* END GENERATED world_core */
/* TSC_NOT_FOUND if no actor has this id. */
TSC_API tsc_status_t tsc_world_get_actor(tsc_world_t *world, uint32_t actor_id,
                                         tsc_actor_t **out_actor);
TSC_API tsc_status_t tsc_world_get_settings(tsc_world_t *world, tsc_world_settings_t *out);
TSC_API tsc_status_t tsc_world_apply_settings(tsc_world_t *world,
                                              const tsc_world_settings_t *settings,
                                              double timeout_seconds, uint64_t *out_frame);

/* ------------------------------------------------------------------------ */
/* Actor list                                                               */
/* ------------------------------------------------------------------------ */

/* BEGIN GENERATED actor_list_items from bindings/lists.yaml, do not edit */
TSC_API size_t tsc_actor_list_size(const tsc_actor_list_t *list);
TSC_API tsc_status_t tsc_actor_list_get(const tsc_actor_list_t *list, size_t index,
                                        tsc_actor_t **out_actor);
/* END GENERATED actor_list_items */
/* BEGIN GENERATED actor_list from bindings/actor_list.yaml, do not edit */
TSC_API tsc_status_t tsc_actor_list_filter(const tsc_actor_list_t *list,
                                           const char *pattern, size_t pattern_len,
                                           tsc_actor_list_t **out_list);
/* END GENERATED actor_list */

/* ------------------------------------------------------------------------ */
/* Actor (also accepts vehicle handles)                                     */
/* ------------------------------------------------------------------------ */

/* BEGIN GENERATED actor from bindings/actor.yaml, do not edit */
TSC_API tsc_status_t tsc_actor_get_id(tsc_actor_t *actor, uint32_t *out_id);
TSC_API tsc_status_t tsc_actor_get_type_id(tsc_actor_t *actor, tsc_string_t *out);
TSC_API tsc_status_t tsc_actor_is_alive(tsc_actor_t *actor, int32_t *out_alive);
TSC_API tsc_status_t tsc_actor_get_transform(tsc_actor_t *actor, tsc_transform_t *out);
TSC_API tsc_status_t tsc_actor_set_transform(tsc_actor_t *actor, const tsc_transform_t *transform);
TSC_API tsc_status_t tsc_actor_get_location(tsc_actor_t *actor, tsc_location_t *out);
TSC_API tsc_status_t tsc_actor_set_location(tsc_actor_t *actor, const tsc_location_t *location);
TSC_API tsc_status_t tsc_actor_get_velocity(tsc_actor_t *actor, tsc_vector3d_t *out);
TSC_API tsc_status_t tsc_actor_set_target_velocity(tsc_actor_t *actor,
                                                   const tsc_vector3d_t *velocity);
TSC_API tsc_status_t tsc_actor_get_acceleration(tsc_actor_t *actor, tsc_vector3d_t *out);
TSC_API tsc_status_t tsc_actor_get_angular_velocity(tsc_actor_t *actor, tsc_vector3d_t *out);
/* END GENERATED actor */
/* BEGIN GENERATED actor_destroy from bindings/actor.yaml, do not edit */
/* out_destroyed may be NULL. */
TSC_API tsc_status_t tsc_actor_destroy(tsc_actor_t *actor, int32_t *out_destroyed);
/* END GENERATED actor_destroy */
/* Checked downcast. TSC_TYPE_ERROR if the actor is not a vehicle. */
TSC_API tsc_status_t tsc_actor_as_vehicle(tsc_actor_t *actor, tsc_vehicle_t **out_vehicle);

/* ------------------------------------------------------------------------ */
/* Vehicle                                                                  */
/* ------------------------------------------------------------------------ */

/* BEGIN GENERATED vehicle_control from bindings/vehicle.yaml, do not edit */
TSC_API tsc_status_t tsc_vehicle_apply_control(tsc_vehicle_t *vehicle,
                                               const tsc_vehicle_control_t *control);
TSC_API tsc_status_t tsc_vehicle_get_control(tsc_vehicle_t *vehicle, tsc_vehicle_control_t *out);
/* END GENERATED vehicle_control */
/* BEGIN GENERATED vehicle from bindings/vehicle.yaml, do not edit */
TSC_API tsc_status_t tsc_vehicle_set_autopilot(tsc_vehicle_t *vehicle, int32_t enabled,
                                               uint16_t tm_port);
/* END GENERATED vehicle */

/* ------------------------------------------------------------------------ */
/* Blueprints                                                               */
/* ------------------------------------------------------------------------ */

/* BEGIN GENERATED blueprint_library_items from bindings/lists.yaml, do not edit */
TSC_API size_t tsc_blueprint_library_size(const tsc_blueprint_library_t *library);
TSC_API tsc_status_t tsc_blueprint_library_get(const tsc_blueprint_library_t *library, size_t index,
                                               tsc_actor_blueprint_t **out);
/* END GENERATED blueprint_library_items */
/* TSC_NOT_FOUND if no blueprint has this id. */
TSC_API tsc_status_t tsc_blueprint_library_find(const tsc_blueprint_library_t *library,
                                                const char *id, size_t id_len,
                                                tsc_actor_blueprint_t **out);
/* BEGIN GENERATED blueprint_library from bindings/blueprint_library.yaml, do not edit */
TSC_API tsc_status_t tsc_blueprint_library_filter(const tsc_blueprint_library_t *library,
                                                  const char *pattern, size_t pattern_len,
                                                  tsc_blueprint_library_t **out);
/* END GENERATED blueprint_library */

/* BEGIN GENERATED actor_blueprint from bindings/actor_blueprint.yaml, do not edit */
TSC_API tsc_status_t tsc_actor_blueprint_get_id(const tsc_actor_blueprint_t *blueprint,
                                                tsc_string_t *out);
TSC_API tsc_status_t tsc_actor_blueprint_has_tag(const tsc_actor_blueprint_t *blueprint,
                                                 const char *tag, size_t tag_len, int32_t *out_has);
TSC_API tsc_status_t tsc_actor_blueprint_has_attribute(const tsc_actor_blueprint_t *blueprint,
                                                       const char *id, size_t id_len,
                                                       int32_t *out_has);
/* END GENERATED actor_blueprint */
/* TSC_NOT_FOUND if the blueprint has no such attribute. */
TSC_API tsc_status_t tsc_actor_blueprint_get_attribute(const tsc_actor_blueprint_t *blueprint,
                                                       const char *id, size_t id_len,
                                                       tsc_actor_attribute_t *out);
/* TSC_NOT_FOUND for an unknown attribute, TSC_INVALID_ARGUMENT for a
 * non-modifiable attribute or a value that does not parse as its type. */
TSC_API tsc_status_t tsc_actor_blueprint_set_attribute(tsc_actor_blueprint_t *blueprint,
                                                       const char *id, size_t id_len,
                                                       const char *value, size_t value_len);
/* Issue #33. The ids of the blueprint's attributes, in LibCarla's iteration
 * order (ActorBlueprint::begin/end; unspecified, but stable for a blueprint). */
TSC_API tsc_status_t tsc_actor_blueprint_get_attribute_ids(const tsc_actor_blueprint_t *blueprint,
                                                           tsc_string_list_t *out);
/* ActorAttribute::GetRecommendedValues of attribute `id` (often empty).
 * TSC_NOT_FOUND if the blueprint has no such attribute. */
TSC_API tsc_status_t tsc_actor_blueprint_get_recommended_values(
    const tsc_actor_blueprint_t *blueprint, const char *id, size_t id_len, tsc_string_list_t *out);
/* BEGIN GENERATED actor_blueprint_size from bindings/actor_blueprint.yaml, do not edit */
TSC_API tsc_status_t tsc_actor_blueprint_size(const tsc_actor_blueprint_t *blueprint,
                                              size_t *out_count);
/* END GENERATED actor_blueprint_size */

/* ------------------------------------------------------------------------ */
/* Milestone 1 (ABI 1.2)                                                    */
/* ------------------------------------------------------------------------ */

/* --- Bounding box -------------------------------------------------------- */

typedef struct {
  tsc_location_t location; /* center, actor-local */
  tsc_vector3d_t extent;   /* half size */
  tsc_rotation_t rotation;
} tsc_bounding_box_t;

/* BEGIN GENERATED actor_bounding_box from bindings/actor.yaml, do not edit */
TSC_API tsc_status_t tsc_actor_get_bounding_box(tsc_actor_t *actor, tsc_bounding_box_t *out);
/* END GENERATED actor_bounding_box */

/* --- Snapshots ----------------------------------------------------------- */

typedef struct tsc_world_snapshot tsc_world_snapshot_t;

typedef struct {
  uint64_t frame;
  double elapsed_seconds;
  double delta_seconds;
  double platform_timestamp;
} tsc_timestamp_t;

typedef struct {
  uint32_t id;
  uint32_t reserved0;
  tsc_transform_t transform;
  tsc_vector3d_t velocity;
  tsc_vector3d_t angular_velocity;
  tsc_vector3d_t acceleration;
} tsc_actor_snapshot_t;

/* BEGIN GENERATED world_snapshots from bindings/world.yaml, do not edit */
TSC_API tsc_status_t tsc_world_get_snapshot(tsc_world_t *world, tsc_world_snapshot_t **out);
TSC_API tsc_status_t tsc_world_wait_for_tick(tsc_world_t *world, double timeout_seconds,
                                             tsc_world_snapshot_t **out);
/* END GENERATED world_snapshots */
/* BEGIN GENERATED world_snapshot from bindings/world_snapshot.yaml, do not edit */
TSC_API tsc_status_t tsc_world_snapshot_get_id(const tsc_world_snapshot_t *snapshot, uint64_t *out);
TSC_API tsc_status_t tsc_world_snapshot_get_timestamp(const tsc_world_snapshot_t *snapshot,
                                                      tsc_timestamp_t *out);
/* END GENERATED world_snapshot */
/* BEGIN GENERATED world_snapshot_items from bindings/lists.yaml, do not edit */
TSC_API size_t tsc_world_snapshot_size(const tsc_world_snapshot_t *snapshot);
TSC_API tsc_status_t tsc_world_snapshot_get(const tsc_world_snapshot_t *snapshot, size_t index,
                                            tsc_actor_snapshot_t *out);
/* END GENERATED world_snapshot_items */
/* TSC_NOT_FOUND if the actor is not in the snapshot. */
TSC_API tsc_status_t tsc_world_snapshot_find(const tsc_world_snapshot_t *s, uint32_t actor_id,
                                             tsc_actor_snapshot_t *out);

/* --- Map and waypoints ----------------------------------------------------- */

typedef struct tsc_map tsc_map_t;
typedef struct tsc_waypoint tsc_waypoint_t;
typedef struct tsc_waypoint_list tsc_waypoint_list_t;

/* BEGIN GENERATED world_map from bindings/world.yaml, do not edit */
TSC_API tsc_status_t tsc_world_get_map(tsc_world_t *world, tsc_map_t **out);
/* END GENERATED world_map */
/* BEGIN GENERATED map_new from bindings/map.yaml, do not edit */
/* carla.Map(name, xodr_content), no server. TSC_ERROR when the XML does not parse; bad OpenDRIVE may give another status, or crash LibCarla (a road without planView). */
TSC_API tsc_status_t tsc_map_new_from_opendrive(const char *name, size_t name_len,
                                                const char *xodr_content, size_t xodr_content_len,
                                                tsc_map_t **out);
/* END GENERATED map_new */
/* BEGIN GENERATED map_core from bindings/map.yaml, do not edit */
TSC_API tsc_status_t tsc_map_get_name(const tsc_map_t *map, tsc_string_t *out);
TSC_API tsc_status_t tsc_map_to_opendrive(const tsc_map_t *map, tsc_string_t *out);
/* Two-call pattern: pass out=NULL to get the count. Copies min(capacity, n). */
TSC_API tsc_status_t tsc_map_get_spawn_points(
    const tsc_map_t *map, tsc_transform_t *out, size_t capacity, size_t *out_count);
/* *out = NULL (TSC_OK) when no waypoint matches. lane_type: CARLA LaneType bits. */
TSC_API tsc_status_t tsc_map_get_waypoint(const tsc_map_t *map, const tsc_location_t *location,
                                          int32_t project_to_road, int32_t lane_type,
                                          tsc_waypoint_t **out);
TSC_API tsc_status_t tsc_map_generate_waypoints(const tsc_map_t *map, double distance,
                                                tsc_waypoint_list_t **out);
/* END GENERATED map_core */

typedef struct {
  uint64_t id;
  uint32_t road_id;
  uint32_t section_id;
  int32_t lane_id;
  int32_t is_junction;
  int32_t junction_id;
  int32_t lane_type; /* CARLA LaneType bit value */
  double s;
  double lane_width;
  tsc_transform_t transform;
} tsc_waypoint_info_t;

TSC_API tsc_status_t tsc_waypoint_get_info(const tsc_waypoint_t *wp, tsc_waypoint_info_t *out);
/* BEGIN GENERATED waypoint_navigation from bindings/waypoint.yaml, do not edit */
TSC_API tsc_status_t tsc_waypoint_next(const tsc_waypoint_t *waypoint, double distance,
                                       tsc_waypoint_list_t **out);
TSC_API tsc_status_t tsc_waypoint_previous(const tsc_waypoint_t *waypoint, double distance,
                                           tsc_waypoint_list_t **out);
TSC_API tsc_status_t tsc_waypoint_next_until_lane_end(const tsc_waypoint_t *waypoint,
                                                      double distance, tsc_waypoint_list_t **out);
TSC_API tsc_status_t tsc_waypoint_previous_until_lane_start(const tsc_waypoint_t *waypoint,
                                                            double distance,
                                                            tsc_waypoint_list_t **out);
/* END GENERATED waypoint_navigation */
/* BEGIN GENERATED waypoint_list_items from bindings/lists.yaml, do not edit */
TSC_API size_t tsc_waypoint_list_size(const tsc_waypoint_list_t *list);
TSC_API tsc_status_t tsc_waypoint_list_get(const tsc_waypoint_list_t *list, size_t index,
                                           tsc_waypoint_t **out);
/* END GENERATED waypoint_list_items */

/* --- Physics control ------------------------------------------------------ */

/* Every field of LibCarla UE5's rpc::VehiclePhysicsControl and
 * rpc::WheelPhysicsControl (identical in CARLA 0.10.0 and ue5-dev), with the
 * Python API's names. LibCarla's float fields cross as double, uint8_t enum
 * codes and bools as int32_t. Variable-length data (curves, gear ratios,
 * wheels, each wheel's lateral slip graph) is a (pointer, size) pair:
 *   - in tsc_physics_control_view(), the pointers borrow from the
 *     tsc_physics_control_t handle and stay valid while it is alive;
 *   - in tsc_vehicle_apply_physics_control(), they point to caller memory that
 *     is only read during the call (NULL is allowed for size 0). (ABI 3.0) */
typedef struct {
  double x;
  double y;
} tsc_vector2d_t;

typedef struct {
  const tsc_vector2d_t *lateral_slip_graph;
  size_t lateral_slip_graph_size;
  tsc_vector3d_t offset;
  tsc_vector3d_t suspension_axis;
  tsc_vector3d_t suspension_force_offset;
  tsc_location_t location;
  tsc_location_t old_location;
  tsc_vector3d_t velocity; /* geom::Location in LibCarla */
  double wheel_radius;
  double wheel_width;
  double wheel_mass;
  double cornering_stiffness;
  double friction_force_multiplier;
  double side_slip_modifier;
  double slip_threshold;
  double skid_threshold;
  double max_steer_angle;
  double max_wheelspin_rotation;
  double suspension_max_raise;
  double suspension_max_drop;
  double suspension_damping_ratio;
  double wheel_load_ratio;
  double spring_rate;
  double spring_preload;
  double rollbar_scaling;
  double max_brake_torque;
  double max_hand_brake_torque;
  int32_t axle_type;                      /* uint8_t: 0..255 */
  int32_t external_torque_combine_method; /* uint8_t: 0..255 */
  int32_t sweep_shape;                    /* uint8_t: 0..255 */
  int32_t sweep_type;                     /* uint8_t: 0..255 */
  int32_t suspension_smoothing;
  int32_t wheel_index;
  int32_t affected_by_steering;
  int32_t affected_by_brake;
  int32_t affected_by_handbrake;
  int32_t affected_by_engine;
  int32_t abs_enabled;
  int32_t traction_control_enabled;
} tsc_wheel_physics_control_t;

typedef struct {
  const tsc_vector2d_t *torque_curve;
  size_t torque_curve_size;
  const tsc_vector2d_t *steering_curve;
  size_t steering_curve_size;
  const double *forward_gear_ratios;
  size_t forward_gear_ratios_size;
  const double *reverse_gear_ratios;
  size_t reverse_gear_ratios_size;
  const tsc_wheel_physics_control_t *wheels;
  size_t wheel_count;
  double max_torque;
  double max_rpm;
  double idle_rpm;
  double brake_effect;
  double rev_up_moi;
  double rev_down_rate;
  double front_rear_split;
  double gear_change_time;
  double final_ratio;
  double change_up_rpm;
  double change_down_rpm;
  double transmission_efficiency;
  double mass;
  double drag_coefficient;
  double chassis_width;
  double chassis_height;
  double downforce_coefficient;
  double drag_area;
  double sleep_threshold;
  double sleep_slope_limit;
  tsc_location_t center_of_mass;
  tsc_vector3d_t inertia_tensor_scale;
  int32_t differential_type; /* uint8_t: 0..255 */
  int32_t use_automatic_gears;
  int32_t use_sweep_wheel_collision;
  int32_t reserved0;
} tsc_vehicle_physics_control_t;

/* A snapshot of one vehicle's physics control (one RPC), owned by the caller. */
typedef struct tsc_physics_control tsc_physics_control_t;

/* BEGIN GENERATED vehicle_physics from bindings/vehicle.yaml, do not edit */
TSC_API tsc_status_t tsc_vehicle_get_physics_control(tsc_vehicle_t *vehicle,
                                                     tsc_physics_control_t **out);
/* END GENERATED vehicle_physics */
/* Fills *out from the snapshot; its arrays borrow from `control`. */
TSC_API tsc_status_t tsc_physics_control_view(const tsc_physics_control_t *control,
                                              tsc_vehicle_physics_control_t *out);
/* Reads the vehicle's current control, overwrites every field above and
 * applies it, so fields a future LibCarla adds keep the server's values.
 * wheel_count must equal the vehicle's; mass must be positive; every value
 * must be finite and every uint8_t field in range (TSC_INVALID_ARGUMENT). */
TSC_API tsc_status_t tsc_vehicle_apply_physics_control(tsc_vehicle_t *vehicle,
                                                       const tsc_vehicle_physics_control_t *pc);

/* --- Batch commands -------------------------------------------------------- */

typedef enum {
  TSC_COMMAND_SPAWN_ACTOR = 1,
  TSC_COMMAND_DESTROY_ACTOR = 2,
  TSC_COMMAND_APPLY_VEHICLE_CONTROL = 3,
  TSC_COMMAND_APPLY_TRANSFORM = 4,
  TSC_COMMAND_APPLY_TARGET_VELOCITY = 5,
  TSC_COMMAND_SET_AUTOPILOT = 6,
  TSC_COMMAND_SET_SIMULATE_PHYSICS = 7,
  /* ABI 2.0 */
  TSC_COMMAND_APPLY_WALKER_CONTROL = 8,       /* vector: direction, scalar: speed, flag: jump */
  TSC_COMMAND_APPLY_TARGET_ANGULAR_VELOCITY = 9, /* vector */
  TSC_COMMAND_APPLY_IMPULSE = 10,             /* vector */
  TSC_COMMAND_APPLY_FORCE = 11,               /* vector */
  TSC_COMMAND_APPLY_ANGULAR_IMPULSE = 12,     /* vector */
  TSC_COMMAND_APPLY_TORQUE = 13,              /* vector */
  TSC_COMMAND_SET_ENABLE_GRAVITY = 14,        /* flag */
  TSC_COMMAND_SET_VEHICLE_LIGHT_STATE = 15,   /* flag: light state bits */
  TSC_COMMAND_APPLY_LOCATION = 16,            /* transform.location */
  TSC_COMMAND_SET_TRAFFIC_LIGHT_STATE = 17,   /* flag: tsc_traffic_light_state_t */
  /* ABI 3.2 (#20) */
  TSC_COMMAND_APPLY_VEHICLE_ACKERMANN_CONTROL = 18, /* vector: (steer, steer_speed, speed),
                                                       transform.location: (acceleration,
                                                       jerk, unused) */
  TSC_COMMAND_SHOW_DEBUG_TELEMETRY = 19        /* flag */
} tsc_command_type_t;

/* Flat tagged record; only the fields the type uses are read.
 * then_of: -1 for a top-level command, otherwise the index of an earlier
 * SPAWN_ACTOR command this one runs after. As on the CARLA server, such a
 * command always acts on the spawned actor (its actor_id is ignored; pass 0,
 * carla.command.FutureActor), and its failure does not fail the spawn. */
typedef struct {
  int32_t type;
  int32_t then_of;
  uint32_t actor_id;
  uint32_t parent_id; /* SPAWN_ACTOR: 0 = no parent */
  const tsc_actor_blueprint_t *blueprint; /* SPAWN_ACTOR */
  tsc_transform_t transform;              /* SPAWN_ACTOR, APPLY_TRANSFORM */
  tsc_vehicle_control_t control;          /* APPLY_VEHICLE_CONTROL */
  tsc_vector3d_t vector;                  /* APPLY_TARGET_VELOCITY */
  int32_t flag;                           /* SET_AUTOPILOT, SET_SIMULATE_PHYSICS, ... */
  uint16_t tm_port;                       /* SET_AUTOPILOT */
  uint16_t reserved0;
  double scalar;                          /* APPLY_WALKER_CONTROL: speed (ABI 2.0) */
} tsc_command_t;

typedef struct {
  uint32_t actor_id; /* spawned actor, or the command's actor */
  int32_t has_error;
  tsc_string_t error; /* free with tsc_string_free */
} tsc_command_response_t;

TSC_API tsc_status_t tsc_client_apply_batch(tsc_client_t *client, const tsc_command_t *commands,
                                            size_t count, int32_t do_tick);
/* out must have room for one response per top-level command (then_of == -1). */
TSC_API tsc_status_t tsc_client_apply_batch_sync(tsc_client_t *client,
                                                 const tsc_command_t *commands, size_t count,
                                                 int32_t do_tick, tsc_command_response_t *out,
                                                 size_t out_capacity, size_t *out_count);

/* ------------------------------------------------------------------------ */
/* Milestone 2: sensors (ABI 1.3)                                           */
/*                                                                          */
/* LibCarla delivers measurements on its own worker threads. They are put   */
/* into a per-handle queue; the Codon side polls it, or drains it into      */
/* callbacks on the program's own thread (design §15). Codon code never     */
/* runs on a LibCarla thread.                                               */
/* ------------------------------------------------------------------------ */

typedef struct tsc_sensor tsc_sensor_t; /* also a valid actor handle */
typedef struct tsc_sensor_data tsc_sensor_data_t;

typedef enum {
  TSC_SENSOR_DATA_OTHER = 0,
  TSC_SENSOR_DATA_IMAGE = 1,
  TSC_SENSOR_DATA_LIDAR = 2,
  TSC_SENSOR_DATA_GNSS = 3,
  TSC_SENSOR_DATA_IMU = 4,
  TSC_SENSOR_DATA_COLLISION = 5,
  /* ABI 3.6 (issue #24) */
  TSC_SENSOR_DATA_RADAR = 6,
  TSC_SENSOR_DATA_SEMANTIC_LIDAR = 7,
  TSC_SENSOR_DATA_LANE_INVASION = 8,
  TSC_SENSOR_DATA_OBSTACLE = 9,
  TSC_SENSOR_DATA_DVS = 10,
  TSC_SENSOR_DATA_OPTICAL_FLOW = 11
} tsc_sensor_data_type_t;

/* Checked downcast. TSC_TYPE_ERROR if the actor is not a sensor. */
TSC_API tsc_status_t tsc_actor_as_sensor(tsc_actor_t *actor, tsc_sensor_t **out_sensor);

/* Starts delivering measurements into this handle's queue, which keeps at
 * most queue_capacity items (oldest dropped first); queue_capacity 0 means
 * unbounded (ABI 2.1; earlier versions reject 0). Listening again stops the
 * previous stream and replaces the queue. Listening state belongs to this
 * handle's client-side sensor object; releasing it stops the stream. */
TSC_API tsc_status_t tsc_sensor_listen(tsc_sensor_t *sensor, size_t queue_capacity);
/* Idempotent. */
TSC_API tsc_status_t tsc_sensor_stop(tsc_sensor_t *sensor);
/* BEGIN GENERATED sensor from bindings/sensor.yaml, do not edit */
TSC_API tsc_status_t tsc_sensor_is_listening(tsc_sensor_t *sensor, int32_t *out);
/* END GENERATED sensor */
/* Number of measurements dropped because the queue was full. */
TSC_API tsc_status_t tsc_sensor_dropped_count(tsc_sensor_t *sensor, uint64_t *out);
/* Number of measurements currently queued (ABI 2.1). More may arrive at any
 * time; items leave only through poll / wait_for_data, a bounded queue's
 * overflow, or listen (which replaces the queue). */
TSC_API tsc_status_t tsc_sensor_pending_count(tsc_sensor_t *sensor, size_t *out);
/* *out = NULL (TSC_OK) when the queue is empty. */
TSC_API tsc_status_t tsc_sensor_poll(tsc_sensor_t *sensor, tsc_sensor_data_t **out);
/* TSC_TIMEOUT when nothing arrives within timeout_seconds. */
TSC_API tsc_status_t tsc_sensor_wait_for_data(tsc_sensor_t *sensor, double timeout_seconds,
                                              tsc_sensor_data_t **out);

/* Issue #33: ServerSideSensor. A client-side sensor (lane invasion) is
 * TSC_TYPE_ERROR. G-buffer texture ids (GBufferTextureID) are below
 * TSC_GBUFFER_TEXTURE_COUNT (TSC_INVALID_ARGUMENT otherwise). */
#define TSC_GBUFFER_TEXTURE_COUNT 13
/* BEGIN GENERATED sensor_ros from bindings/server_side_sensor.yaml, do not edit */
TSC_API tsc_status_t tsc_sensor_enable_for_ros(tsc_sensor_t *sensor);
TSC_API tsc_status_t tsc_sensor_disable_for_ros(tsc_sensor_t *sensor);
TSC_API tsc_status_t tsc_sensor_is_enabled_for_ros(tsc_sensor_t *sensor, int32_t *out);
/* END GENERATED sensor_ros */
/* BEGIN GENERATED sensor_gbuffer from bindings/server_side_sensor.yaml, do not edit */
TSC_API tsc_status_t tsc_sensor_is_listening_gbuffer(tsc_sensor_t *sensor, uint32_t gbuffer_id,
                                                     int32_t *out);
/* Stops the stream (the queue of tsc_sensor_listen_to_gbuffer stays). */
TSC_API tsc_status_t tsc_sensor_stop_gbuffer(tsc_sensor_t *sensor, uint32_t gbuffer_id);
/* END GENERATED sensor_gbuffer */
/* Starts delivering G-buffer texture gbuffer_id into a queue of this handle
 * (ServerSideSensor::ListenToGBuffer). TSC_INVALID_ARGUMENT for a sensor
 * other than an RGB camera (LibCarla would only log a warning and deliver
 * nothing). queue_capacity as in
 * tsc_sensor_listen. Listening again replaces the stream and the queue. */
TSC_API tsc_status_t tsc_sensor_listen_to_gbuffer(tsc_sensor_t *sensor, uint32_t gbuffer_id,
                                                  size_t queue_capacity);
/* Items queued for gbuffer_id (0 if it was never listened to). */
TSC_API tsc_status_t tsc_sensor_gbuffer_pending_count(tsc_sensor_t *sensor, uint32_t gbuffer_id,
                                                      size_t *out);
/* *out = NULL (TSC_OK) when that queue is empty or was never created. */
TSC_API tsc_status_t tsc_sensor_gbuffer_poll(tsc_sensor_t *sensor, uint32_t gbuffer_id,
                                             tsc_sensor_data_t **out);

typedef struct {
  uint64_t frame;
  double timestamp;
  tsc_transform_t sensor_transform;
  int32_t type; /* tsc_sensor_data_type_t */
  int32_t reserved0;
} tsc_sensor_data_info_t;

TSC_API tsc_status_t tsc_sensor_data_get_info(const tsc_sensor_data_t *data,
                                              tsc_sensor_data_info_t *out);

/* Image: BGRA, 4 bytes per pixel, row-major. The pointer stays valid while
 * the data handle is alive (zero copy, design §16). */
typedef struct {
  uint32_t width;
  uint32_t height;
  double fov;
  const uint8_t *data;
  size_t size; /* bytes = width * height * 4 */
} tsc_image_t;
TSC_API tsc_status_t tsc_sensor_data_as_image(const tsc_sensor_data_t *data, tsc_image_t *out);

/* LiDAR: points are {float x, y, z, intensity} (16 bytes each). */
typedef struct {
  uint32_t channels;
  uint32_t reserved0;
  double horizontal_angle; /* radians */
  const float *points;
  size_t point_count;
} tsc_lidar_t;
TSC_API tsc_status_t tsc_sensor_data_as_lidar(const tsc_sensor_data_t *data, tsc_lidar_t *out);
/* Points produced by one channel, of a LiDAR or (ABI 3.6) semantic LiDAR
 * measurement. */
TSC_API tsc_status_t tsc_lidar_channel_point_count(const tsc_sensor_data_t *data, uint32_t channel,
                                                   uint32_t *out);

typedef struct {
  double latitude;
  double longitude;
  double altitude;
} tsc_gnss_t;
TSC_API tsc_status_t tsc_sensor_data_as_gnss(const tsc_sensor_data_t *data, tsc_gnss_t *out);

typedef struct {
  tsc_vector3d_t accelerometer; /* m/s^2 */
  tsc_vector3d_t gyroscope;     /* rad/s */
  double compass;               /* radians, 0 = north */
} tsc_imu_t;
TSC_API tsc_status_t tsc_sensor_data_as_imu(const tsc_sensor_data_t *data, tsc_imu_t *out);

typedef struct {
  uint32_t actor_id;
  uint32_t other_actor_id; /* 0 if the other actor is unknown (e.g. static geometry) */
  tsc_vector3d_t normal_impulse;
} tsc_collision_t;
TSC_API tsc_status_t tsc_sensor_data_as_collision(const tsc_sensor_data_t *data,
                                                  tsc_collision_t *out);

/* ------------------------------------------------------------------------ */
/* Issue #24: more measurement types, image conversion, saving (ABI 3.6)    */
/* ------------------------------------------------------------------------ */

/* Collision events: the actors, as handles of the most derived kind. */
/* BEGIN GENERATED collision_event from bindings/collision_event.yaml, do not edit */
/* The sensor's parent; *out = NULL (TSC_OK) when LibCarla gives none. */
TSC_API tsc_status_t tsc_collision_event_get_actor(const tsc_sensor_data_t *data,
                                                   tsc_actor_t **out);
/* *out = NULL (TSC_OK) when LibCarla gives none. */
TSC_API tsc_status_t tsc_collision_event_get_other_actor(const tsc_sensor_data_t *data,
                                                         tsc_actor_t **out);
/* END GENERATED collision_event */

/* The Python API's carla.ColorConverter. */
typedef enum {
  TSC_COLOR_CONVERTER_RAW = 0,
  TSC_COLOR_CONVERTER_DEPTH = 1,
  TSC_COLOR_CONVERTER_LOGARITHMIC_DEPTH = 2,
  TSC_COLOR_CONVERTER_CITYSCAPES_PALETTE = 3
} tsc_color_converter_t;

/* Converts an image's pixels in place (Image.convert), with the formulas of
 * LibCarla's image::ColorConverter: Depth and LogarithmicDepth give gray
 * BGRA pixels (alpha 255), CityScapesPalette maps the red channel (the
 * semantic tag) to the palette. RAW leaves the pixels unchanged. Existing
 * views of the pixels see the change. */
TSC_API tsc_status_t tsc_image_convert(tsc_sensor_data_t *data, int32_t color_converter);
/* Writes the image, converted with color_converter (the image itself is not
 * changed), as a PNG: 8-bit gray for Depth and LogarithmicDepth, 8-bit RGBA
 * otherwise. As in LibCarla's ImageIO (built with PNG support only), the
 * path's extension is replaced by ".png" unless it is already ".png", and
 * missing directories are created. *out_path receives the path written. */
TSC_API tsc_status_t tsc_image_save_to_disk(const tsc_sensor_data_t *data, const char *path,
                                            size_t path_len, int32_t color_converter,
                                            tsc_string_t *out_path);
/* Writes a LiDAR or semantic LiDAR measurement as ASCII PLY with LibCarla's
 * pointcloud::PointCloudIO (extension forced to ".ply", directories created).
 * *out_path receives the path written. */
TSC_API tsc_status_t tsc_point_cloud_save_to_disk(const tsc_sensor_data_t *data, const char *path,
                                                  size_t path_len, tsc_string_t *out_path);

/* Radar: a zero-copy view of LibCarla's detections. */
typedef struct {
  float velocity; /* m/s, towards the sensor is negative */
  float azimuth;  /* rad */
  float altitude; /* rad */
  float depth;    /* m */
} tsc_radar_detection_t;
typedef struct {
  const tsc_radar_detection_t *detections;
  size_t detection_count;
} tsc_radar_t;
TSC_API tsc_status_t tsc_sensor_data_as_radar(const tsc_sensor_data_t *data, tsc_radar_t *out);

/* Semantic LiDAR: a zero-copy view of LibCarla's detections (24 bytes each).
 * Per-channel counts: tsc_lidar_channel_point_count. */
typedef struct {
  float x, y, z;
  float cos_inc_angle;
  uint32_t object_idx;
  uint32_t object_tag;
} tsc_semantic_lidar_detection_t;
typedef struct {
  uint32_t channels;
  uint32_t reserved0;
  double horizontal_angle; /* radians */
  const tsc_semantic_lidar_detection_t *points;
  size_t point_count;
} tsc_semantic_lidar_t;
TSC_API tsc_status_t tsc_sensor_data_as_semantic_lidar(const tsc_sensor_data_t *data,
                                                       tsc_semantic_lidar_t *out);

/* Obstacle detection. */
/* BEGIN GENERATED obstacle_detection from bindings/obstacle_detection_event.yaml, do not edit */
/* Distance to the obstacle, in meters. */
TSC_API tsc_status_t tsc_obstacle_detection_get_distance(const tsc_sensor_data_t *data,
                                                         double *out);
/* The sensor's parent; *out = NULL (TSC_OK) when LibCarla gives none. */
TSC_API tsc_status_t tsc_obstacle_detection_get_actor(const tsc_sensor_data_t *data,
                                                      tsc_actor_t **out);
/* The obstacle; *out = NULL (TSC_OK) when LibCarla gives none. */
TSC_API tsc_status_t tsc_obstacle_detection_get_other_actor(const tsc_sensor_data_t *data,
                                                            tsc_actor_t **out);
/* END GENERATED obstacle_detection */

/* DVS camera: a zero-copy view of LibCarla's packed events, 13 bytes each,
 * little-endian and unaligned: {uint16 x; uint16 y; int64 t (ns); uint8 pol}. */
#define TSC_DVS_EVENT_SIZE 13
typedef struct {
  uint32_t width;
  uint32_t height;
  double fov;
  const uint8_t *events;
  size_t event_count;
} tsc_dvs_t;
TSC_API tsc_status_t tsc_sensor_data_as_dvs(const tsc_sensor_data_t *data, tsc_dvs_t *out);

/* Optical flow camera: a zero-copy view of {float x, y} per pixel, row-major. */
typedef struct {
  uint32_t width;
  uint32_t height;
  double fov;
  const float *pixels;
  size_t pixel_count;
} tsc_optical_flow_t;
TSC_API tsc_status_t tsc_sensor_data_as_optical_flow(const tsc_sensor_data_t *data,
                                                     tsc_optical_flow_t *out);
/* OpticalFlowImage.get_color_coded_flow: BGRA, 4 bytes per pixel (alpha 0,
 * as in the Python API), into out, which must hold 4 * width * height bytes. */
TSC_API tsc_status_t tsc_optical_flow_color_coded(const tsc_sensor_data_t *data, uint8_t *out,
                                                  size_t capacity);

/* ------------------------------------------------------------------------ */
/* Milestone 4: broader CARLA coverage (ABI 2.0)                            */
/* ------------------------------------------------------------------------ */

typedef struct tsc_walker tsc_walker_t;                   /* also an actor */
typedef struct tsc_walker_ai_controller tsc_walker_ai_controller_t; /* also an actor */
typedef struct tsc_traffic_light tsc_traffic_light_t;     /* also an actor */
typedef struct tsc_traffic_manager tsc_traffic_manager_t;
typedef struct tsc_landmark_list tsc_landmark_list_t;
typedef struct tsc_landmark_handle tsc_landmark_handle_t; /* one landmark */
typedef struct tsc_junction tsc_junction_t;

/* --- More actor operations ----------------------------------------------- */

/* BEGIN GENERATED actor_physics from bindings/actor.yaml, do not edit */
TSC_API tsc_status_t tsc_actor_set_target_angular_velocity(tsc_actor_t *actor,
                                                           const tsc_vector3d_t *v);
TSC_API tsc_status_t tsc_actor_add_impulse(tsc_actor_t *actor, const tsc_vector3d_t *impulse);
TSC_API tsc_status_t tsc_actor_add_force(tsc_actor_t *actor, const tsc_vector3d_t *force);
TSC_API tsc_status_t tsc_actor_add_angular_impulse(tsc_actor_t *actor,
                                                   const tsc_vector3d_t *impulse);
TSC_API tsc_status_t tsc_actor_add_torque(tsc_actor_t *actor, const tsc_vector3d_t *torque);
TSC_API tsc_status_t tsc_actor_set_simulate_physics(tsc_actor_t *actor, int32_t enabled);
TSC_API tsc_status_t tsc_actor_set_enable_gravity(tsc_actor_t *actor, int32_t enabled);
/* END GENERATED actor_physics */

/* Checked downcasts; TSC_TYPE_ERROR on a mismatch. */
TSC_API tsc_status_t tsc_actor_as_walker(tsc_actor_t *actor, tsc_walker_t **out);
TSC_API tsc_status_t tsc_actor_as_walker_ai_controller(tsc_actor_t *actor,
                                                       tsc_walker_ai_controller_t **out);
TSC_API tsc_status_t tsc_actor_as_traffic_light(tsc_actor_t *actor, tsc_traffic_light_t **out);

/* --- Vehicle lights and traffic lights ------------------------------------- */

/* BEGIN GENERATED vehicle_lights from bindings/vehicle.yaml, do not edit */
/* light_state: CARLA VehicleLightState bit flags. */
TSC_API tsc_status_t tsc_vehicle_set_light_state(tsc_vehicle_t *vehicle, uint32_t light_state);
TSC_API tsc_status_t tsc_vehicle_get_light_state(tsc_vehicle_t *vehicle, uint32_t *out);
TSC_API tsc_status_t tsc_vehicle_get_speed_limit(tsc_vehicle_t *vehicle, double *out);
/* state: tsc_traffic_light_state_t */
TSC_API tsc_status_t tsc_vehicle_get_traffic_light_state(tsc_vehicle_t *vehicle, int32_t *out);
TSC_API tsc_status_t tsc_vehicle_is_at_traffic_light(tsc_vehicle_t *vehicle, int32_t *out);
/* END GENERATED vehicle_lights */
/* BEGIN GENERATED vehicle_traffic_light from bindings/vehicle.yaml, do not edit */
/* *out = NULL (TSC_OK) when the vehicle is not affected by a traffic light. */
TSC_API tsc_status_t tsc_vehicle_get_traffic_light(tsc_vehicle_t *vehicle,
                                                   tsc_traffic_light_t **out);
/* END GENERATED vehicle_traffic_light */

typedef enum {
  TSC_TRAFFIC_LIGHT_RED = 0,
  TSC_TRAFFIC_LIGHT_YELLOW = 1,
  TSC_TRAFFIC_LIGHT_GREEN = 2,
  TSC_TRAFFIC_LIGHT_OFF = 3,
  TSC_TRAFFIC_LIGHT_UNKNOWN = 4
} tsc_traffic_light_state_t;

typedef struct {
  int32_t state; /* tsc_traffic_light_state_t */
  int32_t is_frozen;
  double green_time;
  double yellow_time;
  double red_time;
  double elapsed_time;
  uint32_t pole_index;
  uint32_t reserved0;
} tsc_traffic_light_info_t;

TSC_API tsc_status_t tsc_traffic_light_get_info(tsc_traffic_light_t *light,
                                                tsc_traffic_light_info_t *out);
/* BEGIN GENERATED traffic_light from bindings/traffic_light.yaml, do not edit */
TSC_API tsc_status_t tsc_traffic_light_set_state(tsc_traffic_light_t *light, int32_t state);
TSC_API tsc_status_t tsc_traffic_light_set_green_time(tsc_traffic_light_t *light, double t);
TSC_API tsc_status_t tsc_traffic_light_set_yellow_time(tsc_traffic_light_t *light, double t);
TSC_API tsc_status_t tsc_traffic_light_set_red_time(tsc_traffic_light_t *light, double t);
TSC_API tsc_status_t tsc_traffic_light_freeze(tsc_traffic_light_t *light, int32_t freeze);
TSC_API tsc_status_t tsc_traffic_light_reset_group(tsc_traffic_light_t *light);
/* END GENERATED traffic_light */

/* --- Walkers ------------------------------------------------------------- */

typedef struct {
  tsc_vector3d_t direction;
  double speed; /* m/s */
  int32_t jump;
  int32_t reserved0;
} tsc_walker_control_t;

/* BEGIN GENERATED walker_control from bindings/walker.yaml, do not edit */
TSC_API tsc_status_t tsc_walker_apply_control(tsc_walker_t *walker,
                                              const tsc_walker_control_t *control);
TSC_API tsc_status_t tsc_walker_get_control(tsc_walker_t *walker, tsc_walker_control_t *out);
/* END GENERATED walker_control */
/* BEGIN GENERATED walker_ai_controller from bindings/walker_ai_controller.yaml, do not edit */
TSC_API tsc_status_t tsc_walker_ai_controller_start(tsc_walker_ai_controller_t *controller);
TSC_API tsc_status_t tsc_walker_ai_controller_stop(tsc_walker_ai_controller_t *controller);
TSC_API tsc_status_t tsc_walker_ai_controller_go_to_location(tsc_walker_ai_controller_t *controller,
                                                             const tsc_location_t *destination);
TSC_API tsc_status_t tsc_walker_ai_controller_set_max_speed(tsc_walker_ai_controller_t *controller,
                                                            double max_speed);
/* END GENERATED walker_ai_controller */
/* BEGIN GENERATED world_navigation from bindings/world.yaml, do not edit */
/* *out_found = 0 when the navigation mesh yields no location. */
TSC_API tsc_status_t tsc_world_get_random_location_from_navigation(
    tsc_world_t *world, tsc_location_t *out, int32_t *out_found);
/* END GENERATED world_navigation */

/* --- Weather -------------------------------------------------------------- */

typedef struct {
  double cloudiness;
  double precipitation;
  double precipitation_deposits;
  double wind_intensity;
  double sun_azimuth_angle;
  double sun_altitude_angle;
  double fog_density;
  double fog_distance;
  double fog_falloff;
  double wetness;
  double scattering_intensity;
  double mie_scattering_scale;
  double rayleigh_scattering_scale;
  double dust_storm;
} tsc_weather_t;

/* BEGIN GENERATED world_weather from bindings/world.yaml, do not edit */
TSC_API tsc_status_t tsc_world_get_weather(tsc_world_t *world, tsc_weather_t *out);
TSC_API tsc_status_t tsc_world_set_weather(tsc_world_t *world, const tsc_weather_t *weather);
/* Whether the server simulates weather (when not, set_weather has no effect). */
TSC_API tsc_status_t tsc_world_is_weather_enabled(tsc_world_t *world, int32_t *out);
/* END GENERATED world_weather */
/* LibCarla's named presets ("ClearNoon", "HardRainNoon", ...); TSC_NOT_FOUND otherwise. */
TSC_API tsc_status_t tsc_weather_preset(const char *name, size_t name_len, tsc_weather_t *out);

/* --- Debug drawing ---------------------------------------------------------- */

/* Passed by pointer like every struct here: Codon passes small structs by value
 * as separate scalars, which does not match the C calling convention. */
typedef struct {
  uint8_t r, g, b, a;
} tsc_color_t;

/* BEGIN GENERATED debug_draw from bindings/debug.yaml, do not edit */
TSC_API tsc_status_t tsc_debug_draw_point(tsc_world_t *world, const tsc_location_t *location,
                                          double size, const tsc_color_t *color, double life_time,
                                          int32_t persistent_lines);
TSC_API tsc_status_t tsc_debug_draw_line(tsc_world_t *world, const tsc_location_t *begin,
                                         const tsc_location_t *end, double thickness,
                                         const tsc_color_t *color, double life_time,
                                         int32_t persistent_lines);
TSC_API tsc_status_t tsc_debug_draw_arrow(tsc_world_t *world, const tsc_location_t *begin,
                                          const tsc_location_t *end, double thickness,
                                          double arrow_size, const tsc_color_t *color,
                                          double life_time, int32_t persistent_lines);
TSC_API tsc_status_t tsc_debug_draw_box(tsc_world_t *world, const tsc_bounding_box_t *box,
                                        const tsc_rotation_t *rotation, double thickness,
                                        const tsc_color_t *color, double life_time,
                                        int32_t persistent_lines);
TSC_API tsc_status_t tsc_debug_draw_string(tsc_world_t *world, const tsc_location_t *location,
                                           const char *text, size_t text_len, int32_t draw_shadow,
                                           const tsc_color_t *color, double life_time,
                                           int32_t persistent_lines);
/* END GENERATED debug_draw */

/* --- Recorder ---------------------------------------------------------------- */

/* BEGIN GENERATED client_recorder from bindings/client.yaml, do not edit */
/* The recorder functions return the server's text output in *out (tsc_string_free). */
TSC_API tsc_status_t tsc_client_start_recorder(tsc_client_t *client,
                                               const char *name, size_t name_len,
                                               int32_t additional_data, tsc_string_t *out);
TSC_API tsc_status_t tsc_client_stop_recorder(tsc_client_t *client);
TSC_API tsc_status_t tsc_client_show_recorder_file_info(tsc_client_t *client,
                                                        const char *name, size_t name_len,
                                                        int32_t show_all, tsc_string_t *out);
TSC_API tsc_status_t tsc_client_show_recorder_collisions(tsc_client_t *client,
                                                         const char *name, size_t name_len,
                                                         char type1, char type2, tsc_string_t *out);
TSC_API tsc_status_t tsc_client_show_recorder_actors_blocked(tsc_client_t *client,
                                                             const char *name, size_t name_len,
                                                             double min_time, double min_distance,
                                                             tsc_string_t *out);
TSC_API tsc_status_t tsc_client_replay_file(tsc_client_t *client, const char *name, size_t name_len,
                                            double start, double duration, uint32_t follow_id,
                                            int32_t replay_sensors, tsc_string_t *out);
TSC_API tsc_status_t tsc_client_stop_replayer(tsc_client_t *client, int32_t keep_actors);
TSC_API tsc_status_t tsc_client_start_recorder_ex(tsc_client_t *client,
                                                  const char *name, size_t name_len,
                                                  int32_t additional_data, int32_t stop_replayer,
                                                  tsc_string_t *out);
TSC_API tsc_status_t tsc_client_replay_file_ex(tsc_client_t *client,
                                               const char *name, size_t name_len, double start,
                                               double duration, uint32_t follow_id,
                                               int32_t replay_sensors, int32_t replay_weather,
                                               const tsc_transform_t *offset,
                                               const char *map_override, size_t map_override_len,
                                               tsc_string_t *out);
TSC_API tsc_status_t tsc_client_set_replayer_time_factor(tsc_client_t *client, double factor);
/* END GENERATED client_recorder */

/* --- OpenDRIVE worlds ---------------------------------------------------------- */

typedef struct {
  double vertex_distance;
  double max_road_length;
  double wall_height;
  double additional_width;
  int32_t smooth_junctions;
  int32_t enable_mesh_visibility;
  int32_t enable_pedestrian_navigation;
  int32_t reserved0;
} tsc_opendrive_parameters_t;

/* BEGIN GENERATED client_opendrive from bindings/client.yaml, do not edit */
TSC_API tsc_status_t tsc_client_generate_opendrive_world(
    tsc_client_t *client, const char *opendrive, size_t opendrive_len,
    const tsc_opendrive_parameters_t *parameters, int32_t reset_settings, tsc_world_t **out);
/* END GENERATED client_opendrive */

/* --- Map queries ------------------------------------------------------------------ */

/* BEGIN GENERATED map_topology from bindings/map.yaml, do not edit */
/* Lane segments (begin, end) as a waypoint list [b0, e0, b1, e1, ...]. */
TSC_API tsc_status_t tsc_map_get_topology(const tsc_map_t *map, tsc_waypoint_list_t **out);
/* Two-call pattern, like tsc_map_get_spawn_points. */
TSC_API tsc_status_t tsc_map_get_crosswalks(
    const tsc_map_t *map, tsc_location_t *out, size_t capacity, size_t *out_count);
/* END GENERATED map_topology */

typedef struct {
  tsc_string_t id;
  tsc_string_t name;
  tsc_string_t type;
  tsc_string_t sub_type;
  tsc_string_t country;
  tsc_string_t unit;
  tsc_string_t text;
  uint32_t road_id;
  int32_t orientation; /* CARLA SignalOrientation */
  double s;
  double t;
  double distance;
  double z_offset;
  double value;
  double height;
  double width;
  tsc_transform_t transform;
} tsc_landmark_t;

/* Frees the landmark's strings. */
TSC_API void tsc_landmark_free(tsc_landmark_t *landmark);
/* BEGIN GENERATED map_landmarks from bindings/map.yaml, do not edit */
TSC_API tsc_status_t tsc_map_get_all_landmarks(const tsc_map_t *map, tsc_landmark_list_t **out);
TSC_API tsc_status_t tsc_map_get_landmarks_of_type(const tsc_map_t *map,
                                                   const char *type, size_t type_len,
                                                   tsc_landmark_list_t **out);
/* END GENERATED map_landmarks */
/* BEGIN GENERATED landmark_list_items from bindings/lists.yaml, do not edit */
TSC_API size_t tsc_landmark_list_size(const tsc_landmark_list_t *list);
TSC_API tsc_status_t tsc_landmark_list_get(const tsc_landmark_list_t *list, size_t index,
                                           tsc_landmark_t *out);
TSC_API tsc_status_t tsc_landmark_list_get_landmark(const tsc_landmark_list_t *list, size_t index,
                                                    tsc_landmark_handle_t **out);
/* END GENERATED landmark_list_items */

/* BEGIN GENERATED waypoint_junction from bindings/waypoint.yaml, do not edit */
/* *out = NULL (TSC_OK) when the waypoint is not in a junction. */
TSC_API tsc_status_t tsc_waypoint_get_junction(const tsc_waypoint_t *waypoint,
                                               tsc_junction_t **out);
/* END GENERATED waypoint_junction */
/* BEGIN GENERATED junction from bindings/junction.yaml, do not edit */
TSC_API tsc_status_t tsc_junction_get_id(const tsc_junction_t *junction, int32_t *out);
TSC_API tsc_status_t tsc_junction_get_bounding_box(const tsc_junction_t *junction,
                                                   tsc_bounding_box_t *out);
/* Pairs (entry, exit) as a waypoint list [b0, e0, b1, e1, ...]. */
TSC_API tsc_status_t tsc_junction_get_waypoints(const tsc_junction_t *junction, int32_t lane_type,
                                                tsc_waypoint_list_t **out);
/* END GENERATED junction */

/* --- Lane markings (shared with #24 (LaneInvasionEvent)) ------------------------------ */

/* road::element::LaneMarking. type, color and lane_change hold the values of
 * LibCarla's LaneMarking::Type, ::Color and ::LaneChange enums. */
typedef struct {
  int32_t type;
  int32_t color;
  int32_t lane_change;
  int32_t reserved0;
  double width;
} tsc_lane_marking_t;

/* Lane-invasion events (issue #24; after tsc_lane_marking_t). */
/* BEGIN GENERATED lane_invasion_event from bindings/lane_invasion_event.yaml, do not edit */
/* LibCarla never gives these client-side events an episode, so this fails (TSC_ERROR) on 0.10.0 and ue5-dev. */
TSC_API tsc_status_t tsc_lane_invasion_event_get_actor(const tsc_sensor_data_t *data,
                                                       tsc_actor_t **out);
TSC_API tsc_status_t tsc_lane_invasion_event_get_crossed_lane_markings(
    const tsc_sensor_data_t *data, tsc_lane_marking_t *out, size_t capacity, size_t *out_count);
/* END GENERATED lane_invasion_event */

/* --- Issue #22: geo-reference, XODR waypoints, landmarks, traffic light geometry --- */

typedef struct tsc_traffic_light_list tsc_traffic_light_list_t;

typedef struct {
  double latitude, longitude, altitude;
} tsc_geo_location_t;

/* geom::ProjectionType (CARLA ue5-dev). */
typedef enum {
  TSC_GEO_PROJECTION_TM = 0,       /* TransverseMercatorParams */
  TSC_GEO_PROJECTION_UTM = 1,      /* UniversalTransverseMercatorParams */
  TSC_GEO_PROJECTION_WEB_MERC = 2, /* WebMercatorParams */
  TSC_GEO_PROJECTION_LCC2SP = 3    /* LambertConformalConicParams */
} tsc_geo_projection_type_t;

/* One geom::GeoProjection; `type` says which fields are used:
 *   TM:       lat_0, lon_0, k, x_0, y_0, ellipsoid_*
 *   UTM:      utm_zone, utm_north, ellipsoid_*, offset_* if utm_has_offset
 *   WEB_MERC: ellipsoid_*
 *   LCC2SP:   lat_0, lat_1, lat_2, lon_0, x_0, y_0, ellipsoid_* */
typedef struct {
  int32_t type; /* tsc_geo_projection_type_t */
  int32_t utm_zone;
  int32_t utm_north;
  int32_t utm_has_offset;
  double ellipsoid_a;
  double ellipsoid_f_inv;
  double lat_0, lat_1, lat_2, lon_0, k, x_0, y_0;
  double offset_x, offset_y, offset_z, offset_cos_h, offset_sin_h;
} tsc_geo_projection_t;

/* BEGIN GENERATED map from bindings/map.yaml, do not edit */
TSC_API tsc_status_t tsc_map_get_georeference(const tsc_map_t *map, tsc_geo_location_t *out);
/* *out = NULL (TSC_OK) when the road or lane does not exist. */
TSC_API tsc_status_t tsc_map_get_waypoint_xodr(const tsc_map_t *map, uint32_t road_id,
                                               int32_t lane_id, double s, tsc_waypoint_t **out);
TSC_API tsc_status_t tsc_map_get_landmarks_from_id(
    const tsc_map_t *map, const char *opendrive_id, size_t opendrive_id_len,
    tsc_landmark_list_t **out);
TSC_API tsc_status_t tsc_map_get_landmark_group(const tsc_map_t *map,
                                                const tsc_landmark_handle_t *landmark,
                                                tsc_landmark_list_t **out);
/* Writes the Traffic Manager's in-memory map; empty path: "<map name>.bin". LibCarla only logs a file that cannot be opened. */
TSC_API tsc_status_t tsc_map_cook_in_memory_map(const tsc_map_t *map,
                                                const char *path, size_t path_len);
/* END GENERATED map */
/* The map's projection. TSC_ERROR when LibCarla has no geo projections
 * (CARLA 0.10.0; ue5-dev has them). */
TSC_API tsc_status_t tsc_map_get_geoprojection(const tsc_map_t *map, tsc_geo_projection_t *out);
/* projection NULL: the map's own (with CARLA 0.10.0, the geo-reference's
 * Mercator approximation). A non-NULL projection needs ue5-dev. */
TSC_API tsc_status_t tsc_map_transform_to_geolocation(const tsc_map_t *map,
                                                      const tsc_location_t *location,
                                                      const tsc_geo_projection_t *projection,
                                                      tsc_geo_location_t *out);
/* The inverse; needs ue5-dev (TSC_ERROR with CARLA 0.10.0). */
TSC_API tsc_status_t tsc_map_geolocation_to_transform(const tsc_map_t *map,
                                                      const tsc_geo_location_t *geolocation,
                                                      const tsc_geo_projection_t *projection,
                                                      tsc_location_t *out);
/* Writes the OpenDRIVE file like the Python API (".xodr" replaces another
 * extension, parent directories are created; empty path: the map name).
 * TSC_ERROR when the file cannot be written. */
TSC_API tsc_status_t tsc_map_save_to_disk(const tsc_map_t *map, const char *path, size_t path_len);

typedef struct {
  int32_t from_lane;
  int32_t to_lane;
} tsc_lane_validity_t;

/* BEGIN GENERATED landmark from bindings/landmark.yaml, do not edit */
TSC_API tsc_status_t tsc_landmark_get_h_offset(const tsc_landmark_handle_t *landmark, double *out);
TSC_API tsc_status_t tsc_landmark_get_pitch(const tsc_landmark_handle_t *landmark, double *out);
TSC_API tsc_status_t tsc_landmark_get_roll(const tsc_landmark_handle_t *landmark, double *out);
TSC_API tsc_status_t tsc_landmark_is_dynamic(const tsc_landmark_handle_t *landmark, int32_t *out);
/* *out = NULL (TSC_OK) when the landmark has no waypoint (Map.get_all_landmarks*). */
TSC_API tsc_status_t tsc_landmark_get_waypoint(const tsc_landmark_handle_t *landmark,
                                               tsc_waypoint_t **out);
/* Two-call pattern, like tsc_map_get_spawn_points. */
TSC_API tsc_status_t tsc_landmark_get_lane_validities(
    const tsc_landmark_handle_t *landmark,
    tsc_lane_validity_t *out, size_t capacity, size_t *out_count);
/* END GENERATED landmark */

/* Waypoint: neighbour lanes, lane markings, traffic side, landmarks. */
/* BEGIN GENERATED waypoint from bindings/waypoint.yaml, do not edit */
/* *out = NULL (TSC_OK) when there is no such lane. */
TSC_API tsc_status_t tsc_waypoint_get_left_lane(const tsc_waypoint_t *waypoint,
                                                tsc_waypoint_t **out);
TSC_API tsc_status_t tsc_waypoint_get_right_lane(const tsc_waypoint_t *waypoint,
                                                 tsc_waypoint_t **out);
/* Right-hand traffic. CARLA 0.10.0 has no left-hand traffic: always 1 there. */
TSC_API tsc_status_t tsc_waypoint_is_rht(const tsc_waypoint_t *waypoint, int32_t *out);
/* *has_value = 0 (and *out zeroed) when there is no marking on that side. */
TSC_API tsc_status_t tsc_waypoint_get_left_lane_marking(
    const tsc_waypoint_t *waypoint, int32_t *has_value, tsc_lane_marking_t *out);
TSC_API tsc_status_t tsc_waypoint_get_right_lane_marking(
    const tsc_waypoint_t *waypoint, int32_t *has_value, tsc_lane_marking_t *out);
/* lane_change: LaneMarking::LaneChange flags. */
TSC_API tsc_status_t tsc_waypoint_get_lane_change(const tsc_waypoint_t *waypoint, int32_t *out);
TSC_API tsc_status_t tsc_waypoint_get_landmarks(const tsc_waypoint_t *waypoint, double distance,
                                                int32_t stop_at_junction,
                                                tsc_landmark_list_t **out);
TSC_API tsc_status_t tsc_waypoint_get_landmarks_of_type(const tsc_waypoint_t *waypoint,
                                                        double distance,
                                                        const char *type, size_t type_len,
                                                        int32_t stop_at_junction,
                                                        tsc_landmark_list_t **out);
/* END GENERATED waypoint */

/* Traffic light geometry. The waypoint and light lists skip null entries
 * (LibCarla returns them for lanes or actors that do not exist). */
/* BEGIN GENERATED traffic_light_list_items from bindings/lists.yaml, do not edit */
TSC_API size_t tsc_traffic_light_list_size(const tsc_traffic_light_list_t *list);
TSC_API tsc_status_t tsc_traffic_light_list_get(const tsc_traffic_light_list_t *list, size_t index,
                                                tsc_traffic_light_t **out);
/* END GENERATED traffic_light_list_items */
/* BEGIN GENERATED traffic_light_geometry from bindings/traffic_light.yaml, do not edit */
TSC_API tsc_status_t tsc_traffic_light_get_opendrive_id(tsc_traffic_light_t *light,
                                                        tsc_string_t *out);
TSC_API tsc_status_t tsc_traffic_light_get_trigger_volume(tsc_traffic_light_t *light,
                                                          tsc_bounding_box_t *out);
/* Fills up to `capacity` boxes, the total in *out_count (one server call). */
TSC_API tsc_status_t tsc_traffic_light_get_light_boxes(
    tsc_traffic_light_t *light, tsc_bounding_box_t *out, size_t capacity, size_t *out_count);
TSC_API tsc_status_t tsc_traffic_light_get_affected_lane_waypoints(tsc_traffic_light_t *light,
                                                                   tsc_waypoint_list_t **out);
TSC_API tsc_status_t tsc_traffic_light_get_stop_waypoints(tsc_traffic_light_t *light,
                                                          tsc_waypoint_list_t **out);
TSC_API tsc_status_t tsc_traffic_light_get_group_traffic_lights(tsc_traffic_light_t *light,
                                                                tsc_traffic_light_list_t **out);
/* END GENERATED traffic_light_geometry */

/* --- Traffic Manager ---------------------------------------------------------------- */

/* BEGIN GENERATED client_traffic_manager from bindings/client.yaml, do not edit */
TSC_API tsc_status_t tsc_client_get_traffic_manager(tsc_client_t *client, uint16_t port,
                                                    tsc_traffic_manager_t **out);
/* END GENERATED client_traffic_manager */
/* BEGIN GENERATED traffic_manager from bindings/traffic_manager.yaml, do not edit */
TSC_API tsc_status_t tsc_traffic_manager_get_port(tsc_traffic_manager_t *tm, uint16_t *out);
TSC_API tsc_status_t tsc_traffic_manager_set_synchronous_mode(tsc_traffic_manager_t *tm,
                                                              int32_t enabled);
TSC_API tsc_status_t tsc_traffic_manager_set_random_device_seed(tsc_traffic_manager_t *tm,
                                                                uint64_t seed);
TSC_API tsc_status_t tsc_traffic_manager_set_hybrid_physics_mode(tsc_traffic_manager_t *tm,
                                                                 int32_t enabled);
TSC_API tsc_status_t tsc_traffic_manager_set_global_percentage_speed_difference(
    tsc_traffic_manager_t *tm, double percentage);
TSC_API tsc_status_t tsc_traffic_manager_set_global_distance_to_leading_vehicle(
    tsc_traffic_manager_t *tm, double distance);
/* END GENERATED traffic_manager */

/* Per-vehicle settings. */
typedef enum {
  TSC_TM_PERCENTAGE_SPEED_DIFFERENCE = 1,
  TSC_TM_DISTANCE_TO_LEADING_VEHICLE = 2,
  TSC_TM_RANDOM_LEFT_LANECHANGE_PERCENTAGE = 3,
  TSC_TM_RANDOM_RIGHT_LANECHANGE_PERCENTAGE = 4,
  TSC_TM_IGNORE_LIGHTS_PERCENTAGE = 5,
  TSC_TM_IGNORE_SIGNS_PERCENTAGE = 6,
  TSC_TM_IGNORE_VEHICLES_PERCENTAGE = 7,
  TSC_TM_IGNORE_WALKERS_PERCENTAGE = 8,
  TSC_TM_KEEP_RIGHT_PERCENTAGE = 9,
  TSC_TM_DESIRED_SPEED = 10,
  TSC_TM_LANE_OFFSET = 11
} tsc_tm_vehicle_setting_t;

TSC_API tsc_status_t tsc_traffic_manager_set_vehicle_value(tsc_traffic_manager_t *tm,
                                                           tsc_vehicle_t *vehicle,
                                                           int32_t setting, double value);
/* BEGIN GENERATED traffic_manager_vehicle from bindings/traffic_manager.yaml, do not edit */
TSC_API tsc_status_t tsc_traffic_manager_set_auto_lane_change(tsc_traffic_manager_t *tm,
                                                              tsc_vehicle_t *vehicle,
                                                              int32_t enabled);
TSC_API tsc_status_t tsc_traffic_manager_force_lane_change(tsc_traffic_manager_t *tm,
                                                           tsc_vehicle_t *vehicle, int32_t to_left);
TSC_API tsc_status_t tsc_traffic_manager_set_update_vehicle_lights(tsc_traffic_manager_t *tm,
                                                                   tsc_vehicle_t *vehicle,
                                                                   int32_t enabled);
/* END GENERATED traffic_manager_vehicle */

/* ------------------------------------------------------------------------ */
/* Shared value types: string and transform lists, textures (#19, #21)     */
/* ------------------------------------------------------------------------ */

/* Lists the library allocates for the caller: free with the matching *_free,
 * which frees the items (and their strings) and resets the struct. Freeing a
 * zeroed or already freed list is a no-op. */
/* tsc_string_list_t and tsc_string_list_free are declared with tsc_string_t. */

typedef struct {
  tsc_transform_t *items;
  size_t size;
} tsc_transform_list_t;
TSC_API void tsc_transform_list_free(tsc_transform_list_t *list);

/* rpc::FloatColor. */
typedef struct {
  float r, g, b, a;
} tsc_float_color_t;

/* Textures (rpc::TextureColor / rpc::TextureFloatColor): width * height
 * pixels, row-major (pixel (x, y) at index y * width + x). The pixels are
 * caller memory, only read during the call; NULL is allowed for 0 pixels. */
typedef struct {
  uint32_t width;
  uint32_t height;
  const tsc_color_t *pixels;
} tsc_texture_color_t;

typedef struct {
  uint32_t width;
  uint32_t height;
  const tsc_float_color_t *pixels;
} tsc_texture_float_color_t;

/* rpc::MaterialParameter. */
typedef enum {
  TSC_MATERIAL_NORMAL = 0,
  TSC_MATERIAL_AO_ROUGHNESS_METALLIC_EMISSIVE = 1,
  TSC_MATERIAL_DIFFUSE = 2,
  TSC_MATERIAL_EMISSIVE = 3
} tsc_material_parameter_t;

/* ------------------------------------------------------------------------ */
/* Issue #20: Vehicle and Walker API gaps (ABI 3.2)                         */
/* ------------------------------------------------------------------------ */

/* carla.VehicleAckermannControl: steer in radians, steer_speed in rad/s,
 * speed in m/s, acceleration in m/s^2, jerk in m/s^3. Values must be finite. */
typedef struct {
  double steer;
  double steer_speed;
  double speed;
  double acceleration;
  double jerk;
} tsc_vehicle_ackermann_control_t;

/* carla.AckermannControllerSettings: PID gains. Values must be finite. */
typedef struct {
  double speed_kp;
  double speed_ki;
  double speed_kd;
  double accel_kp;
  double accel_ki;
  double accel_kd;
} tsc_ackermann_controller_settings_t;

/* carla.VehicleDoor (LibCarla rpc::VehicleDoor); other values are rejected. */
typedef enum {
  TSC_VEHICLE_DOOR_FL = 0,
  TSC_VEHICLE_DOOR_FR = 1,
  TSC_VEHICLE_DOOR_RL = 2,
  TSC_VEHICLE_DOOR_RR = 3,
  TSC_VEHICLE_DOOR_HOOD = 4,
  TSC_VEHICLE_DOOR_TRUNK = 5,
  TSC_VEHICLE_DOOR_ALL = 6
} tsc_vehicle_door_t;

/* carla.VehicleWheelLocation (Front_Wheel = FL, Back_Wheel = FR for 2-wheel
 * vehicles, as in LibCarla); other values are rejected. */
typedef enum {
  TSC_WHEEL_FL = 0,
  TSC_WHEEL_FR = 1,
  TSC_WHEEL_BL = 2,
  TSC_WHEEL_BR = 3
} tsc_vehicle_wheel_location_t;

/* carla.VehicleFailureState. */
typedef enum {
  TSC_VEHICLE_FAILURE_NONE = 0,
  TSC_VEHICLE_FAILURE_ROLLOVER = 1,
  TSC_VEHICLE_FAILURE_ENGINE = 2,
  TSC_VEHICLE_FAILURE_TIRE_PUNCTURE = 3
} tsc_vehicle_failure_state_t;

/* BEGIN GENERATED vehicle_ackermann from bindings/vehicle_ext.yaml, do not edit */
TSC_API tsc_status_t tsc_vehicle_apply_ackermann_control(
    tsc_vehicle_t *vehicle, const tsc_vehicle_ackermann_control_t *control);
TSC_API tsc_status_t tsc_vehicle_get_ackermann_controller_settings(
    tsc_vehicle_t *vehicle, tsc_ackermann_controller_settings_t *out);
TSC_API tsc_status_t tsc_vehicle_apply_ackermann_controller_settings(
    tsc_vehicle_t *vehicle, const tsc_ackermann_controller_settings_t *settings);
/* END GENERATED vehicle_ackermann */
/* BEGIN GENERATED vehicle_state from bindings/vehicle_ext.yaml, do not edit */
/* door: tsc_vehicle_door_t */
TSC_API tsc_status_t tsc_vehicle_open_door(tsc_vehicle_t *vehicle, int32_t door);
TSC_API tsc_status_t tsc_vehicle_close_door(tsc_vehicle_t *vehicle, int32_t door);
/* state: tsc_vehicle_failure_state_t */
TSC_API tsc_status_t tsc_vehicle_get_failure_state(tsc_vehicle_t *vehicle, int32_t *out);
TSC_API tsc_status_t tsc_vehicle_show_debug_telemetry(tsc_vehicle_t *vehicle, int32_t enabled);
/* wheel_location: tsc_vehicle_wheel_location_t; *out in degrees */
TSC_API tsc_status_t tsc_vehicle_get_wheel_steer_angle(tsc_vehicle_t *vehicle,
                                                       int32_t wheel_location, double *out);
/* Rotates the wheel's bone only (visual); physics is unaffected. */
TSC_API tsc_status_t tsc_vehicle_set_wheel_steer_direction(tsc_vehicle_t *vehicle,
                                                           int32_t wheel_location,
                                                           double angle_in_deg);
TSC_API tsc_status_t tsc_vehicle_use_carsim_road(tsc_vehicle_t *vehicle, int32_t enabled);
/* Needs the server's CarSim plugin; the server ignores it otherwise. */
TSC_API tsc_status_t tsc_vehicle_enable_carsim(tsc_vehicle_t *vehicle,
                                               const char *simfile_path, size_t simfile_path_len);
/* Needs the server's Chrono plugin; the server ignores it otherwise. */
TSC_API tsc_status_t tsc_vehicle_enable_chrono_physics(
    tsc_vehicle_t *vehicle, uint64_t max_substeps, double max_substep_delta_time,
    const char *vehicle_json, size_t vehicle_json_len,
    const char *powertrain_json, size_t powertrain_json_len,
    const char *tire_json, size_t tire_json_len,
    const char *base_json_path, size_t base_json_path_len);
/* END GENERATED vehicle_state */

typedef struct {
  double lat_slip;
  double long_slip;
  double omega;
} tsc_wheel_telemetry_data_t;

typedef struct {
  double speed;
  double steer;
  double throttle;
  double brake;
  double engine_rpm;
  int32_t gear;
  int32_t reserved0;
} tsc_vehicle_telemetry_data_t;

/* One RPC. Fills *out and up to wheel_capacity wheels (wheels may be NULL when
 * wheel_capacity is 0); *out_wheel_count is the vehicle's wheel count, which
 * may exceed wheel_capacity. TSC_ERROR when LibCarla has no
 * Vehicle::GetTelemetryData (CARLA 0.10.0; it is in ue5-dev). */
TSC_API tsc_status_t tsc_vehicle_get_telemetry_data(tsc_vehicle_t *vehicle,
                                                    tsc_vehicle_telemetry_data_t *out,
                                                    tsc_wheel_telemetry_data_t *wheels,
                                                    size_t wheel_capacity,
                                                    size_t *out_wheel_count);
/* BEGIN GENERATED vehicle_bones from bindings/vehicle_ext.yaml, do not edit */
/* Two-call buffer. TSC_ERROR with LibCarla 0.10.0, which lacks the method. */
TSC_API tsc_status_t tsc_vehicle_get_vehicle_bone_world_transforms(
    tsc_vehicle_t *vehicle, tsc_transform_t *out, size_t capacity, size_t *out_count);
/* END GENERATED vehicle_bones */

/* --- Walker bones and poses ------------------------------------------------ */

/* BEGIN GENERATED walker_pose from bindings/walker.yaml, do not edit */
/* blend: 0 = animation, 1 = the custom pose set with tsc_walker_set_bones. */
TSC_API tsc_status_t tsc_walker_blend_pose(tsc_walker_t *walker, double blend);
TSC_API tsc_status_t tsc_walker_show_pose(tsc_walker_t *walker);
TSC_API tsc_status_t tsc_walker_hide_pose(tsc_walker_t *walker);
TSC_API tsc_status_t tsc_walker_get_pose_from_animation(tsc_walker_t *walker);
/* END GENERATED walker_pose */

/* carla.bone_transform: one entry of WalkerBoneControlIn. */
typedef struct {
  const char *name; /* (pointer, length); need not be NUL terminated */
  size_t name_len;
  tsc_transform_t transform; /* relative to the parent bone */
} tsc_bone_transform_t;

/* BEGIN GENERATED walker_bones from bindings/walker.yaml, do not edit */
TSC_API tsc_status_t tsc_walker_set_bones(tsc_walker_t *walker,
                                          const tsc_bone_transform_t *bones, size_t count);
/* END GENERATED walker_bones */

/* carla.bone_transform_out: one entry of WalkerBoneControlOut. */
typedef struct {
  tsc_string_t name; /* owned: free with tsc_string_free */
  tsc_transform_t world;
  tsc_transform_t component;
  tsc_transform_t relative;
} tsc_bone_transform_out_t;

/* The result of one Walker::GetBonesTransform RPC (TSC_KIND_BONE_LIST). */
typedef struct tsc_bone_list tsc_bone_list_t;
/* BEGIN GENERATED walker_bone_list from bindings/walker.yaml, do not edit */
TSC_API tsc_status_t tsc_walker_get_bones(tsc_walker_t *walker, tsc_bone_list_t **out);
/* END GENERATED walker_bone_list */
/* BEGIN GENERATED bone_list_items from bindings/lists.yaml, do not edit */
TSC_API size_t tsc_bone_list_size(const tsc_bone_list_t *list);
TSC_API tsc_status_t tsc_bone_list_get(const tsc_bone_list_t *list, size_t index,
                                       tsc_bone_transform_out_t *out);
/* END GENERATED bone_list_items */

/* ------------------------------------------------------------------------ */
/* Issue #23: Client, Traffic Manager, blueprint, debug and value-type gaps  */
/* ------------------------------------------------------------------------ */

/* --- Client ------------------------------------------------------------------- */

/* BEGIN GENERATED client_replayer from bindings/client.yaml, do not edit */
TSC_API tsc_status_t tsc_client_set_replayer_ignore_hero(tsc_client_t *client, int32_t ignore_hero);
TSC_API tsc_status_t tsc_client_set_replayer_ignore_spectator(tsc_client_t *client,
                                                              int32_t ignore_spectator);
/* END GENERATED client_replayer */
/* BEGIN GENERATED client_files from bindings/client.yaml, do not edit */
/* Map names the server can load (an RPC). */
TSC_API tsc_status_t tsc_client_get_available_maps(tsc_client_t *client, tsc_string_list_t *out);
/* Files the map needs under `folder`; `download` fetches missing ones. */
TSC_API tsc_status_t tsc_client_get_required_files(tsc_client_t *client,
                                                   const char *folder, size_t folder_len,
                                                   int32_t download, tsc_string_list_t *out);
/* Downloads one file from the server into the files base folder. */
TSC_API tsc_status_t tsc_client_request_file(tsc_client_t *client,
                                             const char *name, size_t name_len);
/* Local folder for downloaded files (process-wide); *out is 0 for an empty path. */
TSC_API tsc_status_t tsc_client_set_files_base_folder(tsc_client_t *client,
                                                      const char *path, size_t path_len,
                                                      int32_t *out);
/* END GENERATED client_files */
/* LibCarla's LoadWorldIfDifferent: loads `map_name` unless it is the current
 * map (with or without the "Carla/Maps/" prefix). *out is the new world, or
 * NULL when the map was already loaded. map_layers: CARLA MapLayer bit flags. */
TSC_API tsc_status_t tsc_client_load_world_if_different(tsc_client_t *client,
                                                        const char *map_name, size_t map_name_len,
                                                        int32_t reset_settings, uint16_t map_layers,
                                                        tsc_world_t **out);

/* --- Traffic Manager ------------------------------------------------------------ */

/* BEGIN GENERATED traffic_manager_issue23 from bindings/traffic_manager.yaml, do not edit */
TSC_API tsc_status_t tsc_traffic_manager_set_osm_mode(tsc_traffic_manager_t *tm, int32_t enabled);
TSC_API tsc_status_t tsc_traffic_manager_set_respawn_dormant_vehicles(tsc_traffic_manager_t *tm,
                                                                      int32_t enabled);
TSC_API tsc_status_t tsc_traffic_manager_set_boundaries_respawn_dormant_vehicles(
    tsc_traffic_manager_t *tm, double lower_bound, double upper_bound);
TSC_API tsc_status_t tsc_traffic_manager_set_hybrid_physics_radius(tsc_traffic_manager_t *tm,
                                                                   double radius);
TSC_API tsc_status_t tsc_traffic_manager_set_global_lane_offset(tsc_traffic_manager_t *tm,
                                                                double offset);
/* other_actor: any actor handle (a vehicle, a walker, ...). */
TSC_API tsc_status_t tsc_traffic_manager_set_collision_detection(tsc_traffic_manager_t *tm,
                                                                 tsc_vehicle_t *reference_vehicle,
                                                                 tsc_actor_t *other_actor,
                                                                 int32_t detect_collision);
TSC_API tsc_status_t tsc_traffic_manager_shut_down(tsc_traffic_manager_t *tm);
/* Large-vehicle wide turns (LibCarla ue5-dev); TSC_ERROR without them (CARLA 0.10.0). */
TSC_API tsc_status_t tsc_traffic_manager_set_global_large_vehicle_wide_turn(
    tsc_traffic_manager_t *tm, int32_t enabled);
TSC_API tsc_status_t tsc_traffic_manager_set_large_vehicle_wide_turn(tsc_traffic_manager_t *tm,
                                                                     tsc_vehicle_t *vehicle,
                                                                     int32_t enabled);
/* SetCustomPath: the vehicle follows these locations. */
TSC_API tsc_status_t tsc_traffic_manager_set_path(tsc_traffic_manager_t *tm, tsc_vehicle_t *vehicle,
                                                  const tsc_location_t *path, size_t count,
                                                  int32_t empty_buffer);
/* SetImportedRoute: the vehicle follows these road options (codes of tsc_road_option_t, 0..7). */
TSC_API tsc_status_t tsc_traffic_manager_set_route(tsc_traffic_manager_t *tm,
                                                   tsc_vehicle_t *vehicle,
                                                   const uint8_t *route, size_t count,
                                                   int32_t empty_buffer);
/* END GENERATED traffic_manager_issue23 */
typedef enum {
  TSC_ROAD_OPTION_VOID = 0,
  TSC_ROAD_OPTION_LEFT = 1,
  TSC_ROAD_OPTION_RIGHT = 2,
  TSC_ROAD_OPTION_STRAIGHT = 3,
  TSC_ROAD_OPTION_LANE_FOLLOW = 4,
  TSC_ROAD_OPTION_CHANGE_LANE_LEFT = 5,
  TSC_ROAD_OPTION_CHANGE_LANE_RIGHT = 6,
  TSC_ROAD_OPTION_ROAD_END = 7
} tsc_road_option_t;
/* The vehicle's next action: a road option code and its waypoint.
 * TSC_NOT_FOUND if this Traffic Manager does not drive the vehicle (yet) or is
 * shut down, on every LibCarla (ue5-dev returns an empty plan there, 0.10.0
 * throws). In-process Traffic Managers only: LibCarla's TrafficManagerServer
 * binds these RPCs without a result, so a remote one cannot answer. */
TSC_API tsc_status_t tsc_traffic_manager_get_next_action(tsc_traffic_manager_t *tm,
                                                         tsc_vehicle_t *vehicle,
                                                         int32_t *out_road_option,
                                                         tsc_waypoint_t **out_waypoint);
/* Every planned action, in order: parallel lists of road option codes
 * (*out_road_options, `count` bytes, freed with tsc_road_options_free) and
 * waypoints (*out_waypoints). Errors as tsc_traffic_manager_get_next_action;
 * an empty plan is TSC_NOT_FOUND too. */
TSC_API tsc_status_t tsc_traffic_manager_get_all_actions(tsc_traffic_manager_t *tm,
                                                         tsc_vehicle_t *vehicle,
                                                         uint8_t **out_road_options,
                                                         size_t *out_count,
                                                         tsc_waypoint_list_t **out_waypoints);
TSC_API void tsc_road_options_free(uint8_t *road_options);

/* --- Blueprints ------------------------------------------------------------------ */

/* BEGIN GENERATED blueprint_library_attributes from bindings/blueprint_library.yaml, do not edit */
/* Blueprints whose attribute `name` has `value` (or recommends it). */
TSC_API tsc_status_t tsc_blueprint_library_filter_by_attribute(
    const tsc_blueprint_library_t *library, const char *name, size_t name_len,
    const char *value, size_t value_len, tsc_blueprint_library_t **out);
/* END GENERATED blueprint_library_attributes */
/* BEGIN GENERATED actor_blueprint_tags from bindings/blueprint.yaml, do not edit */
TSC_API tsc_status_t tsc_actor_blueprint_get_tags(tsc_actor_blueprint_t *blueprint,
                                                  tsc_string_list_t *out);
/* Whether the id or any tag matches the shell-style wildcard pattern. */
TSC_API tsc_status_t tsc_actor_blueprint_match_tags(tsc_actor_blueprint_t *blueprint,
                                                    const char *pattern, size_t pattern_len,
                                                    int32_t *out);
/* END GENERATED actor_blueprint_tags */

/* --- Debug drawing ------------------------------------------------------------------ */

/* BEGIN GENERATED debug_clear from bindings/debug.yaml, do not edit */
/* Removes the persistent debug shapes (LibCarla ue5-dev); TSC_ERROR without (0.10.0). */
TSC_API tsc_status_t tsc_debug_clear_shapes(tsc_world_t *world);
/* Removes the persistent debug strings; TSC_ERROR as tsc_debug_clear_shapes. */
TSC_API tsc_status_t tsc_debug_clear_strings(tsc_world_t *world);
/* END GENERATED debug_clear */

/* --- World settings ----------------------------------------------------------------- */

/* The EpisodeSettings fields tsc_world_settings_t does not carry. */
typedef struct {
  double max_culling_distance;
  double tile_stream_distance;
  double actor_active_distance;
  int32_t deterministic_ragdolls;
  int32_t spectator_as_ego;
} tsc_world_settings_ext_t;

TSC_API tsc_status_t tsc_world_get_settings_ext(tsc_world_t *world, tsc_world_settings_t *out,
                                                tsc_world_settings_ext_t *out_ext);
/* Like tsc_world_apply_settings, with the extra fields too. Fields LibCarla
 * has beyond these keep the server's values. */
TSC_API tsc_status_t tsc_world_apply_settings_ext(tsc_world_t *world,
                                                  const tsc_world_settings_t *settings,
                                                  const tsc_world_settings_ext_t *ext,
                                                  double timeout_seconds, uint64_t *out_frame);

/* --- Geometry ---------------------------------------------------------------------- */

/* BEGIN GENERATED transform from bindings/transform.yaml, do not edit */
/* 16 doubles, row major, in the linked LibCarla's rotation convention. */
TSC_API tsc_status_t tsc_transform_get_matrix(const tsc_transform_t *transform, double *out16);
TSC_API tsc_status_t tsc_transform_get_inverse_matrix(const tsc_transform_t *transform,
                                                      double *out16);
/* END GENERATED transform */

/* ------------------------------------------------------------------------ */
/* Issue #19: actor state, attributes, parent, tags, physics at a location,  */
/* skeleton queries, textures, traffic signs                                */
/* ------------------------------------------------------------------------ */

/* carla::rpc::ActorState. */
typedef enum {
  TSC_ACTOR_STATE_INVALID = 0,
  TSC_ACTOR_STATE_ACTIVE = 1,
  TSC_ACTOR_STATE_DORMANT = 2,
  TSC_ACTOR_STATE_PENDING_KILL = 3
} tsc_actor_state_t;

/* BEGIN GENERATED actor_state from bindings/actor.yaml, do not edit */
TSC_API tsc_status_t tsc_actor_get_actor_name(tsc_actor_t *actor, tsc_string_t *out);
TSC_API tsc_status_t tsc_actor_get_actor_class_name(tsc_actor_t *actor, tsc_string_t *out);
/* tsc_actor_state_t */
TSC_API tsc_status_t tsc_actor_get_actor_state(tsc_actor_t *actor, int32_t *out);
TSC_API tsc_status_t tsc_actor_is_active(tsc_actor_t *actor, int32_t *out_active);
TSC_API tsc_status_t tsc_actor_is_dormant(tsc_actor_t *actor, int32_t *out_dormant);
/* *out = NULL (TSC_OK) when the actor has no parent. */
TSC_API tsc_status_t tsc_actor_get_parent(tsc_actor_t *actor, tsc_actor_t **out);
/* CityObjectLabel values (client-side data, no RPC). */
TSC_API tsc_status_t tsc_actor_get_semantic_tags(tsc_actor_t *actor,
                                                 uint8_t *out, size_t capacity, size_t *out_count);
/* END GENERATED actor_state */
/* BEGIN GENERATED actor_world from bindings/actor.yaml, do not edit */
/* The world (episode) the actor belongs to. */
TSC_API tsc_status_t tsc_actor_get_world(tsc_actor_t *actor, tsc_world_t **out);
/* END GENERATED actor_world */

/* The actor's attributes (as spawned): out_ids->items[i] has the value
 * out_values->items[i]. Both lists are owned by the caller. */
TSC_API tsc_status_t tsc_actor_get_attributes(tsc_actor_t *actor, tsc_string_list_t *out_ids,
                                              tsc_string_list_t *out_values);

/* BEGIN GENERATED actor_physics_at_location from bindings/actor.yaml, do not edit */
TSC_API tsc_status_t tsc_actor_set_collisions(tsc_actor_t *actor, int32_t enabled);
TSC_API tsc_status_t tsc_actor_enable_constant_velocity(tsc_actor_t *actor,
                                                        const tsc_vector3d_t *velocity);
TSC_API tsc_status_t tsc_actor_disable_constant_velocity(tsc_actor_t *actor);
TSC_API tsc_status_t tsc_actor_add_force_at_location(tsc_actor_t *actor,
                                                     const tsc_vector3d_t *force,
                                                     const tsc_location_t *location);
TSC_API tsc_status_t tsc_actor_add_impulse_at_location(tsc_actor_t *actor,
                                                       const tsc_vector3d_t *impulse,
                                                       const tsc_location_t *location);
/* END GENERATED actor_physics_at_location */

/* Skeleton queries (one RPC each). They exist in LibCarla ue5-dev but not in
 * CARLA 0.10.0's LibCarla: built against that, they fail with TSC_ERROR. */
/* BEGIN GENERATED actor_skeleton from bindings/actor.yaml, do not edit */
TSC_API tsc_status_t tsc_actor_get_bone_names(tsc_actor_t *actor, tsc_string_list_t *out);
TSC_API tsc_status_t tsc_actor_get_bone_world_transforms(tsc_actor_t *actor,
                                                         tsc_transform_list_t *out);
TSC_API tsc_status_t tsc_actor_get_bone_relative_transforms(tsc_actor_t *actor,
                                                            tsc_transform_list_t *out);
TSC_API tsc_status_t tsc_actor_get_component_names(tsc_actor_t *actor, tsc_string_list_t *out);
TSC_API tsc_status_t tsc_actor_get_component_world_transform(
    tsc_actor_t *actor, const char *component_name, size_t component_name_len,
    tsc_transform_t *out);
TSC_API tsc_status_t tsc_actor_get_component_relative_transform(
    tsc_actor_t *actor, const char *component_name, size_t component_name_len,
    tsc_transform_t *out);
TSC_API tsc_status_t tsc_actor_get_socket_names(tsc_actor_t *actor, tsc_string_list_t *out);
TSC_API tsc_status_t tsc_actor_get_socket_world_transforms(tsc_actor_t *actor,
                                                           tsc_transform_list_t *out);
TSC_API tsc_status_t tsc_actor_get_socket_relative_transforms(tsc_actor_t *actor,
                                                              tsc_transform_list_t *out);
/* END GENERATED actor_skeleton */

/* Textures. TSC_INVALID_ARGUMENT for an unknown material parameter or an
 * invalid texture (see tsc_texture_color_t). */
/* BEGIN GENERATED actor_texture from bindings/actor.yaml, do not edit */
/* material_parameter: tsc_material_parameter_t. */
TSC_API tsc_status_t tsc_actor_apply_texture_color(tsc_actor_t *actor, int32_t material_parameter,
                                                   const tsc_texture_color_t *texture);
TSC_API tsc_status_t tsc_actor_apply_texture_float_color(tsc_actor_t *actor,
                                                         int32_t material_parameter,
                                                         const tsc_texture_float_color_t *texture);
/* END GENERATED actor_texture */

/* Traffic signs. A traffic light handle is also a valid traffic sign handle. */
typedef struct tsc_traffic_sign tsc_traffic_sign_t; /* also an actor */
/* Checked downcast (traffic signs and traffic lights); TSC_TYPE_ERROR otherwise. */
TSC_API tsc_status_t tsc_actor_as_traffic_sign(tsc_actor_t *actor, tsc_traffic_sign_t **out);
/* BEGIN GENERATED traffic_sign from bindings/traffic_sign.yaml, do not edit */
TSC_API tsc_status_t tsc_traffic_sign_get_trigger_volume(tsc_traffic_sign_t *sign,
                                                         tsc_bounding_box_t *out);
/* END GENERATED traffic_sign */

/* ------------------------------------------------------------------------ */
/* World queries, environment objects, lights, textures, on_tick (#21)      */
/* ------------------------------------------------------------------------ */

typedef struct tsc_light_manager tsc_light_manager_t;
typedef struct tsc_tick_listener tsc_tick_listener_t;

/* Lists the library allocates for the caller; free with the matching *_free
 * (a no-op on a zeroed or already freed list). */

typedef struct {
  uint32_t actor_id;
  uint32_t light_state; /* VehicleLightState bit flags */
} tsc_vehicle_light_state_t;

typedef struct {
  tsc_vehicle_light_state_t *items;
  size_t size;
} tsc_vehicle_light_state_list_t;
TSC_API void tsc_vehicle_light_state_list_free(tsc_vehicle_light_state_list_t *list);

typedef struct {
  tsc_bounding_box_t *items;
  size_t size;
} tsc_bounding_box_list_t;
TSC_API void tsc_bounding_box_list_free(tsc_bounding_box_list_t *list);

typedef struct {
  uint64_t id;
  tsc_string_t name;
  tsc_transform_t transform;
  tsc_bounding_box_t bounding_box;
  int32_t type; /* CityObjectLabel */
  int32_t reserved0;
} tsc_environment_object_t;

typedef struct {
  tsc_environment_object_t *items;
  size_t size;
} tsc_environment_object_list_t;
/* Also frees every name. */
TSC_API void tsc_environment_object_list_free(tsc_environment_object_list_t *list);

typedef struct {
  tsc_location_t location;
  int32_t label; /* CityObjectLabel */
  int32_t reserved0;
} tsc_labelled_point_t;

typedef struct {
  tsc_labelled_point_t *items;
  size_t size;
} tsc_labelled_point_list_t;
TSC_API void tsc_labelled_point_list_free(tsc_labelled_point_list_t *list);

/* BEGIN GENERATED world from bindings/world.yaml, do not edit */
TSC_API tsc_status_t tsc_world_get_spectator(tsc_world_t *world, tsc_actor_t **out);
/* *out = NULL (TSC_OK) when no traffic light has this OpenDRIVE signal id. */
TSC_API tsc_status_t tsc_world_get_traffic_light_from_opendrive_id(
    tsc_world_t *world, const char *traffic_light_id, size_t traffic_light_id_len,
    tsc_traffic_light_t **out);
/* *out = NULL (TSC_OK) when no traffic sign actor has the landmark's id. */
TSC_API tsc_status_t tsc_world_get_traffic_sign(tsc_world_t *world,
                                                const tsc_landmark_handle_t *landmark,
                                                tsc_actor_t **out);
/* *out = NULL (TSC_OK) when no traffic light actor has the landmark's id. */
TSC_API tsc_status_t tsc_world_get_traffic_light(tsc_world_t *world,
                                                 const tsc_landmark_handle_t *landmark,
                                                 tsc_traffic_light_t **out);
TSC_API tsc_status_t tsc_world_get_traffic_lights_from_waypoint(tsc_world_t *world,
                                                                const tsc_waypoint_t *waypoint,
                                                                double distance,
                                                                tsc_traffic_light_list_t **out);
/* An empty list for an id that names no junction (LibCarla would dereference NULL). */
TSC_API tsc_status_t tsc_world_get_traffic_lights_in_junction(tsc_world_t *world,
                                                              int32_t junction_id,
                                                              tsc_traffic_light_list_t **out);
TSC_API tsc_status_t tsc_world_freeze_all_traffic_lights(tsc_world_t *world, int32_t frozen);
TSC_API tsc_status_t tsc_world_reset_all_traffic_lights(tsc_world_t *world);
TSC_API tsc_status_t tsc_world_get_vehicles_light_states(tsc_world_t *world,
                                                         tsc_vehicle_light_state_list_t *out);
/* bb_type / object_type: a CityObjectLabel in [0, 255] (255 = Any). */
TSC_API tsc_status_t tsc_world_get_level_bbs(tsc_world_t *world, int32_t bb_type,
                                             tsc_bounding_box_list_t *out);
TSC_API tsc_status_t tsc_world_get_environment_objects(tsc_world_t *world, int32_t object_type,
                                                       tsc_environment_object_list_t *out);
TSC_API tsc_status_t tsc_world_enable_environment_objects(
    tsc_world_t *world, const uint64_t *env_objects_ids, size_t count, int32_t enable);
TSC_API tsc_status_t tsc_world_get_names_of_all_objects(tsc_world_t *world, tsc_string_list_t *out);
TSC_API tsc_status_t tsc_world_cast_ray(tsc_world_t *world, const tsc_location_t *initial_location,
                                        const tsc_location_t *final_location,
                                        tsc_labelled_point_list_t *out);
/* *has_value = 0 when nothing is hit within search_distance. */
TSC_API tsc_status_t tsc_world_project_point(tsc_world_t *world, const tsc_location_t *location,
                                             const tsc_vector3d_t *direction,
                                             double search_distance,
                                             int32_t *has_value, tsc_labelled_point_t *out);
TSC_API tsc_status_t tsc_world_ground_projection(tsc_world_t *world, const tsc_location_t *location,
                                                 double search_distance,
                                                 int32_t *has_value, tsc_labelled_point_t *out);
/* map_layers: CARLA MapLayer bit flags. */
TSC_API tsc_status_t tsc_world_load_map_layer(tsc_world_t *world, uint16_t map_layers);
TSC_API tsc_status_t tsc_world_unload_map_layer(tsc_world_t *world, uint16_t map_layers);
TSC_API tsc_status_t tsc_world_set_pedestrians_seed(tsc_world_t *world, uint32_t seed);
TSC_API tsc_status_t tsc_world_set_pedestrians_cross_factor(tsc_world_t *world, double percentage);
/* LibCarla ue5-dev only; TSC_ERROR with CARLA 0.10.0. */
TSC_API tsc_status_t tsc_world_get_imu_sensor_gravity(tsc_world_t *world, double *out);
TSC_API tsc_status_t tsc_world_set_imu_sensor_gravity(tsc_world_t *world, double gravity);
/* names: count >= 1 object names; material_parameter: tsc_material_parameter_t. */
TSC_API tsc_status_t tsc_world_apply_color_texture_to_objects(
    tsc_world_t *world, const tsc_string_t *names, size_t count, int32_t material_parameter,
    const tsc_texture_color_t *texture);
TSC_API tsc_status_t tsc_world_apply_float_color_texture_to_objects(
    tsc_world_t *world, const tsc_string_t *names, size_t count, int32_t material_parameter,
    const tsc_texture_float_color_t *texture);
TSC_API tsc_status_t tsc_world_apply_textures_to_objects(
    tsc_world_t *world, const tsc_string_t *names, size_t count,
    const tsc_texture_color_t *diffuse_texture, const tsc_texture_float_color_t *emissive_texture,
    const tsc_texture_float_color_t *normal_texture,
    const tsc_texture_float_color_t *ao_roughness_metallic_emissive_texture);
TSC_API tsc_status_t tsc_world_get_light_manager(tsc_world_t *world, tsc_light_manager_t **out);
/* END GENERATED world */

/* --- Light manager (World::GetLightManager) ------------------------------------------ */

typedef enum {
  TSC_LIGHT_GROUP_NONE = 0,
  TSC_LIGHT_GROUP_VEHICLE = 1,
  TSC_LIGHT_GROUP_STREET = 2,
  TSC_LIGHT_GROUP_BUILDING = 3,
  TSC_LIGHT_GROUP_OTHER = 4
} tsc_light_group_t;

typedef struct {
  uint32_t id;
  uint32_t reserved0;
  tsc_location_t location;
} tsc_light_t;

typedef struct {
  tsc_light_t *items;
  size_t size;
} tsc_light_list_t;
TSC_API void tsc_light_list_free(tsc_light_list_t *list);

/* client::LightState. The server keeps no alpha: colors read back with 255. */
typedef struct {
  double intensity;
  tsc_color_t color;
  int32_t group; /* tsc_light_group_t */
  int32_t active;
  int32_t reserved0;
} tsc_light_state_t;

/* Bulk operations on `count` lights given by id; the value arrays have
 * `count` entries. TSC_NOT_FOUND if an id is not a light of this manager.
 * Every id and value is checked first: a failing call changes nothing.
 * Changes reach the server at the next tick. */
TSC_API tsc_status_t tsc_light_manager_get_light_states(tsc_light_manager_t *manager,
                                                        const uint32_t *ids, size_t count,
                                                        tsc_light_state_t *out);
TSC_API tsc_status_t tsc_light_manager_set_active(tsc_light_manager_t *manager,
                                                  const uint32_t *ids, size_t count,
                                                  const int32_t *active);
TSC_API tsc_status_t tsc_light_manager_set_color(tsc_light_manager_t *manager, const uint32_t *ids,
                                                 size_t count, const tsc_color_t *colors);
TSC_API tsc_status_t tsc_light_manager_set_intensity(tsc_light_manager_t *manager,
                                                     const uint32_t *ids, size_t count,
                                                     const double *intensities);
TSC_API tsc_status_t tsc_light_manager_set_light_group(tsc_light_manager_t *manager,
                                                       const uint32_t *ids, size_t count,
                                                       const int32_t *groups);
TSC_API tsc_status_t tsc_light_manager_set_light_state(tsc_light_manager_t *manager,
                                                       const uint32_t *ids, size_t count,
                                                       const tsc_light_state_t *states);
/* BEGIN GENERATED light_manager from bindings/light_manager.yaml, do not edit */
/* group: tsc_light_group_t; TSC_LIGHT_GROUP_NONE for every group. */
TSC_API tsc_status_t tsc_light_manager_get_all_lights(tsc_light_manager_t *manager, int32_t group,
                                                      tsc_light_list_t *out);
TSC_API tsc_status_t tsc_light_manager_get_turned_on_lights(tsc_light_manager_t *manager,
                                                            int32_t group, tsc_light_list_t *out);
TSC_API tsc_status_t tsc_light_manager_get_turned_off_lights(tsc_light_manager_t *manager,
                                                             int32_t group, tsc_light_list_t *out);
TSC_API tsc_status_t tsc_light_manager_set_day_night_cycle(tsc_light_manager_t *manager,
                                                           int32_t active);
/* END GENERATED light_manager */

/* --- on_tick ---------------------------------------------------------------------- */

/* Registers a World::OnTick callback that only queues each tick's snapshot in
 * the listener (at most queue_capacity, oldest dropped first; 0 = unbounded):
 * no caller code runs on LibCarla's threads. Releasing the listener, or
 * tsc_tick_listener_stop, removes the registration. */
TSC_API tsc_status_t tsc_world_on_tick(tsc_world_t *world, size_t queue_capacity,
                                       tsc_tick_listener_t **out);
/* An opaque identity of the client connection (LibCarla's Simulator) the
 * world belongs to: equal for every World of one Client, also across
 * load_world. OnTick callback ids are unique per client, and a client's
 * callbacks survive load_world. */
TSC_API tsc_status_t tsc_world_get_client_token(tsc_world_t *world, uint64_t *out);
/* LibCarla publishes a tick's state before it runs the OnTick callbacks, so
 * World::Tick can return before the listener has queued that frame. Waits up
 * to timeout_seconds until the listener has received a snapshot of frame
 * >= `frame`; *out_reached = 0 on timeout (not an error). */
TSC_API tsc_status_t tsc_tick_listener_wait_for_frame(const tsc_tick_listener_t *listener,
                                                      uint64_t frame, double timeout_seconds,
                                                      int32_t *out_reached);
/* LibCarla's callback id (what World::RemoveOnTick takes). */
TSC_API tsc_status_t tsc_tick_listener_get_id(const tsc_tick_listener_t *listener, uint64_t *out);
TSC_API tsc_status_t tsc_tick_listener_pending_count(const tsc_tick_listener_t *listener,
                                                     size_t *out);
/* *out = NULL (TSC_OK) when the queue is empty. */
TSC_API tsc_status_t tsc_tick_listener_poll(tsc_tick_listener_t *listener,
                                            tsc_world_snapshot_t **out);
/* Removes the registration and drops the queued snapshots. Idempotent. */
TSC_API tsc_status_t tsc_tick_listener_stop(tsc_tick_listener_t *listener);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* TYPESAFE_CARLA_FFI_H */
