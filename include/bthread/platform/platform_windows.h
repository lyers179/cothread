#pragma once

#include "../platform.h"

#include <synchapi.h>

namespace bthread {
namespace platform {

// Windows-specific constants
constexpr size_t STACK_GUARD_PAGES = 2;

// Error codes (Windows equivalents)
constexpr int ETIMEDOUT = 110;
constexpr int EINTR = 4;
constexpr int EAGAIN = 11;

} // namespace platform
} // namespace bthread