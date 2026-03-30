# bthread 库文档

## 概述

bthread 是一个 M:N 线程池库，将 M 个用户级线程映射到 N 个内核级线程上执行。主要特性包括：

- 用户态上下文切换
- 工作窃取调度
- 高效的同步原语（Butex）
- 支持条件变量、Mutex、定时器等

## 快速开始

参见 [BUILD.md](../BUILD.md) 了解编译和基本使用方法。

## 架构文档

- [调度器架构](architecture/scheduler.md) - M:N 调度模型详解
- [Butex 实现原理](architecture/butex.md) - 同步原语实现
- [Generation 机制](architecture/generation.md) - 条件变量实现原理
- [协程池架构](architecture/coroutine.md) - C++20 协程池实现

## 问题修复文档

- [卡死问题修复](BUGFIX_STUCK_ISSUE.md) - 详细的问题分析和解决方案
- [协程池待办事项](COROUTINE_TODO.md) - 协程池已知限制和待解决问题

## API 文档

参见头文件：
- [bthread.h](../include/bthread.h) - 核心 API
- [bthread/mutex.h](../include/bthread/mutex.h) - 互斥锁
- [bthread/cond.h](../include/bthread/cond.h) - 条件变量

## 性能数据

在 8 核机器上的基准测试结果：

| 测试项 | 结果 |
|--------|------|
| Create/Join | ~600,000 ops/sec |
| Yield | ~70,000,000 yields/sec |

## 已知限制

1. **极端高并发 Mutex** - 80000+ 次连续 mutex 操作可能卡住
2. **快速创建/销毁** - 1000 次连续创建/销毁偶发问题
3. **仅支持 64 位系统**

## 版本历史

- 2026-03-30: 修复主要卡死问题
- 2026-03-25: 初始实现