# Syscall 实现路线图

## 目标和策略

**总体目标**：实现200-300+ Linux syscall，支持x86_64、aarch64、riscv64等多架构。

**实现策略**：
- **迭代式开发**：分阶段实现，每个阶段以测例为导向
- **灵活优先级**：根据测例需求和依赖关系调整实现顺序
- **多架构同步**：从设计阶段考虑多架构兼容性
- **质量优先**：每个阶段确保稳定性和正确性

## 阶段划分

### Phase 0：基础设施（P0 - 必须首先完成）

**目标**：建立syscall框架和最小可用环境

| 项 | 描述 | 优先级 | 状态 |
|----|------|--------|------|
| syscall框架 | 返回值约定、分发骨架、多架构支持 | P0 | 规划中 |
| write shim | fd 1/2控制台输出（调试必需） | P0 | 规划中 |
| 基础数据结构 | proc_registry、append区定义 | P0 | 规划中 |

**里程碑**：能够运行简单的用户程序并输出调试信息

### Phase 1：进程管理基础（P1 - 核心功能）

**目标**：支持基本的进程创建和管理

| 组 | syscall | 依赖 |
|----|---------|------|
| 进程信息 | getpid, gettid | Phase 0 |
| 内存管理基础 | brk | Phase 0 |
| 进程生命周期 | exit, exit_group | Phase 0 |
| 进程等待 | wait4, waitpid | exit, proc_registry |

**里程碑**：能够创建简单进程，父子进程能够同步

### Phase 2：内存管理（P1 - 核心功能）

**目标**：实现Linux式内存管理

| 组 | syscall | 依赖 |
|----|---------|------|
| 基础映射 | mmap（匿名）, munmap | Phase 0 |
| 权限管理 | mprotect | mmap |
| 内存统计 | getpagesize, sysinfo | - |

**里程碑**：支持动态内存分配，能够运行复杂的用户程序

### Phase 3：执行流控制机制（P1 - 核心功能）

**目标**：建立通用的用户态执行流控制机制

**设计理念**：
- 多个场景（线程复制、信号处理、fork）都涉及"构造用户态返回帧并恢复执行"
- 这些场景复用相同的底层机制：trap_frame构造、内核栈安装、调度切换
- 先实现同地址空间的简单场景，再扩展到跨地址空间的复杂场景

**核心基础设施（core/）**：

| 接口 | 职责 | 场景 |
|------|------|------|
| `arch_user_return_install()` | 安装trap_frame到内核栈 | 所有场景 |
| `arch_ctx_inherit()` | 复制架构上下文 | 线程复制 |
| `arch_user_return_copy_syscall()` | 复制syscall帧 | fork/clone |

**实现场景（linux_layer/）**：

| 场景 | 复杂度 | 描述 |
|------|--------|------|
| **同地址空间线程复制** | ⭐ 简单 | clone without CLONE_VM<br/>• 共享地址空间<br/>• 复制寄存器状态<br/>• 修改返回值 |
| **信号处理** | ⭐⭐ 中等 | 信号递送<br/>• 切换到信号处理函数<br/>• 可能使用备用栈<br/>• 保存并恢复上下文 |
| **跨地址空间进程复制** | ⭐⭐⭐ 复杂 | fork<br/>• 复制地址空间<br/>• 复制寄存器状态<br/>• 修改返回值 |

**实现顺序**：

1. **同地址空间线程复制**
   - clone(CLONE_VM)
   - 验证：pthread_create、同进程内线程操作

2. **信号处理**
   - rt_sigaction, rt_sigprocmask, sigaltstack
   - 验证：信号处理函数能被正确调用

3. **fork（移到Phase 4）**
   - 需要地址空间复制，依赖Phase 3的基础机制

**里程碑**：
- ✅ 通用的执行流控制框架
- ✅ clone基本功能
- ✅ 信号处理基础
- ✅ 为fork做好准备

### Phase 4：跨地址空间进程复制（P1 - 核心功能）

**目标**：实现fork（需要复制地址空间）

| 组 | syscall/基础设施 | 依赖 |
|----|------------------|------|
| COW基础设施 | 页故障处理、物理页分裂 | mmap, mprotect |
| 地址空间复制 | vspace_copy | Phase 3执行流控制 |
| 进程复制 | fork | Phase 3执行流控制 + COW |

**为什么fork在后面**：
- fork需要同时处理：地址空间复制 + 执行流控制
- 执行流控制部分已经在Phase 3实现并验证
- fork = 地址空间复制 + Phase 3的执行流控制机制

