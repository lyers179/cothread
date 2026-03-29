#pragma once

#include "platform.h"

#include <synchapi.h>

namespace bthread {
namespace platform {

// Windows-specific constants
constexpr size_t STACK_GUARD_PAGES = 2;

} // namespace platform
} // namespace bthread