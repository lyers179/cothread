#include "bthread.h"
#include "bthread/task_group.h"
#include "bthread/task_meta.h"

#include <cstdio>
#include <cassert>

using namespace bthread;

int main() {
    printf("Testing TaskGroup...\n");

    TaskGroup& tg = GetTaskGroup();

    // Test allocation
    printf("  Testing allocation...\n");
    TaskMeta* meta1 = tg.AllocTaskMeta();
    assert(meta1 != nullptr);
    assert(meta1->slot_index < TaskGroup::POOL_SIZE);

    TaskMeta* meta2 = tg.AllocTaskMeta();
    assert(meta2 != nullptr);
    assert(meta2->slot_index != meta1->slot_index);

    // Test ID encoding/decoding
    printf("  Testing ID encoding/decoding...\n");
    bthread_t tid1 = tg.EncodeId(meta1->slot_index, meta1->generation);
    TaskMeta* decoded1 = tg.DecodeId(tid1);
    assert(decoded1 == meta1);

    bthread_t tid2 = tg.EncodeId(meta2->slot_index, meta2->generation);
    TaskMeta* decoded2 = tg.DecodeId(tid2);
    assert(decoded2 == meta2);

    // Test pool reuse
    printf("  Testing pool reuse...\n");
    uint32_t old_gen = meta1->generation;  // Save generation before dealloc
    tg.DeallocTaskMeta(meta1);
    TaskMeta* stale_decoded = tg.DecodeId(tid1);
    assert(stale_decoded == nullptr);  // Should return nullptr for stale ID

    TaskMeta* meta3 = tg.AllocTaskMeta();
    assert(meta3 != nullptr);
    // Generation should have incremented (old_gen was before dealloc, meta3 gets new gen)
    assert(meta3->generation == old_gen + 1);  // Generation increments after dealloc

    // Test GetTaskBySlot
    printf("  Testing GetTaskBySlot...\n");
    TaskMeta* by_slot = tg.GetTaskBySlot(meta3->slot_index);
    assert(by_slot == meta3);

    // Test worker count
    printf("  Testing worker count...\n");
    tg.set_worker_count(4);
    assert(tg.worker_count() == 4);

    // Clean up
    tg.DeallocTaskMeta(meta2);
    tg.DeallocTaskMeta(meta3);

    printf("All TaskGroup tests passed!\n");
    return 0;
}