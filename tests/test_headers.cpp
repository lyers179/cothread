// tests/test_headers.cpp
// Phase 1 verification: Test that new unified headers compile correctly

#include "bthread/core/task_meta_base.hpp"
#include "bthread/core/task_meta.hpp"
#include "coro/meta.h"
#include "bthread/queue/global_queue.hpp"
#include <iostream>

int main() {
    // Test TaskMetaBase
    bthread::TaskMeta meta;
    meta.type = bthread::TaskType::BTHREAD;
    meta.state.store(bthread::TaskState::READY, std::memory_order_release);

    // Test CoroutineMeta
    coro::CoroutineMeta coro_meta;
    coro_meta.type = bthread::TaskType::COROUTINE;
    coro_meta.state.store(bthread::TaskState::READY, std::memory_order_release);

    // Test GlobalQueue with TaskMetaBase
    bthread::GlobalQueue queue;
    queue.Push(&meta);
    queue.Push(&coro_meta);

    bthread::TaskMetaBase* task = queue.Pop();
    if (task) {
        std::cout << "Popped task type: "
                  << (task->type == bthread::TaskType::BTHREAD ? "BTHREAD" : "COROUTINE")
                  << std::endl;
    }

    task = queue.Pop();
    if (task) {
        std::cout << "Popped task type: "
                  << (task->type == bthread::TaskType::BTHREAD ? "BTHREAD" : "COROUTINE")
                  << std::endl;
    }

    std::cout << "Phase 1 header test passed!" << std::endl;
    return 0;
}