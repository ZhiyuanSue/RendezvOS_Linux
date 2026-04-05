# 数据模型：进程、线程、append 区

## 1. 与 Linux 概念的映射

| Linux | RendezvOS（初期） | 说明 |
|-------|-------------------|------|
| 调度实体 `task_struct` | `Thread_Base` | `tid`、`ctx`、IPC 队列 |
| 线程组 / 进程 `tgid` | `Tcb_Base` + 未来 `tgid` 字段 | 初期单线程进程：`getpid` = `Tcb_Base::pid` |
| 内存描述 `mm_struct` | `Tcb_Base::vs` → `VS_Common` | nexus 挂在 vspace |
| 子进程、wait | `linux_proc_registry`（阶段 1）或 `proc_coordinator`（阶段 2） | 见下文 |

## 2. append 区（单一真源）

**约束**：不在 [`core/include/rendezvos/task/tcb.h`](../../core/include/rendezvos/task/tcb.h) 的 `TCB_COMMON` / `THREAD_COMMON` 里加入 Linux 专用字段。

建议在 **`linux_layer`** 新增头文件（名称待定，如 `linux_layer/include/proc_compat.h` 或 `include/linux_compat/proc_compat.h`）：

- `struct linux_proc_append` — 放入 `Tcb_Base.append_tcb_info[]`
- `struct linux_thread_append` — 放入 `Thread_Base.append_thread_info[]`
- `LINUX_PROC_APPEND_BYTES` / `LINUX_THREAD_APPEND_BYTES` — **全仓库唯一宏**，与 `new_task_structure`、`new_thread_structure`、`gen_task_from_elf`、`create_thread` 的 append 长度参数一致。

### 2.1 `linux_proc_append`（建议字段，分阶段落地）

| 字段 | 阶段 | 目的 |
|------|------|------|
| `start_brk`, `brk` | P0 | `brk()` 与 ELF 初始 brk 对齐 |
| `tgid` | P1 | 多线程时 `getpid`≠`gettid` |
| `parent_pid` / `parent_tcb` 弱引用 | P1 | `wait4`、信号投递 |
| `exit_signal` / `clone` 相关 | P2 | `clone` flags |

**不**在此结构保存「VMA 根指针」：用户映射由 **`vs` 的 nexus** 表达。

### 2.2 `linux_thread_append`（建议字段）

| 字段 | 阶段 | 目的 |
|------|------|------|
| `clear_child_tid` 用户 VA | P1 | `set_tid_address` / futex 类（后期） |
| 每线程 `sigmask` | P2 | 与 `rt_sigprocmask` 对齐 |

## 3. 进程登记簿（proc registry）

**阶段 1（推荐先实现）**

- 新文件建议：`linux_layer/proc/proc_registry.c` + `proc_registry.h`（名称可调整）。
- 数据结构：`pid` → `Tcb_Base*` 或 `struct proc_desc`（含 `zombie`、`exit_code`、`parent`、兄弟链表）。
- 同步：**一把 `cas_lock` 或 `spin_lock`** 保护注册、注销、`wait` 扫描。
- **目的**：在 **不增加 server** 的情况下完成 `wait4`/`exit` 父子交互；与 `ARCHITECTURE.md` 中「先锁后 IPC」一致。

**阶段 2（可选）**

- 将 **同一套操作** 移到 **`proc_coordinator`** 内核线程；syscall 侧 **send 请求 / recv 回复**（或共享内存槽位 + 序列号，仍由 coordinator 顺序处理）。
- **目的**：锁序简化、与无锁 IPC 哲学一致；**不改变** Linux 语义，只改变实现位置。

## 4. 与 `clean_server` 的关系

- 线程物理回收仍在 [`servers/clean_server.c`](../../servers/clean_server.c)（或等价路径）。
- **进程级** `exit_group`：最后一线程才 `delete_task`；在 **登记簿** 上标记 **zombie 进程**、写 `exit_code`、唤醒阻塞在 `wait4` 的父线程——可在 **linux_layer** 内由 `sys_exit` / clean_server 回调钩子完成（具体见 `SYSCALLS.md`）。
