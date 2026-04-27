# Syscall 实现路线图

## 🎯 阶段总览

> **📅 2026-04-25**: Phase 1 已完成！详见 [Phase 1 总结文档](PHASE1_SUMMARY.md)

| 阶段 | 状态 | 目标 | 主要syscall |
|------|------|------|-------------|
| **Phase 0** | ✅ 完成 | 基础设施 | syscall框架、write shim |
| **Phase 1** | ✅ **完成** | **进程与内存管理基础** | fork, exit, wait4, getpid, brk, mmap, mprotect, munmap, mremap |
| **Phase 2** | 📋 计划中 | 程序执行 | execve |
| **Phase 3** | 📋 计划中 | 线程控制 | clone |
| **Phase 4** | 📋 计划中 | 信号机制 | signal, sigaction, sigprocmask |
| **Phase 5** | 📋 计划中 | 文件系统 | open, close, read, write, stat |
| **Phase 6** | 📋 计划中 | 高级功能 | IPC, socket, 时间等 |

## 目标和策略

**总体目标**：实现200-300+ Linux syscall，支持x86_64、aarch64、riscv64等多架构。

**实现策略**：
- **迭代式开发**：分阶段实现，每个阶段以测例为导向
- **灵活优先级**：根据测例需求和依赖关系调整实现顺序
- **多架构同步**：从设计阶段考虑多架构兼容性
- **质量优先**：每个阶段确保稳定性和正确性

---

## ✅ Phase 1：进程与内存管理基础（已完成）

**完成时间**: 2026-04-25
**测试状态**: 11/11 测试通过，包括用户态多进程测试

**主要内容**:
- 进程管理：fork, exit, exit_group, wait4, waitpid, getpid, gettid, getppid
- 内存管理：brk, mmap, munmap, mprotect, mremap
- 核心基础设施：proc_registry, IPC阻塞机制, 进程组支持, COW机制

**里程碑达成**:
- ✅ 能够创建和管理进程，父子进程能够同步
- ✅ 支持完整的Linux标准wait4语义（所有pid选项 + WNOHANG）
- ✅ 支持动态内存管理（mmap/munmap/mprotect/mremap）
- ✅ 用户态多进程程序正常运行
- ✅ 测试框架支持多核可扩展性

**详细文档**: [Phase 1 总结文档](PHASE1_SUMMARY.md)

---

## 📋 Phase 2：进程控制完善 + 文件系统基础（计划中）

**目标**：完善进程控制能力，并建立文件系统基础，能够运行更复杂的用户程序

### Phase 2A：线程控制（clone）

**目标**：支持pthread库

| Syscall | 功能 | 优先级 |
|---------|------|--------|
| `clone` | 创建线程/进程 | ⭐⭐⭐ 核心 |
| `set_tid_address` | 设置线程ID | ⭐⭐ pthread需要 |
| `set_robust_list` | robust futex列表 | ⭐ 后续 |

**实现要点**：
- `clone`支持基础flags（CLONE_VM, CLONE_FS, CLONE_FILES等）
- 共享地址空间 vs 独立地址空间
- 线程本地存储（TLS）基础

**依赖**：Phase 1（进程管理、内存管理）✅

**测试验证**：
- 简单clone测试（共享地址空间）
- pthread_create基础功能

**里程碑**：支持pthread库基础功能

### Phase 2B：信号机制

**目标**：支持基本的信号语义

| Syscall | 功能 | 优先级 |
|---------|------|--------|
| `rt_sigaction` | 设置信号处理函数 | ⭐⭐⭐ 核心 |
| `rt_sigprocmask` | 信号掩码 | ⭐⭐⭐ 核心 |
| `kill` / `tgkill` | 发送信号 | ⭐⭐⭐ 核心 |
| `sigaltstack` | 信号栈 | ⭐⭐ 栈溢出保护 |
| `rt_sigreturn` | 从信号处理返回 | ⭐⭐⭐ 核心 |

