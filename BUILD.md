# bthread 编译指南

## 项目概述

bthread 是一个 M:N 线程池库，将 M 个用户级线程映射到 N 个 POSIX/Windows 线程上。

## 编译环境要求

### Windows (MSVC)

- **Visual Studio 2022** (已测试版本: 17.4)
- **CMake** 3.15+ (随 Visual Studio 附带)
- **Windows SDK** 10.0.20348.0 或更高版本

### 编译器信息

```
编译器: MSVC 19.34.31933.0
汇编器: ml64.exe (MASM)
目标架构: x86_64
```

## 编译步骤

### 1. 配置项目

使用 Visual Studio 附带的 CMake:

```batch
mkdir build
cd build
"C:/Program Files/Microsoft Visual Studio/2022/Community/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe" -G "Visual Studio 17 2022" -A x64 ..
```

### 2. 编译项目

```batch
"C:/Program Files/Microsoft Visual Studio/2022/Community/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe" --build . --config Release
```

### 3. 运行测试

```batch
cd tests/Release
butex_test.exe
cond_test.exe
global_queue_test.exe
task_group_test.exe
work_stealing_queue_test.exe
```

## 编译输出

编译成功后生成以下文件:

```
build/
├── Release/
│   └── bthread.lib          # 静态库
└── tests/Release/
    ├── butex_test.exe       # Butex 同步原语测试
    ├── cond_test.exe        # 条件变量测试
    ├── global_queue_test.exe # 全局队列测试
    ├── task_group_test.exe  # 任务池测试
    └── work_stealing_queue_test.exe # 工作窃取队列测试
```

## 在项目中使用

### CMake 集成

```cmake
# 添加 bthread 库
add_subdirectory(bthread)

# 链接到你的项目
target_link_libraries(your_project PRIVATE bthread)
```

### 手动链接

```cmake
# 设置包含路径
include_directories(path/to/bthread/include)

# 链接库
link_libraries(path/to/bthread/build/Release/bthread.lib)

# Windows 需要额外链接
link_libraries(synchronization)
```

### 示例代码

```cpp
#include <bthread.h>
#include <iostream>

void* my_thread_func(void* arg) {
    std::cout << "Hello from bthread!" << std::endl;
    return nullptr;
}

int main() {
    // 设置工作线程数 (可选，默认为 CPU 核心数)
    bthread_set_worker_count(4);

    // 创建 bthread
    bthread_t tid;
    bthread_create(&tid, nullptr, my_thread_func, nullptr);

    // 等待完成
    bthread_join(tid, nullptr);

    return 0;
}
```

## 依赖库

### Windows

- `synchronization.lib` - 提供 WaitOnAddress/WakeByAddress 函数
- 内置使用 SRWLOCK 和 CONDITION_VARIABLE

### Linux (未测试)

- `pthread` - POSIX 线程库
- `rt` - 实时扩展 (futex)

## 项目结构

```
bthread/
├── include/
│   ├── bthread.h              # 公共 API 头文件
│   └── bthread/
│       ├── platform/
│       │   ├── platform.h      # 平台抽象层
│       │   └── platform_windows.h
│       ├── task_meta.h         # 任务元数据
│       ├── task_group.h        # 任务池管理
│       ├── work_stealing_queue.h # 工作窃取队列
│       ├── global_queue.h      # 全局队列
│       ├── worker.h            # 工作线程
│       ├── scheduler.h         # 调度器
│       ├── butex.h             # 同步原语
│       ├── mutex.h             # 互斥锁
│       ├── cond.h              # 条件变量
│       ├── once.h              # 一次性初始化
│       ├── timer_thread.h      # 定时器线程
│       └── execution_queue.h   # 执行队列
├── src/
│   ├── bthread.cpp             # 公共 API 实现
│   ├── task_meta.cpp
│   ├── task_group.cpp
│   ├── work_stealing_queue.cpp
│   ├── global_queue.cpp
│   ├── worker.cpp
│   ├── scheduler.cpp
│   ├── butex.cpp
│   ├── mutex.cpp
│   ├── cond.cpp
│   ├── once.cpp
│   ├── timer_thread.cpp
│   ├── execution_queue.cpp
│   └── platform/
│       ├── platform.cpp
│       ├── platform_windows.cpp
│       └── context_windows_x64.asm # x86-64 上下文切换汇编
├── tests/
│   └── *_test.cpp              # 单元测试
└── CMakeLists.txt
```

## 已知限制

1. **Linux 版本** - 当前主要在 Windows 上测试，Linux 版本需要 pthread 支持
2. **32位系统** - 仅支持 64 位架构 (x86_64)
3. **测试覆盖** - 部分测试文件需要更新以匹配最新 API

## 常见问题

### Q: 编译时找不到 cmake 命令?

A: 使用 Visual Studio 附带的 CMake 完整路径，或者将以下路径添加到 PATH:

```
C:/Program Files/Microsoft Visual Studio/2022/Community/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/
```

### Q: 链接错误 "无法解析的外部符号 SwapContext"?

A: 确保 CMake 配置时启用了 MASM 汇编器:
```cmake
enable_language(ASM_MASM)
```

### Q: 链接错误 "WaitOnAddress 无法解析"?

A: 需要链接 `synchronization.lib`:
```cmake
target_link_libraries(your_target PRIVATE synchronization)
```

## 测试结果

当前通过的测试:

| 测试名称 | 状态 | 说明 |
|---------|------|------|
| butex_test | ✅ 通过 | Butex 同步原语测试 |
| cond_test | ✅ 通过 | 条件变量测试 |
| global_queue_test | ✅ 通过 | 全局任务队列测试 |
| task_group_test | ✅ 通过 | 任务池管理测试 |
| work_stealing_queue_test | ✅ 通过 | 工作窃取队列测试 |