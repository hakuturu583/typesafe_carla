// Milestone 4: walkers and their AI controllers.
#include "internal.hpp"

using namespace tsc;

namespace {

carla::client::Walker &walker_of(tsc_walker_t *w) {
  return static_cast<carla::client::Walker &>(*check_handle(w, "walker", TSC_KIND_WALKER)->actor);
}

carla::client::WalkerAIController &controller_of(tsc_walker_ai_controller_t *c) {
  return static_cast<carla::client::WalkerAIController &>(
      *check_handle(c, "controller", TSC_KIND_WALKER_AI_CONTROLLER)->actor);
}

}  // namespace

extern "C" {

tsc_status_t tsc_walker_apply_control(tsc_walker_t *walker, const tsc_walker_control_t *control) {
  return TSC_GUARD({
    const auto &c = *require_ptr(control, "control");
    if (!(c.speed >= 0.0) || !std::isfinite(c.speed)) {
      fail(TSC_INVALID_ARGUMENT, "speed must be a finite, non-negative number of m/s");
    }
    walker_of(walker).ApplyControl(carla::rpc::WalkerControl(
        to_carla_vector(c.direction), static_cast<float>(c.speed), c.jump != 0));
  });
}

tsc_status_t tsc_walker_get_control(tsc_walker_t *walker, tsc_walker_control_t *out) {
  return TSC_GUARD({
    require_ptr(out, "out");
    const carla::rpc::WalkerControl c = walker_of(walker).GetWalkerControl();
    *out = tsc_walker_control_t{from_carla(c.direction), c.speed, c.jump ? 1 : 0, 0};
  });
}

tsc_status_t tsc_walker_ai_controller_start(tsc_walker_ai_controller_t *controller) {
  return TSC_GUARD({ controller_of(controller).Start(); });
}

tsc_status_t tsc_walker_ai_controller_stop(tsc_walker_ai_controller_t *controller) {
  return TSC_GUARD({ controller_of(controller).Stop(); });
}

tsc_status_t tsc_walker_ai_controller_go_to_location(tsc_walker_ai_controller_t *controller,
                                                     const tsc_location_t *destination) {
  return TSC_GUARD({
    controller_of(controller).GoToLocation(to_carla(*require_ptr(destination, "destination")));
  });
}

tsc_status_t tsc_walker_ai_controller_set_max_speed(tsc_walker_ai_controller_t *controller,
                                                    double max_speed) {
  return TSC_GUARD({
    if (!(max_speed >= 0.0) || !std::isfinite(max_speed)) {
      fail(TSC_INVALID_ARGUMENT, "max_speed must be a finite, non-negative number of m/s");
    }
    controller_of(controller).SetMaxSpeed(static_cast<float>(max_speed));
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

}  // extern "C"
