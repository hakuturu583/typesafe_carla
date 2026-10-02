// Milestone 4: map queries (topology, crosswalks, landmarks, junctions).
#include "internal.hpp"

using namespace tsc;

namespace {

using WaypointPair = std::pair<carla::SharedPtr<carla::client::Waypoint>,
                               carla::SharedPtr<carla::client::Waypoint>>;

std::vector<carla::SharedPtr<carla::client::Waypoint>> flatten(std::vector<WaypointPair> pairs) {
  std::vector<carla::SharedPtr<carla::client::Waypoint>> flat;
  flat.reserve(2 * pairs.size());
  for (auto &p : pairs) {
    flat.push_back(std::move(p.first));
    flat.push_back(std::move(p.second));
  }
  return flat;
}

const carla::client::Junction &junction_of(const tsc_junction_t *j) {
  return *check_handle(j, "junction", TSC_KIND_JUNCTION)->junction;
}

}  // namespace

extern "C" {

tsc_status_t tsc_map_get_topology(const tsc_map_t *map, tsc_waypoint_list_t **out) {
  return new_handle(__func__, out, [&] { return new tsc_waypoint_list(flatten(map_of(map).GetTopology())); });
}

tsc_status_t tsc_map_get_crosswalks(const tsc_map_t *map, tsc_location_t *out, size_t capacity,
                                    size_t *out_count) {
  return TSC_GUARD({
    copy_out(map_of(map).GetAllCrosswalkZones(), out, capacity, out_count);
  });
}

void tsc_landmark_free(tsc_landmark_t *l) {
  if (l == nullptr) return;
  for (tsc_string_t *s : {&l->id, &l->name, &l->type, &l->sub_type, &l->country, &l->unit, &l->text}) {
    tsc_string_free(s);
  }
}

tsc_status_t tsc_map_get_all_landmarks(const tsc_map_t *map, tsc_landmark_list_t **out) {
  return new_handle(__func__, out, [&] { return new tsc_landmark_list(map_of(map).GetAllLandmarks()); });
}

tsc_status_t tsc_map_get_landmarks_of_type(const tsc_map_t *map, const char *type, size_t type_len,
                                           tsc_landmark_list_t **out) {
  return new_handle(__func__, out, [&] {
    return new tsc_landmark_list(map_of(map).GetAllLandmarksOfType(to_string(type, type_len, "type")));
  });
}

size_t tsc_landmark_list_size(const tsc_landmark_list_t *list) {
  if (list == nullptr || list->kind != TSC_KIND_LANDMARK_LIST) return 0;
  return list->landmarks.size();
}

tsc_status_t tsc_landmark_list_get(const tsc_landmark_list_t *list, size_t index, tsc_landmark_t *out) {
  return TSC_GUARD({
    require_ptr(out, "out");
    *out = tsc_landmark_t{};
    const auto &all = check_handle(list, "list", TSC_KIND_LANDMARK_LIST)->landmarks;
    check_index(index, all.size(), "landmark list");
    const carla::client::Landmark &lm = *all[index];
    tsc_landmark_t r{};
    try {
      string_assign(&r.id, lm.GetId());
      string_assign(&r.name, lm.GetName());
      string_assign(&r.type, lm.GetType());
      string_assign(&r.sub_type, lm.GetSubType());
      string_assign(&r.country, lm.GetCountry());
      string_assign(&r.unit, lm.GetUnit());
      string_assign(&r.text, lm.GetText());
    } catch (...) {
      tsc_landmark_free(&r);
      throw;
    }
    r.road_id = lm.GetRoadId();
    r.orientation = static_cast<int32_t>(lm.GetOrientation());
    r.s = lm.GetS();
    r.t = lm.GetT();
    r.distance = lm.GetDistance();
    r.z_offset = lm.GetZOffset();
    r.value = lm.GetValue();
    r.height = lm.GetHeight();
    r.width = lm.GetWidth();
    r.transform = from_carla(lm.GetTransform());
    *out = r;
  });
}

tsc_status_t tsc_waypoint_get_junction(const tsc_waypoint_t *wp, tsc_junction_t **out) {
  return new_handle(__func__, out, [&]() -> tsc_junction * {
    auto j = waypoint_of(wp).GetJunction();
    return j == nullptr ? nullptr : new tsc_junction(std::move(j));
  });
}

tsc_status_t tsc_junction_get_id(const tsc_junction_t *j, int32_t *out) {
  return TSC_GUARD({ *require_ptr(out, "out") = junction_of(j).GetId(); });
}

tsc_status_t tsc_junction_get_bounding_box(const tsc_junction_t *j, tsc_bounding_box_t *out) {
  return TSC_GUARD({
    *require_ptr(out, "out") = from_carla(junction_of(j).GetBoundingBox());
  });
}

tsc_status_t tsc_junction_get_waypoints(const tsc_junction_t *j, int32_t lane_type,
                                        tsc_waypoint_list_t **out) {
  return new_handle(__func__, out, [&] {
    return new tsc_waypoint_list(
        flatten(junction_of(j).GetWaypoints(static_cast<carla::road::Lane::LaneType>(lane_type))));
  });
}

}  // extern "C"
