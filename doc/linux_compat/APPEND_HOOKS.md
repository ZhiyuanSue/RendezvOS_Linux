# Append 生命周期 Hook（Linux 兼容层）

> **Canonical**：Linux 线程扩展区如何挂到 core、何时调 hook。  
> **Core 侧说明**：[`core/docs/task-thread.md`](../../core/docs/task-thread.md)  
> **数据结构**：[`DATA_MODEL.md`](DATA_MODEL.md) · [`include/linux_compat/proc_compat.h`](../../include/linux_compat/proc_compat.h)

---

## 1. 模型

core 只在 `Thread_Base` 尾部提供 **opaque append 字节区** + **`append_hooks` 指针**。没有 `Tcb_Base` / task append。

Linux **进程**状态是堆上的 `linux_proc_resource_t`，由线程 append 里的 **`res`** 指针共享。core **不** memcpy append、**不**理解字段含义。

```c
typedef struct thread_append_hooks {
    size_t append_info_len;   /* sizeof(linux_thread_append_t) */
    thread_append_init_t init; /* optional; unused on current Linux path */
    thread_append_copy_t copy;
    thread_append_fini_t fini;
} thread_append_hooks_t;
```

Linux 静态表（[`linux_layer/loader/linux_elf_init.c`](../../linux_layer/loader/linux_elf_init.c)）：

| 表 | `append_info_len` | `init` | `copy` | `fini` |
|----|-------------------|--------|--------|--------|
| `linux_thread_append_hooks` | `LINUX_THREAD_APPEND_BYTES` | `NULL` | `linux_thread_append_copy` | `linux_thread_append_fini` |

进程对象不走 hook 表：

| 操作 | API |
|------|-----|
| 分配（refcount=1，分配 pid） | `linux_proc_alloc` |
| 线程加入组（额外 ref） | `linux_proc_attach_thread` |
| 线程离开组 | `linux_proc_detach_thread`（`thread.fini` 调用） |
| fork 拷 signal/fs | `linux_proc_copy_from` |
| clone 拷 signal/fs | `linux_proc_clone_from` |
| wait/clean 收尸 | `linux_proc_reap`（`thread_number==0` 后 fini + put 分配 ref） |

**PID 分配（非 hook）：** core 提供 `Id_Manager` + `get_new_id`；Linux pid 由 compat 自有 `linux_pid_manager`（[`linux_proc.c`](../../linux_layer/proc/linux_proc.c)），`DEFINE_INIT` 在 BSP 初始化。core 仅保留 `tid_manager`（`init_core_thread_ids`）。

---

## 2. core 何时调用 hook

| Hook | 触发点 | Linux 实现职责 |
|------|--------|----------------|
| `thread.init` | （当前未调用） | — |
| `thread.copy` | **`copy_thread`**（core 不拷 append 字节） | 新建 thread signal、继承 mask；清零 `res` / `boot_wait_cookie` / `clear_tid`。**不** attach（调用方在 copy 之后 `linux_proc_attach_thread`） |
| `thread.fini` | `del_thread_structure`（**先于** drop `thread->vs`） | sleep_port teardown、thread signal destroy、`linux_proc_detach_thread` |

---

## 3. 兼容层调用约定

### 3.1 PID1 / 用户镜像

PID1：`linux_boot.c` 里 `create_vspace` → `create_thread(..., vs, ...)`（接管 vs）→ `linux_proc_attach_thread` → `add_thread_to_manager`；用户线程体里 `linux_user_task_prepare_new` + `linux_exec_replace_image`。

普通 `execve`：同进程 `linux_exec_replace_image`（`load_elf_to_vs` + `generate_user_stack` + 栈/auxv）。

Core 侧仍保留 **`gen_thread_from_elf` / `run_elf_program`**（无 FS / incbin harness，Path B + 可选 `append_hooks.init`）；Linux 启动不走这条。

### 3.2 fork

```c
child = linux_proc_alloc();
linux_copy_vspace(parent_vs, &child_vs); /* clone_vspace + register_vspace */
/* fill child pid/ppid/brk… */
linux_proc_copy_from(child, parent);
child_thread = copy_thread(parent_thread, child_vs, 0); /* takes ownership of child_vs */
linux_proc_attach_thread(child, child_thread);
add_thread_to_manager(percpu(core_tm), child_thread);
```

### 3.3 clone

与 fork 类似。`CLONE_THREAD`：attach 到 **父** `linux_proc`。`CLONE_VM`：先 `ref_get(parent_vs)`，再把这个额外引用交给 `copy_thread`（所有权转移）。新 AS：直接把 clone 出来的 vs 交给 `copy_thread`，之后不要 `ref_put`。  
`CLONE_CHILD_CLEARTID` 等在 `copy_thread` **之后**写 `clear_tid`（`thread.copy` 会先清零）。

---

## 4. 访问数据

```c
linux_proc_resource_t *proc = linux_current_proc(); /* 或 linux_proc_of(thread) */
linux_thread_append_t *ta = linux_thread_append(thread);
VSpace *vs = linux_current_vs(); /* 当前线程 thread->vs */
```

syscall 路径用 `linux_current_vs()`。VFS 等 **内核服务线程**必须用查到的 `proc->vs`（非拥有缓存），禁止把服务线程自己的 `root_vspace` 当成客户端 AS。

字段布局见 `proc_compat.h`。heap 对象（signal、fs、fd 表）在 proc 里只存 **指针**。

---

## 5. 测试相关

- **`boot_wait_cookie`**：仅 Path-B `/init` 在 `linux_boot.c` 里设置；**`thread.copy` 必须清零**，避免子进程误唤醒 boot wait（见 [`doc/ai/DECISIONS.md`](../ai/DECISIONS.md)）。
- **`clear_tid`**：`set_tid_address` / clone；子线程 copy 时清零，clone 再按需写入。

---

## 6. 已废弃模式（勿再使用）

| 旧做法 | 现做法 |
|--------|--------|
| `Tcb_Base` + `task_append_hooks` | 堆 `linux_proc_resource_t` + thread append 指针 |
| `new_task_structure` / `delete_task` / `thread_join` / `thread_start` | `linux_proc_alloc` / `linux_proc_reap` / `add_thread_to_manager` |
| `get_cpu_current_task()` / `belong_tcb` | `linux_current_proc()` / `linux_proc_of` |
| `gen_task_from_elf`（已删） | `gen_thread_from_elf` + `run_elf_program`（core harness）；Linux 镜像走 `linux_exec_replace_image` |
| 四处传 `LINUX_*_APPEND_BYTES` | hook 表内 `append_info_len` |
| core `copy_thread` memcpy append | `thread.copy` hook |
| `linux_task_append_clone()` | `linux_proc_clone_from` |

历史 brk/core 传递分析见 [`CORE_MODIFICATION_BRK_FIX.md`](CORE_MODIFICATION_BRK_FIX.md)（**已过时**，仅作考古）。
