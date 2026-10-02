// The subset of LibCarla the shim uses, plus the few spots where the real
// library and the in-memory mock backend (native/mock) differ.
#pragma once

#include <carla/Memory.h>
#include <carla/Time.h>
#include <carla/Version.h>
#include <carla/client/ActorBlueprint.h>
#include <carla/client/ActorList.h>
#include <carla/client/BlueprintLibrary.h>
#include <carla/client/Client.h>
#include <carla/client/TimeoutException.h>
#include <carla/client/Vehicle.h>
#include <carla/client/World.h>
#include <carla/geom/Transform.h>
#include <carla/rpc/EpisodeSettings.h>
#include <carla/rpc/VehicleControl.h>

namespace tsc {

// carla::SharedPtr is std::shared_ptr in CARLA UE5 (it was boost::shared_ptr
// in UE4, which is not supported).
template <typename To, typename From>
carla::SharedPtr<To> downcast(const carla::SharedPtr<From> &p) {
  return std::dynamic_pointer_cast<To>(p);
}

#ifdef TSC_MOCK_LIBCARLA
inline constexpr const char *kBackendName = "mock";
#else
inline constexpr const char *kBackendName = "libcarla";
#endif

}  // namespace tsc
