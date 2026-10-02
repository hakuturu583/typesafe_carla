// Client: the hand-written part (the constructor, and load_world_if_different,
// which compares episodes around the load). The rest is generated
// (bindings/client.yaml).
#include "internal.hpp"

using namespace tsc;

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

tsc_status_t tsc_client_load_world_if_different(tsc_client_t *client, const char *map_name,
                                                 size_t map_name_len, int32_t reset_settings,
                                                 tsc_world_t **out) {
  return new_handle(__func__, out, [&]() -> tsc_world_t * {
    auto &c = client_of(client);
    const std::string name = to_string(map_name, map_name_len, "map_name");
    if (name.empty()) fail(TSC_INVALID_ARGUMENT, "map_name must not be empty");
    // LibCarla decides (by map name) and returns nothing; a load always
    // starts a new episode, which is how we tell whether it loaded.
    const auto before = c.GetWorld().GetId();
    c.LoadWorldIfDifferent(name, reset_settings != 0);
    auto world = c.GetWorld();
    if (world.GetId() == before) return nullptr;
    return new tsc_world(std::move(world));
  });
}

}  // extern "C"
