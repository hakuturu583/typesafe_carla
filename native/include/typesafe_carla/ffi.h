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
 * 2.1: tsc_sensor_pending_count; tsc_sensor_listen queue_capacity 0 = unbounded. */
#define TSC_ABI_VERSION_MAJOR 2
#define TSC_ABI_VERSION_MINOR 1
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
  TSC_KIND_JUNCTION = 19
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

TSC_API tsc_status_t tsc_client_create(const char *host, size_t host_len, uint16_t port,
                                       tsc_client_t **out_client);
TSC_API tsc_status_t tsc_client_set_timeout(tsc_client_t *client, double seconds);
TSC_API tsc_status_t tsc_client_get_timeout(tsc_client_t *client, double *out_seconds);
TSC_API tsc_status_t tsc_client_get_client_version(tsc_client_t *client, tsc_string_t *out);
TSC_API tsc_status_t tsc_client_get_server_version(tsc_client_t *client, tsc_string_t *out);
TSC_API tsc_status_t tsc_client_get_world(tsc_client_t *client, tsc_world_t **out_world);
TSC_API tsc_status_t tsc_client_load_world(tsc_client_t *client, const char *map_name,
                                           size_t map_name_len, int32_t reset_settings,
                                           tsc_world_t **out_world);
TSC_API tsc_status_t tsc_client_reload_world(tsc_client_t *client, int32_t reset_settings,
                                             tsc_world_t **out_world);

/* ------------------------------------------------------------------------ */
/* World                                                                    */
/* ------------------------------------------------------------------------ */

TSC_API tsc_status_t tsc_world_get_id(tsc_world_t *world, uint64_t *out_id);
TSC_API tsc_status_t tsc_world_get_actors(tsc_world_t *world, tsc_actor_list_t **out_list);
/* TSC_NOT_FOUND if no actor has this id. */
TSC_API tsc_status_t tsc_world_get_actor(tsc_world_t *world, uint32_t actor_id,
                                         tsc_actor_t **out_actor);
TSC_API tsc_status_t tsc_world_get_blueprint_library(tsc_world_t *world,
                                                     tsc_blueprint_library_t **out_library);
TSC_API tsc_status_t tsc_world_spawn_actor(tsc_world_t *world,
                                           const tsc_actor_blueprint_t *blueprint,
                                           const tsc_transform_t *transform,
                                           tsc_actor_t *parent, /* nullable */
                                           tsc_actor_t **out_actor);
/* TSC_OK with *out_actor == NULL when the spawn location is occupied. */
TSC_API tsc_status_t tsc_world_try_spawn_actor(tsc_world_t *world,
                                               const tsc_actor_blueprint_t *blueprint,
                                               const tsc_transform_t *transform,
                                               tsc_actor_t *parent, /* nullable */
                                               tsc_actor_t **out_actor);
TSC_API tsc_status_t tsc_world_tick(tsc_world_t *world, double timeout_seconds,
                                    uint64_t *out_frame);
TSC_API tsc_status_t tsc_world_get_settings(tsc_world_t *world, tsc_world_settings_t *out);
TSC_API tsc_status_t tsc_world_apply_settings(tsc_world_t *world,
                                              const tsc_world_settings_t *settings,
                                              double timeout_seconds, uint64_t *out_frame);

/* ------------------------------------------------------------------------ */
/* Actor list                                                               */
/* ------------------------------------------------------------------------ */

TSC_API size_t tsc_actor_list_size(const tsc_actor_list_t *list);
TSC_API tsc_status_t tsc_actor_list_get(const tsc_actor_list_t *list, size_t index,
                                        tsc_actor_t **out_actor);
TSC_API tsc_status_t tsc_actor_list_filter(const tsc_actor_list_t *list, const char *pattern,
                                           size_t pattern_len, tsc_actor_list_t **out_list);

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
TSC_API tsc_status_t tsc_actor_destroy(tsc_actor_t *actor, int32_t *out_destroyed);
/* Checked downcast. TSC_TYPE_ERROR if the actor is not a vehicle. */
TSC_API tsc_status_t tsc_actor_as_vehicle(tsc_actor_t *actor, tsc_vehicle_t **out_vehicle);

