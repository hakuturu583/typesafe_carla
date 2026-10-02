// Issue #21: World queries (spectator, traffic lights, vehicle light states,
// environment objects, ray casts and projections, IMU gravity, textures) and
// World::OnTick, whose snapshots are queued for the program's thread.
#include "internal.hpp"

using namespace tsc;

namespace tsc {

// Conversions named in bindings/types.yaml (# --- #21 ---).

std::vector<std::string> to_names(const tsc_string_t *names, size_t count, const char *name) {
  require_array(names, count, name);
  if (count == 0) fail(TSC_INVALID_ARGUMENT, std::string(name) + " must not be empty");
  std::vector<std::string> result;
  result.reserve(count);
  for (size_t i = 0; i < count; ++i) {
    result.push_back(to_string(names[i].data, names[i].size, name));
  }
  return result;
}

std::vector<uint64_t> to_vector(const uint64_t *values, size_t count, const char *name) {
  require_array(values, count, name);
  return values == nullptr ? std::vector<uint64_t>() : std::vector<uint64_t>(values, values + count);
}

std::vector<carla::SharedPtr<carla::client::Actor>> traffic_lights_in_junction(
    const carla::client::World &world, int32_t junction_id) {
  // LibCarla dereferences the junction without checking that it exists.
  if (world.GetMap()->GetMap().GetJunction(junction_id) == nullptr) return {};
  return world.GetTrafficLightsInJunction(junction_id);
}

void light_list_assign(tsc_light_list_t *out, const std::vector<carla::client::Light> &lights) {
  list_assign(out, lights, [](const carla::client::Light &l) {
    return tsc_light_t{l.GetId(), 0, from_carla(l.GetLocation())};
  });
}

std::vector<carla::SharedPtr<carla::client::TrafficLight>> traffic_lights_of(
    const std::vector<carla::SharedPtr<carla::client::Actor>> &actors) {
  std::vector<carla::SharedPtr<carla::client::TrafficLight>> lights;
  for (const auto &actor : actors) {
    if (auto light = downcast<carla::client::TrafficLight>(actor)) lights.push_back(std::move(light));
  }
  return lights;
}

void vehicle_light_state_list_assign(tsc_vehicle_light_state_list_t *out,
                                     const carla::rpc::VehicleLightStateList &states) {
  list_assign(out, states, [](const auto &entry) {
    return tsc_vehicle_light_state_t{entry.first, static_cast<uint32_t>(entry.second)};
  });
}

void bounding_box_list_assign(tsc_bounding_box_list_t *out,
                              const std::vector<carla::geom::BoundingBox> &boxes) {
  list_assign(out, boxes, [](const carla::geom::BoundingBox &b) { return from_carla(b); });
}

void labelled_point_list_assign(tsc_labelled_point_list_t *out,
                                const std::vector<carla::rpc::LabelledPoint> &points) {
  list_assign(out, points, [](const carla::rpc::LabelledPoint &p) { return from_carla(p); });
}

void environment_object_list_assign(tsc_environment_object_list_t *out,
                                    const std::vector<carla::rpc::EnvironmentObject> &objects) {
  require_ptr(out, "out");
  tsc_environment_object_list_t list{alloc_list_items<tsc_environment_object_t>(objects.size()),
                                     objects.size()};
  try {
    for (size_t i = 0; i < objects.size(); ++i) {
      const auto &o = objects[i];
      auto &r = list.items[i];
      r.id = o.id;
      r.transform = from_carla(o.transform);
      r.bounding_box = from_carla(o.bounding_box);
      r.type = static_cast<int32_t>(o.type);
      string_assign(&r.name, o.name);
    }
  } catch (...) {
    tsc_environment_object_list_free(&list);  // unassigned names are zeroed
    throw;
  }
  *out = list;
}

}  // namespace tsc

namespace {

tsc_tick_listener &listener_of(const tsc_tick_listener_t *l) {
  return *const_cast<tsc_tick_listener *>(check_handle(l, "listener", TSC_KIND_TICK_LISTENER));
}

}  // namespace

tsc_tick_listener::~tsc_tick_listener() {
  try {
    unregister();
  } catch (...) {
    // The episode may be gone (e.g. the client disconnected): nothing to remove.
  }
}

extern "C" {

void tsc_vehicle_light_state_list_free(tsc_vehicle_light_state_list_t *list) { free_list_items(list); }
void tsc_bounding_box_list_free(tsc_bounding_box_list_t *list) { free_list_items(list); }
void tsc_labelled_point_list_free(tsc_labelled_point_list_t *list) { free_list_items(list); }

void tsc_environment_object_list_free(tsc_environment_object_list_t *list) {
  if (list == nullptr) return;
  for (size_t i = 0; list->items != nullptr && i < list->size; ++i) {
    tsc_string_free(&list->items[i].name);
  }
  free_list_items(list);
}

tsc_status_t tsc_world_on_tick(tsc_world_t *world, size_t queue_capacity,
                               tsc_tick_listener_t **out) {
  return new_handle(__func__, out, [&] {
    auto &w = world_of(world);
    auto listener = std::make_unique<tsc_tick_listener>(w);
    auto queue = std::make_shared<tsc_tick_listener::Queue>(queue_capacity);
    auto frames = std::make_shared<FrameWatch>();
    // Runs on LibCarla's thread: it only touches the queue and the frame watch.
    listener->callback_id = listener->world.OnTick([queue, frames](carla::client::WorldSnapshot s) {
      const uint64_t frame = s.GetFrame();
      queue->push(std::make_shared<carla::client::WorldSnapshot>(std::move(s)));
      frames->seen(frame);
    });
    listener->registered = true;
    listener->queue = std::move(queue);
    listener->frames = std::move(frames);
    return listener.release();
  });
}

tsc_status_t tsc_world_get_client_token(tsc_world_t *world, uint64_t *out) {
  return TSC_GUARD({ *require_ptr(out, "out") = client_token(world_of(world)); });
}

tsc_status_t tsc_tick_listener_wait_for_frame(const tsc_tick_listener_t *listener, uint64_t frame,
                                              double timeout_seconds, int32_t *out_reached) {
  return TSC_GUARD({
    auto &l = listener_of(listener);
    require_ptr(out_reached, "out_reached");
    const auto timeout =
        std::chrono::milliseconds(seconds_to_duration(timeout_seconds).milliseconds());
    *out_reached = l.frames->wait_for(frame, timeout) ? 1 : 0;
  });
}

tsc_status_t tsc_tick_listener_get_id(const tsc_tick_listener_t *listener, uint64_t *out) {
  return TSC_GUARD({ *require_ptr(out, "out") = listener_of(listener).callback_id; });
}

tsc_status_t tsc_tick_listener_pending_count(const tsc_tick_listener_t *listener, size_t *out) {
  return TSC_GUARD({ *require_ptr(out, "out") = listener_of(listener).queue->size(); });
}

tsc_status_t tsc_tick_listener_poll(tsc_tick_listener_t *listener, tsc_world_snapshot_t **out) {
  return new_handle(__func__, out, [&]() -> tsc_world_snapshot * {
    auto item = listener_of(listener).queue->pop();
    return item != nullptr ? new tsc_world_snapshot(std::move(*item)) : nullptr;
  });
}

tsc_status_t tsc_tick_listener_stop(tsc_tick_listener_t *listener) {
  return TSC_GUARD({
    auto &l = listener_of(listener);
    l.unregister();
    while (l.queue->pop() != nullptr) {
    }
  });
}

}  // extern "C"