**实现要点**：
- 信号递送机制（基于IPC的异步通知）
- 信号处理函数调用（构造trap frame）
- 信号掩码管理
- 基础信号（SIGKILL, SIGTERM, SIGCHLD等）

**依赖**：Phase 1（进程管理、IPC机制）✅

**测试验证**：
- kill + 信号处理函数
- sigprocmask阻塞/解除阻塞
- SIGCHLD（子进程退出通知）

**里程碑**：支持基本的信号语义

### Phase 2C：文件系统基础（VFS + initramfs）

**目标**：建立文件系统基础，支持基本的文件操作

**架构设计**：
- **VFS Server**（单线程，全局唯一）
  - 统一的VFS层
  - 文件描述符管理
  - 块设备缓存（统一的页缓存）
  - 挂载点管理
- **initramfs驱动**（cpio格式）
  - 打包在内核镜像中
  - 启动时解压并挂载到根目录
  - 用于测试和execve
- **块设备层**（简化，无真实磁盘）
  - 内存块设备
  - 暂不实现真实磁盘驱动

**实现的Syscall**：

| Syscall | 功能 | 优先级 |
|---------|------|--------|
| `open` / `openat` | 打开文件 | ⭐⭐⭐ 核心 |
| `close` | 关闭文件 | ⭐⭐⭐ 核心 |
| `read` | 读文件 | ⭐⭐⭐ 核心 |
| `write` | 写文件 | ⭐⭐⭐ 核心 |
| `lseek` | 文件定位 | ⭐⭐ 重要 |
| `stat` / `fstat` | 文件信息 | ⭐⭐ 重要 |
| `getcwd` | 当前目录 | ⭐ 基础 |

**代码组织**：
```
servers/fs/
├── vfs_server.c          # VFS server主循环
├── vfs.c                  # VFS核心逻辑
├── vfs_cache.c            # 块设备缓存
├── file_table.c           # 文件描述符表
├── mount.c                # 挂载点管理
└── initramfs/             # initramfs支持
    ├── initramfs.c        # initramfs驱动
    ├── cpio.c             # cpio格式解析
    └── cpio.h
```

**测试策略**：
1. **cpio打包工具**：将测试文件打包成cpio格式
2. **initramfs测试**：从内存文件系统读取文件
3. **集成测试**：用户程序通过文件系统加载配置

**IPC机制说明**：
- 使用现有无锁IPC基础设施
- VFS server作为单线程server运行
- 支持批处理和Token缓存

**里程碑**：
- ✅ 能够从initramfs读取文件
- ✅ 支持基本的文件操作
- ✅ 为execve奠定基础

---

## 📋 Phase 3：程序执行（计划中）

**目标**：能够运行shell和不同的用户程序

| 组 | syscall | 依赖 |
|----|---------|------|
| 程序加载 | execve | Phase 2（文件系统） |
| 环境变量 | execve相关 | - |

**里程碑**：能够运行shell和复杂的用户程序

---

## 📋 Phase 4：高级功能（计划中）

**目标**：实现更多Linux功能

**功能组**：
- **IPC**：pipe, socket, shm, msgq
- **时间**：clock相关syscall
- **资源限制**：getrlimit, setrlimit
- **用户/组**：getuid, setuid等

**里程碑**：逐步接近完整的Linux兼容性

---

## 📋 Phase 5：文件系统（计划中）

**目标**：实现基本的文件系统访问

| 组 | syscall | 依赖 |
|----|---------|------|
| 文件描述符 | open, close, read, write | - |
| 文件操作 | lseek, stat, fstat | - |
| 目录操作 | getdents | - |

**里程碑**：支持基本的文件操作

---

## 📋 Phase 6：高级功能（计划中）

**目标**：实现更多Linux功能

**功能组**：
- **IPC**：pipe, socket, shm, msgq
- **时间**：clock相关syscall
- **资源限制**：getrlimit, setrlimit
- **用户/组**：getuid, setuid等

**里程碑**：逐步接近完整的Linux兼容性

