// Mock V2X measurements (issue #42). CAMEvent and CustomV2XEvent mirror
// LibCarla ue5-dev's sensor/data/V2XEvent.h: an Array<T> of received messages
// (size, at, operator[], begin/end) with GetMessageCount(). The messages are
// synthetic (see Episode::SenseV2XLocked in mock.cpp).
#pragma once
#include "carla/mock/Mock.h"
#include "carla/sensor/data/V2XData.h"

#include <stdexcept>
#include <vector>

namespace carla {
namespace sensor {
namespace data {

// The subset of LibCarla's sensor::data::Array<T> the shim uses.
template <typename T>
class V2XArray : public SensorData {
 public:
  using value_type = T;
  using size_type = size_t;

  V2XArray(size_t frame, double timestamp, const geom::Transform &t, std::vector<T> messages)
      : SensorData(frame, timestamp, t), _messages(std::move(messages)) {}

  size_type size() const { return _messages.size(); }
  bool empty() const { return _messages.empty(); }
  const T &at(size_type pos) const {
    if (pos >= size()) throw std::out_of_range("out of range");
    return _messages[pos];
  }
  const T &operator[](size_type pos) const { return _messages[pos]; }
  auto begin() const { return _messages.begin(); }
  auto end() const { return _messages.end(); }

 private:
  std::vector<T> _messages;
};

class CAMEvent : public V2XArray<CAMData> {
 public:
  using V2XArray<CAMData>::V2XArray;
  size_type GetMessageCount() const { return size(); }
};

class CustomV2XEvent : public V2XArray<CustomV2XData> {
 public:
  using V2XArray<CustomV2XData>::V2XArray;
  size_type GetMessageCount() const { return size(); }
};

}  // namespace data
}  // namespace sensor
}  // namespace carla
