# Syscall 实现顺序与逐项清单

约定：

- **编号** `Step N` 为推荐实现顺序；后项依赖前项除非标明「可并行」。
- **不改代码** 时本文档为协作契约；落地后以 PR 勾选为准。
- **x86_64** 系统调用号见 [`include/arch/x86_64/syscall_ids.h`](../../include/arch/x86_64/syscall_ids.h)。
- **直接调 core**：本清单默认 `mmap`/`brk` 等 **不调 IPC**；登记簿阶段 1 用 **锁**，见 [`DATA_MODEL.md`](DATA_MODEL.md)。

---

## 总览顺序

| 顺序 | 项 | 类型 |
|------|-----|------|
| 0 | 返回值约定 + 分发骨架 | 前置 |
| 1 | `getpid` | syscall |
| 2 | `brk` | syscall |
| 3 | `mmap`（匿名） | syscall |
| 4 | `munmap` | syscall |
| 5 | `mprotect` | syscall |
| 6 | `exit` / `exit_group` | syscall + server 扩展 |
| 7 | `wait4` / `waitpid` | syscall |
| 8 | COW + 页故障路径 | 基础设施（含可选 Server） |
| 9 | `fork` | syscall |
| 10 | `clone`（子集） | syscall |
| 11 | `execve` | syscall |
| 12 | `kill` | syscall |
| 13 | `rt_sigprocmask` / `rt_sigaction` / `rt_sigreturn` | syscall 组 |
| 14 | `mremap` | syscall（可选） |

**与 Server 平级的条目**（见文末）：`clean_server` 扩展、`proc_coordinator`（可选）、`cow_fault_handler`（可选）。

---

## Step 0：syscall 返回值与分发骨架

**目的**：用户态通过 `trap_frame` 拿到 **单一真源** 的返回值；避免在 `sys_*` 与 `syscall()` 内 **重复 `schedule`** 或漏写 `rax`。

**依赖**：无。

**建议改动文件**

| 文件 | 改动目的 |
|------|----------|
| [`linux_layer/syscall/syscall_entry.c`](../../linux_layer/syscall/syscall_entry.c) | 每个分支调用 `sys_*` 后写入 **返回值** 到 `syscall_ctx->ARCH_SYSCALL_RET`（若宏未定义则用 `ARCH_SYSCALL_ID` 所在寄存器兼用 — **需定义清晰**）；统一 **是否** 在出口 `schedule` 一次 |
| [`core/include/arch/x86_64/trap/trap.h`](../../core/include/arch/x86_64/trap/trap.h)（及 aarch64 对称） | x86_64 上 **`rax` 兼作系统调用号与返回值**：进入 `syscall()` 后应先 **保存** `ARCH_SYSCALL_ID`，再写入 **返回值** 到 `tf->rax`（或引入单独保存字段） |
| [`include/syscall.h`](../../include/syscall.h) | 声明各 `sys_*` 与（可选）`linux_syscall_set_ret` 内联 |

**建议新增函数（linux_layer）**

- `linux_syscall_set_return(struct trap_frame *tf, i64 value)` — 写 arch 相关寄存器。
- （可选）`linux_syscall_dispatch(struct trap_frame *tf)` — 从 `switch` 中拆出便于测试。

**注意**：[`thread_syscall.c`](../../linux_layer/syscall/thread_syscall.c) 中 `sys_exit` 当前末尾自旋 `schedule`；与 Step 0 的「单出口调度策略」需 **统一设计**（文档层先记矛盾，实现时合并）。

---

## Step 1：`getpid`（`__NR_getpid` 39）

**目的**：验证「当前任务 → pid」路径与 syscall 返回链路。

**依赖**：Step 0。

**文件**

| 文件 | 改动 |
|------|------|
| [`include/syscall.h`](../../include/syscall.h) | `long sys_getpid(void);`（或 `i64`，与内核类型统一） |
| [`linux_layer/syscall/syscall_entry.c`](../../linux_layer/syscall/syscall_entry.c) | `case __NR_getpid:` |
| 新建或 `linux_layer/proc/sys_proc.c` | 实现 `sys_getpid` |

**实现要点**

- 使用 [`get_cpu_current_task()`](../../core/include/rendezvos/task/tcb.h) → `tcb->pid`；多线程后改为 **`tgid`**（`DATA_MODEL.md`）。

**测试**：payload 或 `linux_layer/tests` 打印 pid。

---

## Step 2：`brk`（`__NR_brk` 12）

