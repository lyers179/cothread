#include "bthread/core/task_meta.hpp"
#include "bthread/platform/platform.h"

#include <cstdio>
#include <cstdint>

// Simple stack
alignas(16) static char simple_stack[4096];

static bool returned = false;
static bthread::platform::Context main_ctx;
static bthread::platform::Context fiber_ctx;

void simple_func(void* arg) {
    int val = (int)(intptr_t)arg;
    printf("    In fiber: received %d\n", val);
    returned = true;
    // Switch back
    printf("    Switching back...\n");
    bthread::platform::SwapContext(&fiber_ctx, &main_ctx);
    printf("    ERROR: Should not reach here!\n");
}

int main() {
    printf("=== Basic Context Switch Test ===\n");

    printf("1. Creating context...\n");
    bthread::platform::MakeContext(&fiber_ctx, simple_stack + sizeof(simple_stack),
                                    sizeof(simple_stack), simple_func, (void*)42);

    printf("   stack_ptr = %p\n", fiber_ctx.stack_ptr);
    printf("   return_addr = %p\n", fiber_ctx.return_addr);
    printf("   gp_regs[8] = %p\n", (void*)fiber_ctx.gp_regs[8]);
    printf("   gp_regs[9] = %p\n", (void*)fiber_ctx.gp_regs[9]);

    printf("2. Switching to fiber...\n");
    bthread::platform::SwapContext(&main_ctx, &fiber_ctx);

    printf("3. Returned from fiber\n");
    if (returned) {
        printf("   SUCCESS!\n");
        return 0;
    } else {
        printf("   FAILED: Did not execute fiber\n");
        return 1;
    }
}