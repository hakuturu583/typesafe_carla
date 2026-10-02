// Milestone 4: walkers and their AI controllers.
#include "internal.hpp"

using namespace tsc;

extern "C" {

tsc_status_t tsc_walker_apply_control(tsc_walker_t *walker, const tsc_walker_control_t *control) {
  return TSC_GUARD({
    const auto &c = *require_ptr(control, "control");
    walker_of(walker).ApplyControl(to_carla_walker_control(c.direction, c.speed, c.jump != 0));
  });
}

tsc_status_t tsc_walker_get_control(tsc_walker_t *walker, tsc_walker_control_t *out) {
  return TSC_GUARD({
    require_ptr(out, "out");
    const carla::rpc::WalkerControl c = walker_of(walker).GetWalkerControl();
    *out = tsc_walker_control_t{from_carla(c.direction), c.speed, c.jump ? 1 : 0, 0};
  });
}

tsc_status_t tsc_world_get_random_location_from_navigation(tsc_world_t *world, tsc_location_t *out,
                                                           int32_t *out_found) {
  return TSC_GUARD({
    require_ptr(out, "out");
    require_ptr(out_found, "out_found");
    auto location = world_of(world).GetRandomLocationFromNavigation();
    *out_found = location ? 1 : 0;
    *out = location ? from_carla(*location) : tsc_location_t{};
  });
}

// --- Issue #20: bones and poses -----------------------------------------------

tsc_status_t tsc_walker_set_bones(tsc_walker_t *walker, const tsc_bone_transform_t *bones,
                                  size_t count) {
  return TSC_GUARD({
    auto &w = walker_of(walker);
    require_array(bones, count, "bones");
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
    w.SetBonesTransform(control);
  });
}

tsc_status_t tsc_walker_get_bones(tsc_walker_t *walker, tsc_bone_list_t **out) {
  return new_handle(__func__, out, [&] {
    return new tsc_bone_list(walker_of(walker).GetBonesTransform().bone_transforms);
  });
}

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
