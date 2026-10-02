// Walkers: the bone conversion and the bone list accessors (issue #20). The
// rest is generated (bindings/walker.yaml, walker_ai_controller.yaml).
#include "internal.hpp"

using namespace tsc;

namespace tsc {

carla::rpc::WalkerBoneControlIn to_bone_control(const tsc_bone_transform_t *bones, size_t count,
                                                const char *name) {
  require_array(bones, count, name);
  carla::rpc::WalkerBoneControlIn control;
  control.bone_transforms.reserve(count);
  for (size_t i = 0; i < count; ++i) {
    const auto &t = bones[i].transform;
    for (double v : {t.location.x, t.location.y, t.location.z, t.rotation.pitch, t.rotation.yaw,
                     t.rotation.roll}) {
      check_float(v, "bone transform");
    }
    control.bone_transforms.emplace_back(to_string(bones[i].name, bones[i].name_len, "bone name"),
                                         to_carla(t));
  }
  return control;
}

}  // namespace tsc

extern "C" {

size_t tsc_bone_list_size(const tsc_bone_list_t *list) {
  if (list == nullptr || list->kind != TSC_KIND_BONE_LIST) return 0;
  return list->bones.size();
}

tsc_status_t tsc_bone_list_get(const tsc_bone_list_t *list, size_t index,
                               tsc_bone_transform_out_t *out) {
  return TSC_GUARD({
    const auto &bones = check_handle(list, "list", TSC_KIND_BONE_LIST)->bones;
    require_ptr(out, "out");
    check_index(index, bones.size(), "bone list");
    const auto &b = bones[index];
    tsc_bone_transform_out_t r{};
    r.world = from_carla(b.world);
    r.component = from_carla(b.component);
    r.relative = from_carla(b.relative);
    string_assign(&r.name, b.bone_name);
    *out = r;
  });
}

}  // extern "C"
