// Issue #86: the queue signal. Every push into a sensor or tick-listener queue
// (on a LibCarla thread) increments a process-wide counter and wakes the
// threads waiting in tsc_queue_signal_wait, so a background callback
// dispatcher can block until there is something to deliver instead of
// polling. The counter only says "something was queued somewhere"; the
// dispatcher still drains each queue through its own poll function.
#include "internal.hpp"

#include <chrono>
#include <condition_variable>
#include <mutex>

namespace tsc {
namespace {

struct QueueSignal {
  std::mutex mutex;
  std::condition_variable changed;
  uint64_t count = 0;
};

// Never destroyed: LibCarla threads and a dispatcher thread may still use it
// while static destructors run at exit.
QueueSignal &queue_signal() {
  static QueueSignal *signal = new QueueSignal();
  return *signal;
}

}  // namespace

void signal_queue_activity() {
  QueueSignal &s = queue_signal();
  {
    std::lock_guard<std::mutex> lock(s.mutex);
    ++s.count;
  }
  s.changed.notify_all();
}

}  // namespace tsc

using namespace tsc;

tsc_status_t tsc_queue_signal_count(uint64_t *out) {
  return TSC_GUARD({
    require_ptr(out, "out");
    QueueSignal &s = queue_signal();
    std::lock_guard<std::mutex> lock(s.mutex);
    *out = s.count;
  });
}

tsc_status_t tsc_queue_signal_wait(uint64_t seen, double timeout_seconds, uint64_t *out) {
  return TSC_GUARD({
    require_ptr(out, "out");
    const auto timeout =
        std::chrono::milliseconds(seconds_to_duration(timeout_seconds).milliseconds());
    QueueSignal &s = queue_signal();
    std::unique_lock<std::mutex> lock(s.mutex);
    s.changed.wait_for(lock, timeout, [&] { return s.count != seen; });
    *out = s.count;
  });
}

tsc_status_t tsc_queue_signal_notify(void) {
  return TSC_GUARD({ signal_queue_activity(); });
}
