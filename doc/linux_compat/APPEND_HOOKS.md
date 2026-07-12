# Append 生命周期 Hook（Linux 兼容层）

> **Canonical**：Linux 进程/线程扩展区如何挂到 core、何时调 hook。  
> **Core 侧说明**：[`core/docs/task-thread.md`](../../core/docs/task-thread.md)  
> **数据结构**：[`DATA_MODEL.md`](DATA_MODEL.md) · [`include/linux_compat/proc_compat.h`](../../include/linux_compat/proc_compat.h)

---

## 1. 模型

core 在 `Tcb_Base` / `Thread_Base` 尾部提供 **opaque append 字节区** + **`append_hooks` 指针**。  
Linux 语义全部在 compat 的 struct 与 hook 里实现；core **不** memcpy append、**不**理解字段含义。

每张 hook 表对应 **一种固定 append 布局**：

```c
typedef struct task_append_hooks {
    size_t append_info_len;   /* sizeof(linux_proc_append_t) */
    task_append_init_t init;
    task_append_copy_t copy;
    task_append_fini_t fini;
} task_append_hooks_t;

typedef struct thread_append_hooks {
    size_t append_info_len;   /* sizeof(linux_thread_append_t) */
    thread_append_init_t init; /* ELF 首次 exec：elf_load_info 非 NULL */
    thread_append_copy_t copy;
    thread_append_fini_t fini;
} thread_append_hooks_t;
```

Linux 静态表（[`linux_layer/loader/linux_elf_init.c`](../../linux_layer/loader/linux_elf_init.c)）：

| 表 | `append_info_len` | `init` | `copy` | `fini` |
|----|-------------------|--------|--------|--------|
| `linux_task_append_hooks` | `LINUX_PROC_APPEND_BYTES` | — | `linux_task_append_copy` | `linux_task_append_fini` |
| `linux_thread_append_hooks` | `LINUX_THREAD_APPEND_BYTES` | `linux_thread_append_init` | `linux_thread_append_copy` | `linux_thread_append_fini` |

Compat-only helper（不在 hook 表内）：`linux_task_append_clone(dst, src, clone_flags)` — `sys_clone` 在填好 proc 静态字段后调用。

---

## 2. core 何时调用 hook

| Hook | 触发点 | Linux 实现职责 |
|------|--------|----------------|
| `task.init` | `new_task_structure` | （当前 NULL） |
| `task.copy` | **`sys_fork` / `sys_clone`** 在填好静态 proc 字段后 | signal/fs fork、共享或新建 heap 状态 |
| `task.fini` | `delete_task` | reparent、unregister、signal/fs destroy |
| `thread.init` | **`run_elf_program`** PT_LOAD + user SP 后 | brk、signal/fs attach、register_process、drop staging slice |
| `thread.copy` | **`copy_thread`**（core 不拷 append 字节） | 新建 thread signal、继承 mask；清零 test_cookie/clear_tid |
| `thread.fini` | `del_thread_structure` | sleep_port teardown、thread signal destroy |

**注意**：`run_elf_program` 里 `init` 失败只打日志，不 return——当前线程已在 loader 上下文，返回到 `thread_entry` 无意义。

---

## 3. 兼容层调用约定

### 3.1 首次 exec（测例 / init）

```c
gen_task_from_elf(&thr,
                  &linux_task_append_hooks,
                  &linux_thread_append_hooks,
                  elf_slice);
```

- 只需传 **两张 hook 表 + slice**；长度在表的 `append_info_len` 里。
- `thread.init` 在 ELF map 完成后运行（原 `elf_init_handler` 职责）。

### 3.2 fork

```c
child = new_task_structure(percpu(kallocator), &linux_task_append_hooks);
/* … copy vspace，memset + 填 child_pa 静态字段 … */
child->append_hooks->copy(child, parent);
child_thread = copy_thread(parent_thread, child, 0);
```

- **Task**：core 不复制 append；compat 清零并填 brk/ppid 等，再 `task.copy`。
- **Thread**：`copy_thread` 传 `src->append_hooks` 分配 dst append，core 调 `thread.copy` 构建状态。

### 3.3 clone

与 fork 类似；`CLONE_VM` 时 task 级 signal 走 attach、fs 仍 fork 共享（`linux_task_append_clone`）。  
`CLONE_CHILD_CLEARTID` 等在 `copy_thread` **之后**写 `clear_tid`（`thread.copy` 会先清零）。

---

## 4. 访问 append 数据

```c
linux_proc_append_t *pa = linux_proc_append(tcb);
linux_thread_append_t *ta = linux_thread_append(thread);
```

字段布局见 `proc_compat.h`。heap 对象（signal、fs、fd 表）在 append 里只存 **指针**；生命周期由 hook 管理。

---

## 5. 测试相关

- **`test_cookie`**：仅 runner 主线程在 `user_test_runner` 里设置；**`thread.copy` 必须清零**，避免子进程误触发 harness（见 [`doc/ai/DECISIONS.md`](../ai/DECISIONS.md)）。
- **`clear_tid`**：`set_tid_address` / clone；子线程 copy 时清零，clone 再按需写入。

---

## 6. 已废弃模式（勿再使用）

| 旧做法 | 现做法 |
|--------|--------|
| 单独 `elf_init_handler_t` 参数给 `gen_task_from_elf` | `thread_append_hooks.init` |
| 四处传 `LINUX_*_APPEND_BYTES` | hook 表内 `append_info_len` |
| core `copy_thread` memcpy append | `thread.copy` hook |
| `append_fini` 分散函数指针 | `task/thread_append_hooks` 静态表 |
| `linux_elf_init_handler_ptr` 全局 | `linux_thread_append_hooks` |

历史 brk/core 传递分析见 [`CORE_MODIFICATION_BRK_FIX.md`](CORE_MODIFICATION_BRK_FIX.md)（**已过时**，仅作考古）。
