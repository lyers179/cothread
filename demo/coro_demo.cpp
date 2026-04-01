// demo/coro_demo.cpp
#include "coro/coroutine.h"
#include "coro/scheduler.h"
#include "bthread/sync/mutex.hpp"
#include "coro/cancel.h"

#include <iostream>
#include <atomic>
#include <thread>

coro::Task<void> demo_task(int id, bthread::Mutex& m, std::atomic<int>& counter) {
    std::cerr << "Task " << id << " starting\n";

    co_await m.lock_async();
    counter++;
    std::cerr << "Task " << id << " incremented counter to " << counter << "\n";
    m.unlock();

    co_await coro::yield();

    std::cerr << "Task " << id << " done\n";
}

int main() {
    setvbuf(stderr, nullptr, _IONBF, 0);

    bthread::Scheduler::Instance().Init();

    bthread::Mutex mutex;
    std::atomic<int> counter{0};

    for (int i = 0; i < 5; ++i) {
        coro::co_spawn(demo_task(i, mutex, counter));
    }

    // Wait
    std::this_thread::sleep_for(std::chrono::seconds(2));

    std::cerr << "Final counter: " << counter << "\n";

    bthread::Scheduler::Instance().Shutdown();

    return 0;
}