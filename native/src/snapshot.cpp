// World snapshots: the state of every actor at one frame.
#include "internal.hpp"

using namespace tsc;

namespace {

const carla::client::WorldSnapshot &snapshot_of(const tsc_world_snapshot_t *s) {
  return check_handle(s, "snapshot", TSC_KIND_WORLD_SNAPSHOT)->snapshot;
}

carla::client::World &world_of(tsc_world_t *w) {
  return check_handle(w, "world", TSC_KIND_WORLD)->world;
}

tsc_actor_snapshot_t to_c(const carla::client::ActorSnapshot &a) {
  tsc_actor_snapshot_t out{};
  out.id = a.id;
  out.transform = from_carla(a.transform);
  out.velocity = from_carla(a.velocity);
  out.angular_velocity = from_carla(a.angular_velocity);
  out.acceleration = from_carla(a.acceleration);
  return out;
}

}  // namespace

extern "C" {

tsc_status_t tsc_world_get_snapshot(tsc_world_t *world, tsc_world_snapshot_t **out) {
  return TSC_GUARD({
    require_ptr(out, "out");
    *out = nullptr;
    *out = new tsc_world_snapshot(world_of(world).GetSnapshot());
  });
}

tsc_status_t tsc_world_wait_for_tick(tsc_world_t *world, double timeout_seconds,
                                     tsc_world_snapshot_t **out) {
  return TSC_GUARD({
    require_ptr(out, "out");
    *out = nullptr;
    *out = new tsc_world_snapshot(world_of(world).WaitForTick(seconds_to_duration(timeout_seconds)));
  });
}

tsc_status_t tsc_world_snapshot_get_id(const tsc_world_snapshot_t *s, uint64_t *out) {
  return TSC_GUARD({ *require_ptr(out, "out") = snapshot_of(s).GetId(); });
}

tsc_status_t tsc_world_snapshot_get_timestamp(const tsc_world_snapshot_t *s, tsc_timestamp_t *out) {
  return TSC_GUARD({
    const carla::client::Timestamp &t = snapshot_of(s).GetTimestamp();
    *require_ptr(out, "out") =
        tsc_timestamp_t{t.frame, t.elapsed_seconds, t.delta_seconds, t.platform_timestamp};
  });
}

size_t tsc_world_snapshot_size(const tsc_world_snapshot_t *s) {
  if (s == nullptr || s->kind != TSC_KIND_WORLD_SNAPSHOT) return 0;
  return s->snapshot.size();
}

tsc_status_t tsc_world_snapshot_get(const tsc_world_snapshot_t *s, size_t index,
                                    tsc_actor_snapshot_t *out) {
  return TSC_GUARD({
    require_ptr(out, "out");
    const auto &snapshot = snapshot_of(s);
    if (index >= snapshot.size()) {
      fail(TSC_NOT_FOUND, "index " + std::to_string(index) + " out of range for snapshot of size " +
                              std::to_string(snapshot.size()));
    }
    auto it = snapshot.begin();
    std::advance(it, static_cast<std::ptrdiff_t>(index));
    *out = to_c(*it);
  });
}

tsc_status_t tsc_world_snapshot_find(const tsc_world_snapshot_t *s, uint32_t actor_id,
                                     tsc_actor_snapshot_t *out) {
  return TSC_GUARD({
    require_ptr(out, "out");
    auto found = snapshot_of(s).Find(actor_id);
    if (!found) fail(TSC_NOT_FOUND, "actor " + std::to_string(actor_id) + " is not in the snapshot");
    *out = to_c(*found);
  });
}

}  // extern "C"
