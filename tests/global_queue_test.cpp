#include "bthread.h"
#include "bthread/queue/global_queue.hpp"
#include "bthread/core/task_meta.hpp"

#include <cstdio>
#include <cassert>

using namespace bthread;

int main() {
    printf("Testing GlobalQueue...\n");

    printf("  Testing basic operations...\n");
    GlobalQueue q;

    TaskMeta t1, t2, t3;
    t1.slot_index = 1;
    t2.slot_index = 2;
    t3.slot_index = 3;

    // Test Push
    q.Push(&t1);
    q.Push(&t2);
    q.Push(&t3);

    // Test Pop (FIFO order - pops from tail)
    TaskMetaBase* head = q.Pop();
    assert(head != nullptr);
    // FIFO: t1 was pushed first, so it's popped first
    assert(head == &t1);
    assert(head->next == nullptr);  // Pop returns single task

    // Pop remaining tasks
    head = q.Pop();
    assert(head == &t2);
    head = q.Pop();
    assert(head == &t3);

    printf("  Testing empty after all pops...\n");
    assert(q.Empty());

    head = q.Pop();
    assert(head == nullptr);

    printf("All GlobalQueue tests passed!\n");
    return 0;
}