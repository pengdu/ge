// ExecutorPool: CPU MPSC work queue (12 §6.1). 0 threads == inline
// executor pumped by the host (RunPending / RunOne).
#include <ge/cpp/scheduler.h>

namespace ge {

ExecutorPool::ExecutorPool(std::uint32_t cpu_threads) {
  for (std::uint32_t i = 0; i < cpu_threads; ++i) workers_.emplace_back([this] { Worker(); });
}

ExecutorPool::~ExecutorPool() { Stop(); }

void ExecutorPool::SubmitCpu(Task task) {
  {
    std::lock_guard lock(mutex_);
    if (stopped_) return;
    queue_.push_back(std::move(task));
  }
  cv_.notify_one();
}

bool ExecutorPool::RunPending() {
  bool ran = false;
  while (RunOne()) ran = true;
  return ran;
}

bool ExecutorPool::RunOne() {
  Task task;
  {
    std::lock_guard lock(mutex_);
    if (queue_.empty()) return false;
    task = std::move(queue_.front());
    queue_.pop_front();
  }
  task();
  return true;
}

void ExecutorPool::Stop() {
  {
    std::lock_guard lock(mutex_);
    if (stopped_) return;
    stopped_ = true;
    queue_.clear();
  }
  cv_.notify_all();
  for (std::thread& t : workers_) {
    if (t.joinable()) t.join();
  }
  workers_.clear();
}

void ExecutorPool::Worker() {
  for (;;) {
    Task task;
    {
      std::unique_lock lock(mutex_);
      cv_.wait(lock, [this] { return stopped_ || !queue_.empty(); });
      if (stopped_) return;
      task = std::move(queue_.front());
      queue_.pop_front();
    }
    task();
  }
}
}  // namespace ge
