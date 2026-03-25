#include "bthread/once.h"

#ifdef __cplusplus
#include <mutex>

namespace bthread {

} // namespace bthread

extern "C" {

#endif

int bthread_once(bthread_once_t* once, void (*init_routine)(void)) {
#ifdef __cplusplus
    static std::once_flag flag;
    std::call_once(flag, init_routine);
    (void)once;
    return 0;
#else
    // C implementation would use pthread_once or atomic flag
    (void)once;
    (void)init_routine;
    return 0;
#endif
}

#ifdef __cplusplus
}
#endif