#include "bthread.h"
#include "bthread/work_stealing_queue.h"

#include <cstdio>
#include <cassert>
#include <thread>

using namespace bthread;

int main() {
    printf("Testing WorkStealingQueue...\n");

    printf("  Testing basic operations...\n");
    WorkStealingQueue q;

    TaskMeta t1, t2, t3;
    t1.slot_index = 1;
    t2.slot_index = 2;
    t3.slot_index = 3;

    // Test Push
    q.Push(&t1);
    q.Push(&t2);
    q.Push(&t3);

    // Test Pop (LIFO for owner)
    TaskMeta* popped = q.Pop();
    assert(popped == &t3);

    popped = q.Pop();
    assert(popped == &t2);

    popped = q.Pop();
    assert(popped == &t1);

    popped = q.Pop();
    assert(popped == nullptr);

    printf("  Testing empty check...\n");
    assert(q.Empty());

    printf("  Testing concurrent operations...\n");
    q.Push(&t1);

    std::thread stealer([&]() {
        TaskMeta* stolen = q.Steal();
        assert(stolen == &t1);
    });

    stealer.join();
    assert(q.Empty());

    printf("All WorkStealingQueue tests passed!\n");
    return 0;
}