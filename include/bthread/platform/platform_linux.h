#pragma once

#include "platform.h"

namespace bthread {
namespace platform {

// Linux-specific constants
constexpr int FUTEX_WAIT_PRIVATE = 0;
constexpr int FUTEX_WAKE_PRIVATE = 1;

// ETIMEDOUT is already defined in <errno.h> on Linux, no need to redefine

} // namespace platform
} // namespace bthread