#ifndef GE_SRC_SESSION_MANAGER_H_
#define GE_SRC_SESSION_MANAGER_H_

#include <ge/cpp/engine.h>
#include <ge/cpp/graph_template.h>
#include <ge/cpp/session.h>

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace ge {

// Everything a SessionManager borrows from its Engine. Raw pointers: the
// Engine owns all of them and outlives the manager (it is a member).
struct SessionManagerServices {
  OperatorFactory* factory = nullptr;      // capability resolution + instantiation
  ExecutorPool* executor = nullptr;
  OperationRegistry* operations = nullptr;
  AsyncRuntime* async = nullptr;
  ResourceLedger* ledger = nullptr;        // null when admission is off
  // PluginRegistry::operator_generation via the engine; template caches are
  // stamped with it (GM-3 / TD-01).
  std::function<std::uint64_t()> operator_generation;
};

// Session ownership and lifecycle for one Engine: id allocation, the live
// map, admission (spec or template instantiation, validation, resource
// reservation) and teardown. Split out of Engine so session admission
// changes (SC-, RES-, GM-3 rules) stop competing for the same file as the
// plugin lifecycle; the two never touch each other's state, they only meet
// in Engine, which owns both.
//
// Locking: |mutex_| guards the map and the id counter, and is always taken
// *before* a Session's own locks, so the only legal order is
// SessionManager::mutex_ -> Session::lease_mutex_ -> ResourceLedger::mutex_.
// Nothing here calls into a Session under |mutex_|: callers get a pinned
// shared_ptr and walk it outside. EngineTest.
// SessionLedgerAndMetricsPathsDoNotDeadlock is the canary for that invariant.
class SessionManager final {
 public:
  // Builds the per-session event hooks (engine policy: events + audit).
  using SessionEventsFactory = std::function<SessionEvents(SessionId id, CallerContext caller)>;

  SessionManager(SessionManagerServices services, const EngineConfig& config,
                 SessionEventsFactory make_events);
  ~SessionManager();

  SessionManager(const SessionManager&) = delete;
  SessionManager& operator=(const SessionManager&) = delete;

  // GM-3: validates |tmpl|'s skeleton against the current operator set once.
  [[nodiscard]] Status PrevalidateTemplate(GraphTemplate& tmpl);

  // Full admission: validate (unless |prevalidated| is supplied), reserve
  // resources, build the topology, register. Never registers a session that
  // failed admission.
  [[nodiscard]] Result<Session*> Create(const GraphSpec& spec, CallerContext caller, OperationId* out_operation,
                                        std::shared_ptr<const ValidatedGraph> prevalidated);
  // GM-3 instance: skips validation/negotiation unless the operator set
  // moved since the template's last Prevalidate.
  [[nodiscard]] Result<Session*> CreateFromTemplate(const GraphTemplate& tmpl, const JsonValue& arguments,
                                                   std::string_view instance_name, CallerContext caller,
                                                   OperationId* out_operation);

  // Borrowed pointer for hosts with their own lifetime discipline; FindShared
  // for callers that must survive a concurrent Destroy.
  [[nodiscard]] Session* Find(SessionId id) const;
  [[nodiscard]] std::shared_ptr<Session> FindShared(SessionId id) const;

  // Removes the session from the map, stops it (blocking until closed) and
  // waits for the last reference to drop, so ~Session has released its plugin
  // leases by the time this returns (the caller's unload pass depends on it).
  [[nodiscard]] Status Destroy(SessionId id);

  void StopAll(bool fast);
  [[nodiscard]] std::vector<SessionId> Ids() const;
  [[nodiscard]] std::vector<std::shared_ptr<Session>> Refs() const;
  // Ids and references from one pass under |mutex_|, so a cross-session sweep
  // (UpgradeOperator) sees a coherent set instead of racing a destroy between
  // two separate calls.
  [[nodiscard]] std::vector<std::pair<SessionId, std::shared_ptr<Session>>> RefsWithIds() const;

  // One watchdog pass: Tick every live session outside |mutex_|.
  void TickAll();
  // Shutdown only: drops the map without stopping (Engine stops first,
  // 13 §7.3: sessions before shared services).
  void Clear();

 private:
  [[nodiscard]] Result<Session*> CreateLocked(const GraphSpec& spec, CallerContext caller,
                                              OperationId* out_operation,
                                              std::shared_ptr<const ValidatedGraph> prevalidated);

  SessionManagerServices services_;
  const EngineConfig& config_;
  SessionEventsFactory make_events_;

  mutable std::mutex mutex_;
  std::map<SessionId, std::shared_ptr<Session>> sessions_;
  SessionId next_session_id_ = 1;
};

}  // namespace ge

#endif  // GE_SRC_SESSION_MANAGER_H_
