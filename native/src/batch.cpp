// Batch commands: many actor operations in one RPC (design section 30).
#include "internal.hpp"

using namespace tsc;
using Command = carla::rpc::Command;

namespace {

Command to_command(const tsc_command_t &c) {
  switch (c.type) {
    case TSC_COMMAND_SPAWN_ACTOR: {
      const auto &bp = check_handle(c.blueprint, "command.blueprint", TSC_KIND_ACTOR_BLUEPRINT)->blueprint;
      if (c.parent_id != 0) {
        return Command::SpawnActor(bp.MakeActorDescription(), to_carla(c.transform), c.parent_id);
      }
      return Command::SpawnActor(bp.MakeActorDescription(), to_carla(c.transform));
    }
    case TSC_COMMAND_DESTROY_ACTOR:
      return Command::DestroyActor(c.actor_id);
    case TSC_COMMAND_APPLY_VEHICLE_CONTROL:
      return Command::ApplyVehicleControl(c.actor_id, to_carla(c.control));
    case TSC_COMMAND_APPLY_TRANSFORM:
      return Command::ApplyTransform(c.actor_id, to_carla(c.transform));
    case TSC_COMMAND_APPLY_TARGET_VELOCITY:
      return Command::ApplyTargetVelocity(c.actor_id, to_carla_vector(c.vector));
    case TSC_COMMAND_SET_AUTOPILOT:
      return Command::SetAutopilot(c.actor_id, c.flag != 0, c.tm_port);
    case TSC_COMMAND_SET_SIMULATE_PHYSICS:
      return Command::SetSimulatePhysics(c.actor_id, c.flag != 0);
    case TSC_COMMAND_APPLY_WALKER_CONTROL:
      if (!(c.scalar >= 0.0) || !std::isfinite(c.scalar)) {
        fail(TSC_INVALID_ARGUMENT, "walker speed must be a finite, non-negative number of m/s");
      }
      return Command::ApplyWalkerControl(
          c.actor_id, carla::rpc::WalkerControl(to_carla_vector(c.vector),
                                                static_cast<float>(c.scalar), c.flag != 0));
    case TSC_COMMAND_APPLY_TARGET_ANGULAR_VELOCITY:
      return Command::ApplyTargetAngularVelocity(c.actor_id, to_carla_vector(c.vector));
    case TSC_COMMAND_APPLY_IMPULSE:
      return Command::ApplyImpulse(c.actor_id, to_carla_vector(c.vector));
    case TSC_COMMAND_APPLY_FORCE:
      return Command::ApplyForce(c.actor_id, to_carla_vector(c.vector));
    case TSC_COMMAND_APPLY_ANGULAR_IMPULSE:
      return Command::ApplyAngularImpulse(c.actor_id, to_carla_vector(c.vector));
    case TSC_COMMAND_APPLY_TORQUE:
      return Command::ApplyTorque(c.actor_id, to_carla_vector(c.vector));
    case TSC_COMMAND_SET_ENABLE_GRAVITY:
      return Command::SetEnableGravity(c.actor_id, c.flag != 0);
    case TSC_COMMAND_SET_VEHICLE_LIGHT_STATE:
      return Command::SetVehicleLightState(c.actor_id, static_cast<uint32_t>(c.flag));
    case TSC_COMMAND_APPLY_LOCATION:
      return Command::ApplyLocation(c.actor_id, to_carla(c.transform.location));
    case TSC_COMMAND_SET_TRAFFIC_LIGHT_STATE:
      if (c.flag < TSC_TRAFFIC_LIGHT_RED || c.flag > TSC_TRAFFIC_LIGHT_UNKNOWN) {
        fail(TSC_INVALID_ARGUMENT, "invalid traffic light state " + std::to_string(c.flag));
      }
      return Command::SetTrafficLightState(c.actor_id,
                                           static_cast<carla::rpc::TrafficLightState>(c.flag));
    default:
      fail(TSC_INVALID_ARGUMENT, "unknown command type " + std::to_string(c.type));
  }
}

// Builds the top-level command list, attaching `then_of` commands to the
// do_after list of the SpawnActor they follow.
std::vector<Command> build(const tsc_command_t *commands, size_t count) {
  if (commands == nullptr && count != 0) fail(TSC_INVALID_ARGUMENT, "commands must not be NULL");
  std::vector<Command> top;
  std::vector<long> top_index(count, -1);  // input index -> index in `top`
  for (size_t i = 0; i < count; ++i) {
    const tsc_command_t &c = commands[i];
    if (c.then_of < 0) {
      top_index[i] = static_cast<long>(top.size());
      top.push_back(to_command(c));
      continue;
    }
    const auto parent = static_cast<size_t>(c.then_of);
    if (parent >= i || top_index[parent] < 0 ||
        commands[parent].type != TSC_COMMAND_SPAWN_ACTOR) {
      fail(TSC_INVALID_ARGUMENT, "command " + std::to_string(i) +
                                     ": then_of must name an earlier top-level SpawnActor");
    }
    auto &spawn = std::get<Command::SpawnActor>(top[top_index[parent]].command);
    spawn.do_after.push_back(to_command(c));
  }
  return top;
}

// Like the official Python API: autopilot set through a batch also registers
// the vehicle with the Traffic Manager on the client side.
void register_autopilot(carla::client::Client &client, const std::vector<Command> &top,
                        const std::vector<carla::rpc::CommandResponse> &responses) {
  std::optional<carla::client::World> world;
  for (size_t i = 0; i < top.size() && i < responses.size(); ++i) {
    if (responses[i].HasError()) continue;
    const Command::SetAutopilot *autopilot = nullptr;
    if (const auto *spawn = std::get_if<Command::SpawnActor>(&top[i].command)) {
      for (const auto &after : spawn->do_after) {
        if (const auto *a = std::get_if<Command::SetAutopilot>(&after.command)) autopilot = a;
      }
    } else {
      autopilot = std::get_if<Command::SetAutopilot>(&top[i].command);
    }
    if (autopilot == nullptr) continue;
    if (!world) world = client.GetWorld();
    auto vehicle = downcast<carla::client::Vehicle>(world->GetActor(responses[i].Get()));
    if (vehicle) vehicle->SetAutopilot(autopilot->enabled, autopilot->tm_port);
  }
}

}  // namespace