**目的**：堆增长/收缩；与 ELF 载入的初始 brk 对齐。

**依赖**：Step 0；append 区定义（[`DATA_MODEL.md`](DATA_MODEL.md)）。

**文件**

| 文件 | 改动 |
|------|------|
| `linux_layer/include/proc_compat.h`（新建） | `struct linux_proc_append` 含 `start_brk`, `brk` |
| [`core/kernel/task/thread_loader.c`](../../core/kernel/task/thread_loader.c) 或 loader 初始化路径 | 首线程/首 task 创建后 **初始化 `start_brk`/`brk`**（来自 ELF `brk` Phdr 或 `_end` 约定） |
| [`linux_layer/syscall/syscall_entry.c`](../../linux_layer/syscall/syscall_entry.c) | `case __NR_brk:` |
| `linux_layer/mm/sys_brk.c`（新建）或 `linux_mm.c` | `sys_brk(unsigned long addr)` |

**调用 core**

- [`get_free_page`](../../core/include/rendezvos/mm/nexus.h) / [`free_pages`](../../core/include/rendezvos/mm/nexus.h)（经 `nexus_root` + `vs` 查找，与现有用户分配路径一致）。
- [`map`](../../core/include/rendezvos/mm/map_handler.h) / [`unmap`](../../core/include/rendezvos/mm/map_handler.h)，`handler = &percpu(Map_Handler)`。

**目的（小结）**：在不实现 `mmap` 前即可支持 **简单堆**，便于 libc 极简移植。

---

## Step 3：`mmap` 匿名（`__NR_mmap` 9）

**目的**：匿名映射；为动态链接与大块堆铺路。

**依赖**：Step 0；熟悉 nexus 插入语义。

**文件**

| 文件 | 改动 |
|------|------|
| [`linux_layer/syscall/syscall_entry.c`](../../linux_layer/syscall/syscall_entry.c) | `case __NR_mmap:`（注意 x86_64 `mmap` 参数寄存器布局） |
| `linux_layer/mm/sys_mmap.c`（新建） | `sys_mmap(...)` → 解析 `prot`/`flags`；**先支持** `MAP_ANONYMOUS`；`MAP_FIXED`/`MAP_PRIVATE` 与 COW 策略见 `MM_AND_COW.md` |
| 必要时 [`core/kernel/mm/nexus.c`](../../core/kernel/mm/nexus.c) | 导出 **分裂/合并区间** 辅助函数（若内部能力不足） |

**调用 core**：`get_free_page`、`map`、nexus 登记（与 `user_fill_range` 路径对齐或复用）。

**目的**：建立 **nexus 为真源** 的第一条完整映射路径。

---

## Step 4：`munmap`（`__NR_munmap` 11）

**目的**：解除映射；**部分解除** 需分裂 nexus。

**依赖**：Step 3。

**文件**

| 文件 | 改动 |
|------|------|
| [`linux_layer/syscall/syscall_entry.c`](../../linux_layer/syscall/syscall_entry.c) | `case __NR_munmap:` |
| `linux_layer/mm/sys_munmap.c` 或合并入 `linux_mm.c` | `sys_munmap` |
| [`core/kernel/mm/nexus.c`](../../core/kernel/mm/nexus.c)（可选） | 区间分裂工具函数 |

**调用 core**：`unmap`、`free_pages`、更新 nexus。

---

## Step 5：`mprotect`（`__NR_mprotect` 10）

**目的**：修改区间权限；与 COW 只读页、后续写故障一致。

**依赖**：Step 3（最好 Step 4 已完成分裂逻辑）。

**文件**

| 文件 | 改动 |
|------|------|
| [`linux_layer/syscall/syscall_entry.c`](../../linux_layer/syscall/syscall_entry.c) | `case __NR_mprotect:` |
| `linux_layer/mm/sys_mprotect.c` | 遍历相交 nexus、分裂边界、改 `region_flags` / PTE |

**调用 core**：`map`/`unmap` 或专用 PTE 更新路径（视架构封装而定）。

---

## Step 6：`exit` / `exit_group`（`__NR_exit` 60 / `__NR_exit_group` 231）

**目的**：线程退出 + **进程**退出语义；与 clean_server、登记簿一致。

**依赖**：Step 0；建议 Step 7 前至少完成 **登记簿** 的「子进程槽位」数据结构草图。

**文件**

