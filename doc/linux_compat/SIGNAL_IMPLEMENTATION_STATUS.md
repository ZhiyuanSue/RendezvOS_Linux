# Phase 2B Signal Implementation Status

> Last updated: compat-layer iteration (kernel restore + rt_sigreturn + safe user copy).  
> Design reference: [`SIGNAL_DELIVERY_TRAP_PATHS.md`](SIGNAL_DELIVERY_TRAP_PATHS.md), [`IPC_BASED_SIGNAL_DESIGN.md`](IPC_BASED_SIGNAL_DESIGN.md).

## Summary

| Area | x86_64 | aarch64 | Notes |
|------|--------|---------|-------|
| Layer A `linux_queue_signal` | ✅ | ✅ | Thread walk + pending; SIGCHLD on child exit |
| Layer B syscall return delivery | ✅ | ✅ | Path A: rcx/ELR + SP |
| `rt_sigaction` / `rt_sigprocmask` | ✅ | ✅ | `linux_mm_*_user` |
| `sigaltstack` | ✅ | ✅ | map_handler |
| `rt_sigreturn` | ✅ | ✅ | Kernel `linux_signal_restore_t` |
| User-stack `rt_sigframe` | ❌ | ❌ | glibc needs layout + restorer |
| Path B (fork first return) | ❌ | ❌ | Needs hook at `kstack_bottom-1` |
| Page-fault → SIGSEGV queue | ❌ | ❌ | See core/trap + compat fault |
| SA_SIGINFO 3-arg handlers | ❌ | ❌ | Flag stored only |
| Core dump on SIGQUIT/… | ❌ | ❌ | No ELF core yet |

---

## Implemented (compat layer)

### Syscalls

- `kill`, `rt_sigaction`, `rt_sigprocmask`, `sigaltstack`, **`rt_sigreturn`**
- Wired in `linux_layer/syscall/syscall_entry.c`

### Layer A — `linux_layer/signal/signal_queue.c`

- Queue to process + thread pending; wake non-running target
- `linux_queue_signal_thread()` for thread-directed queue
- **SIGCHLD**: `sys_exit` queues to parent unless parent has `SA_NOCLDWAIT`

### Layer B — `linux_layer/signal/signal_deliver.c`

- Default actions: terminate (`sys_exit(128+sig)`), ignore, stop (stub)
- User handler: path A register install (x86: `rcx`/`rdi`/`user_rsp_scratch`; aarch64: `ELR`/`SP`/`REGS[0]`/`SP_EL0`)
- **SA_RESETHAND**, **SA_NODEFER**, handler **mask** on entry
- Saves **`linux_signal_restore_t`** in thread append for `rt_sigreturn`

### Return from handler — `signal_restore.c` + `sys_rt_sigreturn.c`

- Restores PC, SP, syscall return value (aarch64 x0), blocked mask, alt-stack `SS_ONSTACK`

### Safe user memory

- `linux_mm_load_from_user` / `linux_mm_store_to_user` / `linux_mm_copy_user_range` → `map_handler_copy_data_range` (`linux_mm_radix.c`)
- `rt_sigaction` / `rt_sigprocmask` / `sigaltstack` / `clone` SETTID / `clear_tid` on exit

### Init / fork

- `linux_signal_init_proc_append` / `linux_signal_init_thread_append` (`signal_init.c`)
- ELF init + fork/clone: disposition inherit; child pending cleared

---

## Not implemented (confirmed gaps)

### 1. User-stack Linux `rt_sigframe`

Handlers cannot use glibc’s implicit sigreturn trampoline until compat builds a real frame on the user stack (`linux_mm_store_to_user`) with arch-specific layout.

**Core**: No change required. Optional thin `arch_signal_set_return_regs(tf, pc, sp, arg0)` only if x86/aarch64 glue duplicates — **not requested yet**.

### 2. Path B — delivery without syscall (fork child / `arch_return_to_user`)

Child’s first return uses `kstack_bottom - 1` trap frame, not the syscall scratch frame. Pending signals queued before first userspace run may not run until a syscall unless compat patches that frame in `sys_fork` / `copy_thread` completion.

**Core reuse**: `arch_return_to_user`, `arch_drop_to_user` (read-only). **Compat work**: call shared deliver helper on child bootstrap frame.

### 3. Async delivery (interrupt return)

Page faults queue `SIGSEGV` and call `linux_deliver_pending_signals(tf)` before returning to user (`linux_page_fault_irq.c`). Other trap classes (non-PF) still need the same pattern if required.

**Core**: May need a single “before eret to EL0” hook if multiple compat entry points duplicate — **evaluate with maintainer** before core change.

### 4. `tgkill`, `pause`, `signalfd`, timer signals

Not in current syscall table.

### 5. `SA_SIGINFO`, `SA_RESTART`

Flags stored; 3-arg handlers and syscall restart not implemented.

### 6. Process stop / continue (SIGSTOP/SIGCONT)

Logged / stub; needs scheduler + job control semantics.

---

## Core reuse map

| Need | Core API | Compat usage |
|------|----------|--------------|
| Trap frame / syscall ABI | `struct trap_frame`, `ARCH_SYSCALL_*` | deliver + sigreturn |
| x86 user SP scratch | `percpu(user_rsp_scratch)` | deliver / restore |
| aarch64 EL0 SP | `Arch_Task_Context.sp_el0`, `msr SP_EL0` | deliver / restore |
| Fresh context on fork | `arch_ctx_refresh` (in `copy_thread`) | already used by fork path |
| Return to user (bootstrap) | `arch_return_to_user` | path B (not wired) |
| User page access | `have_mapped`, `map_handler` | via `linux_mm_*_user` |
| Process lookup | `find_task_by_pid` (compat registry) | kill, SIGCHLD |
| Exit / wait IPC | kmsg + wait_port | orthogonal to signal queue |

**No new core APIs are strictly required** for the current Phase 2B scope except optionally an **EL0 return hook** for fault/interrupt delivery (maintainer decision).

---

## Suggested test order

1. `rt_sigaction` + `kill(self)` + handler calls `rt_sigreturn`
2. Blocked signal + `sigprocmask` unblock + delivery on next syscall
3. `fork` + child exit → parent `SIGCHLD` pending
4. Repeat on `ARCH=aarch64`

```bash
make ARCH=x86_64 config && make ARCH=x86_64 user && make ARCH=x86_64 build
make ARCH=x86_64 run
```

---

## File index

| File | Role |
|------|------|
| `linux_layer/signal/signal_queue.c` | Layer A |
| `linux_layer/signal/signal_deliver.c` | Layer B |
| `linux_layer/signal/signal_restore.c` | rt_sigreturn restore |
| `linux_layer/signal/signal_stack.c` | User SP restore (x86 scratch / aarch64 SP_EL0) |
| `linux_layer/signal/signal_init.c` | Disposition/mask init |
| `linux_layer/proc/sys_rt_sigreturn.c` | Syscall wrapper |
| `include/linux_compat/proc_compat.h` | `linux_signal_restore_t` |