/* ------------------------------------------------------------------------ */
/* Vehicle                                                                  */
/* ------------------------------------------------------------------------ */

TSC_API tsc_status_t tsc_vehicle_apply_control(tsc_vehicle_t *vehicle,
                                               const tsc_vehicle_control_t *control);
TSC_API tsc_status_t tsc_vehicle_get_control(tsc_vehicle_t *vehicle, tsc_vehicle_control_t *out);
/* BEGIN GENERATED vehicle from bindings/vehicle.yaml, do not edit */
TSC_API tsc_status_t tsc_vehicle_set_autopilot(tsc_vehicle_t *vehicle, int32_t enabled,
                                               uint16_t tm_port);
/* END GENERATED vehicle */

/* ------------------------------------------------------------------------ */
/* Blueprints                                                               */
/* ------------------------------------------------------------------------ */

TSC_API size_t tsc_blueprint_library_size(const tsc_blueprint_library_t *library);
TSC_API tsc_status_t tsc_blueprint_library_get(const tsc_blueprint_library_t *library,
                                               size_t index, tsc_actor_blueprint_t **out);
/* TSC_NOT_FOUND if no blueprint has this id. */
TSC_API tsc_status_t tsc_blueprint_library_find(const tsc_blueprint_library_t *library,
                                                const char *id, size_t id_len,
                                                tsc_actor_blueprint_t **out);
TSC_API tsc_status_t tsc_blueprint_library_filter(const tsc_blueprint_library_t *library,
                                                  const char *pattern, size_t pattern_len,
                                                  tsc_blueprint_library_t **out);

TSC_API tsc_status_t tsc_actor_blueprint_get_id(const tsc_actor_blueprint_t *blueprint,
                                                tsc_string_t *out);
TSC_API tsc_status_t tsc_actor_blueprint_has_tag(const tsc_actor_blueprint_t *blueprint,
                                                 const char *tag, size_t tag_len,
                                                 int32_t *out_has);
TSC_API tsc_status_t tsc_actor_blueprint_has_attribute(const tsc_actor_blueprint_t *blueprint,
                                                       const char *id, size_t id_len,
                                                       int32_t *out_has);
/* TSC_NOT_FOUND if the blueprint has no such attribute. */
TSC_API tsc_status_t tsc_actor_blueprint_get_attribute(const tsc_actor_blueprint_t *blueprint,
                                                       const char *id, size_t id_len,
                                                       tsc_actor_attribute_t *out);
/* TSC_NOT_FOUND for an unknown attribute, TSC_INVALID_ARGUMENT for a
 * non-modifiable attribute or a value that does not parse as its type. */
TSC_API tsc_status_t tsc_actor_blueprint_set_attribute(tsc_actor_blueprint_t *blueprint,
                                                       const char *id, size_t id_len,
                                                       const char *value, size_t value_len);

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

TSC_API tsc_status_t tsc_world_get_snapshot(tsc_world_t *world, tsc_world_snapshot_t **out);
TSC_API tsc_status_t tsc_world_wait_for_tick(tsc_world_t *world, double timeout_seconds,
                                             tsc_world_snapshot_t **out);
TSC_API tsc_status_t tsc_world_snapshot_get_id(const tsc_world_snapshot_t *s, uint64_t *out);
TSC_API tsc_status_t tsc_world_snapshot_get_timestamp(const tsc_world_snapshot_t *s,
                                                      tsc_timestamp_t *out);
TSC_API size_t tsc_world_snapshot_size(const tsc_world_snapshot_t *s);
/* By position, 0 <= index < size. */
TSC_API tsc_status_t tsc_world_snapshot_get(const tsc_world_snapshot_t *s, size_t index,
                                            tsc_actor_snapshot_t *out);
/* TSC_NOT_FOUND if the actor is not in the snapshot. */
TSC_API tsc_status_t tsc_world_snapshot_find(const tsc_world_snapshot_t *s, uint32_t actor_id,
                                             tsc_actor_snapshot_t *out);

