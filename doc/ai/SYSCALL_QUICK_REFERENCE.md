# Syscall 实现快速参考

## 当前状态（2026-05-17）

### 已实现：16个syscall

| 类别 | Syscall | 质量 | 文件 |
|------|---------|------|------|
| **进程控制** | fork | ⭐⭐⭐⭐⭐ 10/10 | `linux_layer/proc/sys_fork.c` |
| **进程控制** | getpid | ⭐⭐⭐⭐⭐ 10/10 | `linux_layer/proc/sys_proc.c` |
| **进程控制** | gettid | ⭐⭐⭐⭐⭐ 10/10 | `linux_layer/proc/sys_proc.c` |
| **进程控制** | getppid | ⭐⭐⭐⭐⭐ 10/10 | `linux_layer/proc/sys_proc.c` |
| **进程控制** | exit | ⭐⭐⭐⭐ 8/10 | `linux_layer/syscall/thread_syscall.c` |
| **进程控制** | exit_group | ⭐⭐⭐⭐ 8/10 | `linux_layer/syscall/thread_syscall.c` |
| **线程控制** | clone | ⭐⭐⭐ 7/10 | `linux_layer/proc/sys_clone.c` |
| **线程控制** | set_tid_address | ⭐⭐⭐⭐ 8/10 | `linux_layer/proc/sys_set_tid_address.c` |
| **线程控制** | set_robust_list | ⭐⭐⭐ 6/10 | `linux_layer/proc/sys_set_robust_list.c` |
| **等待** | wait4 | ⭐⭐⭐⭐ 8/10 | `linux_layer/proc/sys_wait.c` |
| **等待** | waitpid | ⭐⭐⭐⭐ 8/10 | `linux_layer/proc/sys_wait.c` |
| **内存管理** | brk | ⭐⭐⭐⭐ 9/10 | `linux_layer/mm/sys_brk.c` |
| **内存管理** | mprotect | ⭐⭐⭐⭐ 9/10 | `linux_layer/mm/sys_mprotect.c` |
| **内存管理** | mmap | ⭐⭐⭐⭐ 8/10 | `linux_layer/mm/sys_mmap.c` |
| **内存管理** | munmap | ⭐⭐⭐ 7/10 | `linux_layer/mm/sys_munmap.c` |
| **内存管理** | mremap | ⭐⭐⭐ 6/10 | `linux_layer/mm/sys_mremap.c` |
| **I/O** | write | ⭐⭐⭐ 6/10 | `linux_layer/io/sys_write.c` |

### Phase 1 完成 ✅（2026-04-25）

**核心进程和内存管理**：
- fork, exit, exit_group, getpid, gettid, getppid, wait4, waitpid
- brk, mmap, munmap, mprotect, mremap
- write (I/O shim)

**质量**：基础实现完整，COW机制已集成

### Phase 2A 完成 ✅（2026-05-17）

**线程控制支持（pthread库基础）**：
- clone：支持线程创建（CLONE_VM|CLONE_THREAD等）
- set_tid_address：pthread退出通知机制
- set_robust_list：pthread robust futex支持

**质量**：基础线程创建功能实现，待完善TLS和用户内存写入

### 下一步（P0 - Phase 2B/2C）

**Phase 2B - 信号机制**（`linux_layer/proc/sys_*.c` 已有骨架，**尚未**接入 `syscall_entry.c`）：
- rt_sigaction, rt_sigprocmask, rt_sigreturn
- kill/tgkill, sigaltstack
- 设计见 [`doc/linux_compat/IPC_BASED_SIGNAL_DESIGN.md`](../linux_compat/IPC_BASED_SIGNAL_DESIGN.md)（v1.1：IPC 辅助 + syscall 出口投递，非纯 IPC 服务器）

**Phase 2C - 文件系统基础**：
- VFS + initramfs
- open/close/read/write/lseek/stat/fstat/getcwd

### 当前clone实现的限制

1. **TLS支持**：CLONE_SETTLS占位实现（架构特定）
2. **用户内存写入**：CLONE_PARENT_SETTID/CLONE_CHILD_SETTID待实现
3. **共享资源**：CLONE_FS/CLONE_FILES/CLONE_SIGHAND占位（Phase 2B/2C）
4. **命名空间**：不支持CLONE_NEW*标志

### 架构说明

- **线程vs进程**：CLONE_VM区分线程（共享地址空间）和进程（独立地址空间）
- **栈管理**：CLONE_VM要求用户提供新栈（栈向下增长，stack参数指向栈顶）
- **引用计数**：共享VSpace时不增加refcount，独立VSpace时在错误路径需ref_put

## 依赖core/的新增能力

### 地址空间管理
- `clone_vspace(..., VSPACE_CLONE_F_COW_PREP)` - COW 地址空间克隆（`linux_copy_vspace()` 封装）
- `vmm_radix_tree_change_range_flags()` - 批量flags更新

### 物理内存管理
- `pmm_change_pages_ref()` - 引用计数管理+回滚
- `map_handler_copy_paddr_range()` - 物理页拷贝

### 执行流控制
- `copy_thread()` - 线程拷贝（包括trap_frame）
- `run_copied_thread()` - 子线程启动

## 详细文档

- **完整实现记录**：`doc/ai/SYSCALL_IMPLEMENTATION_STATUS.md`
- **路线图**：`doc/linux_compat/SYSCALLS.md`
- **架构设计**：`doc/linux_compat/ARCHITECTURE.md`
