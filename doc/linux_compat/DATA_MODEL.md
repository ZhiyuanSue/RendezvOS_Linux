# 数据模型：进程、线程、append 区

> **阶段状态**
> - ✅ Phase 1：资源束 + proc_registry + thread append
> - 📋 后续：多线程 `tgid`、clone 扩展字段

## 1. 与 Linux 概念的映射

| Linux | RendezvOS | 说明 |
|-------|-----------|------|
| 调度实体 `task_struct` | `Thread_Base` | `tid`、`ctx`、IPC、`thread->vs` |
| 线程组 / 进程 `tgid` | 堆上 **`linux_proc_resource_t`**（`pid`） | 多线程经 append **`ta->res`** 共享 |
| 内存描述 `mm_struct` | `Thread_Base::vs` → `VSpace` | Radix 在 vspace；`proc->vs` 仅为非拥有缓存 |
| 子进程、wait | `proc_registry` | O(1) `pid` → `linux_proc_resource_t*` |

core **没有**进程对象；`linux_proc_resource_t` 是 **共享资源束**（pid / brk / fs / signal / wait），不是 TCB 替身。

## 2. 两类结构（单一真源）

定义：[`include/linux_compat/proc_compat.h`](../../include/linux_compat/proc_compat.h)

| 结构 | 分配 | 与线程关系 |
|------|------|------------|
| **`linux_proc_resource_t`** | `linux_proc_alloc()`（堆） | 若干线程 `linux_proc_attach_thread` → `ref_get` |
| **`linux_thread_append_t`** | core `Thread_Base.append_thread_info[]` | 每线程一份；`ta->res` 指向资源束 |

**约束**：不在 core `THREAD_COMMON` 里塞 Linux 字段；`create_thread` 只传 hook 表指针（`LINUX_THREAD_APPEND_BYTES`）。

### 2.1 `linux_proc_resource_t`（资源束）

| 字段 | 说明 |
|------|------|
| `refcount` | alloc ref + 每 attach 一线程一票 |
| `pid` / `ppid` / `pgid` | 进程关系；`LINUX_INIT_REAP_PPID`(0) → Link B orphan |
| `thread_number` / `thread_head_node` | 附着线程数；`res_thread_node` 链表 |
| `start_brk` / `brk` / `mmap_hint` | 堆与 mmap 游标 |
| `uid`…`egid` | ID（`sys_id`） |
| `exit_code` / `exit_state` | wait4；见 [`EXIT_CLEAN.md`](protocols/EXIT_CLEAN.md) |
| `exit_last_thread` | **sys_exit 提示**（`tn==1`）；**clean 在 `tn==0` 时置位**（权威） |
| `pending_exits` | wait4 pid 不匹配时的 EXIT_NOTIFY 队列 |
| `signal` / `fs` | 堆上 proc 级状态 |
| `vs` | 非拥有 AS 缓存（末线程 detach 后清空） |

`exit_state`：`RUNNING` / `ZOMBIE` / `NOTIFIED` / `CLAIMED`（`linux_proc_reap` 认领中）。Link B 由 clean 侧 `proc_has_wait_reaper` 判定，不在 sys_exit 自标单独状态。

**不**在资源束存 VMA 根：用户映射由 **`thread->vs` Radix** 表达。

### 2.2 `linux_thread_append_t`（每线程）

| 字段 | 说明 |
|------|------|
| `signal` | 每线程 signal 状态 |
| `sleep_port` / `sleep_timer_*` | nanosleep / 定时器 |
| `clear_tid` | `set_tid_address` / CLEARTID |
| `boot_wait_cookie` | Path-B boot 等待（fork 子须为 0） |
| **`res`** | 指向共享 `linux_proc_resource_t` |
| **`res_thread_node`** | 挂到 `proc->thread_head_node` |

访问：`linux_thread_append(thread)`、`linux_proc_of(thread)` → `ta->res`。

## 3. 生命周期（摘要）

| 路径 | 资源束 | thread append |
|------|--------|---------------|
| boot / exec | `alloc` + `attach` | `prepare_new` + exec |
| fork | `alloc` 或共享父 `res` | `copy_thread` 后 `attach` |
| 线程退出 | detach 降 `thread_number`；末线程 zombie 壳 | `fini` → `linux_proc_detach_thread` |
| wait / clean | `linux_proc_reap`（fini + 末 `ref_put`） | 已由 `delete_thread` 回收 |

详见 [`APPEND_HOOKS.md`](APPEND_HOOKS.md)、[`protocols/EXIT_CLEAN.md`](protocols/EXIT_CLEAN.md)。

## 4. proc registry

实现：`linux_layer/proc/sys_proc_registry.c`、`include/linux_compat/proc_registry.h`

- `register_process` / `unregister_process` / `find_proc_by_pid`
- `proc_parent_has_unreaped_child` — wait4 阻塞 / ECHILD
- `proc_has_wait_reaper` — Link A vs B

**已删除（v1）**：`find_zombie_child*`（wait4 用 EXIT_NOTIFY `proc*` handoff）。

## 5. 与 clean_server

- **THREAD_REAP** → `delete_thread` + sync detach
- **Link A**：EXIT_NOTIFY → parent `wait4` → `linux_proc_reap`
- **Link B**：clean inline `linux_proc_reap`
