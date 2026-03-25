#pragma once

#include "../platform.h"

namespace bthread {
namespace platform {

// Linux-specific constants
constexpr int FUTEX_WAIT_PRIVATE = 0;
constexpr int FUTEX_WAKE_PRIVATE = 1;

// Error codes (from errno.h, defined here for consistency)
constexpr int ETIMEDOUT = 110;

} // namespace platform
} // namespace bthread