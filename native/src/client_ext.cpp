// Milestone 4: recorder, replayer and OpenDRIVE worlds.
#include "internal.hpp"

using namespace tsc;

extern "C" {

tsc_status_t tsc_client_start_recorder(tsc_client_t *client, const char *name, size_t name_len,
                                       int32_t additional_data, tsc_string_t *out) {
  return TSC_GUARD({
    string_assign(out, client_of(client).StartRecorder(to_string(name, name_len, "name"),
                                                       additional_data != 0));
  });
}

tsc_status_t tsc_client_stop_recorder(tsc_client_t *client) {
  return TSC_GUARD({ client_of(client).StopRecorder(); });
}

tsc_status_t tsc_client_show_recorder_file_info(tsc_client_t *client, const char *name,
                                                size_t name_len, int32_t show_all,
                                                tsc_string_t *out) {
  return TSC_GUARD({
    string_assign(out, client_of(client).ShowRecorderFileInfo(to_string(name, name_len, "name"),
                                                              show_all != 0));
  });
}

tsc_status_t tsc_client_show_recorder_collisions(tsc_client_t *client, const char *name,
                                                 size_t name_len, char type1, char type2,
                                                 tsc_string_t *out) {
  return TSC_GUARD({
    string_assign(out, client_of(client).ShowRecorderCollisions(
                           to_string(name, name_len, "name"), type1, type2));
  });
}

tsc_status_t tsc_client_show_recorder_actors_blocked(tsc_client_t *client, const char *name,
                                                     size_t name_len, double min_time,
                                                     double min_distance, tsc_string_t *out) {
  return TSC_GUARD({
    string_assign(out, client_of(client).ShowRecorderActorsBlocked(
                           to_string(name, name_len, "name"), min_time, min_distance));
  });
}

tsc_status_t tsc_client_replay_file(tsc_client_t *client, const char *name, size_t name_len,
                                    double start, double duration, uint32_t follow_id,
                                    int32_t replay_sensors, tsc_string_t *out) {
  return TSC_GUARD({
    string_assign(out, client_of(client).ReplayFile(to_string(name, name_len, "name"), start,
                                                    duration, follow_id, replay_sensors != 0));
  });
}

tsc_status_t tsc_client_stop_replayer(tsc_client_t *client, int32_t keep_actors) {
  return TSC_GUARD({ client_of(client).StopReplayer(keep_actors != 0); });
}

tsc_status_t tsc_client_set_replayer_time_factor(tsc_client_t *client, double factor) {
  return TSC_GUARD({
    if (!(factor > 0.0) || !std::isfinite(factor)) {
      fail(TSC_INVALID_ARGUMENT, "time factor must be a positive finite number");
    }
    client_of(client).SetReplayerTimeFactor(factor);
  });
}

tsc_status_t tsc_client_generate_opendrive_world(tsc_client_t *client, const char *opendrive,
                                                 size_t opendrive_len,
                                                 const tsc_opendrive_parameters_t *parameters,
                                                 int32_t reset_settings, tsc_world_t **out) {
  return new_handle(__func__, out, [&] {
    const auto &p = *require_ptr(parameters, "parameters");
    const std::string xodr = to_string(opendrive, opendrive_len, "opendrive");
    if (xodr.empty()) fail(TSC_INVALID_ARGUMENT, "opendrive must not be empty");
    carla::rpc::OpendriveGenerationParameters params(
        p.vertex_distance, p.max_road_length, p.wall_height, p.additional_width,
        p.smooth_junctions != 0, p.enable_mesh_visibility != 0,
        p.enable_pedestrian_navigation != 0);
    return new tsc_world(client_of(client).GenerateOpenDriveWorld(xodr, params, reset_settings != 0));
  });
}

}  // extern "C"
