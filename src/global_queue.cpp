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
    // Pop single task atomically
    TaskMeta* head = head_.load(std::memory_order_acquire);
    while (head) {
        TaskMeta* next = head->next;
        if (head_.compare_exchange_weak(head, next,
                std::memory_order_acq_rel, std::memory_order_acquire)) {
            head->next = nullptr;
            version_.fetch_add(1, std::memory_order_release);
            return head;
        }
    }
    return nullptr;
}

} // namespace bthread