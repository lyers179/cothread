# Butex 实现原理

## 概述

Butex (Binaryutex) 是 bthread 库的核心同步原语，用于实现条件变量、mutex 和 join 等同步机制。

## 数据结构

```cpp
class Butex {
private:
    std::atomic<TaskMeta*> waiters_{nullptr};  // 等待队列
    std::atomic<int> value_{0};                 // generation 计数器
};
```

### 等待队列节点

```cpp
struct WaiterState {
    std::atomic<bool> wakeup{false};      // 是否已被唤醒
    std::atomic<bool> timed_out{false};   // 是否超时
    std::atomic<TaskMeta*> next{nullptr}; // 队列中的下一个
    int64_t deadline_us{0};                // 超时截止时间
    uint64_t timer_id{0};                  // 定时器 ID
};
```

## Wait 实现

### 完整流程

```cpp
int Butex::Wait(int expected_value, const timespec* timeout) {
    // 1. 如果从 pthread 调用，使用系统 futex
    if (!Worker::Current()) {
        return platform::FutexWait(&value_, expected_value, timeout);
    }

    TaskMeta* task = Worker::Current()->current_task();

    // 2. 快速路径：generation 已变化
    if (value_.load() != expected_value) {
        return 0;
    }

    // 3. 初始化等待状态
    WaiterState& ws = task->waiter;
    ws.wakeup.store(false);
    ws.timed_out.store(false);

    // 4. 加入等待队列（无锁栈）
    TaskMeta* old_head = waiters_.load();
    do {
        ws.next.store(old_head);
    } while (!waiters_.compare_exchange_weak(old_head, task));

    // 5. 再次检查 generation（防止竞态）
    if (value_.load() != expected_value) {
        RemoveFromWaitQueue(task);
        return 0;
    }

    // 6. 设置超时（如果指定）
    if (timeout) {
        ws.timer_id = ScheduleTimeout(task, timeout);
    }

    // 7. 设置 SUSPENDED 状态
    task->state.store(TaskState::SUSPENDED);

    // 8. 最终检查 wakeup（在 SUSPENDED 之后）
    if (ws.wakeup.load()) {
        // Wake 已执行，恢复并返回
        task->state.store(TaskState::READY);
        CancelTimeout(ws.timer_id);
        return 0;
    }

    // 9. 安全挂起
    Worker::Current()->SuspendCurrent();

    // 10. 被唤醒，检查结果
    return ws.timed_out.load() ? ETIMEDOUT : 0;
}
```

### 关键点

1. **无锁队列**: 使用 CAS 操作实现线程安全的栈
2. **双重检查**: 加入队列后再次检查 generation，防止丢失唤醒
3. **状态检查顺序**: 先设置 SUSPENDED，再检查 wakeup，防止竞态

## Wake 实现

### 完整流程

```cpp
void Butex::Wake(int count) {
    // 1. 唤醒 pthread 等待者（使用系统 futex）
    platform::FutexWake(&value_, count);

    int woken = 0;
    while (woken < count) {
        // 2. 从队列头部取出等待者
        TaskMeta* waiter = waiters_.load();
        if (!waiter) break;

        TaskMeta* next = waiter->waiter.next.load();
        if (!waiters_.compare_exchange_weak(waiter, next)) {
            continue;  // CAS 失败，重试
        }

        WaiterState& ws = waiter->waiter;

        // 3. 原子设置 wakeup 标志
        bool expected = false;
        if (!ws.wakeup.compare_exchange_strong(expected, true)) {
            continue;  // 已被其他线程唤醒
        }
        woken++;

        // 4. 取消超时定时器
        if (ws.timer_id != 0) {
            CancelTimeout(ws.timer_id);
        }

        // 5. 如果任务已 SUSPENDED，放入运行队列
        if (waiter->state.load() == TaskState::SUSPENDED) {
            waiter->state.store(TaskState::READY);
            Scheduler::Instance().EnqueueTask(waiter);
        }
        // 如果不是 SUSPENDED，任务会检查 wakeup 并正确返回
    }
}
```

### 关键点

1. **CAS 设置 wakeup**: 防止同一等待者被多次唤醒
2. **检查 SUSPENDED 状态**: 只有已挂起的任务才放入运行队列
3. **非 SUSPENDED 处理**: 任务会在 Wait 中检测到 wakeup 并正确返回

## 竞态条件分析

### 场景 1: Wake 在 Wait 加入队列之前执行

```
时间线:
  T1: Wait() 检查 value_, 进入等待路径
  T2: Wake() 增加 value_, 发现队列为空
  T3: Wait() 加入队列
  T4: Wait() 再次检查 value_, 发现已变化, 返回

结果: 正确，Wait 检测到 generation 变化
```

### 场景 2: Wake 在 Wait 设置 SUSPENDED 之前执行

```
时间线:
  T1: Wait() 加入队列
  T2: Wake() 取出等待者, 设置 wakeup=true
  T3: Wake() 检查 state != SUSPENDED, 不放入队列
  T4: Wait() 设置 SUSPENDED
  T5: Wait() 检查 wakeup=true, 恢复 READY, 返回

结果: 正确，Wait 检测到 wakeup 并返回
```

### 场景 3: Wake 在 Wait 检查 wakeup 之后执行

```
时间线:
  T1: Wait() 加入队列
  T2: Wait() 设置 SUSPENDED
  T3: Wait() 检查 wakeup=false
  T4: Wake() 取出等待者, 设置 wakeup=true
  T5: Wake() 检查 state=SUSPENDED, 放入队列
  T6: Wait() 挂起, 但已在队列中, 会被调度执行

结果: 正确，任务被唤醒并执行
```

## 与 Linux futex 的对比

| 特性 | Linux futex | Butex |
|------|-------------|-------|
| 等待机制 | 内核等待队列 | 用户态等待队列 |
| 唤醒开销 | 系统调用 | 用户态操作 |
| 线程模型 | 1:1 | M:N |
| 适用场景 | pthread | bthread |

## 性能优化

### 1. 无锁队列

使用栈结构（LIFO）而不是队列（FIFO）：
- 只需一个 CAS 操作即可入队/出队
- 缓存局部性更好

### 2. 快速路径

在进入慢路径之前先检查 generation：
```cpp
if (value_.load() != expected_value) {
    return 0;  // 快速返回，无需加锁
}
```

### 3. 批量唤醒

`Wake(count)` 支持一次唤醒多个等待者：
- 用于 `bthread_cond_broadcast`
- 减少系统调用次数

## 使用示例

### 实现信号量

```cpp
class Semaphore {
    Butex butex;
    std::atomic<int> count{0};

public:
    void Wait() {
        while (true) {
            int c = count.load();
            if (c > 0 && count.compare_exchange_weak(c, c - 1)) {
                return;
            }
            int gen = butex.value();
            butex.Wait(gen, nullptr);
        }
    }

    void Signal() {
        count.fetch_add(1);
        butex.set_value(butex.value() + 1);
        butex.Wake(1);
    }
};
```

### 实现事件

```cpp
class Event {
    Butex butex;
    bool signaled = false;

public:
    void Wait() {
        if (signaled) return;
        int gen = butex.value();
        butex.Wait(gen, nullptr);
    }

    void Set() {
        signaled = true;
        butex.set_value(butex.value() + 1);
        butex.Wake(INT_MAX);
    }
};
```