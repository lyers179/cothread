# Performance Test Results

## Test Environment
- Platform: Windows 11 (MinGW GCC 15.2.0)
- Worker threads: 8
- Build: Static linking
- Date: 2026-04-08

## Performance Benchmarks

### Test 1: Create/Join Throughput
- Threads: 10, Iterations: 10
- Total operations: 100
- Total time: 2.88-3.17 ms
- Throughput: 31,512-34,763 ops/sec
- Latency: 28.77-31.73 us/op

### Test 2: Yield Performance  
- Threads: 4, Yields per thread: 1000
- Total yields: 4,000
- Total time: 0.32 ms
- Throughput: 12,376,238 yields/sec
- Latency: 80.80 ns/yield

### Test 3: Mutex Contention
- **Low contention (2 threads, 100 iterations)**: PASS
- **High contention (4 threads, 1000 iterations)**: TIMEOUT

## Recent Fixes

### Butex Race Condition Fix
**Problem**: Race condition between Wait and Wake causing mutex hangs under high contention.

**Root Cause**: When Wait sets SUSPENDED during/after Wake processes the task, the task hangs forever.

**Solution**:
1. Added `wake_count` to TaskMeta
2. Wait uses CAS to detect Wake setting READY before SUSPENDED
3. Wait enqueues self if Wake happened during CAS
4. Wake always increments `wake_count` when popping from queue

**Files Modified**:
- `include/bthread/task_meta.h` - Added `wake_count` field
- `src/butex.cpp` - Fixed Wait/Wake race condition

### XMM Lazy Saving Revert
Reverted XMM lazy saving changes that caused DLL loading issues (0xC0000139). Static linking resolves this.

## Known Issues

### Mutex High Contention
- **Status**: Tests with 4 threads × 1000 iterations still hang
- **Workaround**: Reduce iterations or use lower thread counts
- **Next Steps**: Further optimization needed for extreme contention scenarios

## Comparison

| Metric | Before Fix | After Fix | Improvement |
|--------|-----------|-----------|-------------|
| Create/Join | N/A | ~33K ops/sec | - |
| Yield | N/A | ~12M yields/sec | - |
| Mutex (low) | HANG | PASS | ✓ |
| Mutex (high) | HANG | HANG | - |

## Build Notes
- Use static linking: `-static -static-libgcc -static-libstdc++ -lsynchronization`
- Dynamic linking causes DLL loading errors on Windows