// Map and waypoints: the hand-written part (structs of several calls and the
// file write of Map.save_to_disk). The rest is generated (bindings/map.yaml,
// waypoint.yaml, junction.yaml, landmark.yaml, lists.yaml).
#include "internal.hpp"

#include <fstream>

using namespace tsc;

tsc_landmark_t tsc::from_carla(const carla::client::Landmark &lm) {
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
  return r;
}

extern "C" {

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

void tsc_landmark_free(tsc_landmark_t *l) {
  if (l == nullptr) return;
  for (tsc_string_t *s : {&l->id, &l->name, &l->type, &l->sub_type, &l->country, &l->unit, &l->text}) {
    tsc_string_free(s);
  }
}

tsc_status_t tsc_map_save_to_disk(const tsc_map_t *map, const char *path, size_t path_len) {
  return TSC_GUARD({
    const auto &m = map_of(map);
    std::string file = to_string(path, path_len, "path");
    if (file.empty()) file = m.GetName();
    // What the Python API's Map.save_to_disk does, except that a file that
    // cannot be written is an error rather than silently ignored.
    carla::FileSystem::ValidateFilePath(file, ".xodr");
    std::ofstream stream(file);
    if (!stream) fail(TSC_ERROR, "cannot open " + file + " for writing");
    stream << m.GetOpenDrive() << std::endl;
    if (!stream) fail(TSC_ERROR, "cannot write " + file);
  });
}

}  // extern "C"
