# Thread + VSpace：compat 对 core 的契约

**权威 core 侧：** [`core/docs/task-thread.md`](../../../core/docs/task-thread.md) § VSpace ownership · [`core/docs/USING_CORE.md`](../../../core/docs/USING_CORE.md) §3.0 · [`memory.md`](../../../core/docs/memory.md) §0.8

**头文件（无 shim）：**

| 用途 | Include |
|------|---------|
| 线程 / 调度 / IPC 队列 | `<rendezvos/task/thread.h>` |
| Arch 上下文 / syscall 返回 | `<arch/<ARCH>/thread_arch.h>` |

**禁止**再 `#include <rendezvos/task/tcb.h>` 或 `<arch/*/tcb_arch.h>`；core 无进程对象 `Tcb_Base`。

---

## 1. 对象映射

| Linux 概念 | compat 真源 | core |
|------------|-------------|------|
| 调度实体 | `Thread_Base` + `linux_thread_append_t` | `create_thread` / `copy_thread` |
| 进程 / 线程组 | 堆 `linux_proc_resource_t` + `proc_registry` | **无** |
| 地址空间 | `thread->vs`（ownership）+ `proc->vs`（非拥有缓存） | `VSpace` + radix |

---

## 2. 创建线程（ownership）

1. **`vs` 非 NULL** 传入 `create_thread` / `copy_thread`。
2. Caller 持 **一条 live ref**（`create_vspace` / `clone_vspace` / `ref_get` / `ref_get_not_zero(&root_vspace)`）。
3. 成功 ⇒ ref **转移**到 `thread->vs`；caller **不得**再 `ref_put`。
4. 新建 user `VSpace`：`create_vspace` → **`register_vspace(vs, &root_vspace)`** → `create_thread` / `copy_thread`。
5. 用户线程：`THREAD_FLAG_USER` 且 `vs` 为 **user** 表（非 `&root_vspace`）。
6. 内核 server：`gen_thread_from_func` 或显式 `ref_get_not_zero` + `create_thread(..., &root_vspace, ...)`。

### fork / clone

| 场景 | compat 顺序 |
|------|-------------|
| 新 AS | `linux_copy_vspace`（内部 `clone_vspace` + **`register_vspace`**）→ `copy_thread(parent, child_vs, ret)` |
| 共享 AS（`CLONE_VM`） | `ref_get(&parent->vs->refcount)` → `copy_thread(..., same_vs, ...)` |

`copy_thread` 前 syscall 路径：`arch_ctx_refresh` / `arch_ctx_merge_from_src`（见 [`BUGFIX_FORK_SYSCALL_STALE_USER_CONTEXT.md`](../BUGFIX_FORK_SYSCALL_STALE_USER_CONTEXT.md)）。

---

## 3. 类型与 API 命名

| 旧（已删除） | 现行 |
|--------------|------|
| `Tcb_Base` / `get_cpu_current_task()` | `linux_proc_resource_t` / `linux_current_proc()` |
| `delete_task` / `new_task_structure` | `linux_proc_reap` / `linux_proc_alloc` + `create_thread` |
| `gen_task_from_elf` | `gen_thread_from_elf`（core harness）；Linux PID1 用 `linux_exec_replace_image` |
| `Arch_Thread_Context` | **`Arch_Thread_Context`** |
| `thread.h` / `thread_arch.h` | **`thread.h` / `thread_arch.h`** |

---

## 4. VSpace 与 schedule / exit（与 EXIT_CLEAN 一致）

- `schedule` 切到 kernel/idle **不强制**卸用户 AS；`current_vspace` 可仍为末进程 user vs。
- 末线程 `delete_thread` 只 drop **`thread->vs` ownership**；`linux_proc_reap` **不放** vs。
- **EXIT_NOTIFY / Link B**：clean 在 **`thread_number==0`** 后 notify/reap（sync detach + 可选 wait）；parent **`wait4` 本地 `linux_proc_reap`**（EXIT_CLEAN v2）。
- `linux_current_vs()` / `proc->vs`：内核 server 线程 **不得**用当前 CPU 的 `current_vspace` 代替客户端 proc 的 vs（VFS 等）。

详见 [`EXIT_CLEAN.md`](EXIT_CLEAN.md) 对象拆分表。

---

## 5. 代码落点（compat）

| 模块 | 路径 |
|------|------|
| proc 分配 / reap | `linux_layer/proc/linux_proc.c` |
| fork / clone | `sys_fork.c`, `sys_clone.c` |
| vspace 复制 + register | `linux_layer/mm/linux_vspace.c` |
| boot PID1 | `linux_layer/init/linux_boot.c` |
| exec | `linux_exec*.c`, `sys_execve.c` |
| 当前 proc / vs 助手 | `include/linux_compat/proc_compat.h` |
