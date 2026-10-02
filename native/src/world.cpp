#include "internal.hpp"

using namespace tsc;

namespace {

carla::client::Actor *parent_of(tsc_actor_t *parent) {
  return parent == nullptr ? nullptr : check_actor(parent, "parent")->actor.get();
}

// Shared body of spawn_actor / try_spawn_actor. TrySpawnActor returns NULL
// when the spot is occupied; SpawnActor failing that way is an error.
tsc_actor_t *spawn(tsc_world_t *world, const tsc_actor_blueprint_t *blueprint,
                   const tsc_transform_t *transform, tsc_actor_t *parent, bool try_spawn) {
  auto &w = world_of(world);
  const auto &bp = check_handle(blueprint, "blueprint", TSC_KIND_ACTOR_BLUEPRINT)->blueprint;
  const auto t = to_carla(*require_ptr(transform, "transform"));
  auto actor = try_spawn ? w.TrySpawnActor(bp, t, parent_of(parent))
                         : w.SpawnActor(bp, t, parent_of(parent));
  if (actor == nullptr && !try_spawn) fail(TSC_ERROR, "spawn failed for " + bp.GetId());
  return make_actor_handle(actor);
}

}  // namespace

extern "C" {

tsc_status_t tsc_world_get_id(tsc_world_t *world, uint64_t *out_id) {
  return TSC_GUARD({ *require_ptr(out_id, "out_id") = world_of(world).GetId(); });
}

tsc_status_t tsc_world_get_actors(tsc_world_t *world, tsc_actor_list_t **out_list) {
  return TSC_GUARD({
    require_ptr(out_list, "out_list");
    *out_list = nullptr;
    *out_list = new tsc_actor_list(world_of(world).GetActors());
  });
}

tsc_status_t tsc_world_get_actor(tsc_world_t *world, uint32_t actor_id, tsc_actor_t **out_actor) {
  return TSC_GUARD({
    require_ptr(out_actor, "out_actor");
    *out_actor = nullptr;
    auto actor = world_of(world).GetActor(actor_id);
    if (actor == nullptr) fail(TSC_NOT_FOUND, "no actor with id " + std::to_string(actor_id));
    *out_actor = make_actor_handle(actor);
  });
}

tsc_status_t tsc_world_get_blueprint_library(tsc_world_t *world,
                                             tsc_blueprint_library_t **out_library) {
  return TSC_GUARD({
    require_ptr(out_library, "out_library");
    *out_library = nullptr;
    *out_library = new tsc_blueprint_library(world_of(world).GetBlueprintLibrary());
  });
}

tsc_status_t tsc_world_spawn_actor(tsc_world_t *world, const tsc_actor_blueprint_t *blueprint,
                                   const tsc_transform_t *transform, tsc_actor_t *parent,
                                   tsc_actor_t **out_actor) {
  return TSC_GUARD({
    require_ptr(out_actor, "out_actor");
    *out_actor = nullptr;
    *out_actor = spawn(world, blueprint, transform, parent, false);
  });
}

tsc_status_t tsc_world_try_spawn_actor(tsc_world_t *world, const tsc_actor_blueprint_t *blueprint,
                                       const tsc_transform_t *transform, tsc_actor_t *parent,
                                       tsc_actor_t **out_actor) {
  return TSC_GUARD({
    require_ptr(out_actor, "out_actor");
    *out_actor = nullptr;
    *out_actor = spawn(world, blueprint, transform, parent, true);
  });
}

tsc_status_t tsc_world_tick(tsc_world_t *world, double timeout_seconds, uint64_t *out_frame) {
  return TSC_GUARD({
    require_ptr(out_frame, "out_frame");
    *out_frame = world_of(world).Tick(seconds_to_duration(timeout_seconds));
  });
}

tsc_status_t tsc_world_get_settings(tsc_world_t *world, tsc_world_settings_t *out) {
  return tsc_world_get_settings_ext(world, out, nullptr);
}

tsc_status_t tsc_world_apply_settings(tsc_world_t *world, const tsc_world_settings_t *settings,
                                      double timeout_seconds, uint64_t *out_frame) {
  return tsc_world_apply_settings_ext(world, settings, nullptr, timeout_seconds, out_frame);
}

// out_ext / ext may be NULL (tsc_world_get_settings / tsc_world_apply_settings above:
// the extended fields are then not read, or keep the server's values).
tsc_status_t tsc_world_get_settings_ext(tsc_world_t *world, tsc_world_settings_t *out,
                                        tsc_world_settings_ext_t *out_ext) {
  return TSC_GUARD({
    require_ptr(out, "out");
    const carla::rpc::EpisodeSettings s = world_of(world).GetSettings();
    *out = tsc_world_settings_t{};
    out->synchronous_mode = s.synchronous_mode ? 1 : 0;
    out->no_rendering_mode = s.no_rendering_mode ? 1 : 0;
    out->has_fixed_delta_seconds = s.fixed_delta_seconds.has_value() ? 1 : 0;
    out->fixed_delta_seconds = s.fixed_delta_seconds.has_value() ? *s.fixed_delta_seconds : 0.0;
    out->substepping = s.substepping ? 1 : 0;
    out->max_substep_delta_time = s.max_substep_delta_time;
    out->max_substeps = s.max_substeps;
    if (out_ext != nullptr) {
      *out_ext = tsc_world_settings_ext_t{};
      out_ext->max_culling_distance = s.max_culling_distance;
      out_ext->tile_stream_distance = s.tile_stream_distance;
      out_ext->actor_active_distance = s.actor_active_distance;
      out_ext->deterministic_ragdolls = s.deterministic_ragdolls ? 1 : 0;
      out_ext->spectator_as_ego = s.spectator_as_ego ? 1 : 0;
    }
  });
}

tsc_status_t tsc_world_apply_settings_ext(tsc_world_t *world, const tsc_world_settings_t *settings,
                                          const tsc_world_settings_ext_t *ext,
                                          double timeout_seconds, uint64_t *out_frame) {
  return TSC_GUARD({
    require_ptr(settings, "settings");
    require_ptr(out_frame, "out_frame");
    auto &w = world_of(world);
    if (settings->has_fixed_delta_seconds && !(settings->fixed_delta_seconds > 0.0)) {
      fail(TSC_INVALID_ARGUMENT, "fixed_delta_seconds must be positive");
    }
    if (settings->max_substeps < 1) fail(TSC_INVALID_ARGUMENT, "max_substeps must be >= 1");
    // Start from the current settings so fields this ABI does not expose
    // (or, without `ext`, the extended ones) keep their server values.
    carla::rpc::EpisodeSettings s = w.GetSettings();
    s.synchronous_mode = settings->synchronous_mode != 0;
    s.no_rendering_mode = settings->no_rendering_mode != 0;
    if (settings->has_fixed_delta_seconds) {
      s.fixed_delta_seconds = settings->fixed_delta_seconds;
    } else {
      s.fixed_delta_seconds.reset();
    }
    s.substepping = settings->substepping != 0;
    s.max_substep_delta_time = settings->max_substep_delta_time;
    s.max_substeps = settings->max_substeps;
    if (ext != nullptr) {
      s.max_culling_distance = check_non_negative(ext->max_culling_distance, "max_culling_distance");
      s.tile_stream_distance = check_non_negative(ext->tile_stream_distance, "tile_stream_distance");
      s.actor_active_distance =
          check_non_negative(ext->actor_active_distance, "actor_active_distance");
      s.deterministic_ragdolls = ext->deterministic_ragdolls != 0;
      s.spectator_as_ego = ext->spectator_as_ego != 0;
    }
    *out_frame = w.ApplySettings(s, seconds_to_duration(timeout_seconds));
  });
}

// ---------------------------------------------------------------------------
// Actor list
// ---------------------------------------------------------------------------

size_t tsc_actor_list_size(const tsc_actor_list_t *list) {
  if (list == nullptr || list->kind != TSC_KIND_ACTOR_LIST) return 0;
  return list->list->size();
}

tsc_status_t tsc_actor_list_get(const tsc_actor_list_t *list, size_t index,
                                tsc_actor_t **out_actor) {
  return TSC_GUARD({
    require_ptr(out_actor, "out_actor");
    *out_actor = nullptr;
    const auto &l = check_handle(list, "list", TSC_KIND_ACTOR_LIST)->list;
    check_index(index, l->size(), "actor list");
    *out_actor = make_actor_handle(l->at(index));
  });
}

tsc_status_t tsc_actor_list_filter(const tsc_actor_list_t *list, const char *pattern,
                                   size_t pattern_len, tsc_actor_list_t **out_list) {
  return TSC_GUARD({
    require_ptr(out_list, "out_list");
    *out_list = nullptr;
    const auto &l = check_handle(list, "list", TSC_KIND_ACTOR_LIST)->list;
    *out_list = new tsc_actor_list(l->Filter(to_string(pattern, pattern_len, "pattern")));
  });
}

}  // extern "C"
