#include "bthread/sync/mutex.hpp"
#include "bthread/platform/platform.h"

#include <cstdio>
#include <cassert>
#include <thread>

int main() {
    printf("Testing Mutex...\n");

    printf("  Testing lock/unlock...\n");
    bthread::Mutex m;

    m.lock();
    m.unlock();

    printf("  Testing trylock...\n");
    m.lock();

    bool result = m.try_lock();
    assert(!result);  // Should fail, already locked

    m.unlock();

    result = m.try_lock();
    assert(result);  // Should succeed

    m.unlock();

    printf("  Testing concurrent access...\n");
    bthread::Mutex m2;

    int counter = 0;
    const int iterations = 1000;

    std::thread t1([&]() {
        for (int i = 0; i < iterations; ++i) {
            m2.lock();
            counter++;
            m2.unlock();
        }
    });

    std::thread t2([&]() {
        for (int i = 0; i < iterations; ++i) {
            m2.lock();
            counter++;
            m2.unlock();
        }
    });

    t1.join();
    t2.join();

    assert(counter == iterations * 2);

    printf("All Mutex tests passed!\n");
    return 0;
}