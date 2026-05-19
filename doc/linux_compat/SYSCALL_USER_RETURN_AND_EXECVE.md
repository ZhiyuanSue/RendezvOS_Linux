# 路径 A：syscall 用户返回与 execve 接线

本文说明 core 中 `arch_syscall_*` 三个 API 的语义、与信号/exec 的关系，以及 **execve** 在 linux_layer 的推荐实现顺序。声明位于各架构 `core/include/arch/<arch>/tcb_arch.h`，实现在 `core/arch/<arch>/task/arch_thread.c`。

相关文档：

- [`SIGNAL_DELIVERY_TRAP_PATHS.md`](SIGNAL_DELIVERY_TRAP_PATHS.md) — 路径 A / 路径 B 总览
- [`BUGFIX_FORK_SYSCALL_STALE_USER_CONTEXT.md`](BUGFIX_FORK_SYSCALL_STALE_USER_CONTEXT.md) — syscall 内 `ctx` 可能陈旧；与 **get** 不读 `ctx` 的原因
- [`doc/ai/INVARIANTS.md`](../ai/INVARIANTS.md) — `vspace_clear_user_mappings` 前置条件

---

## 1. 两条返回用户态的路径

| | **路径 A** | **路径 B** |
|--|------------|------------|
| 场景 | 线程**已在 syscall 中**（`syscall_ctx`） | 子线程**首次**运行、`run_elf_program`、`run_copied_thread` |
| 帧位置 | 当前 syscall 在内核栈上的 `struct trap_frame` | `((trap_frame *)kstack_bottom) - 1` |
| core 入口 | `arch_syscall_set/get_user_return` | `arch_empty_drop_trap_frame` + `arch_return_to_user` |
| 汇编出口 | x86 `arch_exit_kernel` / aarch64 `el0_trap_exit` | `arch_drop_to_user`（若已实现） |

**execve 成功**、**rt_sigreturn**、**信号投递到 handler** 均走路径 A，**不得**对正在 syscall 返回的线程调用 `arch_return_to_user`。

### 1.1 路径 A 上硬件实际使用的字段

| 语义 | x86_64 | aarch64 |
|------|--------|---------|
| 用户 PC | `trap_frame->rcx`（`sysret`） | `trap_frame->ELR` |
| 用户 SP | `percpu(user_rsp_scratch)` | `trap_frame->SP` + `SP_EL0`（`set` 时同步） |
| syscall 返回值 | `trap_frame->rax` | `trap_frame->REGS[0]`（x0） |
| 用户函数第 1 个整型参数（额外） | `trap_frame->rdi` | `trap_frame->REGS[0]`（与返回值**共槽**，见下文） |

---

## 2. `arch_syscall_get_user_return` 的 `ctx` 参数

### 2.1 现状

两个架构的实现里 **`ctx` 均被 `(void)ctx` 忽略**：

- **PC / syscall 返回值**：只从 `trap_frame` 读（与 syscall 压栈/出口一致）。
- **用户 SP（x86）**：只从 **`percpu(user_rsp_scratch)`** 读，**不**读 `ctx->user_rsp`。  
  原因：在 syscall 处理过程中，权威的用户栈指针在入口路径写入 scratch；`Arch_Task_Context::user_rsp` 主要在 **上下文切换** 时更新，可能**落后于** scratch（与 fork 文档中的「陈旧 ctx」同类问题）。
- **用户 SP（aarch64）**：从 `trap_frame->SP` 读（与 `el0_trap_exit` 一致）；`ctx` 同样未使用。

### 2.2 为何签名仍带 `ctx`

1. **与 `arch_syscall_set_user_return` 对称**：`set` 在写入 scratch / `tf->SP` 的同时会更新 `ctx->user_rsp` 或 `ctx->sp_el0`，便于之后 `switch_to` / fork 合并看到一致 SP。  
2. **预留扩展**：若将来需要在「syscall 内、且 scratch 不可用」的场景从 `ctx` 回填，可在 **refresh 之后** 再读 `ctx`，而不改 linux 调用签名。  
3. **调用方习惯**：linux 侧统一传入 `&current_thread->ctx`，与 `set` 相同，避免「有时传 NULL、有时不传线程」的分叉。

