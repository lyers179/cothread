#include "bthread/platform/platform_windows.h"
#include "bthread/platform/platform.h"

#include <windows.h>
#include <excpt.h>
#include <processthreadsapi.h>
#include <synchapi.h>

namespace bthread {
namespace platform {

// Stack overflow handling
LONG WINAPI StackOverflowHandler(EXCEPTION_POINTERS* info) {
    if (info->ExceptionRecord->ExceptionCode == EXCEPTION_STACK_OVERFLOW) {
        fprintf(stderr, "Fatal: Stack overflow detected\n");
        _Exit(1);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

void SetupStackOverflowHandler() {
    SetUnhandledExceptionFilter(StackOverflowHandler);
}

// Stack allocation
void* AllocateStack(size_t size) {
    size_t total = size + PAGE_SIZE * STACK_GUARD_PAGES;

    void* ptr = VirtualAlloc(nullptr, total, MEM_COMMIT | MEM_RESERVE,
                             PAGE_READWRITE);
    if (!ptr) {
        return nullptr;
    }

    // Guard pages at lowest address
    DWORD old;
    for (size_t i = 0; i < STACK_GUARD_PAGES; ++i) {
        VirtualProtect(static_cast<char*>(ptr) + i * PAGE_SIZE, PAGE_SIZE,
                       PAGE_NOACCESS, &old);
    }

    // Stack top is at highest address, 16-byte aligned
    void* stack_top = static_cast<char*>(ptr) + total;
    stack_top = reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(stack_top) & ~0xF);

    return stack_top;
}

void DeallocateStack(void* stack_top, size_t size) {
    if (!stack_top) return;

    size_t total = size + PAGE_SIZE * STACK_GUARD_PAGES;
    void* ptr = static_cast<char*>(stack_top) - total;
    VirtualFree(ptr, 0, MEM_RELEASE);
}

// Thread management
struct ThreadStartData {
    ThreadFunc fn;
    void* arg;
};

static DWORD WINAPI ThreadWrapper(LPVOID arg) {
    auto* data = static_cast<ThreadStartData*>(arg);
    data->fn(data->arg);
    delete data;
    return 0;
}

ThreadId CreateThread(ThreadFunc fn, void* arg) {
    auto* data = new ThreadStartData{fn, arg};

    HANDLE thread = CreateThread(nullptr, 0, ThreadWrapper, data, 0, nullptr);
    if (!thread) {
        delete data;
        return nullptr;
    }

    return reinterpret_cast<ThreadId>(thread);
}

void JoinThread(ThreadId thread) {
    WaitForSingleObject(reinterpret_cast<HANDLE>(thread), INFINITE);
    CloseHandle(reinterpret_cast<HANDLE>(thread));
}

// Futex operations using Windows WaitOnAddress
int FutexWait(std::atomic<int>* addr, int expected, const timespec* timeout) {
    DWORD ms = timeout ?
        static_cast<DWORD>(timeout->tv_sec * 1000 + timeout->tv_nsec / 1000000) : INFINITE;

    BOOL ok = WaitOnAddress(static_cast<volatile VOID*>(addr),
                            &expected, sizeof(int), ms);

    if (!ok) {
        DWORD err = GetLastError();
        return (err == ERROR_TIMEOUT) ? ETIMEDOUT : 0;
    }
    return 0;
}

int FutexWake(std::atomic<int>* addr, int count) {
    if (count == 1) {
        WakeByAddressSingle(static_cast<volatile VOID*>(addr));
    } else {
        WakeByAddressAll(static_cast<volatile VOID*>(addr));
    }
    return 0;
}

// Time utilities
int64_t GetTimeOfDayUs() {
    LARGE_INTEGER freq, counter;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&counter);

    int64_t sec = counter.QuadPart / freq.QuadPart;
    int64_t frac = (counter.QuadPart % freq.QuadPart) * 1000000 / freq.QuadPart;
    return sec * 1000000 + frac;
}

} // namespace platform
} // namespace bthread