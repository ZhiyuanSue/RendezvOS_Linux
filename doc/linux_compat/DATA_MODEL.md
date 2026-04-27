# 数据模型：进程、线程、append 区

> **📅 阶段状态**：
> - ✅ **Phase 1 完成**：基础的进程/线程数据模型、proc_registry、append区
> - 📋 **后续阶段**：多线程支持（tgid）、clone相关字段

## 1. 与 Linux 概念的映射

| Linux | RendezvOS（Phase 1） | 说明 |
|-------|-------------------|------|
| 调度实体 `task_struct` | `Thread_Base` | `tid`、`ctx`、IPC 队列 |
| 线程组 / 进程 `tgid` | `Tcb_Base::pid`（初期单线程） | ✅ Phase 1：单线程进程 `getpid` = `Tcb_Base::pid`<br/>📋 后续：多线程需要 `tgid` 字段 |
| 内存描述 `mm_struct` | `Tcb_Base::vs` → `VS_Common` | nexus 挂在 vspace |
| 子进程、wait | `proc_registry`（✅ Phase 1完成） | O(1) PID查找，支持wait4 |

## 2. append 区（单一真源）

**约束**：不在 [`core/include/rendezvos/task/tcb.h`](../../core/include/rendezvos/task/tcb.h) 的 `TCB_COMMON` / `THREAD_COMMON` 里加入 Linux 专用字段。

建议在 **`linux_layer`** 新增头文件（名称待定，如 `linux_layer/include/proc_compat.h` 或 `include/linux_compat/proc_compat.h`）：

- `struct linux_proc_append` — 放入 `Tcb_Base.append_tcb_info[]`
- `struct linux_thread_append` — 放入 `Thread_Base.append_thread_info[]`
- `LINUX_PROC_APPEND_BYTES` / `LINUX_THREAD_APPEND_BYTES` — **全仓库唯一宏**，与 `new_task_structure`、`new_thread_structure`、`gen_task_from_elf`、`create_thread` 的 append 长度参数一致。

### 2.1 `linux_proc_append`（Phase 1 已实现字段）

| 字段 | 状态 | 阶段 | 目的 |
|------|------|------|------|
| `start_brk`, `brk` | ✅ 完成 | P1 | `brk()` 与 ELF 初始 brk 对齐 |
| `ppid` | ✅ 完成 | P1 | `getppid()`、进程树查询 |
| `pgid` | ✅ 完成 | P1 | 进程组，wait4进程组语义 |
| `exit_code` | ✅ 完成 | P1 | 进程退出码 |
| `exit_state` | ✅ 完成 | P1 | 进程状态（running/zombie/reaped） |
| `tgid` | 📋 未实现 | 后续 | 多线程时 `getpid`≠`gettid` |
| `exit_signal` / `clone` 相关 | 📋 未实现 | 后续 | `clone` flags |

**不**在此结构保存「VMA 根指针」：用户映射由 **`vs` 的 nexus** 表达。

### 2.2 `linux_thread_append`（建议字段）

| 字段 | 阶段 | 目的 |
|------|------|------|
| `clear_child_tid` 用户 VA | P1 | `set_tid_address` / futex 类（后期） |
| 每线程 `sigmask` | P2 | 与 `rt_sigprocmask` 对齐 |

## 3. 进程登记簿（proc registry）

> **状态**: ✅ Phase 1 完成（采用阶段1方案：锁保护）

**Phase 1 实现**（已完成）：

- **实现文件**：`linux_layer/proc/proc_registry.c` + `include/linux_compat/proc_registry.h`
- **数据结构**：`pid` → `Tcb_Base*` 映射，基于core的name_index实现O(1)查找
- **扩展查询**：支持反向查询（ppid、pgid查找），为wait4提供支持
- **同步机制**：使用core的锁机制保护注册、注销
- **功能**：
  - `register_process()` - 注册进程
  - `unregister_process()` - 注销进程
  - `find_task_by_pid()` - O(1) PID查找
  - `find_zombie_child()` - 查找zombie子进程（wait4支持）
  - `find_zombie_child_in_pgid()` - 进程组查找（wait4支持）
- **目的**：完成 `wait4`/`exit` 父子交互，与 `ARCHITECTURE.md` 中「先锁后 IPC」一致
- **测试验证**: ✅ 所有wait4测试通过，O(1)查找性能正常

**阶段 2**（可选，暂不需要）：

- 将 **同一套操作** 移到 **`proc_coordinator`** 内核线程；syscall 侧 **send 请求 / recv 回复**。
- **目的**：锁序简化、与无锁 IPC 哲学一致；**不改变** Linux 语义，只改变实现位置。

## 4. 与 `clean_server` 的关系

> **状态**: ✅ Phase 1 完成

- **线程物理回收**：仍在 [`servers/clean_server.c`](../../servers/clean_server.c)。
- **进程级** `exit_group`：
  - ✅ 最后一线程才 `delete_task`
  - ✅ 在 `proc_registry` 上标记 `exit_state=2`（reaped）
  - ✅ 写 `exit_code`
  - ✅ 唤醒阻塞在 `wait4` 的父线程（IPC通知）
  - ✅ 竞态条件修复：exit_state三态管理防止clean_server过早删除
- **实现**：在 **linux_layer** 内由 `sys_exit` 完成，clean_server检查exit_state
- **测试验证**: ✅ 所有exit/wait4测试通过，无竞态问题
