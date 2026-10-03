// Mirrors LibCarla ue5-dev's carla/sensor/data/V2XData.h (issue #42): one
// received V2X message and its receive power (dBm). The CAMDataS /
// CustomV2XDataS server-side accumulators are not needed by the client.
#pragma once

#include "carla/sensor/data/LibITS.h"

namespace carla {
namespace sensor {
namespace data {

class CAMData {
 public:
  float Power;
  CAM_t Message;
};

class CustomV2XData {
 public:
  float Power;
  CustomV2XM Message;
};

}  // namespace data
}  // namespace sensor
}  // namespace carla
