// Mock V2X measurements (issue #42). CAMEvent and CustomV2XEvent mirror
// LibCarla ue5-dev's sensor/data/V2XEvent.h: an Array<T> of received messages
// with GetMessageCount(). The messages are synthetic (see Episode::SenseV2XLocked in mock.cpp).
#pragma once
#include "carla/mock/Mock.h"
#include "carla/sensor/data/V2XData.h"

#include <vector>

namespace carla {
namespace sensor {
namespace data {

// The subset of LibCarla's sensor::data::Array<T> (and of the two events
// derived from it) that the shim uses. LibCarla's at() does not check the
// index either (the shim does).
template <typename T>
class V2XArray : public SensorData {
 public:
  using value_type = T;
  using size_type = size_t;

  V2XArray(size_t frame, double timestamp, const geom::Transform &t, std::vector<T> messages)
      : SensorData(frame, timestamp, t), _messages(std::move(messages)) {}

  size_type size() const { return _messages.size(); }
  const T &at(size_type pos) const { return _messages[pos]; }
  size_type GetMessageCount() const { return size(); }

 private:
  std::vector<T> _messages;
};

using CAMEvent = V2XArray<CAMData>;
using CustomV2XEvent = V2XArray<CustomV2XData>;

}  // namespace data
}  // namespace sensor
}  // namespace carla
