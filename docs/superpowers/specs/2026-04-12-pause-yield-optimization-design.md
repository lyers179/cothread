# Pause/Yield Spin 优化设计规格

> **设计日期**: 2026-04-12
> **设计者**: Claude

## 问题分析

### 当前问题

Phase 4 的 lock-free 实现使用了 `std::this_thread::yield()` 进行 spin，导致不必要的上下文切换开销：

| 问题 | Phase 4 实现 | 影响 |
|------|-------------|------|
| spin 使用 yield | 每次 spin 都调用 `yield()` | 上下文切换开销 (~1000 周期) |
| MAX_SPINS 过大 | 10000 次 | 过度等待，即使操作已完成也要等待 |
| 内存序过强 | 部分使用 `seq_cst` | 不必要的同步开销 |

### 症状

1. Create/Join 性能波动大（有时 21K ops/sec，有时 115K ops/sec）
2. yield() 调用频繁，即使 CAS 很快就成功
3. 无锁算法本应比 mutex 快，但实际表现不稳定

## 优化目标

将 spin 策略从纯 yield 改为自适应 pause → yield：
- **Phase 1**: CPU pause 指令（低延迟，无上下文切换）
- **Phase 2**: yield（作为 pause 失败后的 fallback）

## 问题根因

### Phase 5 引入的 Bug

Phase 5 的 PopFromHead 在队列非空时引入了 timeout 返回 nullptr:

```cpp
// 错误行为
if (pause_count >= MAX_PAUSE && yield_count >= MAX_YIELD) {
    return nullptr;  // ← 即使队列有节点也返回 nullptr
}
```

这导致 Wake 调用 PopFromHead 后认为队列空了，停止唤醒 waiter。实际 waiter 还在队列中未被唤醒，导致 bthread 创建后卡住。

### 正确行为

PopFromHead 应该只在队列真正空时返回 nullptr:

```cpp
// 正确行为
if (!head && !tail) return nullptr;  // ← 只有真正空才返回 nullptr
// 其他情况继续 retry
```

## 设计方案

### pause vs yield 对比

| 特性 | pause | yield |
|------|-------|-------|
| 级别 | CPU 指令 | 系统调用 |
| 上下文切换 | **无** | **有** |
| 内核介入 | **无** | **有** |
| 延迟 | ~10-100 周期 | ~1000-10000 周期 |
| 功耗 | 低 | 中 |
| 适用场景 | 短等待（微秒级） | 中等待（毫秒级） |

### 自适应 Spin 算法

```cpp
constexpr int MAX_PAUSE_SPINS = 100;   // Phase 1: CPU pause
constexpr int MAX_YIELD_SPINS = 10;    // Phase 2: yield

int pause_count = 0;
int yield_count = 0;

while (!ready) {
    if (pause_count < MAX_PAUSE_SPINS) {
        __builtin_ia32_pause();  // CPU 指令，无上下文切换
        ++pause_count;
        continue;
    }
    if (yield_count < MAX_YIELD_SPINS) {
        std::this_thread::yield();  // 仅在 pause 失败后 yield
        ++yield_count;
        pause_count = 0;  // Reset pause counter
        continue;
    }
    // Timeout after both phases
    return nullptr;
}
```

**预期效果**:
- 大多数 CAS 在 pause phase 内成功，无上下文切换
- 仅在真正需要等待时才 yield
- 总等待时间可控（100 pause + 10 yield ≈ 3-10 us + 1-10 ms）

### 批量 Pop 减少 CAS 开销

Wake 操作唤醒多个 waiter 时，使用批量 Pop 减少 CAS 开销：

```cpp
// Phase 4: 单个 Pop，每次 CAS
while (woken < count) {
    TaskMeta* waiter = queue_.PopFromHead();
    // ... 每次都有 CAS 操作
}

// Phase 5: 批量 Pop
TaskMeta* tasks[16];
int batch_count = queue_.PopMultipleFromHead(tasks, 16);
// 一次 CAS 获取多个 waiter
```

### 内存序优化

将不必要的 `seq_cst` 改为 `acquire`：

```cpp
// Phase 4: seq_cst（过强）
T* n = t->next.load(std::memory_order_seq_cst);

// Phase 5: acquire（足够）
T* n = t->next.load(std::memory_order_acquire);
```

## 技术细节

### 平台抽象

```cpp
#if defined(__x86_64__) || defined(__i386__)
    #define BTHREAD_PAUSE() __builtin_ia32_pause()
#elif defined(__aarch64__)
    #define BTHREAD_PAUSE() asm volatile("isb" ::: "memory")
#else
    #define BTHREAD_PAUSE() do {} while(0)  // compiler barrier
#endif
```

### 修改文件

| 文件 | 修改内容 |
|------|----------|
| `src/bthread/sync/butex_queue.cpp` | PopFromHead 改用 pause → yield |
| `include/bthread/queue/mpsc_queue.hpp` | Pop 改用 pause → yield + acquire |
| `include/bthread/sync/butex_queue.hpp` | 添加 PopMultipleFromHead |
| `src/bthread/sync/butex.cpp` | Wake 使用批量 Pop |

## 性能预期

| 指标 | Phase 4 | Phase 5 预期 |
|------|---------|-------------|
| Create/Join | 92K ops/sec | **110K+ ops/sec** |
| 无竞争 Pop | ~100 ns | **~50 ns** |
| 高竞争 Pop | ~500 ns | **~100 ns** |
| Benchmark 通过率 | 100% | **100%** |

## 风险评估

| 风险 | 缓解措施 |
|------|----------|
| pause 在非 x86 平台效果不同 | 提供平台抽象，ARM 用 isb |
| MAX_PAUSE_SPINS 太小导致过早 yield | 设置 100 次，覆盖大多数 CAS 场景 |
| 批量 Pop 导致内存分配 | 使用固定大小静态数组 (16) |

## 参考资料

- [Intel Pause Instruction](https://www.intel.com/content/www/us/en/docs/intrinsics-guide/index.html#text=__builtin_ia32_pause)
- [Adaptive Spinning in Linux Kernel](https://lwn.net/Articles/752628/)
- 项目已有 pause 使用: `src/bthread/core/worker.cpp:295`