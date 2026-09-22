#ifndef GE_CPP_EVENT_BUS_H_
#define GE_CPP_EVENT_BUS_H_

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include <ge/c/ge_abi.h>
#include <ge/cpp/json.h>
#include <ge/cpp/types.h>

namespace ge {

enum class EventKind : std::uint8_t { kDataPlane, kObservation };

struct Event {
  std::uint64_t event_id = 0;
  EventKind kind = EventKind::kObservation;
  std::string type;  // eos / error / node_failed / mutation_state / plugin_state / ...
  Severity severity = Severity::kInfo;
  SessionId session = 0;
  NodeId source_node = 0;
  std::int64_t timestamp_ns = 0;
  std::optional<PacketSeq> seq;
  std::optional<std::int64_t> pts_ns;
  JsonValue detail = JsonValue(JsonObject{});

  [[nodiscard]] JsonValue ToJson() const;
};

// "session_id": n}. Every field optional; empty == everything.
struct EventFilter {
  std::set<std::string> types;
  std::optional<Severity> min_severity;
  std::optional<SessionId> session;
  [[nodiscard]] static Result<EventFilter> ParseJson(std::string_view json);
  [[nodiscard]] bool Matches(const Event& e) const;
};

using EventHandler = std::function<void(const Event&)>;
using SubscriptionId = std::uint64_t;

// per subscription (ObserverExecutorPool), subscriptions run in parallel.
// Publish never blocks the data plane: a full queue drops and counts.
struct EventBusOptions {
  std::size_t queue_capacity = 1024;
  std::uint32_t observer_threads = 1;
};

class EventBus final {
 public:
  using Options = EventBusOptions;
  explicit EventBus(Options options = Options{});
  ~EventBus();

  void Publish(Event event);
  [[nodiscard]] SubscriptionId Subscribe(EventFilter filter, EventHandler handler);
  // Pending events of the subscription are dropped; a running callback
  // finishes first (no callback runs after Cancel returns unless called
  // from inside that callback).
  void Cancel(SubscriptionId id);
  // Waits until every queued event has been delivered (tests/shutdown).
  void Flush();

  struct Stats {
    std::uint64_t published = 0;
    std::uint64_t delivered = 0;
    std::uint64_t dropped = 0;
  };
  [[nodiscard]] Stats stats() const;
  [[nodiscard]] std::uint64_t dropped(SubscriptionId id) const;

 private:
  struct Subscription {
    SubscriptionId id = 0;
    EventFilter filter;
    EventHandler handler;
    std::deque<Event> queue;  // guarded by EventBus::mutex_
    bool scheduled = false;   // a drain task is queued/running
    bool cancelled = false;
    std::uint64_t dropped = 0;
    std::condition_variable idle_cv;
  };
  void Worker();
  void Drain(const std::shared_ptr<Subscription>& sub);

  Options options_;
  mutable std::mutex mutex_;
  std::condition_variable cv_;
  std::map<SubscriptionId, std::shared_ptr<Subscription>> subscriptions_;
  std::deque<std::shared_ptr<Subscription>> ready_;
  std::vector<std::thread> workers_;
  bool stopped_ = false;
  SubscriptionId next_id_ = 1;
  std::atomic<std::uint64_t> next_event_id_{1};
  std::uint64_t published_ = 0;
  std::uint64_t delivered_ = 0;
  std::uint64_t dropped_ = 0;
  std::uint32_t running_ = 0;
  std::condition_variable flush_cv_;
};

}  // namespace ge

#endif
