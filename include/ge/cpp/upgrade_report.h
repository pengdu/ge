#ifndef GE_CPP_UPGRADE_REPORT_H_
#define GE_CPP_UPGRADE_REPORT_H_

#include <string>
#include <vector>

#include <ge/cpp/json.h>
#include <ge/cpp/types.h>

namespace ge {

// Plugin retire/upgrade result contracts (12 §9), split from engine.h so the
// lifecycle service does not depend on the Engine class itself.

struct SessionUpgradeResult {
  SessionId session = 0;
  std::vector<std::string> nodes;  // replaced node ids
  OperationId operation = 0;       // the ReplaceNode mutation
  Status result;
  [[nodiscard]] JsonValue ToJson() const;
};

struct UpgradeReport {
  OperationId operation = 0;
  PluginId old_plugin = 0;
  PluginId new_plugin = 0;
  std::vector<SessionUpgradeResult> sessions;
  bool unloaded = false;  // old plugin logically unloaded (all succeeded, refs == 0)
  [[nodiscard]] bool all_succeeded() const noexcept;
  [[nodiscard]] JsonValue ToJson() const;
};

struct RetirePluginOptions {
  bool request_physical_unload = false;
};

}  // namespace ge

#endif  // GE_CPP_UPGRADE_REPORT_H_
