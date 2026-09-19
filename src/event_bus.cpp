#include <ge/cpp/event_bus.h>

#include <chrono>

namespace ge {

namespace {

std::string_view SeverityName(Severity s) noexcept {
  switch (s) {
    case Severity::kDebug: return "debug";
    case Severity::kInfo: return "info";
    case Severity::kWarning: return "warning";
    case Severity::kError: return "error";
  }
  return "info";
}

std::optional<Severity> ParseSeverity(std::string_view s) noexcept {
  if (s == "debug") return Severity::kDebug;
  if (s == "info") return Severity::kInfo;
  if (s == "warning") return Severity::kWarning;
  if (s == "error") return Severity::kError;
  return std::nullopt;
}

}  // namespace

JsonValue Event::ToJson() const {
  JsonObject o;
  o["event_id"] = JsonValue(event_id);
  o["kind"] = JsonValue(kind == EventKind::kDataPlane ? "data_plane" : "observation");
  o["type"] = JsonValue(type);
  o["severity"] = JsonValue(SeverityName(severity));
  o["session_id"] = JsonValue(session);
  o["source_node_id"] = JsonValue(source_node);
  o["timestamp_ns"] = JsonValue(timestamp_ns);
  if (seq) o["seq"] = JsonValue(*seq);
  if (pts_ns) o["pts_ns"] = JsonValue(*pts_ns);
  o["detail"] = detail;
  return JsonValue(std::move(o));
}

Result<EventFilter> EventFilter::ParseJson(std::string_view json) {
  EventFilter f;
  if (json.empty()) return f;
  JsonParseResult parsed = ge::ParseJson(json);
  if (!parsed.ok()) return Status::InvalidArgument("invalid filter JSON: " + parsed.error);
  const JsonValue& doc = *parsed.value;
  if (!doc.is_object()) return Status::InvalidArgument("filter must be a JSON object");
  for (const auto& [key, value] : doc.as_object()) {
    if (key == "types") {
      if (!value.is_array()) return Status::InvalidArgument("filter.types must be an array");
      for (const JsonValue& t : value.as_array()) {
        if (!t.is_string()) return Status::InvalidArgument("filter.types entries must be strings");
        f.types.insert(t.as_string());
      }
    } else if (key == "min_severity") {
      if (!value.is_string()) return Status::InvalidArgument("filter.min_severity must be a string");
      const auto sev = ParseSeverity(value.as_string());
      if (!sev) return Status::InvalidArgument("unknown severity '" + value.as_string() + "'");
      f.min_severity = sev;
    } else if (key == "session_id") {
      if (!value.is_integer()) return Status::InvalidArgument("filter.session_id must be an integer");
      f.session = static_cast<SessionId>(value.as_integer());
    } else {
      return Status::InvalidArgument("unknown filter field '" + key + "'");
    }
  }
  return f;
}

bool EventFilter::Matches(const Event& e) const {
  if (!types.empty() && !types.contains(e.type)) return false;
  if (min_severity && static_cast<int>(e.severity) < static_cast<int>(*min_severity)) return false;
  if (session && *session != e.session) return false;
  return true;
}

EventBus::EventBus(Options options) : options_(options) {
  for (std::uint32_t i = 0; i < options_.observer_threads; ++i) {
    workers_.emplace_back([this] { Worker(); });
  }
}

EventBus::~EventBus() {
  {
    std::lock_guard lock(mutex_);
    stopped_ = true;
    ready_.clear();
    cv_.notify_all();
  }
  for (std::thread& t : workers_) t.join();
}

void EventBus::Publish(Event event) {
  if (event.event_id == 0) event.event_id = next_event_id_.fetch_add(1, std::memory_order_relaxed);
  if (event.timestamp_ns == 0) {
    event.timestamp_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                             std::chrono::system_clock::now().time_since_epoch())
                             .count();
  }
  std::lock_guard lock(mutex_);
  ++published_;
  for (auto& [id, sub] : subscriptions_) {
    if (sub->cancelled || !sub->filter.Matches(event)) continue;
    if (sub->queue.size() >= options_.queue_capacity) {
      ++sub->dropped;
      ++dropped_;
      continue;
    }
    sub->queue.push_back(event);
    if (!sub->scheduled) {
      sub->scheduled = true;
      ready_.push_back(sub);
      cv_.notify_one();
    }
  }
}

SubscriptionId EventBus::Subscribe(EventFilter filter, EventHandler handler) {
  std::lock_guard lock(mutex_);
  auto sub = std::make_shared<Subscription>();
  sub->id = next_id_++;
  sub->filter = std::move(filter);
  sub->handler = std::move(handler);
  subscriptions_[sub->id] = sub;
  return sub->id;
}

void EventBus::Cancel(SubscriptionId id) {
  std::unique_lock lock(mutex_);
  const auto it = subscriptions_.find(id);
  if (it == subscriptions_.end()) return;
  std::shared_ptr<Subscription> sub = it->second;
  sub->cancelled = true;
  sub->queue.clear();
  subscriptions_.erase(it);
  // A drain task may be mid-callback on an observer thread; wait for it
  // unless we *are* that thread (callback cancelling itself).
  const bool self = std::any_of(workers_.begin(), workers_.end(), [](const std::thread& t) {
    return t.get_id() == std::this_thread::get_id();
  });
  if (!self) sub->idle_cv.wait(lock, [&] { return !sub->scheduled; });
}

void EventBus::Flush() {
  std::unique_lock lock(mutex_);
  flush_cv_.wait(lock, [this] {
    if (running_ != 0 || !ready_.empty()) return false;
    for (const auto& [id, sub] : subscriptions_) {
      if (sub->scheduled || !sub->queue.empty()) return false;
    }
    return true;
  });
}

EventBus::Stats EventBus::stats() const {
  std::lock_guard lock(mutex_);
  return Stats{published_, delivered_, dropped_};
}

std::uint64_t EventBus::dropped(SubscriptionId id) const {
  std::lock_guard lock(mutex_);
  const auto it = subscriptions_.find(id);
  return it == subscriptions_.end() ? 0 : it->second->dropped;
}

void EventBus::Worker() {
  std::unique_lock lock(mutex_);
  for (;;) {
    cv_.wait(lock, [this] { return stopped_ || !ready_.empty(); });
    if (stopped_) return;
    std::shared_ptr<Subscription> sub = std::move(ready_.front());
    ready_.pop_front();
    ++running_;
    lock.unlock();
    Drain(sub);
    lock.lock();
    --running_;
    flush_cv_.notify_all();
  }
}

// Same subscription: strictly serial, in publish order (13 §5.4).
void EventBus::Drain(const std::shared_ptr<Subscription>& sub) {
  for (;;) {
    Event ev;
    {
      std::lock_guard lock(mutex_);
      if (sub->cancelled || sub->queue.empty()) {
        sub->scheduled = false;
        sub->idle_cv.notify_all();
        flush_cv_.notify_all();
        return;
      }
      ev = std::move(sub->queue.front());
      sub->queue.pop_front();
    }
    sub->handler(ev);
    std::lock_guard lock(mutex_);
    ++delivered_;
  }
}

}  // namespace ge
