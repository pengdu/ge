// OBS-1 / OBS-2 (12 §12.1): Prometheus text exposition.
#include <ge/cpp/metrics_export.h>

#include <cstdio>
#include <limits>

#include <ge/cpp/audit_log.h>
#include <ge/cpp/engine.h>
#include <ge/cpp/resource_ledger.h>
#include <ge/cpp/session.h>

namespace ge {

namespace {

std::string Labels(std::initializer_list<std::pair<std::string_view, std::string>> kv) {
  std::string out;
  for (const auto& [k, v] : kv) {
    if (!out.empty()) out += ',';
    out += k;
    out += "=\"";
    out += PrometheusWriter::EscapeLabel(v);
    out += '"';
  }
  return out;
}

std::string Join(std::string_view a, std::string_view b) {
  if (a.empty()) return std::string(b);
  if (b.empty()) return std::string(a);
  std::string out(a);
  out += ',';
  out += b;
  return out;
}

std::string FormatDouble(double v) {
  char buf[64];
  std::snprintf(buf, sizeof(buf), "%.9g", v);
  return buf;
}

}  // namespace

// ---------------------------------------------------------------------------
// PrometheusWriter
// ---------------------------------------------------------------------------

PrometheusWriter::PrometheusWriter(std::string prefix) : prefix_(std::move(prefix)) {}

std::string PrometheusWriter::EscapeLabel(std::string_view v) {
  std::string out;
  out.reserve(v.size());
  for (const char c : v) {
    switch (c) {
      case '\\': out += "\\\\"; break;
      case '"': out += "\\\""; break;
      case '\n': out += "\\n"; break;
      default: out += c;
    }
  }
  return out;
}

void PrometheusWriter::Header(std::string_view name, std::string_view help, std::string_view type) {
  const std::string full = prefix_ + std::string(name);
  if (!declared_.insert(full).second) return;
  out_ += "# HELP ";
  out_ += full;
  out_ += ' ';
  out_ += help;
  out_ += "\n# TYPE ";
  out_ += full;
  out_ += ' ';
  out_ += type;
  out_ += '\n';
}

void PrometheusWriter::Counter(std::string_view name, std::string_view help, std::string_view labels,
                               std::uint64_t v) {
  Header(name, help, "counter");
  out_ += prefix_;
  out_ += name;
  if (!labels.empty()) {
    out_ += '{';
    out_ += labels;
    out_ += '}';
  }
  out_ += ' ';
  out_ += std::to_string(v);
  out_ += '\n';
}

void PrometheusWriter::Gauge(std::string_view name, std::string_view help, std::string_view labels, double v) {
  Header(name, help, "gauge");
  out_ += prefix_;
  out_ += name;
  if (!labels.empty()) {
    out_ += '{';
    out_ += labels;
    out_ += '}';
  }
  out_ += ' ';
  out_ += FormatDouble(v);
  out_ += '\n';
}

void PrometheusWriter::Histogram(std::string_view name, std::string_view help, std::string_view labels,
                                 const LatencyHistogram& h) {
  Header(name, help, "histogram");
  std::uint64_t cumulative = 0;
  for (std::size_t b = 0; b < LatencyHistogram::kBuckets; ++b) {
    cumulative += h.BucketCount(b);
    const std::uint64_t upper = LatencyHistogram::BucketUpperNs(b);
    out_ += prefix_;
    out_ += name;
    out_ += "_bucket{";
    out_ += labels;
    if (!labels.empty()) out_ += ',';
    out_ += "le=\"";
    out_ += upper == std::numeric_limits<std::uint64_t>::max() ? std::string("+Inf")
                                                                : FormatDouble(static_cast<double>(upper) * 1e-9);
    out_ += "\"} ";
    out_ += std::to_string(cumulative);
    out_ += '\n';
  }
  out_ += prefix_;
  out_ += name;
  out_ += "_sum";
  if (!labels.empty()) {
    out_ += '{';
    out_ += labels;
    out_ += '}';
  }
  out_ += ' ';
  out_ += FormatDouble(static_cast<double>(h.sum_ns()) * 1e-9);
  out_ += '\n';
  out_ += prefix_;
  out_ += name;
  out_ += "_count";
  if (!labels.empty()) {
    out_ += '{';
    out_ += labels;
    out_ += '}';
  }
  out_ += ' ';
  out_ += std::to_string(h.count());
  out_ += '\n';
}

std::string PrometheusWriter::Finish() {
  declared_.clear();
  return std::move(out_);
}

// ---------------------------------------------------------------------------
// Session / node / edge
// ---------------------------------------------------------------------------

void RenderSessionMetrics(PrometheusWriter& w, const Session& session) {
  const std::string sl = Labels({{"session", std::to_string(session.id())}});
  const std::shared_ptr<RuntimeTopology> topo = session.current_topology();
  const SessionMetrics& sm = session.scheduler().metrics();

  w.Gauge("session_state",
          "Session state (0 created, 1 starting, 2 running, 3 pausing, 4 paused, 5 stopping, 6 stopped, 7 failed)",
          sl, static_cast<double>(static_cast<int>(session.state())));
  w.Gauge("session_topology_version", "Current topology version", sl, static_cast<double>(topo->version()));
  w.Gauge("session_pending_retirements", "Retired topology versions still draining", sl,
          static_cast<double>(session.scheduler().pending_retirements()));
  w.Counter("session_mutations_succeeded_total", "Mutations applied", sl,
            sm.mutations_succeeded.load(std::memory_order_relaxed));
  w.Counter("session_mutations_failed_total", "Mutations rejected or failed", sl,
            sm.mutations_failed.load(std::memory_order_relaxed));
  w.Counter("session_parameter_updates_total", "Hot parameter updates accepted", sl,
            sm.parameter_updates.load(std::memory_order_relaxed));
  w.Counter("session_retired_topologies_total", "Topology versions fully retired", sl,
            sm.retired_topologies.load(std::memory_order_relaxed));
  w.Counter("session_drain_timeouts_total", "Graceful retires upgraded to fast after drain_timeout", sl,
            sm.drain_timeouts.load(std::memory_order_relaxed));
  w.Histogram("session_mutation_publish_seconds", "Apply accepted -> topology swapped", sl, sm.mutation_publish);
  w.Histogram("session_mutation_retire_seconds", "Topology swapped -> last removed node closed", sl,
              sm.mutation_retire);
  w.Histogram("session_end_to_end_seconds", "Source emit -> sink consume per packet", sl, sm.end_to_end);

  // Nodes: rows for every node of the current topology (retiring versions
  // share NodeRuntime objects for kept nodes, so no double counting).
  for (const NodeRuntimeRef& n : topo->nodes()) {
    const std::string nl = Join(sl, Labels({{"node", n->external_id()}, {"op", n->operator_key().ToString()}}));
    const NodeMetrics& m = n->metrics();
    w.Gauge("node_state", "Node state (12 §7 NodeState ordinal)", nl,
            static_cast<double>(static_cast<int>(n->state())));
    w.Counter("node_invocations_total", "Process/Submit calls", nl, m.invocations.load(std::memory_order_relaxed));
    w.Counter("node_packets_in_total", "Packets consumed", nl, m.packets_in.load(std::memory_order_relaxed));
    w.Counter("node_packets_out_total", "Packets emitted", nl, m.packets_out.load(std::memory_order_relaxed));
    w.Counter("node_errors_total", "Failed invocations", nl, m.errors.load(std::memory_order_relaxed));
    w.Counter("node_would_block_total", "Emits that hit a full block-policy edge", nl,
              m.would_block.load(std::memory_order_relaxed));
    w.Gauge("node_in_flight", "Invocations currently running", nl, static_cast<double>(n->in_flight()));
    w.Histogram("node_process_seconds", "Synchronous Process call latency", nl, m.process_latency);
    if (n->is_async()) {
      w.Counter("node_async_submitted_total", "Async requests submitted", nl,
                m.submitted.load(std::memory_order_relaxed));
      w.Counter("node_async_completed_total", "Async requests completed", nl,
                m.completed.load(std::memory_order_relaxed));
      w.Gauge("node_async_pending", "Async requests in flight", nl, static_cast<double>(n->async_pending()));
      w.Counter("node_async_batches_total", "Batches flushed", nl, m.batch_count.load(std::memory_order_relaxed));
      w.Counter("node_async_batch_size_sum", "Sum of batch sizes", nl,
                m.batch_size_sum.load(std::memory_order_relaxed));
      w.Counter("node_async_reorder_gaps_total", "Reorder gaps emitted", nl,
                m.reorder_gaps.load(std::memory_order_relaxed));
      w.Counter("node_async_late_completions_total", "Completions after a gap was emitted", nl,
                m.late_completions.load(std::memory_order_relaxed));
      w.Counter("node_async_orphan_completions_total", "Completions for unknown requests", nl,
                m.orphan_completions.load(std::memory_order_relaxed));
      w.Histogram("node_async_wait_seconds", "Submit -> completion", nl, m.async_wait);
    }
  }
  for (const EdgeChannelRef& e : topo->edges()) {
    const std::string el = Join(sl, Labels({{"edge", e->external_id()}}));
    const EdgeMetrics::Snapshot m = e->metrics().Load();
    w.Gauge("edge_queue_depth", "Packets queued", el, m.queue_depth);
    w.Gauge("edge_max_depth", "High-water mark of queue depth", el, m.max_depth);
    w.Counter("edge_pushed_total", "Packets pushed", el, m.pushed);
    w.Counter("edge_popped_total", "Packets popped", el, m.popped);
    w.Counter("edge_dropped_total", "Packets dropped by the overflow policy", el, m.drop_count);
    w.Counter("edge_would_block_total", "Pushes refused by the block policy", el, m.would_block_count);
    w.Counter("edge_cancelled_total", "Packets discarded on retire/cancel", el, m.cancelled_count);
  }
}

// ---------------------------------------------------------------------------
// Engine
// ---------------------------------------------------------------------------

std::string RenderPrometheus(Engine& engine) {
  PrometheusWriter w;
  const std::vector<std::shared_ptr<Session>> sessions = engine.SessionRefs();
  w.Gauge("engine_sessions", "Live sessions", "", static_cast<double>(sessions.size()));
  w.Gauge("engine_executor_threads", "CPU executor worker threads", "",
          static_cast<double>(engine.executor().cpu_threads()));
  w.Gauge("engine_async_in_flight", "Async requests in flight across all sessions", "",
          static_cast<double>(engine.async_runtime().in_flight()));
  w.Counter("engine_audit_records_total", "Audit records appended", "", engine.audit().last_audit_id());
  w.Counter("engine_audit_dropped_total", "Audit records evicted from the ring", "", engine.audit().dropped());
  if (ResourceLedger* ledger = engine.resource_ledger(); ledger != nullptr) {
    w.Gauge("engine_resource_leases", "Live resource leases", "", static_cast<double>(ledger->live_leases()));
    for (const ResourceUsage& u : ledger->Usage()) {
      const std::string rl =
          Labels({{"kind", std::string(ToString(u.kind))}, {"device", std::to_string(u.device_id)}});
      w.Gauge("resource_capacity", "Configured capacity (0 = unlimited)", rl, static_cast<double>(u.capacity));
      w.Gauge("resource_reserved", "Sum of live leases", rl, static_cast<double>(u.reserved));
      w.Gauge("resource_peak", "High-water mark of reserved", rl, static_cast<double>(u.peak));
    }
  }
  for (const std::shared_ptr<Session>& s : sessions) RenderSessionMetrics(w, *s);
  return w.Finish();
}

}  // namespace ge
