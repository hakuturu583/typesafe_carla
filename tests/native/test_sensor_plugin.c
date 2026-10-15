/* A minimal sensor plugin (ABI 5.3) for test_sensor_source: it finds the
 * library's functions through get_proc only (it does not link against it)
 * and installs a source that claims nothing. */
#include "typesafe_carla/ffi.h"

#include <string.h>

static int32_t claims(void *user, tsc_actor_t *sensor, uint32_t id, const char *type_id,
                      size_t len) {
  (void)user;
  (void)sensor;
  (void)id;
  return len == 14 && memcmp(type_id, "plugin.claimed", 14) == 0;
}

static const char *last_error(void *user) {
  (void)user;
  return "";
}

typedef tsc_status_t (*install_fn)(const tsc_sensor_source_t *);

TSC_API int32_t tsc_sensor_plugin_init(uint32_t abi_version, void *(*get_proc)(const char *),
                                       const char **out_error) {
  if ((abi_version >> 16) != TSC_ABI_VERSION_MAJOR) {
    *out_error = "ABI major mismatch";
    return TSC_VERSION_ERROR;
  }
  if (get_proc("not_a_tsc_function") != NULL) {
    *out_error = "get_proc returned a non-tsc symbol";
    return TSC_ERROR;
  }
  install_fn install;
  void *proc = get_proc("tsc_sensor_source_install");
  memcpy(&install, &proc, sizeof install); /* object to function pointer, C99-clean */
  if (install == NULL) {
    *out_error = "tsc_sensor_source_install not found";
    return TSC_ERROR;
  }
  tsc_sensor_source_t source;
  memset(&source, 0, sizeof source);
  source.abi_major = TSC_ABI_VERSION_MAJOR;
  source.struct_size = sizeof source;
  source.claims = claims;
  source.last_error = last_error;
  return install(&source);
}
