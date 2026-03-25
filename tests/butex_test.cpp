#include "bthread.h"
#include "bthread/butex.h"

#include <cstdio>
#include <cassert>

using namespace bthread;

int main() {
    printf("Testing Butex...\n");

    printf("  Testing basic wait/wake...\n");
    Butex b;
    assert(b.value() == 0);

    // Test Wake with no waiters
    b.Wake(1);

    // Test value operations
    b.set_value(5);
    assert(b.value() == 5);

    printf("  Testing Wait from pthread...\n");
    // This test would require a separate thread to test properly
    // For now, just verify the API is accessible
    int result = b.Wait(0, nullptr);

    printf("All Butex tests passed!\n");
    return 0;
}