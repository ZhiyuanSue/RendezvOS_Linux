# Syscall 实现路线图

## 🎯 阶段总览

> **📅 2026-04-25**: Phase 1 已完成！详见 [Phase 1 总结文档](PHASE1_SUMMARY.md)

| 阶段 | 状态 | 目标 | 主要syscall |
|------|------|------|-------------|
| **Phase 0** | ✅ 完成 | 基础设施 | syscall框架、write shim |
| **Phase 1** | ✅ **完成** | **进程与内存管理基础** | fork, exit, wait4, getpid, brk, mmap, mprotect, munmap, mremap |
| **Phase 2A** | ✅ **完成** | **线程控制** | clone, set_tid_address, set_robust_list |
| **Phase 2B** | ✅ **完成** | **信号排队机制** | rt_sigaction, rt_sigprocmask, kill |
| **Phase 2C** | ✅ **完成** | **信号投递机制** | rt_sigreturn, sigaltstack, 信号处理器调用 |
| **Phase 2D** | ✅ **完成** | **缺页信号处理** | SIGSEGV信号投递, 用户空间错误处理 |
| **Phase 3** | 📋 计划中 | 程序执行 | execve |
| **Phase 4** | 📋 计划中 | 文件系统 | open, close, read, write, stat |
| **Phase 5** | 📋 计划中 | 高级功能 | IPC, socket, 时间等 |

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

## ✅ Phase 2：线程控制与信号机制（已完成）

**完成时间**: 2026-05-17
**测试状态**: 双架构编译验证通过，信号处理机制完善

### Phase 2A：线程控制（已完成）

**完成时间**: 2026-05-15

**主要内容**:
- 线程创建：clone支持CLONE_VM, CLONE_THREAD, CLONE_SIGHAND等标志
- 线程ID管理：set_tid_address, set_robust_list
- 信号处理器共享：CLONE_SIGHAND支持
- 栈指针设置：架构特定的用户栈设置

**里程碑达成**:
- ✅ 支持pthread_create基础功能
- ✅ 正确处理共享vs独立地址空间
- ✅ 支持线程本地存储（TLS）基础
- ✅ 双架构支持（x86_64 + aarch64）

### Phase 2B：信号排队机制（已完成）

**完成时间**: 2026-05-16

**主要内容**:
- 信号排队：linux_queue_signal支持标准信号和实时信号
- 信号处理器设置：rt_sigaction支持SA_RESETHAND, SA_NODEFER等标志
- 信号掩码：rt_sigprocmask支持SIG_BLOCK/SIG_UNBLOCK/SIG_SETMASK
- 信号发送：kill/tgkill系统调用
- SIGKILL/SIGSTOP特殊处理：不可阻塞的强制信号

**里程碑达成**:
- ✅ 两层信号模型：排队（per-thread）+ 投递（trap路径）
- ✅ 支持完整的Linux信号标志语义
- ✅ 正确处理SIGKILL/SIGSTOP不可阻塞语义
- ✅ 信号掩码清理机制自动执行

### Phase 2C：信号投递机制（已完成）

**完成时间**: 2026-05-17

**主要内容**:
- 信号投递：linux_deliver_pending_signals在syscall返回路径调用
- 默认信号动作：Term/Ign/Core/Stop四类动作完整实现
- 用户处理器：构建信号帧，修改trap_frame，调用用户处理器
- 备用信号栈：sigaltstack支持SA_ONSTACK，正确处理栈切换
- rt_sigreturn：从信号处理器返回，恢复上下文
- 架构特定返回路径：x86_64 (sysretq) 和 aarch64 (eret) 支持

**里程碑达成**:
- ✅ 完整的默认信号动作实现（符合Linux语义）
- ✅ 支持用户自定义信号处理器
- ✅ 备用信号栈防止栈溢出
- ✅ 正确的信号上下文保存和恢复
- ✅ 双架构信号返回路径实现

### Phase 2D：缺页信号处理（已完成）

**完成时间**: 2026-05-17

**主要内容**:
- SIGSEGV信号投递：用户空间缺页错误处理
- 与COW集成：写故障时分裂页面或投递SIGSEGV
- 与lazy allocation集成：按需分配页面或投递SIGSEGV
- 错误分类：NULL指针、真正的segfault、权限错误

**里程碑达成**:
- ✅ 用户空间错误正确投递SIGSEGV
- ✅ 与COW和lazy allocation协同工作
- ✅ 内核错误导致kernel_panic
- ✅ 支持fatal_user_fault机制

### 超出原目标的改进 ⭐

**进程生命周期增强**:
- ✅ clear_tid机制：支持线程库的futex等待
- ✅ SIGCHLD自动通知：子进程退出时自动通知父进程
- ✅ exit_group系统调用：杀死进程的所有线程
- ✅ SA_NOCLDWAIT支持：避免产生僵尸进程

**代码质量改进**:
- ✅ 用户空间访问机制简化：直接使用linux_mm_store_to_user/load_from_user
- ✅ 架构特定代码隔离：使用条件编译和helper函数
- ✅ 错误处理完善：所有用户空间访问都有错误检查
- ✅ 注释详细清晰：关键逻辑都有解释性注释

**详细审阅报告**: [Cursor技术改进审阅报告](CURSOR_IMPROVEMENTS_REVIEW.md)

---

## 📋 Phase 3：程序执行（计划中）

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

