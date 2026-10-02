#include "internal.hpp"

using namespace tsc;

namespace {

const carla::SharedPtr<carla::client::BlueprintLibrary> &library_of(
    const tsc_blueprint_library_t *l) {
  return check_handle(l, "library", TSC_KIND_BLUEPRINT_LIBRARY)->library;
}

carla::client::ActorBlueprint &blueprint_of(tsc_actor_blueprint_t *b) {
  return check_handle(b, "blueprint", TSC_KIND_ACTOR_BLUEPRINT)->blueprint;
}

const carla::client::ActorBlueprint &blueprint_of(const tsc_actor_blueprint_t *b) {
  return check_handle(b, "blueprint", TSC_KIND_ACTOR_BLUEPRINT)->blueprint;
}

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
    if (index >= l->size()) {
      fail(TSC_NOT_FOUND, "index " + std::to_string(index) +
                              " out of range for blueprint library of size " +
                              std::to_string(l->size()));
    }
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

tsc_status_t tsc_blueprint_library_filter(const tsc_blueprint_library_t *library,
                                          const char *pattern, size_t pattern_len,
                                          tsc_blueprint_library_t **out) {
  return TSC_GUARD({
    require_ptr(out, "out");
    *out = nullptr;
    *out = new tsc_blueprint_library(
        library_of(library)->Filter(to_string(pattern, pattern_len, "pattern")));
  });
}

tsc_status_t tsc_actor_blueprint_get_id(const tsc_actor_blueprint_t *blueprint, tsc_string_t *out) {
  return TSC_GUARD({ string_assign(out, blueprint_of(blueprint).GetId()); });
}

tsc_status_t tsc_actor_blueprint_has_tag(const tsc_actor_blueprint_t *blueprint, const char *tag,
                                         size_t tag_len, int32_t *out_has) {
  return TSC_GUARD({
    require_ptr(out_has, "out_has");
    *out_has = blueprint_of(blueprint).ContainsTag(to_string(tag, tag_len, "tag")) ? 1 : 0;
  });
}

tsc_status_t tsc_actor_blueprint_has_attribute(const tsc_actor_blueprint_t *blueprint,
                                               const char *id, size_t id_len, int32_t *out_has) {
  return TSC_GUARD({
    require_ptr(out_has, "out_has");
    *out_has = blueprint_of(blueprint).ContainsAttribute(to_string(id, id_len, "id")) ? 1 : 0;
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

}  // extern "C"
