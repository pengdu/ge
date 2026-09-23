#include <ge/cpp/batch_runner.h>

#include <set>
#include <thread>

namespace ge {

struct BatchRunner::Slot {
  std::size_t item = 0;
  Session* session = nullptr;
  std::chrono::steady_clock::time_point started;
  bool active = false;
};

BatchRunner::BatchRunner(Engine& engine, GraphTemplate tmpl, BatchOptions options)
    : engine_(engine), template_(std::move(tmpl)), options_(std::move(options)) {
  if (options_.concurrency == 0) options_.concurrency = 1;
  if (options_.max_attempts < 1) options_.max_attempts = 1;
}

void BatchRunner::Pump() {
  if (engine_.config().cpu_threads == 0) {
    (void)engine_.executor().RunOne();
    engine_.Tick();
  } else if (!engine_.config().watchdog_thread) {
    engine_.Tick();
  }
}

Result<BatchReport> BatchRunner::Run(const std::vector<BatchItem>& items) {
  {
    std::set<std::string_view> names;
    for (const BatchItem& item : items) {
      if (item.name.empty()) return Status::InvalidArgument("batch item with empty name");
      if (!names.insert(item.name).second) {
        return Status::InvalidArgument("duplicate batch item name '" + item.name + "'");
      }
    }
  }
  if (!template_.validated_for(engine_.operator_generation())) {
    if (Status s = engine_.PrevalidateTemplate(template_); !s.ok()) return s;
  }

  BatchReport report;
  report.items.resize(items.size());
  for (std::size_t i = 0; i < items.size(); ++i) report.items[i].name = items[i].name;

  const bool inline_engine = engine_.config().cpu_threads == 0;
  std::vector<Slot> slots(options_.concurrency);
  std::vector<std::size_t> queue;  // indices still to run (retries re-append)
  queue.reserve(items.size());
  for (std::size_t i = 0; i < items.size(); ++i) queue.push_back(i);

  const auto start = [&](Slot& slot, std::size_t index) -> void {
    BatchItemResult& r = report.items[index];
    ++r.attempts;
    slot.item = index;
    slot.started = std::chrono::steady_clock::now();
    auto session = engine_.CreateSession(template_, items[index].arguments,
                                         items[index].name + "-a" + std::to_string(r.attempts), options_.caller);
    Status failure;
    if (session.ok()) {
      slot.session = *session;
      if (Status s = slot.session->Start(); s.ok()) {
        slot.active = true;
        return;
      } else {
        failure = s;
      }
      (void)engine_.DestroySession(slot.session->id());
      slot.session = nullptr;
    } else {
      failure = session.status();
    }
    r.status = failure;
    r.elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - slot.started);
    if (r.attempts < options_.max_attempts) queue.push_back(index);
    slot.active = false;
  };

  const auto finish = [&](Slot& slot) -> void {
    BatchItemResult& r = report.items[slot.item];
    const SessionState state = slot.session->state();
    r.status = state == SessionState::kStopped ? Status::Ok() : slot.session->failure();
    if (state != SessionState::kStopped && r.status.ok()) {
      r.status = Status::Internal("session ended in state " + std::string(ToString(state)));
    }
    r.elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - slot.started);
    (void)engine_.DestroySession(slot.session->id());
    slot.session = nullptr;
    slot.active = false;
    if (!r.status.ok() && r.attempts < options_.max_attempts) queue.push_back(slot.item);
  };

  for (;;) {
    bool any_active = false;
    for (Slot& slot : slots) {
      if (!slot.active && !queue.empty()) {
        const std::size_t index = queue.front();
        queue.erase(queue.begin());
        start(slot, index);
      }
      if (!slot.active) continue;
      if (slot.session->WaitStopped(std::chrono::milliseconds(0))) {
        finish(slot);
      } else if (std::chrono::steady_clock::now() - slot.started > options_.item_timeout) {
        (void)slot.session->Stop(true);
        for (int i = 0; i < 100000 && !slot.session->WaitStopped(std::chrono::milliseconds(inline_engine ? 0 : 5)); ++i) {
          Pump();
        }
        BatchItemResult& r = report.items[slot.item];
        const std::size_t item = slot.item;
        finish(slot);
        if (r.status.ok()) {
          // A fast stop ends in kStopped; the item still failed its budget.
          r.status = Status::Cancelled("item timeout after " + std::to_string(options_.item_timeout.count()) + "ms");
          if (r.attempts < options_.max_attempts) queue.push_back(item);
        }
      }
      any_active = any_active || slot.active;
    }
    if (!any_active && queue.empty()) break;
    if (inline_engine) {
      Pump();
    } else {
      Pump();
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  }
  for (const BatchItemResult& r : report.items) {
    if (r.status.ok() && r.attempts > 0) {
      ++report.succeeded;
    } else {
      ++report.failed;
    }
  }
  return report;
}

}  // namespace ge