/* --- Map and waypoints ----------------------------------------------------- */

typedef struct tsc_map tsc_map_t;
typedef struct tsc_waypoint tsc_waypoint_t;
typedef struct tsc_waypoint_list tsc_waypoint_list_t;

TSC_API tsc_status_t tsc_world_get_map(tsc_world_t *world, tsc_map_t **out);
TSC_API tsc_status_t tsc_map_get_name(const tsc_map_t *map, tsc_string_t *out);
TSC_API tsc_status_t tsc_map_to_opendrive(const tsc_map_t *map, tsc_string_t *out);
/* Two-call pattern: pass out=NULL to get the count. Copies min(capacity, n). */
TSC_API tsc_status_t tsc_map_get_spawn_points(const tsc_map_t *map, tsc_transform_t *out,
                                              size_t capacity, size_t *out_count);
/* *out = NULL (TSC_OK) when no waypoint matches. lane_type: CARLA LaneType bits. */
TSC_API tsc_status_t tsc_map_get_waypoint(const tsc_map_t *map, const tsc_location_t *location,
                                          int32_t project_to_road, int32_t lane_type,
                                          tsc_waypoint_t **out);
TSC_API tsc_status_t tsc_map_generate_waypoints(const tsc_map_t *map, double distance,
                                                tsc_waypoint_list_t **out);

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
TSC_API tsc_status_t tsc_waypoint_next(const tsc_waypoint_t *wp, double distance,
                                       tsc_waypoint_list_t **out);
TSC_API tsc_status_t tsc_waypoint_previous(const tsc_waypoint_t *wp, double distance,
                                           tsc_waypoint_list_t **out);
TSC_API tsc_status_t tsc_waypoint_next_until_lane_end(const tsc_waypoint_t *wp, double distance,
                                                      tsc_waypoint_list_t **out);
TSC_API tsc_status_t tsc_waypoint_previous_until_lane_start(const tsc_waypoint_t *wp,
                                                            double distance,
                                                            tsc_waypoint_list_t **out);
/* *out = NULL (TSC_OK) when there is no such lane. */
TSC_API tsc_status_t tsc_waypoint_get_left_lane(const tsc_waypoint_t *wp, tsc_waypoint_t **out);
TSC_API tsc_status_t tsc_waypoint_get_right_lane(const tsc_waypoint_t *wp, tsc_waypoint_t **out);
TSC_API size_t tsc_waypoint_list_size(const tsc_waypoint_list_t *list);
TSC_API tsc_status_t tsc_waypoint_list_get(const tsc_waypoint_list_t *list, size_t index,
                                           tsc_waypoint_t **out);

/* --- Physics control ------------------------------------------------------ */

#define TSC_MAX_WHEELS 8

typedef struct {
  double wheel_radius;
  double wheel_width;
  double wheel_mass;
  double max_steer_angle;
  double max_brake_torque;
  double max_hand_brake_torque;
  double friction_force_multiplier;
  double cornering_stiffness;
  int32_t affected_by_steering;
  int32_t affected_by_brake;
  int32_t affected_by_handbrake;
  int32_t affected_by_engine;
} tsc_wheel_physics_control_t;

/* A typed subset of rpc::VehiclePhysicsControl. apply() reads the current
 * control and overwrites only these fields, so the rest keep server values. */
typedef struct {
  double max_torque;
  double max_rpm;
  double final_ratio;
  double gear_change_time;
  double mass;
  double drag_coefficient;
  tsc_location_t center_of_mass;
  int32_t use_automatic_gears;
  int32_t wheel_count; /* <= TSC_MAX_WHEELS */
  tsc_wheel_physics_control_t wheels[TSC_MAX_WHEELS];
} tsc_vehicle_physics_control_t;

TSC_API tsc_status_t tsc_vehicle_get_physics_control(tsc_vehicle_t *vehicle,
                                                     tsc_vehicle_physics_control_t *out);
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
  TSC_COMMAND_SET_TRAFFIC_LIGHT_STATE = 17    /* flag: tsc_traffic_light_state_t */
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
  TSC_SENSOR_DATA_COLLISION = 5
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
TSC_API tsc_status_t tsc_sensor_is_listening(tsc_sensor_t *sensor, int32_t *out);
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
/* Points produced by one channel. */
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
/* Milestone 4: broader CARLA coverage (ABI 2.0)                            */
/* ------------------------------------------------------------------------ */

