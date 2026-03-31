# Generation 机制详解

## 背景

在传统的条件变量和 mutex 实现中，通常使用一个布尔值来表示是否有信号。但这种设计在 M:N 线程模型中会导致严重的问题。

## 传统实现的问题

### 问题代码

```cpp
class Butex {
    std::atomic<int> value_{0};  // 0 = 无信号, 1 = 有信号
};

void Wake() {
    value_.store(1);  // 设置信号
}

void Wait() {
    while (value_.load() == 0) {  // 等待信号
        // 挂起
    }
    // 问题：如何重置 value？
}
```

### 问题分析

1. **值永久设置为 1**
   - 第一次 `Wake()` 后，`value_` 变为 1
   - 后续所有 `Wait()` 都会立即返回（因为 `value_ != 0`）
   - 导致无限循环

2. **无法区分新旧信号**
   - 如果 `Wait()` 在 `Wake()` 之前被调用，正常工作
   - 如果 `Wake()` 在 `Wait()` 之前被调用，`Wait()` 可能错过信号

## Generation 机制原理

### 核心思想

使用**递增的序列号（generation）**代替布尔值：

- 每个 `Wait()` 记录当前的 generation
- 每个 `Wake()` 增加 generation
- `Wait()` 等待 generation 变化

### 实现代码

```cpp
class Butex {
    std::atomic<int> value_{0};  // generation 计数器
};

void Wake() {
    // 增加 generation（不需要先读取）
    value_.fetch_add(1, std::memory_order_release);
    // 唤醒等待者
    WakeWaiters();
}

void Wait() {
    // 记录当前 generation
    int generation = value_.load(std::memory_order_acquire);

    // 等待 generation 变化
    while (value_.load(std::memory_order_acquire) == generation) {
        // 挂起
    }
}
```

### 工作流程

```
初始状态: value_ = 0

线程 A (Wait):
  1. 读取 generation = 0
  2. 检查 value_ == 0? 是
  3. 挂起等待

线程 B (Wake):
  1. 增加 value_: 0 -> 1
  2. 唤醒等待者

线程 A (被唤醒):
  1. 检查 value_ == 0? 否 (value_ = 1)
  2. 返回成功
```

## 应用场景

### 条件变量

```cpp
int bthread_cond_wait(bthread_cond_t* cond, bthread_mutex_t* mutex) {
    Butex* butex = static_cast<Butex*>(cond->butex);

    // 记录当前 generation
    int generation = butex->value();

    // 释放 mutex 并等待
    bthread_mutex_unlock(mutex);
    butex->Wait(generation, nullptr);
    bthread_mutex_lock(mutex);

    return 0;
}

int bthread_cond_signal(bthread_cond_t* cond) {
    Butex* butex = static_cast<Butex*>(cond->butex);

    // 增加 generation 并唤醒一个等待者
    butex->set_value(butex->value() + 1);
    butex->Wake(1);

    return 0;
}
```

### Mutex

```cpp
int bthread_mutex_lock(bthread_mutex_t* mutex) {
    while (true) {
        // 尝试获取锁
        if (TryAcquire()) return 0;

        // 记录当前 generation
        Butex* butex = mutex->butex;
        int generation = butex->value();

        // 双重检查：防止错过唤醒
        if (TryAcquire()) return 0;

        // 等待唤醒
        butex->Wait(generation, nullptr);
    }
}

int bthread_mutex_unlock(bthread_mutex_t* mutex) {
    // 释放锁
    Release();

    if (HasWaiters()) {
        // 增加 generation 并唤醒一个等待者
        Butex* butex = mutex->butex;
        butex->set_value(butex->value() + 1);
        butex->Wake(1);
    }
}
```

### bthread_join

```cpp
int bthread_join(bthread_t tid, void** retval) {
    TaskMeta* task = GetTaskGroup().DecodeId(tid);
    Butex* join_butex = static_cast<Butex*>(task->join_butex);

    // 先获取 generation，再检查状态（防止竞态）
    int generation = join_butex->value();

    if (task->state.load() == TaskState::FINISHED) {
        return 0;  // 任务已完成
    }

    // 等待任务完成
    join_butex->Wait(generation, nullptr);
    return 0;
}

void HandleFinishedTask(TaskMeta* task) {
    // 增加 generation 并唤醒所有 join 等待者
    Butex* butex = static_cast<Butex*>(task->join_butex);
    butex->set_value(butex->value() + 1);
    butex->Wake(INT_MAX);
}
```

## 与 POSIX 条件变量的对比

### POSIX 条件变量

```cpp
pthread_mutex_lock(&mutex);
while (!condition) {
    pthread_cond_wait(&cond, &mutex);
}
// 临界区
pthread_mutex_unlock(&mutex);

// 发送信号
pthread_mutex_lock(&mutex);
condition = true;
pthread_cond_signal(&cond);
pthread_mutex_unlock(&mutex);
```

### bthread 条件变量

```cpp
bthread_mutex_lock(&mutex);
while (!condition) {
    bthread_cond_wait(&cond, &mutex);
}
// 临界区
bthread_mutex_unlock(&mutex);

// 发送信号
bthread_mutex_lock(&mutex);
condition = true;
bthread_cond_signal(&cond);  // 内部增加 generation
bthread_mutex_unlock(&mutex);
```

**主要区别：**
- POSIX 使用内核提供的等待队列
- bthread 使用用户态的 butex 和 generation

## 优势

1. **无虚假唤醒**
   - Generation 只在显式调用 signal/broadcast 时改变
   - Wait 不会因其他原因返回

2. **无信号丢失**
   - Generation 会累积，不会"消耗"掉
   - 即使 Wake 在 Wait 之前调用，Wait 也能正确检测到

3. **简单高效**
   - 只需要原子计数器
   - 不需要复杂的状态机

## 注意事项

### Generation 溢出

理论上 `int` 溢出会导致问题，但实际上：
- 32 位 int 可表示 2^31 次操作
- 即使每秒 100 万次操作，也需要约 35 分钟才溢出
- 64 位环境下可使用 `int64_t` 彻底避免

### 多等待者场景

一个 `Wake()` 只唤醒一个等待者：
```
初始: value_ = 0
等待者 A, B, C 都记录 generation = 0 并等待

Wake():
  value_ = 1
  唤醒 A

A 被唤醒，检查 value_ != 0，返回

B, C 仍在等待，检查 value_ != 0，也会被唤醒
```

这是正确的行为：所有等待者都检测到了 generation 的变化。