### 2.3 审查结论与可选精简

**当前没有功能依赖 `get` 的 `ctx`。** 若坚持最小 API，可在后续提交中：

- 删除 `get` 的 `ctx` 参数；或  
- 在 `get` 中当 scratch/`tf` 不可用时才读 `ctx`（需先 `arch_ctx_refresh`，并写清不变式）。

在删除前，保留 `ctx` 仅为 **对称与预留**，不是疏漏。

---

## 3. `arch_syscall_set_user_return`

```c
void arch_syscall_set_user_return(struct trap_frame *tf, Arch_Task_Context *ctx,
                                  vaddr user_pc, vaddr user_sp, u64 syscall_ret);
```

在**当前 syscall 帧**上设定：返回用户时的 **PC、SP、syscall 返回值**；`ctx` 非 NULL 时同步 `user_rsp` / `sp_el0`。

| 调用方 | 作用 |
|--------|------|
| 信号投递末尾 | PC → handler，SP → 信号帧，**保留**被中断 syscall 的返回值（x86 在 `rax`） |
| `rt_sigreturn` | 恢复进入 handler 前保存的 PC/SP/返回值 |
| `linux_restore_main_stack_if_needed` | 仅改 SP，PC/返回值不变（先 `get` 再 `set`） |
| **execve 成功（计划）** | PC → `e_entry`，SP → 新初始栈顶，`syscall_ret` → 0 |

---

## 4. `arch_syscall_set_user_int_arg` 与 signal deliver

```c
void arch_syscall_set_user_int_arg(struct trap_frame *tf, unsigned int arg_index,
                                   u64 value);
```

按 **用户态 SysV / AAPCS64 整型参数寄存器顺序** 设置 `trap_frame` 中对应槽位（下标 `0 .. NR_ABI_PARAMETER_INT_REG-1`）：

| `arg_index` | x86_64 | aarch64 |
|-------------|--------|---------|
| 0 | `rdi` | `REGS[0]` (x0) |
| 1 | `rsi` | `REGS[1]` (x1) |
| 2 | `rdx` | `REGS[2]` (x2) |
| 3 | `rcx` | `REGS[3]` (x3) |
| 4 | `r8` | `REGS[4]` (x4) |
| 5 | `r9` | `REGS[5]` (x5) |
| 6–7 | — | `REGS[6]`–`REGS[7]` |

**路径 A 注意（与 syscall 出口复用寄存器）**：

- x86_64：`set_user_return` 已用 **`rcx` 作返回 PC**；**不要**对 `arg_index == 3` 再写用户参数。信号 / 一般 handler 只用 0–2、4–5 即可。  
- aarch64：`REGS[0]` 同时是 **`ARCH_SYSCALL_RET`**；投递 `handler(int sig)` 时先 `set_user_return`（写入待保存的 syscall ret），再 `set_user_int_arg(tf, 0, sig)` 覆盖 x0。

### 4.1 为何 signal deliver 需要 `arg_index == 0`

Phase 2B 为 **`void handler(int sig)`**。返回用户时要跳到 handler，且 **第一个 ABI 整型参数 = `sig`**：

```c
arch_syscall_set_user_return(tf, &th->ctx, (vaddr)handler, user_sp, syscall_ret);
arch_syscall_set_user_int_arg(tf, 0, (u64)sig);
```

将来 **SA_SIGINFO** 可在同一 API 上设 `1`、`2`（x86 `rsi`/`rdx`，aarch64 x1/x2），无需再增单参数函数。

### 4.2 何时不需要 `set_user_int_arg`

| 场景 | 原因 |
|------|------|
| **execve** | 参数在 **用户栈**（`argc`/`argv`/`envp`/`auxv`） |
| **rt_sigreturn** | 只需 `set_user_return` 恢复保存的三元组 |

---

## 5. execve 接线设计（linux_layer）

### 5.1 原则

