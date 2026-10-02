// Generic reference-counted handle lifetime and version queries.
#include "internal.hpp"

#ifndef TSC_BUILD_COMMIT
#define TSC_BUILD_COMMIT "unknown"
#endif
#ifndef TSC_CARLA_GIT_REF
#define TSC_CARLA_GIT_REF "unknown"
#endif
#ifndef TSC_CARLA_GIT_COMMIT
#define TSC_CARLA_GIT_COMMIT "unknown"
#endif

namespace {
std::atomic<uint64_t> g_live_handles{0};
}

tsc_handle::tsc_handle(tsc_handle_kind_t k) : kind(k) { ++g_live_handles; }

tsc_handle::~tsc_handle() { --g_live_handles; }

namespace tsc {

tsc_actor *make_actor_handle(const carla::SharedPtr<carla::client::Actor> &actor) {
  if (actor == nullptr) return nullptr;
  if (auto vehicle = downcast<carla::client::Vehicle>(actor)) {
    return new tsc_vehicle(std::move(vehicle));
  }
  if (auto sensor = downcast<carla::client::Sensor>(actor)) {
    return new tsc_sensor(std::move(sensor));
  }
  if (auto walker = downcast<carla::client::Walker>(actor)) {
    return new tsc_walker(std::move(walker));
  }
  if (auto controller = downcast<carla::client::WalkerAIController>(actor)) {
    return new tsc_walker_ai_controller(std::move(controller));
  }
  if (auto light = downcast<carla::client::TrafficLight>(actor)) {
    return new tsc_traffic_light(std::move(light));
  }
  return new tsc_actor(actor);
}

}  // namespace tsc

extern "C" {

void tsc_handle_retain(tsc_handle_t *handle) {
  if (handle != nullptr) handle->refcount.fetch_add(1, std::memory_order_relaxed);
}

void tsc_handle_release(tsc_handle_t *handle) {
  if (handle == nullptr) return;
  if (handle->refcount.fetch_sub(1, std::memory_order_acq_rel) == 1) {
    // Destructors are implicitly noexcept: a throwing LibCarla destructor
    // terminates rather than unwinding through the C ABI.
    delete handle;
  }
}

tsc_handle_kind_t tsc_handle_kind(const tsc_handle_t *handle) {
  return handle == nullptr ? TSC_KIND_INVALID : handle->kind;
}

uint32_t tsc_handle_refcount(const tsc_handle_t *handle) {
  return handle == nullptr ? 0u : handle->refcount.load(std::memory_order_relaxed);
}

uint64_t tsc_live_handle_count(void) { return g_live_handles.load(); }

uint32_t tsc_abi_version(void) { return TSC_ABI_VERSION; }

const char *tsc_libcarla_version(void) { return carla::version(); }

const char *tsc_libcarla_git_ref(void) { return TSC_CARLA_GIT_REF; }

const char *tsc_libcarla_git_commit(void) { return TSC_CARLA_GIT_COMMIT; }

const char *tsc_build_commit(void) { return TSC_BUILD_COMMIT; }

const char *tsc_backend_name(void) { return tsc::kBackendName; }

}  // extern "C"
