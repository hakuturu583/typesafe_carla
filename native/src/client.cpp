#include "internal.hpp"

using namespace tsc;

namespace {

carla::client::Client &client_of(tsc_client_t *c) {
  return check_handle(c, "client", TSC_KIND_CLIENT)->client;
}

}  // namespace

extern "C" {

tsc_status_t tsc_client_create(const char *host, size_t host_len, uint16_t port,
                               tsc_client_t **out_client) {
  return TSC_GUARD({
    require_ptr(out_client, "out_client");
    *out_client = nullptr;
    const std::string h = to_string(host, host_len, "host");
    if (h.empty()) fail(TSC_INVALID_ARGUMENT, "host must not be empty");
    *out_client = new tsc_client(h, port);
  });
}

tsc_status_t tsc_client_set_timeout(tsc_client_t *client, double seconds) {
  return TSC_GUARD({
    client_of(client).SetTimeout(seconds_to_duration(seconds));
  });
}

tsc_status_t tsc_client_get_timeout(tsc_client_t *client, double *out_seconds) {
  return TSC_GUARD({
    require_ptr(out_seconds, "out_seconds");
    auto timeout = client_of(client).GetTimeout();
    *out_seconds = static_cast<double>(timeout.milliseconds()) / 1000.0;
  });
}

tsc_status_t tsc_client_get_client_version(tsc_client_t *client, tsc_string_t *out) {
  return TSC_GUARD({
    string_assign(out, client_of(client).GetClientVersion());
  });
}

tsc_status_t tsc_client_get_server_version(tsc_client_t *client, tsc_string_t *out) {
  return TSC_GUARD({
    string_assign(out, client_of(client).GetServerVersion());
  });
}

tsc_status_t tsc_client_get_world(tsc_client_t *client, tsc_world_t **out_world) {
  return TSC_GUARD({
    require_ptr(out_world, "out_world");
    *out_world = nullptr;
    *out_world = new tsc_world(client_of(client).GetWorld());
  });
}

tsc_status_t tsc_client_load_world(tsc_client_t *client, const char *map_name,
                                   size_t map_name_len, int32_t reset_settings,
                                   tsc_world_t **out_world) {
  return TSC_GUARD({
    require_ptr(out_world, "out_world");
    *out_world = nullptr;
    auto &c = client_of(client);
    *out_world = new tsc_world(
        c.LoadWorld(to_string(map_name, map_name_len, "map_name"), reset_settings != 0));
  });
}

tsc_status_t tsc_client_reload_world(tsc_client_t *client, int32_t reset_settings,
                                     tsc_world_t **out_world) {
  return TSC_GUARD({
    require_ptr(out_world, "out_world");
    *out_world = nullptr;
    auto &c = client_of(client);
    *out_world = new tsc_world(c.ReloadWorld(reset_settings != 0));
  });
}

}  // extern "C"
