# Syscall 实现路线图

## 🎯 阶段总览

> **📅 2026-04-25**: Phase 1 已完成！详见 [Phase 1 总结文档](archive/PHASE1_SUMMARY.md)  
> **📅 2026-05-19**: Phase 2 **cross-arch gate** — x86_64 + aarch64 52/52; 见 [`CROSS_ARCH_VERIFICATION_LOG.md`](CROSS_ARCH_VERIFICATION_LOG.md)  
> **📅 2026-06-13**: Phase 2E + 3.5 + core DAIF/sleep — 双架构 52/52；time #16–#20 PASS；**下一步 Phase 4 VFS**  
> **追溯索引**: [`PROGRESS.md`](PROGRESS.md) — 当前缺口与文档链

| 阶段 | 状态 | 目标 | 主要syscall |
|------|------|------|-------------|
| **Phase 0** | ✅ 完成 | 基础设施 | syscall框架、write shim |
| **Phase 1** | ✅ **完成** | **进程与内存管理基础** | fork, exit, wait4, getpid, brk, mmap, mprotect, munmap, mremap |
| **Phase 2A** | ✅ **完成** | **线程控制** | clone, set_tid_address, set_robust_list |
| **Phase 2B** | ✅ **完成** | **信号排队机制** | rt_sigaction, rt_sigprocmask, kill |
| **Phase 2C** | ✅ **完成** | **信号投递机制** | rt_sigreturn, sigaltstack, 信号处理器调用 |
| **Phase 2D** | ✅ **完成** | **缺页信号处理** | SIGSEGV信号投递, 用户空间错误处理 |
| **Phase 3** | 🔧 进行中 | 程序执行 | execve（3a 内嵌 ELF 部分完成） |
| **Phase 3.5** | ✅ **完成** | 时间 | gettimeofday, nanosleep, times(stub), clock_gettime, uname |
| **Phase 4** | 📋 **下一步** | 文件系统 | open, close, read, pipe, mount, … |
| **Phase 5** | 📋 待做 | 高级功能 | socket, rlimit, 完整 IPC |

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

**详细文档**: [Phase 1 总结文档](archive/PHASE1_SUMMARY.md)

---

## ✅ Phase 2：线程控制与信号机制（已完成）

**完成时间**: 2026-05-17 (features) · **cross-arch gate**: 2026-05-19  
**测试状态**: x86_64 + aarch64 **52/52 harness PASS**; proc/signal/wait key tests aligned  
**验证日志**: [`CROSS_ARCH_VERIFICATION_LOG.md`](CROSS_ARCH_VERIFICATION_LOG.md)

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

**代码质量**: 见 Phase 2 status 文档与 [`CROSS_ARCH_VERIFICATION_LOG.md`](CROSS_ARCH_VERIFICATION_LOG.md)。

---

## Cross-arch verification log (maintainer)

After Phase 2 proc/signal/wait work, record **paired** runs here:

| Date | Gate | Result | Log |
|------|------|--------|-----|
| 2026-05-19 | Phase 1/2 wait+signal+fork | ✅ x86_64 + aarch64 52/52; #07/#08/#39/#41/#44/#49 stdout parity | [`CROSS_ARCH_VERIFICATION_LOG.md`](CROSS_ARCH_VERIFICATION_LOG.md) §2026-05-19 |
| 2026-06-13 | Phase 2E + 3.5 time + core DAIF/sleep | ✅ 52/52 harness both arches; #16–#21/#52 PASS; #49 stdout regression noted | [`CROSS_ARCH_VERIFICATION_LOG.md`](CROSS_ARCH_VERIFICATION_LOG.md) §2026-06-13 |

**Rule**: Harness PASS is necessary but not sufficient — check #49 `Test Summary` before `[TEST 49/52] PASS`, and #44 `SIG_IGN` subtest.

---

## 🔧 Phase 3：程序执行（进行中）

**状态文档**: [`EXECVE_IMPLEMENTATION_STATUS.md`](EXECVE_IMPLEMENTATION_STATUS.md)  
**设计**: [`SYSCALL_USER_RETURN_AND_EXECVE.md`](SYSCALL_USER_RETURN_AND_EXECVE.md)

| 子阶段 | 内容 | 状态 |
|--------|------|------|
| 3a | 单线程、内嵌 ELF、argv | ✅ 部分（5 个镜像名） |
| 3b | de_thread、TLB、多线程 exec | ❌ |
| 3c | envp、auxv、完整 post-exec 重置 | ❌ |
| 3d | FS 读 ELF、shebang、PT_INTERP | ❌（依赖 Phase 4） |

**Syscall**: `execve`

---

## 📋 Phase 3.5：时间（已完成）

**计划**: [`TIME_SUBSYSTEM_PLAN.md`](TIME_SUBSYSTEM_PLAN.md)  
**完成**: 2026-06（compat-only，无 core 变更）

| Syscall | 测例 # | 状态 |
|---------|--------|------|
| `gettimeofday` | 16 | ✅ |
| `nanosleep` / `clock_nanosleep` | 17 | ✅ |
| `times` | 20 | ✅ stub（CPU 时间未计） |
| `clock_gettime` | — | ✅ |
| `uname` | 19 | ✅ |

**未做 / 暂缓**: `settimeofday`（可选）；`times` 真实 utime/stime；vDSO（无 FS/动态链接前不做）。

---

## 📋 Phase 4：文件系统（当前下一步）

**设计**: [`VFS_SERVER_IPC.md`](VFS_SERVER_IPC.md) · **initramfs 方案**: [`INITRAMFS_PLAN.md`](INITRAMFS_PLAN.md)

| Syscall 组 | 示例 | 测例 stdout FAIL |
|------------|------|------------------|
| fd / IO | open, openat, read, close, dup, dup2 | #10–#11, #23, #25–#26, #37, #50–#51 |
| path | mkdir, chdir, unlink, mount | #15, #28, #45, #48 |
| stat | fstat, getdents | #14, #25 |
| 文件 mmap | MAP_PRIVATE file | #13, #40（依赖 open） |

**里程碑**: initramfs 可读 → execve 3d → 消除 ~14 个 FS stdout FAIL。

---

## 📋 Phase 5：高级功能

- socket、shm、msgq、futex 完整语义
- rlimit、uid/gid
- 更多 arch（riscv64 timer 等）

详细 syscall 清单与当前缺口见 [`PROGRESS.md`](PROGRESS.md)。
