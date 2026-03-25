#include "bthread.h"
#include "bthread/global_queue.h"

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

    // Test Pop (FIFO order)
    TaskMeta* head = q.Pop();
    assert(head == &t1);  // First pushed
    assert(head->next == &t2);
    assert(head->next->next == &t3);

    printf("  Testing empty after pop...\n");
    assert(q.Empty());

    head = q.Pop();
    assert(head == nullptr);

    printf("All GlobalQueue tests passed!\n");
    return 0;
}