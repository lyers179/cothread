# bthread 测试结果

## 测试摘要

**通过率: 14/14 (100%)**

## 通过的测试

| 测试 | 状态 | 说明 |
|------|------|------|
| context_test | ✅ 通过 | 上下文切换基础功能 |
| debug_test | ✅ 通过 | 调试测试（多 bthread 创建和执行）|
| butex_test | ✅ 通过 | Butex 同步原语 |
| cond_test | ✅ 通过 | 条件变量 |
| global_queue_test | ✅ 通过 | 全局任务队列 |
| mutex_test | ✅ 通过 | 互斥锁 |
| task_group_test | ✅ 通过 | 任务池管理 |
| work_stealing_queue_test | ✅ 通过 | 工作窃取队列 |
| scheduler_test | ✅ 通过 | 调度器基础功能 |
| wake_test | ✅ 通过 | 多任务唤醒测试 |
| multi_test | ✅ 通过 | 多 bthread 并发（含互斥锁竞争）|
| timer_test | ✅ 通过 | 定时器功能 |
| bthread_test | ✅ 通过 | 完整 bthread API 测试 |
| stress_test | ✅ 通过 | 压力测试 |

## 已修复的问题

1. **上下文切换崩溃**
   - 原因：MakeContext 栈指针计算错误，参数传递方式错误
   - 修复：正确计算栈布局，添加 BthreadStart trampoline

2. **bthread_exit 崩溃**
   - 原因：在切换回调度器前释放了 TaskMeta
   - 修复：不在 bthread_exit 中释放，交给 HandleFinishedTask 处理

3. **bthread_join 阻塞**
   - 原因：Butex::Wake 没有唤醒 pthread 等待者
   - 修复：在 Wake 中同时调用 FutexWake

4. **任务丢失**
   - 原因：GlobalQueue::Pop 取走所有任务但只返回链表头
   - 修复：修改 Pop 只弹出单个任务

5. **调度器初始化竞争条件**
   - 原因：worker 线程在 `running_` 标志设置前启动，导致 worker 立即退出
   - 修复：在 StartWorkers 之前设置 `running_` 为 true

6. **bthread_yield 挂起**
   - 原因：WorkStealingQueue::Pop 在弹出最后一个元素后版本号不同步，导致后续 Push/Pop 逻辑错误
   - 修复：修正 Pop 函数中单元素情况下的 tail 更新逻辑

7. **互斥锁竞争条件**
   - 原因：当锁被释放但 HAS_WAITERS 标志仍设置时，获取锁的逻辑不正确，导致竞争
   - 修复：重写锁获取逻辑，正确处理 HAS_WAITERS 状态

## 核心功能状态

- ✅ 上下文切换（汇编实现）
- ✅ bthread 创建和执行
- ✅ bthread_join 等待
- ✅ bthread_detach 分离
- ✅ bthread_yield 让出
- ✅ Butex 同步原语
- ✅ Mutex 互斥锁
- ✅ Condition Variable 条件变量
- ✅ 多 bthread 并发
- ✅ 定时器
- ✅ 工作窃取调度

## 平台支持

- Windows x64 (MSVC 2022) ✅
- Linux x64 (GCC/Clang) - 理论支持，未测试

## 与官方 brpc bthread 对比

详见 [COMPARISON.md](COMPARISON.md)

主要差异：
- 本项目支持 Windows，官方仅支持 Linux/macOS
- 本项目功能精简，官方功能完整（tag调度、信号量、读写锁等）
- 本项目无外部依赖，官方依赖 butil 库

## 构建说明

详见 [BUILD.md](BUILD.md)

## 示例程序

运行 demo:
```bash
./build/demo/Release/demo.exe
```

Demo 展示以下功能:
- 基础 bthread 创建和等待
- 互斥锁保护共享数据
- 条件变量同步
- bthread_yield 让出 CPU
- 定时器功能
- detach 分离线程

## 性能测试 (Benchmark)

运行 benchmark:
```bash
./build/benchmark/Release/benchmark.exe
```

### 性能结果 (Windows x64, MSVC 2022, 8-core)

| 测试 | 吞吐量 | 延迟 |
|------|--------|------|
| Create/Join | 620K ops/sec | 1.61 us/op |
| Yield | 65.4M yields/sec | 15.29 ns/yield |
| Mutex | 4.1M lock/unlock/sec | 0.24 us/op |
| Stack | 551K ops/sec | - |
| Producer-Consumer | 1.7M items/sec | - |

### 与 std::thread 对比

bthread 比 std::thread **快 21 倍**，得益于用户态上下文切换避免内核态切换开销。