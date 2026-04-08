/**
 * @file yield_demo.cpp
 * @brief 演示同步原语如何让工作线程切换执行其他 bthread
 *
 * 核心概念：
 * - 当 bthread 等待同步原语时，工作线程会切换执行其他 bthread
 * - 这就是 M:N 线程模型的核心优势
 *
 * 工作原理：
 * 1. bthread 等待 Mutex 时：
 *    - 当前 bthread 状态改为 SUSPENDED
 *    - 工作线程保存当前 bthread 上下文
 *    - 工作线程从队列取下一个就绪 bthread 执行
 *    - 当 Mutex 可用时，等待的 bthread 被 Wake() 唤醒
 *    - 被唤醒的 bthread 重新加入就绪队列
 */

#include "bthread.h"
#include "bthread/sync/mutex.hpp"

#include <cstdio>
#include <atomic>

// ==================== 示例 1: Mutex 等待时切换 ====================

static bthread::Mutex g_mutex;

void* mutex_wait_task(void* arg) {
    int id = *static_cast<int*>(arg);

    printf("[Task %d] 尝试获取锁...\n", id);

    // 当锁被其他任务持有时，这里会：
    // 1. 将当前 bthread 加入 Butex 等待队列
    // 2. 保存当前 bthread 的上下文 (寄存器、栈指针等)
    // 3. 工作线程切换去执行其他就绪的 bthread
    // 4. 当锁可用时，Butex::Wake() 唤醒等待者
    // 5. 被唤醒的 bthread 重新加入调度队列
    g_mutex.lock();

    printf("[Task %d] 获得锁，执行临界区\n", id);

    // 模拟工作
    for (volatile int i = 0; i < 500000; ++i) {}

    g_mutex.unlock();

    printf("[Task %d] 释放锁\n", id);

    return nullptr;
}

void demo_mutex_switch() {
    printf("\n=== 示例: Mutex 等待时工作线程切换 ===\n");
    printf("说明: 只有 1 个工作线程，但多个 bthread 可以\"并发\"执行\n");
    printf("      当一个 bthread 等待锁时，工作线程切换执行其他 bthread\n\n");

    // 设置只有 1 个工作线程，更清楚地展示切换行为
    bthread_set_worker_count(1);

    // 预先获取锁，让所有任务都等待
    g_mutex.lock();

    const int N = 4;
    bthread_t tids[N];
    int ids[N];

    // 创建多个 bthread
    for (int i = 0; i < N; ++i) {
        ids[i] = i + 1;
        bthread_create(&tids[i], nullptr, mutex_wait_task, &ids[i]);
    }

    printf("[Main] 所有任务已创建，都在等待锁...\n");

    // 让出几次，确保所有 bthread 都进入等待状态
    for (int i = 0; i < 3; ++i) {
        bthread_yield();
    }

    printf("[Main] 释放锁，让任务开始执行\n\n");
    g_mutex.unlock();

    // 等待所有任务完成
    for (int i = 0; i < N; ++i) {
        bthread_join(tids[i], nullptr);
    }

    printf("\n[Main] 所有任务完成\n");
    bthread_shutdown();
}

// ==================== 说明: pthread vs bthread 对比 ====================

void demo_pthread_block() {
    printf("\n=== 对比: pthread 阻塞 vs bthread 切换 ===\n\n");

    printf("pthread 行为:\n");
    printf("  - 当 pthread 在 mutex.lock() 阻塞时，整个 OS 线程停止\n");
    printf("  - 如果只有 1 个线程，系统完全停止\n");
    printf("  - 需要创建更多 OS 线程来保持并发\n");
    printf("  - 线程切换代价高 (内核态切换)\n\n");

    printf("bthread 行为:\n");
    printf("  - 当 bthread 在 mutex.lock() 等待时，只停止该 bthread\n");
    printf("  - 工作线程 (pthread) 切换执行其他就绪的 bthread\n");
    printf("  - 1 个工作线程可以服务多个 bthread\n");
    printf("  - 切换代价低 (用户态上下文切换)\n\n");

    printf("M:N 线程模型的优势:\n");
    printf("  - M 个 pthread 工作线程 (通常 = CPU 核心数)\n");
    printf("  - N 个 bthread 用户线程 (N 可以远大于 M)\n");
    printf("  - 阻塞操作不浪费工作线程资源\n");
    printf("  - 高效处理大量并发任务\n");
}

// ==================== Main ====================

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    setvbuf(stderr, nullptr, _IONBF, 0);

    printf("============================================\n");
    printf("   bthread 同步原语与线程切换演示\n");
    printf("============================================\n");

    demo_mutex_switch();
    demo_pthread_block();

    printf("\n============================================\n");
    printf("   演示完成\n");
    printf("============================================\n");

    return 0;
}