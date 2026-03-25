#include "bthread/global_queue.h"

namespace bthread {

void GlobalQueue::Push(TaskMeta* task) {
    task->next = nullptr;

    TaskMeta* old_head = head_.load(std::memory_order_relaxed);
    do {
        task->next = old_head;
    } while (!head_.compare_exchange_weak(old_head, task,
            std::memory_order_release, std::memory_order_relaxed));

    version_.fetch_add(1, std::memory_order_release);
}

TaskMeta* GlobalQueue::Pop() {
    // Take entire list atomically
    TaskMeta* head = head_.exchange(nullptr, std::memory_order_acq_rel);
    if (!head) return nullptr;

    // Reverse list for FIFO order
    TaskMeta* result = nullptr;
    TaskMeta* next = nullptr;
    while (head) {
        next = head->next;
        head->next = result;
        result = head;
        head = next;
    }

    version_.fetch_add(1, std::memory_order_release);
    return result;
}

} // namespace bthread