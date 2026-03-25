#include "bthread/platform/platform.h"

namespace bthread {
namespace platform {

// Context make is implemented in assembly
// Stack overflow handler is platform-specific
// Thread management is platform-specific
// Futex operations are platform-specific
// Stack allocation is platform-specific
// Time utilities are platform-specific

// MakeContext and SwapContext are implemented in assembly files
// and have no C++ implementation here

} // namespace platform
} // namespace bthread