// The queue between a LibCarla thread (sensor measurements, world ticks) and
// the Codon side, which polls it or dispatches callbacks on the program's
// thread (design section 15).
#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>

namespace tsc {

// Issue #86: counts every push into any ItemQueue and wakes the threads in
// tsc_queue_signal_wait (a background callback dispatcher), so they block
// instead of polling. Defined in queue_signal.cpp.
void signal_queue_activity();

// Items (shared pointers) waiting to be polled. Bounded (when full, the
// oldest is dropped) unless the capacity is 0, which means unbounded.
template <typename Item>
class ItemQueue {
 public:
  explicit ItemQueue(size_t capacity) : _capacity(capacity) {}

  void push(Item item) {
    Item evicted;  // released after unlocking: it may own a large buffer
    {
      std::lock_guard<std::mutex> lock(_mutex);
      if (_capacity != 0 && _items.size() >= _capacity) {
        evicted = take_front_locked();
        ++_dropped;
      }
      _items.push_back(std::move(item));
    }
    _ready.notify_one();
    signal_queue_activity();
  }

  // The oldest item, waiting up to `timeout` for one; nullptr on timeout.
  Item wait(std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(_mutex);
    if (!_ready.wait_for(lock, timeout, [this] { return !_items.empty(); })) return nullptr;
    return take_front_locked();
  }

  // The oldest item, or nullptr when empty. Never blocks.
  Item pop() {
    std::lock_guard<std::mutex> lock(_mutex);
    return _items.empty() ? nullptr : take_front_locked();
  }

  uint64_t dropped() const {
    std::lock_guard<std::mutex> lock(_mutex);
    return _dropped;
  }

  size_t size() const {
    std::lock_guard<std::mutex> lock(_mutex);
    return _items.size();
  }

 private:
  Item take_front_locked() {
    Item item = std::move(_items.front());
    _items.pop_front();
    return item;
  }

  mutable std::mutex _mutex;
  std::condition_variable _ready;
  std::deque<Item> _items;
  const size_t _capacity;  // 0 = unbounded
  uint64_t _dropped = 0;
};

// The newest frame a World::OnTick listener has received (issue #21), so that
// World.tick() can wait until a frame's snapshot is queued: LibCarla publishes
// the episode state before it runs the OnTick callbacks.
class FrameWatch {
 public:
  void seen(uint64_t frame) {
    {
      std::lock_guard<std::mutex> lock(_mutex);
      if (frame > _frame) _frame = frame;
    }
    _changed.notify_all();
  }

  // Whether a frame >= `frame` was seen, waiting up to `timeout` for one.
  bool wait_for(uint64_t frame, std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(_mutex);
    return _changed.wait_for(lock, timeout, [&] { return _frame >= frame; });
  }

 private:
  std::mutex _mutex;
  std::condition_variable _changed;
  uint64_t _frame = 0;
};

}  // namespace tsc
