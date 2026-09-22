#ifndef GE_CPP_METRICS_EXPORT_H_
#define GE_CPP_METRICS_EXPORT_H_

#include <set>
#include <string>
#include <string_view>

#include <ge/cpp/node_runtime.h>

namespace ge {

class Engine;
class Session;

// OBS-1 / OBS-2 (12 §12.1): Prometheus text exposition (version 0.0.4) of
// the Engine / Session / Node / Edge counters. The engine embeds no HTTP
// server; the host scrapes this string from its own endpoint. Sampling is
// lock-free on the metric side (relaxed atomics) and tolerates momentary
// inconsistency between counters. Every series carries session="<id>" and,
// for nodes / edges, node="<external id>" / edge="<external id>"; histogram
// series follow the Prometheus histogram convention (_bucket{le=..}, _sum,
// _count) with the LatencyHistogram bucket bounds expressed in seconds.

class PrometheusWriter final {
 public:
  explicit PrometheusWriter(std::string prefix = "ge_");

  // Emits "# HELP"/"# TYPE" once per metric name (first use), then samples.
  void Counter(std::string_view name, std::string_view help, std::string_view labels, std::uint64_t v);
  void Gauge(std::string_view name, std::string_view help, std::string_view labels, double v);
  void Histogram(std::string_view name, std::string_view help, std::string_view labels,
                 const LatencyHistogram& h);
  [[nodiscard]] std::string Finish();

  [[nodiscard]] static std::string EscapeLabel(std::string_view v);

 private:
  void Header(std::string_view name, std::string_view help, std::string_view type);
  std::string prefix_;
  std::string out_;
  std::set<std::string, std::less<>> declared_;
};

// Renders one session's metrics (session, node and edge level).
void RenderSessionMetrics(PrometheusWriter& w, const Session& session);
// Renders everything the engine owns: all live sessions, the resource ledger
// water marks, executor / async runtime gauges and the audit ring counters.
[[nodiscard]] std::string RenderPrometheus(Engine& engine);

}  // namespace ge

#endif
