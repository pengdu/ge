#include <iostream>

#include <ge/cpp/engine.h>
#include <ge/cpp/graph_spec.h>

int main() {
  ge::GraphSpec graph{"install_cpp_sample"};
  ge::EngineConfig config;
  config.cpu_threads = 0;
  config.watchdog_thread = false;

  auto engine = ge::Engine::Create(std::move(config));
  if (!engine.ok()) {
    std::cerr << engine.status().ToString() << '\n';
    return 1;
  }

  std::cout << graph.name() << " engine-created" << '\n';
  return 0;
}
