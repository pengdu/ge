#ifndef GE_SRC_CLOCK_H_
#define GE_SRC_CLOCK_H_

#include <chrono>
#include <cstdint>

namespace ge {

// Wall-clock ns since the Unix epoch (event/operation/audit timestamps).
inline std::int64_t WallClockNs() noexcept {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

}  // namespace ge

#endif
