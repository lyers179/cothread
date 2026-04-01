# Coroutine Pool TODO List

## Fixed Issues (2026-04-01)

### 1. CoroutineQueue Thread Safety
**Status:** FIXED
**Fix:** Added `std::mutex waiters_mutex_` to both `CoMutex` and `CoCond` to protect the waiters queue. The CoroutineQueue is MPSC (Multi-Producer Single-Consumer), but multiple worker threads can access it concurrently. The mutex ensures MPMC safety.

### 2. Scheduler Shutdown Race Condition
**Status:** FIXED
**Fix:** Consolidated shutdown logic in `CoroutineScheduler::Shutdown()`. The method now properly:
- Guards against multiple shutdown calls
- Signals workers to stop
- Shuts down the sleep thread
- Joins all worker threads
- Clears the workers vector

### 3. Nested Coroutine Support
**Status:** FIXED
**Fix:** Implemented custom `FinalAwaiter` for TaskPromise and SafeTaskPromise that:
- Resumes the awaiter in `await_suspend()` after the coroutine reaches final_suspend
- Properly swaps the CoroutineMeta context for nested coroutines
- Returns `std::noop_coroutine()` if there's no awaiter

The previous implementation resumed the awaiter in `return_value()`, but at that point the coroutine hadn't reached `final_suspend` yet, causing `handle.done()` to return false and throwing "Task not completed" error.

### 4. Detached Coroutine Memory Management
**Status:** FIXED
**Fix:** Added `release()` method to Task and SafeTask that sets the handle to nullptr without destroying it. Updated `co_spawn_detached()` to call `release()` on the spawned task to prevent the destructor from destroying the coroutine handle.

### 5. CoCond Mutex Re-acquisition
**Status:** DOCUMENTED
**Note:** The spin-yield loop in `await_resume()` is acceptable for the current design because:
- Condition variables are typically used with short-held mutexes
- The scheduler ensures fair scheduling
- `std::this_thread::yield()` prevents busy-waiting
- The signaling coroutine typically releases the mutex immediately after signal()

## Remaining Work

### Stress Test Improvements
Some tests may still occasionally hang under heavy load due to:
- Complex interactions between multiple synchronization primitives
- Race conditions in concurrent test scenarios

### Future Improvements
- Consider implementing two-phase suspension for CoCond to avoid spin-yield
- Add timeout mechanisms to prevent infinite waits
- Improve error handling and recovery

---
*Updated: 2026-04-01*