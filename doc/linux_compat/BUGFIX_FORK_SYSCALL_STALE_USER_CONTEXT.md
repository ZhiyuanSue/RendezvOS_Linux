# Bug：`copy_thread`/fork 在 syscall 路径上继承陈旧用户栈与 TLS（及测例 cookie 边界）

本篇包含 **两类相关问题**：（A）fork 时用错用户可见上下文；（B）仅影响 **integrated 用户测例** 的 clean_server/test_cookie 同步。二者可独立理解与修复。

---

## A. fork 发生在 syscall 内时，`ctx` 与用户硬件态不同步（根因）

### 适用范围

- **入口**：fork 的实现通过 core 侧 [`copy_thread()`](../../core/kernel/task/thread.c) 克隆用户线程上下文。
- **架构**：尤以 **x86_64** 为典型：**用户态 `%rsp`** 在syscall 入口保存在 **per-CPU scratch**（如 `user_rsp_scratch`），而 `Thread_Base::ctx.user_rsp` 往往主要在 **上下文切换** 的路径上更新；在长 syscall（或尚未切走 CPU）的阶段， **`ctx.user_rsp` 可能落后于真实硬件栈指针**。
- **AArch64**：同类问题表现在 **SP_EL0**、用户 TLS（如 **TPIDR_EL0**）——同样应在 **syscall/陷入服务** 语义下优先读硬件寄存器，而不是盲信最近一次 `switch_to` 写回的 `Arch_Task_Context` 快照。

### 症状

- 子进程 **`arch_ctx_merge`** 时使用 **陈旧 `user_rsp`（或 aarch64 sp_el0）**，用户态 **`ret`/返回桩** 从错误栈偏移取指令或取数；
- 表现包括：**`RIP` 异常、`exec` 对用户地址 `0`、`ret` 弹出非代码地址（如参数区）**；
- fork 后与 **COW 写栈**组合时更明显，易被误判成纯 MM 故障。

### 根本原因

fork 在用户进程里往往通过 **syscall（如 x86 clone/fork ABI）** 进入内核：此时 **权威的 EL0/user 寄存器快照**存在于 **入口保存区 / 系统寄存器**，而不保证已经反映到 **`src_thread->ctx`**。**`copy_thread` 若在合并前直接使用 `src_thread->ctx`**，会把错误栈指针合并到子线程，子线程第一次返回用户态即错误。

### 修复思路（工程上）

1. **在 `copy_thread` 合并寄存器上下文之前**：把 `src_thread->ctx` 拷到局部 **`src_ctx`**，调用架构函数（例如 **`arch_ctx_refresh(&src_ctx)`**）：
   - **x86_64**：从 **percpu `user_rsp_scratch`** 回填 `user_rsp`，并从 **MSR** 取 **FS/GS BASE**（与现有 MSR 使用约定一致）；
   - **AArch64**：`mrs` **SP_EL0**、**TPIDR_EL0** 等。
2. 再对已刷新的快照执行 **`arch_ctx_merge_from_src(&dst_thread->ctx, &src_ctx)`**，保证trap frame 与外显用户状态一致。

这样既不动 Linux 专有 fork，也不把syscall 语义塞进 arch 以外的乱处： **“syscall 进行时读真机寄存器再合并”** 这一事实由 **单次 refresh API**表达。

### 参考锚点

- [`core/kernel/task/thread.c`](../../core/kernel/task/thread.c)： `copy_thread` 内 **`Arch_Task_Context src_ctx`** + **`arch_ctx_refresh`** 再 **`arch_ctx_merge_from_src`**。
- [`core/arch/x86_64/task/arch_thread.c`](../../core/arch/x86_64/task/arch_thread.c)、[`core/arch/aarch64/task/arch_thread.c`](../../core/arch/aarch64/task/arch_thread.c)：`arch_ctx_refresh` 的实现。
- 头文件：[`core/include/arch/x86_64/tcb_arch.h`](../../core/include/arch/x86_64/tcb_arch.h)、[`core/include/arch/aarch64/tcb_arch.h`](../../core/include/arch/aarch64/tcb_arch.h)。

---

## B. （测例跑道）仅有 parent 仍会退出的线程：必须通过 clean_server 完成 cookie（现象与修复）

### 适用范围

Integrated 用户测试中，runner **按 CPU test slot 的 cookie** 等待单次 ELF 退出；往往依赖 **`clean_server` → linux_user_test_notify_exit**。

### 症状

runner 一侧长时间打印类似 **cookie 不匹配 / 仍为 0**（或等价“等不到退出事件”）；而同一进程在用户态已通过 **`exit(0)`** 走 **`sys_exit`**，parent 仍存在时 **曾**跳过对 clean_server 的通知，导致 **测试同步无法前进**。

### 根本原因与修复思路（概念）

这不是 fork 语义本身，而是 **test_cookie 驱动的清理路径**：当有 **`linux_thread_append` cookie** 需要跨模块同步退出时，即使 **存在 parent**（非孤儿进程），仍需 **顺带走 clean_server 通知**，使 **`linux_user_test_notify_exit`** 能推进 slot cookie。

典型修复点：[`linux_layer/syscall/thread_syscall.c`](../../linux_layer/syscall/thread_syscall.c) **`sys_exit`** 中，在 **`test_cookie != 0`** 的条件下 **也向 clean_server 发清理/通知**，与「孤儿才通知」路径解耦组合。

---

## 与「Linux fork 语义完善度」的区分

本节 A 的问题是 **“继承运行中 syscall 的机器态”是否正确**——修完后 fork/wait+COW **用户栈**路径可与 Linux 分层预期一致。**不**等价于已实现完整 Linux：`fd`/`信号`/完整 `vfork`/`clone` flags 等大语义仍可按 [`MM_AND_COW.md`](MM_AND_COW.md)、[`DATA_MODEL.md`](DATA_MODEL.md) 分阶段推进。
