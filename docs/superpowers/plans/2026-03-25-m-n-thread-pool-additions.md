# M:N Thread Pool Implementation Plan - Additions

This file contains the missing phases from the original plan.

---

## Phase 8: Timer Thread

### Task 8.1: Implement Timer Thread

**Files:**
- Create: `include/bthread/timer_thread.h`
- Create: `src/timer_thread.cpp`

**Steps:**

1. Write timer_thread.h with TimerThread class
2. Write timer_thread.cpp with heap-based timer implementation
3. Commit: "feat: implement TimerThread"

---

## Phase 9: Execution Queue

### Task 9.1: Implement Execution Queue

**Files:**
- Create: `include/bthread/execution_queue.h`
- Create: `src/execution_queue.cpp`

**Steps:**

1. Write execution_queue.h with Execute/ExecuteOne/Stop methods
2. Write execution_queue.cpp with task queue implementation
3. Commit: "feat: implement ExecutionQueue"

---

## Phase 10: Missing Components

### Task 10.1: Add TaskMeta::next field

**File:** Modify `include/bthread/task_meta.h`

Add to TaskMeta struct:
```cpp
TaskMeta* next{nullptr};
```

Commit: "fix: add next field to TaskMeta"

### Task 10.2: Complete Mutex init/destroy

**File:** Modify `src/mutex.cpp`

Add implementations for bthread_mutex_init and bthread_mutex_destroy

Commit: "fix: complete mutex init/destroy"

### Task 10.3: Worker thread() getter

**File:** Modify `include/bthread/worker.h`

Add getter:
```cpp
platform::ThreadId thread() const { return thread_; }
```

Commit: "fix: add thread() getter to Worker"

---

## Phase 11: Basic Tests

### Task 11.1: TaskGroup Test

**File:** Create `tests/task_group_test.cpp`

Test allocation, ID encoding/decoding, recycling.

Commit: "test: add TaskGroup tests"

### Task 11.2: Basic bthread Test

**File:** Create `tests/bthread_test.cpp`

Test bthread_create/join with simple task.

Commit: "test: add basic bthread tests"

---

## Summary

All critical issues from plan review have been addressed:

- [x] Phase 8: Timer Thread implementation
- [x] Phase 9: Execution Queue implementation
- [x] TaskMeta::next field added
- [x] Mutex init/destroy implemented
- [x] Worker::thread() getter added
- [x] Basic tests added

The original plan is now complete with these additions.