extern "C" {

tsc_status_t tsc_client_apply_batch(tsc_client_t *client, const tsc_command_t *commands,
                                    size_t count, int32_t do_tick) {
  return TSC_GUARD({
    auto &c = check_handle(client, "client", TSC_KIND_CLIENT)->client;
    c.ApplyBatch(build(commands, count), do_tick != 0);
  });
}

tsc_status_t tsc_client_apply_batch_sync(tsc_client_t *client, const tsc_command_t *commands,
                                         size_t count, int32_t do_tick,
                                         tsc_command_response_t *out, size_t out_capacity,
                                         size_t *out_count) {
  return TSC_GUARD({
    require_ptr(out_count, "out_count");
    *out_count = 0;
    auto &c = check_handle(client, "client", TSC_KIND_CLIENT)->client;
    std::vector<Command> top = build(commands, count);
    if (out_capacity < top.size()) {
      fail(TSC_INVALID_ARGUMENT, "out has room for " + std::to_string(out_capacity) +
                                     " responses, need " + std::to_string(top.size()));
    }
    require_ptr(out, "out");
    auto responses = c.ApplyBatchSync(top, do_tick != 0);
    // On a failure after strings were handed out, free them: the caller only
    // reads `out` when the call succeeds.
    try {
      for (size_t i = 0; i < responses.size(); ++i) {
        tsc_command_response_t r{};
        if (responses[i].HasError()) {
          r.has_error = 1;
          string_assign(&r.error, responses[i].GetError().What());
        } else {
          r.actor_id = responses[i].Get();
        }
        out[i] = r;
        *out_count = i + 1;
      }
      register_autopilot(c, top, responses);
    } catch (...) {
      for (size_t i = 0; i < *out_count; ++i) tsc_string_free(&out[i].error);
      *out_count = 0;
      throw;
    }
  });
}

}  // extern "C"
