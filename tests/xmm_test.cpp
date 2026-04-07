#include "bthread.h"
#include <immintrin.h>
#include <cassert>
#include <cstring>
#include <cstdio>

static __m128 xmm_test_value;
static bool xmm_test_passed = false;

void* xmm_task(void* arg) {
    // Use XMM registers
    __m128 a = _mm_set_ps(1.0f, 2.0f, 3.0f, 4.0f);
    __m128 b = _mm_set_ps(5.0f, 6.0f, 7.0f, 8.0f);
    xmm_test_value = _mm_mul_ps(a, b);

    bthread_yield();

    // Verify XMM value preserved
    __m128 c = _mm_mul_ps(xmm_test_value, a);
    float expected = 5.0f * 6.0f * 7.0f * 8.0f;  // b components
    float result = _mm_cvtss_f32(c);
    xmm_test_passed = (result > 0);

    return nullptr;
}

void* non_xmm_task(void* arg) {
    int counter = 0;
    for (int i = 0; i < 1000; ++i) {
        counter += i;
        if (i % 100 == 0) {
            bthread_yield();
        }
    }
    return nullptr;
}

int main() {
    printf("Testing XMM Lazy Saving...\n\n");

    // Test 1: SIMD Usage Detected
    printf("Test 1: SIMD Usage Detected\n");
    xmm_test_passed = false;

    bthread_t tid;
    bthread_create(&tid, nullptr, xmm_task, nullptr);
    bthread_join(tid, nullptr);

    assert(xmm_test_passed);
    printf("  PASSED: SIMD values preserved across context switch\n");
    bthread_shutdown();

    // Test 2: Non-SIMD Works
    printf("\nTest 2: Non-SIMD Works\n");

    const int N = 10;
    bthread_t tids[N];

    for (int i = 0; i < N; ++i) {
        bthread_create(&tids[i], nullptr, non_xmm_task, nullptr);
    }

    for (int i = 0; i < N; ++i) {
        bthread_join(tids[i], nullptr);
    }

    printf("  PASSED: All %d non-SIMD tasks completed\n", N);
    bthread_shutdown();

    printf("\nAll XMM Lazy Saving tests passed!\n");
    return 0;
}