| 文件 | 改动 |
|------|------|
| [`linux_layer/syscall/thread_syscall.c`](../../linux_layer/syscall/thread_syscall.c) | 区分 **单线程 exit** vs **exit_group**；或先合并语义（单进程单线程时等价） |
| [`servers/clean_server.c`](../../servers/clean_server.c) | 最后一线程时：`delete_task`、在登记簿标 **zombie**、写 `exit_code`、唤醒 `wait` |
| `linux_layer/proc/proc_registry.c`（新建，见 `DATA_MODEL.md`） | `proc_note_thread_exit` / `proc_reap` 钩子 |

**目的**：把 **资源释放**（现有）与 **Linux 可见的进程状态**（zombie/code）接起来。

---

## Step 7：`wait4` / `waitpid`（`__NR_wait4` 61；`waitpid` 常为 libc 包装）

**目的**：父进程回收子进程状态。

**依赖**：Step 6（zombie 状态来源）；**proc_registry**（阶段 1 加锁）。

**文件**

| 文件 | 改动 |
|------|------|
| [`linux_layer/syscall/syscall_entry.c`](../../linux_layer/syscall/syscall_entry.c) | `case __NR_wait4:` |
| `linux_layer/proc/sys_wait.c`（新建） | `sys_wait4`；解析 `options`（`WNOHANG` 等）最小子集 |
| `linux_layer/proc/proc_registry.c` | `wait` 阻塞队列、与 `thread_status_block_on_receive` 或自定义 wait 队列联动 |
| [`core/kernel/task/tcb.h` / IPC](...) | 阻塞/唤醒与现有 thread 状态机一致（查阅 `thread_status_*`） |

**目的**：完成 **fork 最小闭环** 的前半段（回收）。

---

## Step 8：COW + 页故障路径（基础设施）

**目的**：`fork` 共享只读物理页；写时复制。

**依赖**：Step 3–5（理解 PTE 与 nexus）；[`MM_AND_COW.md`](MM_AND_COW.md)。

**文件（阶段 A：内核内联）**

| 文件 | 改动 |
|------|------|
| [`core/arch/x86_64/trap/trap.c`](../../core/arch/x86_64/trap/trap.c)（及 `#PF` 向量路径） | 调用 **可注册** 的 `linux_page_fault_handler`（弱符号，由 linux_layer 提供） |
| `linux_layer/mm/cow_fault.c`（新建） | 判断 vaddr 是否 COW、分裂物理页、刷新 TLB |

**文件（阶段 B：Server，与 syscall 平级）**

- 见 **Server：`cow_fault_handler`**。

**目的**： unblock Step 9 `fork`。

---

## Step 9：`fork`（`__NR_fork` 57）

**目的**：创建子进程，地址空间 COW。

**依赖**：Step 8；Step 7（子进程登记 parent）；Step 0。

**文件**

| 文件 | 改动 |
|------|------|
| [`linux_layer/syscall/syscall_entry.c`](../../linux_layer/syscall/syscall_entry.c) | `case __NR_fork:` |
| `linux_layer/proc/sys_fork.c`（新建） | 分配子 `Tcb_Base`、子主线程、dup/共享页表、子 **返回 0** 父返回 **子 pid** |
| [`core/kernel/task/tcb.c`](../../core/kernel/task/tcb.c) / [`thread_loader.c`](../../core/kernel/task/thread_loader.c) | 若需 **导出** `copy_process` 式辅助（保持 core 无 Linux 头文件） |
| `proc_registry.c` | 注册子进程、`parent` 链接 |

**调用 core**：`new_vspace`、`map`、nexus clone 逻辑、线程创建 API。

**目的**：多进程。

---

## Step 10：`clone`（`__NR_clone` 56）

**目的**：为 pthread 预留；**先实现子集 flags**，其余 `-EINVAL`。

**依赖**：Step 9（或并行于 fork 内部抽取 `copy_process`）。

**文件**

| 文件 | 改动 |
|------|------|
| [`linux_layer/syscall/syscall_entry.c`](../../linux_layer/syscall/syscall_entry.c) | `case __NR_clone:` |
| `linux_layer/proc/sys_clone.c` | 解析 `flags`、`child_stack`；`CLONE_VM` 共享 `vs`；`CLONE_THREAD` 同 `tgid` |

---

## Step 11：`execve`（`__NR_execve` 59）

**目的**：替换当前进程映像。

**依赖**：Step 3–5（清用户映射策略）；[`core/kernel/task/thread_loader.c`](../../core/kernel/task/thread_loader.c) ELF 路径。

