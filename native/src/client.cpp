// Client: the hand-written part (the constructor, and load_world_if_different,
// which compares episodes around the load). The rest is generated
// (bindings/client.yaml).
#include "internal.hpp"

using namespace tsc;

extern "C" {

tsc_status_t tsc_client_create(const char *host, size_t host_len, uint16_t port,
                               size_t worker_threads, tsc_client_t **out_client) {
  return new_handle(__func__, out_client, [&] {
    return new tsc_client(to_nonempty_string(host, host_len, "host"), port, worker_threads);
  }, "out_client");
}

tsc_status_t tsc_client_load_world_if_different(tsc_client_t *client, const char *map_name,
                                                 size_t map_name_len, int32_t reset_settings,
                                                 uint16_t map_layers, tsc_world_t **out) {
  return new_handle(__func__, out, [&]() -> tsc_world_t * {
    auto &c = client_of(client);
    const std::string name = to_nonempty_string(map_name, map_name_len, "map_name");
    // LibCarla decides (by map name) and returns nothing; a load always
    // starts a new episode, which is how we tell whether it loaded.
    const auto before = c.GetWorld().GetId();
    c.LoadWorldIfDifferent(name, reset_settings != 0,
                           static_cast<carla::rpc::MapLayer>(map_layers));
    auto world = c.GetWorld();
    if (world.GetId() == before) return nullptr;
    return new tsc_world(std::move(world));
  });
}

}  // extern "C"
