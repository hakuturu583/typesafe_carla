// Mirrors LibCarla ue5-dev's carla/rpc/CustomV2XBytes.h (issue #42): the
// fixed-size payload of the custom V2X sensor (sensor.other.v2x_custom).
// LibCarla 0.10.0 has no such header. Self-contained (no MsgPack in the mock).
#pragma once

#include <array>
#include <cstdint>

namespace carla {
namespace rpc {

class CustomV2XBytes {
 public:
  uint8_t max_data_size() const { return static_cast<uint8_t>(bytes.max_size()); }

  uint8_t data_size{0u};
  std::array<unsigned char, 100> bytes{};
};

}  // namespace rpc
}  // namespace carla
