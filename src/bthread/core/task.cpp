// src/bthread/core/task.cpp
#include "bthread/core/task.hpp"
#include "bthread/core/scheduler.hpp"
#include "bthread/core/task_meta.hpp"
#include "bthread/core/task_group.hpp"
#include "bthread/sync/butex.hpp"
#include "coro/meta.h"

namespace bthread {

void Task::join() {
    if (!meta_) {
        return;  // Nothing to join
    }

    if (meta_->type == TaskType::BTHREAD) {
        TaskMeta* bthread_meta = static_cast<TaskMeta*>(meta_);

        // Check if already finished
        if (bthread_meta->state.load(std::memory_order_acquire) == TaskState::FINISHED) {
            if (bthread_meta->Release()) {
                GetTaskGroup().DeallocTaskMeta(bthread_meta);
            }
            meta_ = nullptr;
            return;
        }

        // Increment join waiters
        bthread_meta->join_waiters.fetch_add(1, std::memory_order_acq_rel);

        // Wait on join butex
        Butex* join_butex = static_cast<Butex*>(bthread_meta->join_butex);
        if (join_butex) {
            int generation = join_butex->value();
            join_butex->Wait(generation, nullptr);
        }

        bthread_meta->join_waiters.fetch_sub(1, std::memory_order_acq_rel);

        // Release reference
        if (bthread_meta->Release()) {
            GetTaskGroup().DeallocTaskMeta(bthread_meta);
        }
    } else if (meta_->type == TaskType::COROUTINE) {
        coro::CoroutineMeta* coro_meta = static_cast<coro::CoroutineMeta*>(meta_);

        // Wait for coroutine to complete
        while (!is_done()) {
            std::this_thread::yield();
        }

        // Clean up coroutine meta
        delete coro_meta;
    }

    meta_ = nullptr;
}

void Task::detach() {
    if (!meta_) {
        return;
    }

    if (meta_->type == TaskType::BTHREAD) {
        TaskMeta* bthread_meta = static_cast<TaskMeta*>(meta_);

        // Decrement ref count (was 2 for joinable, now 1)
        if (bthread_meta->ref_count.fetch_sub(1, std::memory_order_acq_rel) == 1) {
            // Already finished, clean up
            GetTaskGroup().DeallocTaskMeta(bthread_meta);
        }
    } else if (meta_->type == TaskType::COROUTINE) {
        // Coroutine will clean itself up when done
        // The scheduler owns the coroutine now
    }

    meta_ = nullptr;
}

} // namespace bthread