typedef struct tsc_walker tsc_walker_t;                   /* also an actor */
typedef struct tsc_walker_ai_controller tsc_walker_ai_controller_t; /* also an actor */
typedef struct tsc_traffic_light tsc_traffic_light_t;     /* also an actor */
typedef struct tsc_traffic_manager tsc_traffic_manager_t;
typedef struct tsc_landmark_list tsc_landmark_list_t;
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
/* *out = NULL (TSC_OK) when the vehicle is not affected by a traffic light. */
TSC_API tsc_status_t tsc_vehicle_get_traffic_light(tsc_vehicle_t *vehicle,
                                                   tsc_traffic_light_t **out);

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

TSC_API tsc_status_t tsc_walker_apply_control(tsc_walker_t *walker,
                                              const tsc_walker_control_t *control);
TSC_API tsc_status_t tsc_walker_get_control(tsc_walker_t *walker, tsc_walker_control_t *out);
/* BEGIN GENERATED walker_ai_controller from bindings/walker_ai_controller.yaml, do not edit */
TSC_API tsc_status_t tsc_walker_ai_controller_start(tsc_walker_ai_controller_t *controller);
TSC_API tsc_status_t tsc_walker_ai_controller_stop(tsc_walker_ai_controller_t *controller);
TSC_API tsc_status_t tsc_walker_ai_controller_go_to_location(tsc_walker_ai_controller_t *controller,
                                                             const tsc_location_t *destination);
TSC_API tsc_status_t tsc_walker_ai_controller_set_max_speed(tsc_walker_ai_controller_t *controller,
                                                            double max_speed);
/* END GENERATED walker_ai_controller */
/* *out_found = 0 when the navigation mesh yields no location. */
TSC_API tsc_status_t tsc_world_get_random_location_from_navigation(tsc_world_t *world,
                                                                   tsc_location_t *out,
                                                                   int32_t *out_found);

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

TSC_API tsc_status_t tsc_world_get_weather(tsc_world_t *world, tsc_weather_t *out);
TSC_API tsc_status_t tsc_world_set_weather(tsc_world_t *world, const tsc_weather_t *weather);
/* Whether the server simulates weather (when not, set_weather has no effect). */
TSC_API tsc_status_t tsc_world_is_weather_enabled(tsc_world_t *world, int32_t *out);
/* LibCarla's named presets ("ClearNoon", "HardRainNoon", ...); TSC_NOT_FOUND otherwise. */
TSC_API tsc_status_t tsc_weather_preset(const char *name, size_t name_len, tsc_weather_t *out);

/* --- Debug drawing ---------------------------------------------------------- */

/* Passed by pointer like every struct here: Codon passes small structs by value
 * as separate scalars, which does not match the C calling convention. */
typedef struct {
  uint8_t r, g, b, a;
} tsc_color_t;

TSC_API tsc_status_t tsc_debug_draw_point(tsc_world_t *world, const tsc_location_t *location,
                                          double size, const tsc_color_t *color, double life_time);
TSC_API tsc_status_t tsc_debug_draw_line(tsc_world_t *world, const tsc_location_t *begin,
                                         const tsc_location_t *end, double thickness,
                                         const tsc_color_t *color, double life_time);
TSC_API tsc_status_t tsc_debug_draw_arrow(tsc_world_t *world, const tsc_location_t *begin,
                                          const tsc_location_t *end, double thickness,
                                          double arrow_size, const tsc_color_t *color, double life_time);
TSC_API tsc_status_t tsc_debug_draw_box(tsc_world_t *world, const tsc_bounding_box_t *box,
                                        const tsc_rotation_t *rotation, double thickness,
                                        const tsc_color_t *color, double life_time);
