#include "bthread/task_meta.h"
#include "../include/bthread.h"

namespace bthread {

void detail::BthreadEntry(void* arg) {
    TaskMeta* task = static_cast<TaskMeta*>(arg);
    task->result = task->fn(task->arg);
    bthread_exit(task->result);
}

} // namespace bthread