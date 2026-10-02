// Issue #22: what the binding generator cannot express (bindings/map.yaml,
// landmark.yaml, waypoint.yaml and traffic_light.yaml have the rest): list
// accessors and the multi-step file write of Map.save_to_disk.
#include "internal.hpp"

#include <fstream>

using namespace tsc;

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

}  // extern "C"