**里程碑**：完整的进程创建机制，支持多进程程序

### Phase 5：程序执行（P2 - 重要功能）

**目标**：支持程序加载和执行

| 组 | syscall | 依赖 |
|----|---------|------|
| 程序加载 | execve | 内存管理 |
| 环境变量 | execve相关 | - |

**里程碑**：能够运行shell和复杂的用户程序

### Phase 6：文件系统（P2 - 重要功能）

**目标**：实现基本的文件系统访问

| 组 | syscall | 依赖 |
|----|---------|------|
| 文件描述符 | open, close, read, write | - |
| 文件操作 | lseek, stat, fstat | - |
| 目录操作 | opendir, readdir, closedir | - |

**里程碑**：支持基本的文件操作

### Phase 7：高级功能（P3 - 扩展功能）

**目标**：实现更多Linux功能

**功能组**：
- **IPC**：pipe, socket, shm, msgq
- **网络**：socket相关syscall
- **时间**：clock相关syscall
- **资源限制**：getrlimit, setrlimit
- **用户/组**：getuid, setuid等

**里程碑**：逐步接近完整的Linux兼容性

---

## 执行流控制框架

### 设计理念

多个Linux兼容层场景涉及"构造用户态返回帧并恢复执行"：

- **线程复制**（clone）：复制父线程的寄存器状态，子线程返回不同值
- **信号处理**：切换到信号处理函数，可能使用备用栈
- **fork**：复制父进程的执行状态，子进程返回0

这些场景**共享相同的底层机制**：
1. 构造trap_frame（用户态寄存器状态）
2. 安装到内核栈
3. 配置调度切换路径

### 三层架构

```
┌─────────────────────────────────────────┐
│  第一层：业务逻辑                        │
│  - copy_thread()                        │
│  - signal handling                      │
│  - fork                                │
└──────────────┬──────────────────────────┘
               │ 调用场景接口
┌──────────────▼──────────────────────────┐
│  第二层：场景组装                        │
│  - arch_user_return_copy_syscall()     │
│  - (未来)信号场景接口                    │
└──────────────┬──────────────────────────┘
               │ 调用基础原语
┌──────────────▼──────────────────────────┐
│  第三层：基础原语（架构特定）            │
│  - arch_user_return_install()          │
│  - arch_ctx_inherit()                  │
└─────────────────────────────────────────┘
```

### 核心接口

#### 第三层：基础原语（core/kernel/task/arch_thread.c）

```c
/**
 * 安装用户态返回帧（最底层原语）
 *
 * 将给定的trap_frame安装到线程的内核栈，配置调度切换路径
 * 所有用户态返回的最终步骤
 */
error_t arch_user_return_install(Thread_Base* t, const struct trap_frame* tf);

/**
 * 复制架构上下文
 *
 * 复制Arch_Task_Context（callee-saved寄存器等）
 * 不涉及用户态返回帧
 */
error_t arch_ctx_inherit(const Thread_Base* src, Thread_Base* dst);
```

#### 第二层：场景组装（core/kernel/task/arch_thread.c）

```c
/**
 * 场景：复制syscall帧（fork/clone）
 *
 * 从源线程复制syscall时的trap_frame，修改返回值，安装到目标线程
 */
error_t arch_user_return_copy_syscall(Thread_Base* dst,
                                     const Thread_Base* src,
                                     u64 ret_val);
```

#### 第一层：业务逻辑（linux_layer/）

```c
/* fork场景 */
Thread_Base* copy_thread(Thread_Base* parent, Tcb_Base* child, ...);

/* 信号场景（未来） */
error_t setup_signal_frame(Thread_Base* t, signal_info* info);
```

### 复用性分析

| 场景 | 复用的原语 | 额外工作 |
|------|-----------|---------|
| **clone（同地址空间）** | `arch_ctx_inherit`<br/>`arch_user_return_copy_syscall` | 无额外工作 |
| **信号处理** | `arch_user_return_install` | 构造指向信号处理函数的trap_frame<br/>可能切换到备用栈 |
| **fork** | `arch_ctx_inherit`<br/>`arch_user_return_copy_syscall` | + 地址空间复制<br/>+ 进程管理 |

### 实现顺序

1. **验证基础原语**（x86_64 + aarch64）
   - 确保`arch_user_return_install`正确安装帧
   - 确保调度切换路径正确

2. **实现简单场景**
   - clone without CLONE_VM
   - 验证：同地址空间线程复制

