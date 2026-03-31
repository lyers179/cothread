#include "bthread.h"
#include "bthread/global_queue.h"
#include <cstdio>

using namespace bthread;

int main() {
    printf("Testing GlobalQueue...\n");

    GlobalQueue q;

    TaskMeta t1, t2, t3;
    t1.slot_index = 1;
    t2.slot_index = 2;
    t3.slot_index = 3;
    t1.next = nullptr;
    t2.next = nullptr;
    t3.next = nullptr;

    printf("Push t1\n");
    q.Push(&t1);
    printf("  head=%p, empty=%d\n", (void*)&t1, q.Empty());

    printf("Push t2\n");
    q.Push(&t2);
    printf("  empty=%d\n", q.Empty());

    printf("Push t3\n");
    q.Push(&t3);
    printf("  empty=%d\n", q.Empty());

    printf("Pop\n");
    TaskMeta* head = q.Pop();
    printf("  got %p (expected t3=%p)\n", (void*)head, (void*)&t3);
    printf("  empty=%d\n", q.Empty());

    printf("Pop again\n");
    head = q.Pop();
    printf("  got %p (expected t2=%p)\n", (void*)head, (void*)&t2);
    printf("  empty=%d\n", q.Empty());

    printf("Pop again\n");
    head = q.Pop();
    printf("  got %p (expected t1=%p)\n", (void*)head, (void*)&t1);
    printf("  empty=%d\n", q.Empty());

    printf("Done\n");
    return 0;
}
