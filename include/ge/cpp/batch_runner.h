#ifndef GE_CPP_BATCH_RUNNER_H_
#define GE_CPP_BATCH_RUNNER_H_

// GM-3 companion: runs one GraphTemplate over a list of argument sets
// ("a batch of videos through one graph", "one long video as N slices").
// The template is prevalidated once; each item becomes its own session
// (GM-7 isolation: a failing item never touches the others), at most
// |concurrency| of them live at a time, finished-to-stopped items are
// destroyed before the next one starts. Items whose session fails (or whose
// creation is rejected, e.g. RESOURCE_EXHAUSTED admission) are retried up
// to |max_attempts| times in total.
//
// Run() drives everything itself: with a threaded engine it waits on the
// sessions; with an inline engine (cpu_threads == 0) it pumps the executor
// between checks, so tests stay deterministic.

#include <chrono>
#include <string>
#include <vector>

#include <ge/cpp/engine.h>
#include <ge/cpp/graph_template.h>
#include <ge/cpp/json.h>
#include <ge/cpp/types.h>

namespace ge {

struct BatchItem {
  std::string name;  // instance name, must be unique within the batch
  JsonValue arguments = JsonValue(JsonObject{});
};

struct BatchOptions {
  std::size_t concurrency = 2;
  int max_attempts = 1;
  // Per-item wall clock budget; expired items are fast-stopped and count
  // as failed (then retried like any failure).
  std::chrono::milliseconds item_timeout{600000};
  CallerContext caller;
};

struct BatchItemResult {
  std::string name;
  Status status;  // Ok: the session drained and stopped cleanly
  int attempts = 0;
  std::chrono::milliseconds elapsed{0};
};

struct BatchReport {
  std::vector<BatchItemResult> items;  // input order
  std::size_t succeeded = 0;
  std::size_t failed = 0;
  [[nodiscard]] bool ok() const noexcept { return failed == 0; }
};

class BatchRunner final {
 public:
  BatchRunner(Engine& engine, GraphTemplate tmpl, BatchOptions options = {});

  // Blocks until every item finished (or exhausted its attempts).
  [[nodiscard]] Result<BatchReport> Run(const std::vector<BatchItem>& items);

  [[nodiscard]] const GraphTemplate& graph_template() const noexcept { return template_; }

 private:
  struct Slot;
  void Pump();

  Engine& engine_;
  GraphTemplate template_;
  BatchOptions options_;
};

}  // namespace ge

#endif