3. **扩展到信号处理**
   - 基于相同原语实现信号递送
   - 验证：信号处理函数被正确调用

4. **扩展到fork**
   - 基于相同原语 + 地址空间复制
   - 验证：跨地址空间进程复制

### 优势

- ✅ **代码复用**：多个场景共享底层机制
- ✅ **清晰分层**：每层职责明确
- ✅ **易于扩展**：新增场景只需组合原语
- ✅ **架构独立**：架构特定细节在底层

---

## 详细实现清单

> **注意**：以下详细清单保持原有结构作为参考，但实际实现时应根据测例需求和阶段性目标灵活调整。实现时应遵循"先基础设施、后功能扩展"的原则。

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

## Step 0a：`write`（`__NR_write`：x86_64 为 1，aarch64 为 64）

**目的**：满足 libc/测例对 **stdout/stderr** 的最小输出；在 VFS 之前不实现普通文件与管道。

**依赖**：Step 0（返回值写入 `ARCH_SYSCALL_RET`）。

**文件**

| 文件 | 改动 |
|------|------|
| [`linux_layer/io/sys_write.c`](../../linux_layer/io/sys_write.c) | `sys_write`：仅 fd `1`/`2`，内核缓冲区分块 `memcpy` 后输出 |
| [`linux_layer/syscall/syscall_entry.c`](../../linux_layer/syscall/syscall_entry.c) | `case __NR_write:`；参数顺序与 [`trap.h`](../../core/include/arch/x86_64/trap/trap.h) 中 `ARCH_SYSCALL_ARG_*` 一致 |
| [`include/syscall.h`](../../include/syscall.h) | 声明 `sys_write` |

**设计说明**：[`STDIO_SHIM.md`](STDIO_SHIM.md)（后续 fd 表与 VFS 接入点）。

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

**状态**：当前实现为 **`-ENOSYS`**（保留 syscall 入口，等待 VMA/range 模型补全）。

**文件**：`linux_layer/mm/sys_mremap.c`。

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

---

## 实现灵活性指导

### 测例驱动的优先级调整
**原则**：以上阶段和顺序为参考，实际实现应根据测例需求灵活调整。

**示例**：
- 如果测例需要进程间通信，可以提前实现pipe相关的syscall
- 如果测例需要文件访问，可以提前实现基础文件系统syscall
- 如果遇到"鸡蛋问题"（如write需要文件系统），可以先用简化版本

### 依赖关系处理
**强依赖**：必须按顺序实现（如fork依赖COW）
**弱依赖**：可以并行实现（如信号和文件系统）
**循环依赖**：先用简化版本打破循环（如write shim）

### 多架构考虑
**架构无关代码**：linux_layer/中的主要逻辑
**架构相关代码**：syscall号、寄存器接口、trap处理
**实现策略**：设计时就考虑多架构，避免架构特定逻辑扩散

---

## 状态跟踪

| Phase | 状态 | 当前重点 | 下一阶段 |
|-------|------|----------|----------|
| Phase 0 | ✅ 完成 | syscall框架已实现 | Phase 1 ✅ 完成 |
| Phase 1 | ✅ 完成 | 进程管理基础已实现 | Phase 2 ✅ 完成 |
| Phase 2 | ✅ 完成 | 内存管理已实现 | Phase 3 ✅ 完成 |
| Phase 3 | ✅ 完成 | 执行流控制机制已实现 | Phase 4 ✅ 完成 |
| Phase 4 | ✅ 完成 | 跨地址空间进程复制（fork）已实现 | Phase 5 |
| Phase 5 | 🔄 进行中 | 程序执行（execve）待实现 | Phase 6 |
| Phase 6 | 📋 计划中 | 文件系统待实现 | Phase 7 |
| Phase 7 | 📋 计划中 | 高级功能待实现 | - |

**更新频率**：每完成一个主要功能或阶段性里程碑后更新状态。

**最新更新（2026-04-25）**：
- Phase 0-4：已完成基础syscall实现（11个）
- ✅ **wait4完整实现**：支持所有Linux标准pid选项（>0, -1, 0, <-1）+ WNOHANG
- ✅ **proc_registry扩展**：O(1) PID查找 + 反向查询（ppid/pgid）
- ✅ **IPC阻塞机制**：替代轮询，保持架构一致性
- ✅ **竞态条件修复**：exit_state三态管理（running/zombie/reaped）
- 详细实现记录：见 `doc/linux_compat/WAIT4_IMPLEMENTATION_STATUS.md`
- 下一步：完善write安全性，实现rusage或信号机制支持
