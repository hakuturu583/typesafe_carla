#include "internal.hpp"

using namespace tsc;

namespace {

// Fails with TSC_NOT_FOUND unless the blueprint has attribute `key`.
void require_attribute(const carla::client::ActorBlueprint &bp, const std::string &key) {
  if (!bp.ContainsAttribute(key)) {
    fail(TSC_NOT_FOUND, "blueprint '" + bp.GetId() + "' has no attribute '" + key + "'");
  }
}

}  // namespace

extern "C" {

size_t tsc_blueprint_library_size(const tsc_blueprint_library_t *library) {
  if (library == nullptr || library->kind != TSC_KIND_BLUEPRINT_LIBRARY) return 0;
  return library->library->size();
}

tsc_status_t tsc_blueprint_library_get(const tsc_blueprint_library_t *library, size_t index,
                                       tsc_actor_blueprint_t **out) {
  return TSC_GUARD({
    require_ptr(out, "out");
    *out = nullptr;
    const auto &l = library_of(library);
    check_index(index, l->size(), "blueprint library");
    *out = new tsc_actor_blueprint(l->at(index));
  });
}

tsc_status_t tsc_blueprint_library_find(const tsc_blueprint_library_t *library, const char *id,
                                        size_t id_len, tsc_actor_blueprint_t **out) {
  return TSC_GUARD({
    require_ptr(out, "out");
    *out = nullptr;
    const std::string key = to_string(id, id_len, "id");
    const carla::client::ActorBlueprint *bp = library_of(library)->Find(key);
    if (bp == nullptr) fail(TSC_NOT_FOUND, "no blueprint with id '" + key + "'");
    *out = new tsc_actor_blueprint(*bp);
  });
}

tsc_status_t tsc_actor_blueprint_get_attribute(const tsc_actor_blueprint_t *blueprint,
                                               const char *id, size_t id_len,
                                               tsc_actor_attribute_t *out) {
  return TSC_GUARD({
    require_ptr(out, "out");
    *out = tsc_actor_attribute_t{};
    const auto &bp = blueprint_of(blueprint);
    const std::string key = to_string(id, id_len, "id");
    require_attribute(bp, key);
    const carla::client::ActorAttribute &attribute = bp.GetAttribute(key);
    tsc_actor_attribute_t result{};
    try {
      string_assign(&result.id, attribute.GetId());
      string_assign(&result.value, attribute.GetValue());
    } catch (...) {
      tsc_actor_attribute_free(&result);
      throw;
    }
    result.type = static_cast<int32_t>(attribute.GetType());
    result.is_modifiable = attribute.IsModifiable() ? 1 : 0;
    *out = result;
  });
}

tsc_status_t tsc_actor_blueprint_set_attribute(tsc_actor_blueprint_t *blueprint, const char *id,
                                               size_t id_len, const char *value,
                                               size_t value_len) {
  return TSC_GUARD({
    auto &bp = blueprint_of(blueprint);
    const std::string key = to_string(id, id_len, "id");
    require_attribute(bp, key);
    if (!bp.GetAttribute(key).IsModifiable()) {
      fail(TSC_INVALID_ARGUMENT, "attribute '" + key + "' of '" + bp.GetId() + "' is not modifiable");
    }
    bp.SetAttribute(key, to_string(value, value_len, "value"));
  });
}

// --- Issue #23 -------------------------------------------------------------------

}  // extern "C"
