# bthread - Modern M:N Threading Library

A high-performance M:N threading library that supports both traditional bthread (assembly-based context switching) and C++20 coroutines.

## Features

- **Unified Task Abstraction**: Both bthread and coroutine tasks share a common interface
- **M:N Threading Model**: Multiplexes M user-space tasks onto N OS threads
- **Work Stealing**: Lock-free work stealing for load balancing
- **C++20 Coroutines**: Native coroutine support with `Task<T>` and `SafeTask<T>`
- **Unified Synchronization**: Mutex, CondVar, and Event work for both execution models

## Quick Start

```cpp
#include <bthread.hpp>

int main() {
    // Initialize with 4 worker threads
    bthread::set_worker_count(4);
    bthread::init();

    // Spawn a bthread
    auto task = bthread::spawn([]{
        std::cout << "Hello from bthread!" << std::endl;
        return 42;
    });

    // Wait for completion
    task.join();

    // Cleanup
    bthread::shutdown();
    return 0;
}
```

## Modern C++ API

### Spawning Tasks

```cpp
// Spawn a bthread (assembly context switching)
auto t1 = bthread::spawn([]{ return 42; });

// Explicitly spawn bthread
auto t2 = bthread::spawn_bthread([]{ return 42; });

// Spawn coroutine
auto t3 = bthread::spawn_coro([]() -> coro::Task<int> {
    co_await coro::sleep(std::chrono::milliseconds(100));
    co_return 42;
});

// Fire and forget
bthread::spawn_detached([]{ /* background work */ });
```

### Task Operations

```cpp
bthread::Task task = bthread::spawn([]{ return 42; });

// Check state
if (task.valid()) {
    task.join();   // Wait for completion
    // or
    task.detach(); // Let it run independently
}
```

### Synchronization

```cpp
bthread::Mutex mutex;

// From bthread/pthread
mutex.lock();
// ... critical section ...
mutex.unlock();

// From coroutine
co_await mutex.lock_async();
// ... critical section ...
mutex.unlock();
```

### Condition Variables

```cpp
bthread::Mutex mutex;
bthread::CondVar cond;
bool ready = false;

// Waiter (coroutine)
co_await mutex.lock_async();
while (!ready) {
    co_await cond.wait_async(mutex);
}
mutex.unlock();

// Signaler
mutex.lock();
ready = true;
cond.notify_all();
mutex.unlock();
```

### Events

```cpp
bthread::Event event(false);  // Initially not set

// Waiter (coroutine)
co_await event.wait_async();

// Signaler
event.set();  // Wake all waiters
```

## Legacy C API

The original C API is still supported for backward compatibility:

```c
#include <bthread.h>

void* my_thread(void* arg) {
    // ...
    return NULL;
}

int main() {
    bthread_t tid;
    bthread_create(&tid, NULL, my_thread, NULL);
    bthread_join(tid, NULL);
    return 0;
}
```

## Architecture

```
┌─────────────────────────────────────────────────────────────┐
│                      User Application                        │
├─────────────────────────────────────────────────────────────┤
│  Modern C++ API (spawn, Task)  │  Legacy C API (bthread_*)  │
├─────────────────────────────────────────────────────────────┤
│          Unified Scheduler (bthread::Scheduler)             │
├───────────────────────────┬─────────────────────────────────┤
│   Bthread (TaskMeta)      │    Coroutine (CoroutineMeta)    │
│   Assembly Context        │    C++20 Compiler Context       │
├───────────────────────────┴─────────────────────────────────┤
│              Worker Threads (Work Stealing)                 │
├─────────────────────────────────────────────────────────────┤
│              Platform Abstraction Layer                      │
│              (futex, stack, context switching)              │
└─────────────────────────────────────────────────────────────┘
```

## Building

### Requirements

- C++20 compiler (MSVC 2022, GCC 11+, or Clang 13+)
- CMake 3.15+

### Windows (MSVC)

```bash
mkdir build && cd build
cmake -G "Visual Studio 17 2022" ..
cmake --build . --config Release
```

### Windows (MinGW)

```bash
mkdir build && cd build
cmake -G "MinGW Makefiles" ..
cmake --build .
```

### Linux

```bash
mkdir build && cd build
cmake ..
make -j$(nproc)
```

## License

MIT License