TSC_API tsc_status_t tsc_debug_draw_string(tsc_world_t *world, const tsc_location_t *location,
                                           const char *text, size_t text_len, int32_t draw_shadow,
                                           const tsc_color_t *color, double life_time);

/* --- Recorder ---------------------------------------------------------------- */

/* All return the server's text output in *out (free with tsc_string_free). */
TSC_API tsc_status_t tsc_client_start_recorder(tsc_client_t *client, const char *name,
                                               size_t name_len, int32_t additional_data,
                                               tsc_string_t *out);
TSC_API tsc_status_t tsc_client_stop_recorder(tsc_client_t *client);
TSC_API tsc_status_t tsc_client_show_recorder_file_info(tsc_client_t *client, const char *name,
                                                        size_t name_len, int32_t show_all,
                                                        tsc_string_t *out);
TSC_API tsc_status_t tsc_client_show_recorder_collisions(tsc_client_t *client, const char *name,
                                                         size_t name_len, char type1, char type2,
                                                         tsc_string_t *out);
TSC_API tsc_status_t tsc_client_show_recorder_actors_blocked(tsc_client_t *client,
                                                             const char *name, size_t name_len,
                                                             double min_time,
                                                             double min_distance,
                                                             tsc_string_t *out);
TSC_API tsc_status_t tsc_client_replay_file(tsc_client_t *client, const char *name,
                                            size_t name_len, double start, double duration,
                                            uint32_t follow_id, int32_t replay_sensors,
                                            tsc_string_t *out);
TSC_API tsc_status_t tsc_client_stop_replayer(tsc_client_t *client, int32_t keep_actors);
TSC_API tsc_status_t tsc_client_set_replayer_time_factor(tsc_client_t *client, double factor);

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

TSC_API tsc_status_t tsc_client_generate_opendrive_world(
    tsc_client_t *client, const char *opendrive, size_t opendrive_len,
    const tsc_opendrive_parameters_t *parameters, int32_t reset_settings, tsc_world_t **out);

/* --- Map queries ------------------------------------------------------------------ */

/* Topology: pairs (begin, end) of the road network's lane segments, as a
 * waypoint list of 2 * pair_count entries [b0, e0, b1, e1, ...]. */
TSC_API tsc_status_t tsc_map_get_topology(const tsc_map_t *map, tsc_waypoint_list_t **out);
/* Two-call pattern, like tsc_map_get_spawn_points. */
TSC_API tsc_status_t tsc_map_get_crosswalks(const tsc_map_t *map, tsc_location_t *out,
                                            size_t capacity, size_t *out_count);

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
TSC_API tsc_status_t tsc_map_get_all_landmarks(const tsc_map_t *map, tsc_landmark_list_t **out);
TSC_API tsc_status_t tsc_map_get_landmarks_of_type(const tsc_map_t *map, const char *type,
                                                   size_t type_len, tsc_landmark_list_t **out);
TSC_API size_t tsc_landmark_list_size(const tsc_landmark_list_t *list);
TSC_API tsc_status_t tsc_landmark_list_get(const tsc_landmark_list_t *list, size_t index,
                                           tsc_landmark_t *out);

/* *out = NULL (TSC_OK) when the waypoint is not in a junction. */
TSC_API tsc_status_t tsc_waypoint_get_junction(const tsc_waypoint_t *wp, tsc_junction_t **out);
TSC_API tsc_status_t tsc_junction_get_id(const tsc_junction_t *j, int32_t *out);
TSC_API tsc_status_t tsc_junction_get_bounding_box(const tsc_junction_t *j,
                                                   tsc_bounding_box_t *out);
/* Pairs (entry, exit) as a waypoint list [b0, e0, b1, e1, ...]. */
TSC_API tsc_status_t tsc_junction_get_waypoints(const tsc_junction_t *j, int32_t lane_type,
                                                tsc_waypoint_list_t **out);

/* --- Traffic Manager ---------------------------------------------------------------- */

TSC_API tsc_status_t tsc_client_get_traffic_manager(tsc_client_t *client, uint16_t port,
                                                    tsc_traffic_manager_t **out);
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

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* TYPESAFE_CARLA_FFI_H */
