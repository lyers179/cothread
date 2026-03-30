# Coroutine Pool TODO List

## Skipped Tests (Known Limitations)

### 1. test_comutex_contention
**Status:** SKIPPED
**Reason:** May cause hang in some cases
**Root Cause:** Heavy mutex contention with multiple coroutines can lead to scheduler deadlock
**Location:** `tests/coroutine_test.cpp`
**Potential Fix:** Review scheduler lock handoff logic or add timeout mechanism

### 2. test_cocond_concurrent_signal
**Status:** SKIPPED
**Reason:** Concurrent spawns from multiple threads can cause race conditions
**Root Cause:** Scheduler's concurrent spawn handling has race conditions when multiple OS threads spawn coroutines simultaneously
**Location:** `tests/coroutine_test.cpp`
**Potential Fix:** Add proper synchronization in `CoroutineScheduler::Spawn()` for multi-threaded access

## Shutdown Race Condition

### Segfault on Program Termination
**Status:** Intermittent
**Reason:** Race condition during scheduler shutdown causes segfault
**Impact:** Exit code 1 instead of 0, but all tests complete successfully
**Potential Fix:** Review `CoroutineScheduler::Shutdown()` and worker thread cleanup order

## Future Improvements

### Nested Coroutine Support
**Issue:** `co_await co_spawn(inner_coro())` can cause deadlock
**Tests Affected:**
- `test_nested_coroutines`
- `test_deeply_nested_coroutines`
**Potential Fix:** Implement proper coroutine waiting mechanism that doesn't block the scheduler

### Detached Coroutine Memory Management
**Issue:** Detached coroutines have memory management issues
**Tests Affected:**
- `test_detached_coroutines`
- `test_detached_coroutines_with_mutex`
**Potential Fix:** Track detached coroutine lifetimes and clean up properly

### CoCond Mutex Re-acquisition
**Issue:** Uses spin-yield loop instead of proper coroutine-based waiting
**Location:** `src/coro/cond.cpp:await_resume()`
**Potential Fix:** Implement two-phase suspension or atomic state machine for proper async re-lock

---

*Generated: 2026-03-30*