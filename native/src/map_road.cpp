// Issue #22: what the binding generator cannot express (bindings/map.yaml,
// landmark.yaml, waypoint.yaml and traffic_light.yaml have the rest): list
// accessors, buffers, optional lane markings, a version-dependent call and
// the multi-step file write of Map.save_to_disk.
#include "internal.hpp"

#include <fstream>

using namespace tsc;

namespace {

tsc_status_t lane_marking(const char *function, const tsc_waypoint_t *wp, bool left,
                          int32_t *has_value, tsc_lane_marking_t *out) {
  return guard(function, [&] {
    require_ptr(has_value, "has_value");
    require_ptr(out, "out");
    const auto &w = waypoint_of(wp);
    const auto marking = left ? w.GetLeftLaneMarking() : w.GetRightLaneMarking();
    *has_value = marking.has_value() ? 1 : 0;
    *out = marking.has_value() ? from_carla(*marking) : tsc_lane_marking_t{};
  });
}

}  // namespace

extern "C" {

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

tsc_status_t tsc_landmark_list_get_landmark(const tsc_landmark_list_t *list, size_t index,
                                            tsc_landmark_handle_t **out) {
  return new_handle(__func__, out, [&] {
    const auto &all = check_handle(list, "list", TSC_KIND_LANDMARK_LIST)->landmarks;
    check_index(index, all.size(), "landmark list");
    if (all[index] == nullptr) fail(TSC_ERROR, "LibCarla returned a null landmark");
    return new tsc_landmark_handle(all[index]);
  });
}

tsc_status_t tsc_landmark_get_lane_validities(const tsc_landmark_handle_t *landmark,
                                              tsc_lane_validity_t *out, size_t capacity,
                                              size_t *out_count) {
  return TSC_GUARD({
    require_ptr(out_count, "out_count");
    const auto &validities = landmark_of(landmark).GetValidities();
    *out_count = validities.size();
    if (out == nullptr) return;
    for (size_t i = 0; i < validities.size() && i < capacity; ++i) {
      out[i] = tsc_lane_validity_t{validities[i]._from_lane, validities[i]._to_lane};
    }
  });
}

tsc_status_t tsc_waypoint_get_left_lane_marking(const tsc_waypoint_t *wp, int32_t *has_value,
                                                tsc_lane_marking_t *out) {
  return lane_marking(__func__, wp, true, has_value, out);
}

tsc_status_t tsc_waypoint_get_right_lane_marking(const tsc_waypoint_t *wp, int32_t *has_value,
                                                 tsc_lane_marking_t *out) {
  return lane_marking(__func__, wp, false, has_value, out);
}

tsc_status_t tsc_waypoint_is_rht(const tsc_waypoint_t *wp, int32_t *out) {
  return TSC_GUARD({ *require_ptr(out, "out") = waypoint_is_rht(waypoint_of(wp)) ? 1 : 0; });
}

size_t tsc_traffic_light_list_size(const tsc_traffic_light_list_t *list) {
  if (list == nullptr || list->kind != TSC_KIND_TRAFFIC_LIGHT_LIST) return 0;
  return list->lights.size();
}

tsc_status_t tsc_traffic_light_list_get(const tsc_traffic_light_list_t *list, size_t index,
                                        tsc_traffic_light_t **out) {
  return new_handle(__func__, out, [&] {
    const auto &lights = check_handle(list, "list", TSC_KIND_TRAFFIC_LIGHT_LIST)->lights;
    check_index(index, lights.size(), "traffic light list");
    return new tsc_traffic_light(lights[index]);
  });
}

tsc_status_t tsc_traffic_light_get_light_boxes(tsc_traffic_light_t *light,
                                               tsc_bounding_box_t *out, size_t capacity,
                                               size_t *out_count) {
  return TSC_GUARD({
    require_ptr(out_count, "out_count");
    copy_out(light_of(light).GetLightBoxes(), out, capacity, out_count);
  });
}

}  // extern "C"
