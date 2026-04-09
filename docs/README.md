# bthread 库文档

## 概述

bthread 是一个 M:N 线程池库，将 M 个用户级线程映射到 N 个内核级线程上执行。主要特性包括：

- 用户态上下文切换
- 工作窃取调度
- 高效的同步原语（Butex）
- 支持条件变量、Mutex、定时器等
- C++20 协程支持

## 快速开始

参见 [BUILD.md](../BUILD.md) 了解编译和基本使用方法。

参见 [CLAUDE.md](../CLAUDE.md) 了解项目协作规范。

## 架构文档

- [调度器架构](architecture/scheduler.md) - M:N 调度模型详解
- [Butex 实现原理](architecture/butex.md) - 同步原语实现
- [Generation 机制](architecture/generation.md) - 条件变量实现原理
- [协程池架构](architecture/coroutine.md) - C++20 协程池实现
- [整体架构](ARCHITECTURE.md) - 系统架构总览

## 性能优化文档

- [性能优化历史](performance_history.md) - 按时间线的优化记录
- [性能优化详情](performance_optimization.md) - 当前优化措施详情

## 问题修复文档

- [卡死问题修复](BUGFIX_STUCK_ISSUE.md) - 详细的问题分析和解决方案
- [协程池待办事项](COROUTINE_TODO.md) - 协程池已知限制和待解决问题

## API 文档

参见头文件：
- [bthread.h](../include/bthread.h) - 核心 API
- [bthread/sync/mutex.hpp](../include/bthread/sync/mutex.hpp) - 互斥锁
- [bthread/sync/cond.hpp](../include/bthread/sync/cond.hpp) - 条件变量

## 性能数据

最新基准测试结果（8 核机器）：

| 测试项 | 结果 |
|--------|------|
| Create/Join | ~80,000 ops/sec |
| Yield | ~8,000,000 yields/sec |
| Mutex (高并发) | ~12,000,000 lock/unlock/sec |
| vs std::thread | **快 3x** |

详细性能对比参见 [性能优化历史](performance_history.md)。

## 已知限制

1. **快速创建/销毁** - 1000 次连续创建/销毁偶发问题
2. **仅支持 64 位系统**

## 版本历史

- 2026-04-09: perf 分析优化，Scalability 提升 30%+
- 2026-04-09: 分配开销优化（Stack Pool, TaskMeta Cache, Lazy Butex）
- 2026-04-07: 第一轮性能优化（spin, CAS weak, 内存布局）
- 2026-03-30: 实现 FIFO/LIFO 混合策略，解决极端高并发 mutex 卡死问题
- 2026-03-30: 修复主要卡死问题，分析极端高并发公平性问题
- 2026-03-25: 初始实现