**文件**

| 文件 | 改动 |
|------|------|
| [`linux_layer/syscall/syscall_entry.c`](../../linux_layer/syscall/syscall_entry.c) | `case __NR_execve:` |
| `linux_layer/loader/sys_execve.c`（新建） | 路径参数（初期 **内核指针** 或 VFS stub）；调用 `run_elf_program` / 拆分出的 `load_elf_into_vs` |
| `thread_loader.c` 或 linux_layer 包装 | **同一 task** 清空用户 nexus、重建 root mapping、重置 brk、跳新入口 |

**语义决策（需写进提交说明）**：多线程 `execve` — 初期 **单线程 only** 或 **杀光兄弟线程**。

---

## Step 12：`kill`（`__NR_kill` 62）

**目的**：最小信号投递；可先实现 **SIGKILL 终止目标进程**。

**依赖**：`proc_registry` 按 pid 查找；Step 6 退出路径。

**文件**

| 文件 | 改动 |
|------|------|
| [`linux_layer/syscall/syscall_entry.c`](../../linux_layer/syscall/syscall_entry.c) | `case __NR_kill:` |
| `linux_layer/signal/sys_kill.c`（新建） | 查目标、标记 **pending** 或直接 `exit_group` |

---

## Step 13：信号：`rt_sigprocmask`（14）、`rt_sigaction`（13）、`rt_sigreturn`（15）

**目的**：与 `kill`、可中断 syscall 对齐。

**依赖**：Step 12；`linux_thread_append` / 进程级 sigaction 表。

**文件**

| 文件 | 改动 |
|------|------|
| [`linux_layer/syscall/syscall_entry.c`](../../linux_layer/syscall/syscall_entry.c) | 三个 `case` |
| `linux_layer/signal/signal.c`（新建） | mask、action 表、返回到用户态 handler 的 trap 帧构造（**架构相关**） |

**初期降级**：仅支持 **默认动作** + `rt_sigprocmask` 存掩码；handler 延后。

---

## Step 14：`mremap`（`__NR_mremap` 25，可选）

**目的**：兼容部分 glibc/malloc。

**依赖**：Step 3–4。

**文件**：`linux_layer/mm/sys_mremap.c`；或明确 `-ENOSYS` 并在本文档标记 **未实现**。

---

# 与 syscall 平级：Server / 基础设施条目

## Server A：`clean_server` 扩展（依附 Step 6）

**目的**：继续作为 **线程回收** 的执行者；扩展消息负载以携带 **exit_group**、**进程 zombie** 通知。

**文件**：[`servers/clean_server.c`](../../servers/clean_server.c)。

**与 Step 6 关系**：同一 PR 或紧随；避免 **重复 `delete_task`**。

---

## Server B：`proc_coordinator`（可选，阶段 2）

**目的**：用 **单线程 + IPC** 串行化 pid 分配、fork 登记、`wait` 队列，替代 `proc_registry` 的大锁（[`ARCHITECTURE.md`](ARCHITECTURE.md)）。

**依赖**：现有 port/消息 API；与 Step 7/9 **二选一或迁移**。

**建议新文件**

- `servers/proc_coordinator.c` + 初始化注册端口名（类似 `clean_server_port`）。

**消息类型（文档层列举）**

- `PROC_REGISTER_CHILD`、`PROC_ZOMBIE`、`PROC_WAIT`、`PROC_WAIT_REPLY`。

**目的**：锁序简化、贴近微内核演进。

---

## Server C：`cow_fault_handler`（可选，阶段 B）

**目的**：页故障 **不** 在 trap 上下文做重活，转发给 server（[`MM_AND_COW.md`](MM_AND_COW.md)）。

**依赖**：Step 8 的 upcall 协议；与 **SMP owner CPU** 规则兼容。

**建议新文件**：`servers/cow_fault_server.c`。

**目的**：与 **无锁 IPC** 哲学一致；fault 路径可审计。

---

## 协作建议

- **一个 syscall 一个 PR**（或 Step 0 单独 PR），便于 review 与 `doc/ai/INVARIANTS.md` 同步。
- 每步完成后在 `linux_layer/tests` 或 payload 加 **最小回归**（见 [`linux_layer/tests/linux_compat_tests.c`](../../linux_layer/tests/linux_compat_tests.c)）。

本文档细化程度用于 **达成共识**；若某步发现 core 缺符号，回填 `MM_AND_COW.md` 的 **core 变更清单** 并改顺序依赖。
