# CLAUDE.md - 项目协作指南

本文档定义了 bthread M:N 线程库项目的文档组织风格、临时测试代码规范、以及重要修改的归档流程。

---

## 项目概述

bthread 是一个高性能的 M:N 线程库，支持：
- Work-stealing 调度器
- C++20 协程
- 同步原语（Mutex, Condition Variable, Butex）
- 跨平台支持（Linux, Windows）

---

## 文档组织风格

### 目录结构

```
docs/
├── architecture/           # 架构设计文档
│   ├── scheduler.md
│   ├── butex.md
│   └── coroutine.md
├── superpowers/           # AI 辅助开发文档
│   ├── specs/            # 设计规范（设计阶段）
│   └── plans/            # 实现计划（执行阶段）
├── performance_history.md    # 性能优化历史（按时间线）
├── performance_optimization.md # 当前优化详情
├── ARCHITECTURE.md       # 整体架构说明
├── BUGFIX_STUCK_ISSUE.md # Bug 修复记录
└── README.md             # 文档索引
```

### 文档命名规范

| 类型 | 命名格式 | 示例 |
|------|----------|------|
| 设计规范 | `YYYY-MM-DD-主题-design.md` | `2026-04-09-allocation-optimization-design.md` |
| 实现计划 | `YYYY-MM-DD-主题.md` | `2026-04-09-allocation-optimization.md` |
| 性能文档 | `performance_*.md` | `performance_history.md` |
| Bug 修复 | `BUGFIX_*.md` | `BUGFIX_STUCK_ISSUE.md` |

### 文档内容规范

1. **设计文档**：包含问题分析、方案对比、风险评估
2. **实现计划**：包含分步任务、验证点、提交记录
3. **性能文档**：包含基准数据、对比表格、优化详情

---

## 临时测试代码

### 目录位置

```
temp/                     # 临时测试代码目录
├── .gitignore           # 忽略所有文件
└── (临时测试文件)        # 不被 git 追踪
```

### 使用规范

1. **临时测试代码放在 `temp/` 目录下**
2. **不会被 git 追踪**，用于快速验证想法
3. **正式测试代码放在 `tests/` 目录**
4. **性能分析代码放在 `benchmark/` 目录**

### 示例用法

```bash
# 创建临时测试
cat > temp/test_xxx.cpp << 'EOF'
#include "bthread.h"
// ... 测试代码
EOF

# 编译运行
g++ -std=c++20 -I./include -o temp/test temp/test_xxx.cpp -L./build -lbthread -lpthread

# 清理
rm temp/test*
```

---

## 重要修改归档流程

### 1. 性能优化修改

每次重要性能优化后，**必须**同步更新：

#### 1.1 更新 `docs/performance_history.md`

在对应时间节点添加新条目：

```markdown
## YYYY-MM-DD: 优化标题

**设计文档**: `docs/superpowers/specs/YYYY-MM-DD-xxx-design.md`
**实现计划**: `docs/superpowers/plans/YYYY-MM-DD-xxx.md`

### 问题分析
（描述性能瓶颈）

### 优化方案
（描述优化措施）

### 提交记录

| 提交 | 说明 |
|------|------|
| `abc1234` | feat: xxx |

### 性能结果

| 指标 | 优化前 | 优化后 | 改进 |
|------|--------|--------|------|
| ... | ... | ... | ... |
```

#### 1.2 更新 `docs/performance_optimization.md`

更新当前性能基准表和优化详情。

### 2. Bug 修复

创建 `docs/BUGFIX_*.md` 文件：

```markdown
# Bug 修复：问题描述

## 发现时间
YYYY-MM-DD

## 问题现象
（描述 bug 表现）

## 根因分析
（使用系统性调试流程分析）

## 修复方案
（描述修复方法）

## 验证方法
（如何验证修复有效）

## 提交记录
- `abc1234` fix: xxx
```

### 3. 新功能添加

1. 在 `docs/superpowers/specs/` 创建设计文档
2. 在 `docs/superpowers/plans/` 创建实现计划
3. 完成后在 `docs/performance_history.md` 添加记录

---

## Benchmark 更新要求

### 每次重要修改后必须更新

在 `docs/performance_history.md` 的"完整指标对比"表格中更新数据：

```markdown
| 指标 | 初始 | Phase 1 | Phase 2 | Phase 3 (最新) |
|------|------|---------|---------|----------------|
| Create/Join | ~5K | 81K | 83K | **新数据** |
| vs std::thread | 慢 6.92x | 快 3.19x | 快 3.15x | **新数据** |
| Scalability (8w) | - | 6.64x | 8.50x | **新数据** |
| Stack Performance | - | 148K | 162K | **新数据** |
| Producer-Consumer | - | 492K | 728K | **新数据** |
```

### 运行 Benchmark

```bash
cd build
./benchmark/benchmark
```

### 使用 perf 分析

```bash
perf record -g -o perf.data ./benchmark/benchmark
perf report -i perf.data --stdio --no-children
```

---

## 代码风格

### 命名规范

| 类型 | 风格 | 示例 |
|------|------|------|
| 类名 | PascalCase | `TaskGroup`, `WorkStealingQueue` |
| 函数名 | PascalCase | `AcquireStack`, `ReleaseTaskMeta` |
| 变量名 | snake_case | `task_cache_count_`, `stack_pool_` |
| 常量 | UPPER_CASE | `STACK_POOL_SIZE`, `BATCH_SIZE` |
| 成员变量 | snake_case + 下划线后缀 | `id_`, `stop_flag_` |

### 文件组织

```
include/bthread/          # 公共头文件
├── core/                 # 核心组件
├── sync/                 # 同步原语
├── queue/                # 队列实现
└── platform/             # 平台抽象

src/bthread/              # 实现文件
├── core/
├── sync/
└── ...

tests/                    # 测试文件
benchmark/                # 性能测试
temp/                     # 临时测试（不追踪）
```

---

## 常用命令

```bash
# 构建
mkdir -p build && cd build
cmake .. && make -j$(nproc)

# 运行测试
ctest --output-on-failure
./tests/stack_pool_test

# 运行 benchmark
./benchmark/benchmark

# 代码检查
clang-tidy src/bthread/core/worker.cpp

# 性能分析
perf record -g ./benchmark/benchmark
perf report
```

---

## 提交信息格式

```
<type>(<scope>): <subject>

<body>

Co-Authored-By: Claude Opus 4.6 <noreply@anthropic.com>
```

### Type 类型

| Type | 说明 |
|------|------|
| `feat` | 新功能 |
| `fix` | Bug 修复 |
| `perf` | 性能优化 |
| `refactor` | 重构 |
| `test` | 测试相关 |
| `docs` | 文档更新 |

### 示例

```
perf(scheduler): reduce unnecessary futex syscalls

Two key optimizations:
1. Replace WakeAllWorkers() with WakeIdleWorkers(1)
2. Add is_idle_ flag to skip unnecessary futex calls

Performance: Scalability (8w) 6.5x → 8.5x

Co-Authored-By: Claude Opus 4.6 <noreply@anthropic.com>
```

---

## 注意事项

1. **不要跳过测试验证** - 重要修改后必须运行完整测试
2. **不要忘记文档更新** - 性能修改必须同步更新 benchmark 文档
3. **不要提交 temp/ 目录** - 临时测试代码不进入版本控制
4. **使用系统性调试流程** - Bug 修复遵循 Phase 1-4 调试流程