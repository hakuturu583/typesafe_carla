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

/* ABI version. Bump MAJOR on any incompatible change to this header. */
#define TSC_ABI_VERSION_MAJOR 1
#define TSC_ABI_VERSION_MINOR 0
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
/* LibCarla version the library was built against, e.g. "0.9.15". */
TSC_API const char *tsc_libcarla_version(void);
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
  TSC_KIND_ACTOR_BLUEPRINT = 7
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
TSC_API tsc_status_t tsc_actor_destroy(tsc_actor_t *actor, int32_t *out_destroyed);
/* Checked downcast. TSC_TYPE_ERROR if the actor is not a vehicle. */
TSC_API tsc_status_t tsc_actor_as_vehicle(tsc_actor_t *actor, tsc_vehicle_t **out_vehicle);

/* ------------------------------------------------------------------------ */
/* Vehicle                                                                  */
/* ------------------------------------------------------------------------ */

TSC_API tsc_status_t tsc_vehicle_apply_control(tsc_vehicle_t *vehicle,
                                               const tsc_vehicle_control_t *control);
TSC_API tsc_status_t tsc_vehicle_get_control(tsc_vehicle_t *vehicle, tsc_vehicle_control_t *out);
TSC_API tsc_status_t tsc_vehicle_set_autopilot(tsc_vehicle_t *vehicle, int32_t enabled,
                                               uint16_t tm_port);

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

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* TYPESAFE_CARLA_FFI_H */
