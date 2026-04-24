# Syscall 实现快速参考

## 当前状态（2026-04-21）

### 已实现：11个syscall

| 类别 | Syscall | 质量 | 文件 |
|------|---------|------|------|
| **进程控制** | fork | ⭐⭐⭐⭐⭐ 10/10 | `linux_layer/proc/sys_fork.c` |
| **进程控制** | getpid | ⭐⭐⭐⭐⭐ 10/10 | `linux_layer/proc/sys_proc.c` |
| **进程控制** | exit | ⭐⭐⭐⭐ 8/10 | `linux_layer/syscall/thread_syscall.c` |
| **进程控制** | exit_group | ⭐⭐⭐⭐ 8/10 | `linux_layer/syscall/thread_syscall.c` |
| **内存管理** | brk | ⭐⭐⭐⭐ 9/10 | `linux_layer/mm/sys_brk.c` |
| **内存管理** | mprotect | ⭐⭐⭐⭐ 9/10 | `linux_layer/mm/sys_mprotect.c` |
| **内存管理** | mmap | ⭐⭐⭐⭐ 8/10 | `linux_layer/mm/sys_mmap.c` |
| **内存管理** | munmap | ⭐⭐⭐ ?/10 | `linux_layer/mm/sys_munmap.c` |
| **内存管理** | mremap | ⭐⭐⭐ 6/10 | `linux_layer/mm/sys_mremap.c` |
| **I/O** | write | ⭐⭐⭐ 6/10 | `linux_layer/io/sys_write.c` |

### 第一步最小子集（5个核心syscall）

✅ **已完成**：
- exit, fork, getpid, write, brk

### 下一步（P0 - 本周）

1. **改进write**（6/10 → 8/10）
   - 使用用户内存验证机制
   - 替代直接memcpy

2. **实现getppid**
   - 简单进程信息查询

3. **实现COW页错误处理**
   - 完善fork的COW机制

4. **改进mremap**（6/10 → 8/10）
   - 使用`map_handler_copy_paddr_range()`

### 短期（P1 - 2周）

- wait4/waitpid
- read（伪实现）
- open/close（伪实现）

## 依赖core/的新增能力

### 地址空间管理
- `vspace_clone(VSPACE_CLONE_F_COW_PREP)` - COW地址空间克隆
- `nexus_update_range_flags()` - 批量flags更新

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
