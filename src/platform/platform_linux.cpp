#include "bthread/platform/platform_linux.h"
#include "bthread/platform/platform.h"

#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <time.h>
#include <signal.h>
#include <cstring>
#include <cstdio>
#include <cstdlib>

namespace bthread {
namespace platform {

// SYS_futex is defined in <sys/syscall.h> on Linux, no need to redefine

// Stack overflow handling
static void* g_stack_bottom = nullptr;
static size_t g_stack_size = 0;

// Signal handler for stack overflow
static void StackOverflowHandler(int sig, siginfo_t* info, void* ctx) {
    (void)ctx;

    if (sig == SIGSEGV && g_stack_bottom != nullptr) {
        uintptr_t addr = reinterpret_cast<uintptr_t>(info->si_addr);
        uintptr_t stack_bottom = reinterpret_cast<uintptr_t>(g_stack_bottom);

        // Check if fault is in guard page
        if (addr >= stack_bottom && addr < stack_bottom + PAGE_SIZE) {
            // Stack overflow detected
            fprintf(stderr, "Fatal: Stack overflow detected at %p\n", info->si_addr);
            _exit(1);
        }
    }

    // Not our fault, chain to default handler
    signal(SIGSEGV, SIG_DFL);
    raise(SIGSEGV);
}

void SetupStackOverflowHandler() {
    struct sigaction sa;
    std::memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = StackOverflowHandler;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_RESTART;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, nullptr);
}

// Stack allocation
void* AllocateStack(size_t size) {
    size_t total = size + PAGE_SIZE;

    void* ptr = mmap(nullptr, total, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (ptr == MAP_FAILED) {
        return nullptr;
    }

    // Guard page at lowest address
    mprotect(ptr, PAGE_SIZE, PROT_NONE);

    // Stack top is at highest address, 16-byte aligned
    void* stack_top = static_cast<char*>(ptr) + total;
    stack_top = reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(stack_top) & ~0xF);

    return stack_top;
}

void DeallocateStack(void* stack_top, size_t size) {
    if (!stack_top) return;

    size_t total = size + PAGE_SIZE;
    void* ptr = static_cast<char*>(stack_top) - total;
    munmap(ptr, total);
}

// Thread management
struct ThreadStartData {
    ThreadFunc fn;
    void* arg;
};

static void* ThreadWrapper(void* arg) {
    auto* data = static_cast<ThreadStartData*>(arg);
    data->fn(data->arg);
    delete data;
    return nullptr;
}

ThreadId CreateThread(ThreadFunc fn, void* arg) {
    pthread_t thread;
    auto* data = new ThreadStartData{fn, arg};

    if (pthread_create(&thread, nullptr, ThreadWrapper, data) != 0) {
        delete data;
        return nullptr;
    }

    return reinterpret_cast<ThreadId>(thread);
}

void JoinThread(ThreadId thread) {
    pthread_join(reinterpret_cast<pthread_t>(thread), nullptr);
}

// Futex operations
int FutexWait(std::atomic<int>* addr, int expected, const timespec* timeout) {
    int ret = syscall(SYS_futex, reinterpret_cast<int*>(addr),
                      FUTEX_WAIT_PRIVATE, expected, timeout, nullptr, 0);

    if (ret == -1) {
        int err = errno;
        if (err == EAGAIN || err == EINTR) return 0;
        if (err == ETIMEDOUT) return ETIMEDOUT;
        return err;
    }
    return 0;
}

int FutexWake(std::atomic<int>* addr, int count) {
    syscall(SYS_futex, reinterpret_cast<int*>(addr), FUTEX_WAKE_PRIVATE,
            count, nullptr, nullptr, 0);
    return 0;
}

// Time utilities
int64_t GetTimeOfDayUs() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<int64_t>(ts.tv_sec) * 1000000 + ts.tv_nsec / 1000;
}

} // namespace platform
} // namespace bthread