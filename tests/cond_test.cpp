#include "bthread/sync/cond.hpp"
#include "bthread/sync/mutex.hpp"

#include <cstdio>
#include <cassert>
#include <thread>

int main() {
    printf("Testing Condition Variable...\n");

    printf("  Testing signal/broadcast...\n");
    bthread::CondVar c;

    c.notify_one();
    c.notify_all();

    printf("  Testing wait/signal...\n");
    bthread::Mutex m;
    bthread::CondVar c2;
    bool ready = false;

    std::thread waiter([&]() {
        m.lock();
        while (!ready) {
            c2.wait(m);
        }
        m.unlock();
    });

    std::thread signaler([&]() {
        m.lock();
        ready = true;
        c2.notify_one();
        m.unlock();
    });

    signaler.join();
    waiter.join();

    printf("All Cond tests passed!\n");
    return 0;
}