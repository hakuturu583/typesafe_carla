// Walkers: the bone conversions (issue #20). The rest is generated
// (bindings/walker.yaml, walker_ai_controller.yaml, lists.yaml).
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

tsc_bone_transform_out_t from_carla(const carla::rpc::BoneTransformDataOut &b) {
  tsc_bone_transform_out_t r{{}, from_carla(b.world), from_carla(b.component), from_carla(b.relative)};
  string_assign(&r.name, b.bone_name);
  return r;
}

}  // namespace tsc