| 项 | 选择 |
|----|------|
| 地址空间 | **原地**：同一 `Tcb_Base` / `VSpace` / ASID；**不** `create_vspace` / `del_vspace` |
| 清映射 | `vspace_clear_user_mappings(vs, &percpu(Map_Handler))` |
| 加载 | `load_elf_to_vs` |
| 栈映射 | `generate_user_stack`（`thread_loader.h`） |
| 栈内容 | linux：`argv` / `envp` / `auxv`（`exec_stack.c` 一类） |
| 进用户 | **仅** `arch_syscall_set_user_return(syscall_ctx, &th->ctx, entry, user_sp, 0)` |
| 不要 | 路径 B、对 exec 使用 `set_user_int_arg`、再次 `register_process` |

前置条件见 `INVARIANTS.md`：同 task **其它线程已结束**；`vs->tlb_cpu_mask == 0`。

### 5.2 推荐步骤（`sys_execve`）

```
1. 校验并 copy_from_user：filename、argv、envp
2. 解析 ELF（Phase 3a：内嵌镜像按名匹配；3b：文件系统读入）
3. 校验 ELF（在 vspace_clear 之前失败则不动 vs）
4. de_thread：结束同 task 其它线程（仿 exit_group 设 EXIT_REQUESTED）
5. 等待 TLB quiesce（tlb_cpu_mask == 0）
6. vspace_clear_user_mappings
7. load_elf_to_vs → entry、max_load_end
8. user_sp = generate_user_stack
9. build_initial_stack(user_sp, argv, envp, auxv)
10. linux_exec_reset_proc_state（brk、mmap_hint、清 pending/restore/altstack；不 register_process）
11. arch_syscall_set_user_return(syscall_ctx, &th->ctx, entry, user_sp, 0)
12. syscall_entry：成功时可 skip_syscall_ret_assign；清信号状态后再 deliver
```

失败路径：在步骤 6 之前失败则返回 `-errno`，旧映射保留。

### 5.3 与 `linux_elf_init_handler` 的区别

| | 首次 `gen_task_from_elf` | exec |
|--|--------------------------|------|
| `register_process` | 是 | **否**（同 pid） |
| MM | 新 `create_vspace` | `vspace_clear` + `load_elf_to_vs` |
| 进用户 | 路径 B（`run_elf_program`） | 路径 A（`arch_syscall_set_user_return`） |
| brk / signal | `linux_elf_init_handler` | `linux_exec_reset_proc_state` |

### 5.4 建议文件（linux_layer）

- `linux_layer/proc/sys_execve.c` — syscall
- `linux_layer/loader/exec_stack.c` — 初始栈布局
- `linux_layer/loader/linux_exec_reset.c` — exec 后 proc/thread 状态重置
- `syscall_entry.c` — `__NR_execve` 分支

core 侧 **无需** 再增加 `task_exec_enter_user`；已有 `generate_user_stack`、`load_elf_to_vs`、`vspace_clear_user_mappings`、`arch_syscall_*` 即可。

### 5.5 分阶段交付

| 阶段 | 内容 |
|------|------|
| 3a | 单线程、内嵌 ELF、简单 argv |
| 3b | de_thread + TLB 等待、多线程 task |
| 3c | 完整 auxv、env |
| 3d | FS、shebang、`PT_INTERP` |

---

## 6. 审查清单（exec + 路径 A）

1. ELF 无效时是否在 **`vspace_clear` 之前** 返回？  
2. 其它线程与 `tlb_cpu_mask` 是否满足 core 前置？  
3. 是否**仅**使用 `arch_syscall_set_user_return`，未误用路径 B？  
4. exec 是否**未**调用 `arch_syscall_set_user_int_arg`？  
5. 是否**未**再次 `register_process`？  
6. exec 前是否清 signal pending / restore / altstack？  
7. `get` 的 `ctx` 是否理解为其当前为占位（可选后续删除）？

---

## 7. 维护

- 修改 `arch_syscall_*` 语义或路径 A 汇编出口时，同步更新本文与 [`SIGNAL_DELIVERY_TRAP_PATHS.md`](SIGNAL_DELIVERY_TRAP_PATHS.md)。
- exec 落地后可在 [`SYSCALLS.md`](SYSCALLS.md) Phase 3 条目链接本文。
