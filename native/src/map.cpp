// Map and waypoints.
#include "internal.hpp"

using namespace tsc;

namespace {

using WaypointPtr = carla::SharedPtr<carla::client::Waypoint>;

const carla::client::Map &map_of(const tsc_map_t *m) {
  return *check_handle(m, "map", TSC_KIND_MAP)->map;
}

const carla::client::Waypoint &waypoint_of(const tsc_waypoint_t *w) {
  return *check_handle(w, "waypoint", TSC_KIND_WAYPOINT)->waypoint;
}

tsc_waypoint_t *make_waypoint(WaypointPtr w) {
  return w == nullptr ? nullptr : new tsc_waypoint(std::move(w));
}

template <typename F>
tsc_status_t waypoint_list(const char *function, tsc_waypoint_list_t **out, F &&produce) {
  return guard(function, [&]() {
    require_ptr(out, "out");
    *out = nullptr;
    *out = new tsc_waypoint_list(produce());
  });
}

void check_distance(double distance) {
  if (!(distance > 0.0) || !std::isfinite(distance)) {
    fail(TSC_INVALID_ARGUMENT, "distance must be a positive finite number of meters");
  }
}

}  // namespace

extern "C" {

tsc_status_t tsc_world_get_map(tsc_world_t *world, tsc_map_t **out) {
  return TSC_GUARD({
    require_ptr(out, "out");
    *out = nullptr;
    *out = new tsc_map(check_handle(world, "world", TSC_KIND_WORLD)->world.GetMap());
  });
}

tsc_status_t tsc_map_get_name(const tsc_map_t *map, tsc_string_t *out) {
  return TSC_GUARD({ string_assign(out, map_of(map).GetName()); });
}

tsc_status_t tsc_map_to_opendrive(const tsc_map_t *map, tsc_string_t *out) {
  return TSC_GUARD({ string_assign(out, map_of(map).GetOpenDrive()); });
}

tsc_status_t tsc_map_get_spawn_points(const tsc_map_t *map, tsc_transform_t *out, size_t capacity,
                                      size_t *out_count) {
  return TSC_GUARD({
    require_ptr(out_count, "out_count");
    const auto &points = map_of(map).GetRecommendedSpawnPoints();
    *out_count = points.size();
    if (out == nullptr) return;
    for (size_t i = 0; i < points.size() && i < capacity; ++i) out[i] = from_carla(points[i]);
  });
}

tsc_status_t tsc_map_get_waypoint(const tsc_map_t *map, const tsc_location_t *location,
                                  int32_t project_to_road, int32_t lane_type,
                                  tsc_waypoint_t **out) {
  return TSC_GUARD({
    require_ptr(out, "out");
    *out = nullptr;
    *out = make_waypoint(map_of(map).GetWaypoint(to_carla(*require_ptr(location, "location")),
                                                 project_to_road != 0, lane_type));
  });
}

tsc_status_t tsc_map_generate_waypoints(const tsc_map_t *map, double distance,
                                        tsc_waypoint_list_t **out) {
  return waypoint_list(__func__, out, [&]() {
    check_distance(distance);
    return map_of(map).GenerateWaypoints(distance);
  });
}

tsc_status_t tsc_waypoint_get_info(const tsc_waypoint_t *wp, tsc_waypoint_info_t *out) {
  return TSC_GUARD({
    require_ptr(out, "out");
    const auto &w = waypoint_of(wp);
    tsc_waypoint_info_t info{};
    info.id = w.GetId();
    info.road_id = w.GetRoadId();
    info.section_id = w.GetSectionId();
    info.lane_id = w.GetLaneId();
    info.is_junction = w.IsJunction() ? 1 : 0;
    info.junction_id = w.GetJunctionId();
    info.lane_type = static_cast<int32_t>(w.GetType());
    info.s = w.GetDistance();
    info.lane_width = w.GetLaneWidth();
    info.transform = from_carla(w.GetTransform());
    *out = info;
  });
}

tsc_status_t tsc_waypoint_next(const tsc_waypoint_t *wp, double distance,
                               tsc_waypoint_list_t **out) {
  return waypoint_list(__func__, out, [&]() {
    check_distance(distance);
    return waypoint_of(wp).GetNext(distance);
  });
}

tsc_status_t tsc_waypoint_previous(const tsc_waypoint_t *wp, double distance,
                                   tsc_waypoint_list_t **out) {
  return waypoint_list(__func__, out, [&]() {
    check_distance(distance);
    return waypoint_of(wp).GetPrevious(distance);
  });
}

tsc_status_t tsc_waypoint_next_until_lane_end(const tsc_waypoint_t *wp, double distance,
                                              tsc_waypoint_list_t **out) {
  return waypoint_list(__func__, out, [&]() {
    check_distance(distance);
    return waypoint_of(wp).GetNextUntilLaneEnd(distance);
  });
}

tsc_status_t tsc_waypoint_previous_until_lane_start(const tsc_waypoint_t *wp, double distance,
                                                    tsc_waypoint_list_t **out) {
  return waypoint_list(__func__, out, [&]() {
    check_distance(distance);
    return waypoint_of(wp).GetPreviousUntilLaneStart(distance);
  });
}

tsc_status_t tsc_waypoint_get_left_lane(const tsc_waypoint_t *wp, tsc_waypoint_t **out) {
  return TSC_GUARD({
    require_ptr(out, "out");
    *out = nullptr;
    *out = make_waypoint(waypoint_of(wp).GetLeft());
  });
}

tsc_status_t tsc_waypoint_get_right_lane(const tsc_waypoint_t *wp, tsc_waypoint_t **out) {
  return TSC_GUARD({
    require_ptr(out, "out");
    *out = nullptr;
    *out = make_waypoint(waypoint_of(wp).GetRight());
  });
}

size_t tsc_waypoint_list_size(const tsc_waypoint_list_t *list) {
  if (list == nullptr || list->kind != TSC_KIND_WAYPOINT_LIST) return 0;
  return list->waypoints.size();
}

tsc_status_t tsc_waypoint_list_get(const tsc_waypoint_list_t *list, size_t index,
                                   tsc_waypoint_t **out) {
  return TSC_GUARD({
    require_ptr(out, "out");
    *out = nullptr;
    const auto &w = check_handle(list, "list", TSC_KIND_WAYPOINT_LIST)->waypoints;
    if (index >= w.size()) {
      fail(TSC_NOT_FOUND, "index " + std::to_string(index) + " out of range for waypoint list of size " +
                              std::to_string(w.size()));
    }
    if (w[index] == nullptr) fail(TSC_ERROR, "LibCarla returned a null waypoint");
    *out = new tsc_waypoint(w[index]);
  });
}

}  // extern "C"
