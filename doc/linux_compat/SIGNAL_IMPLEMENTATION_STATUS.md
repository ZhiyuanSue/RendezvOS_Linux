# Phase 2B Signal Implementation Status

> Last updated: **2026-05-19** — cross-arch verification gate (see [`CROSS_ARCH_VERIFICATION_LOG.md`](CROSS_ARCH_VERIFICATION_LOG.md)).  
> Design reference: [`SIGNAL_DELIVERY_TRAP_PATHS.md`](SIGNAL_DELIVERY_TRAP_PATHS.md), [`IPC_BASED_SIGNAL_DESIGN.md`](IPC_BASED_SIGNAL_DESIGN.md).

## Summary

| Area | x86_64 | aarch64 | Notes |
|------|--------|---------|-------|
| Layer A `linux_queue_signal` | ✅ | ✅ | SIG_IGN flushes pending; thread walk |
| Layer B syscall return delivery | ✅ | ✅ | Path A + invalid low handler dropped silently |
| `rt_sigaction` / `rt_sigprocmask` | ✅ | ✅ | `linux_mm_*_user`; flush on SIG_IGN/SIG_DFL |
| `sigaltstack` | ⚠️ | ⚠️ | #21 fix: field copy + alt region validate (2026-05-19) |
| `rt_sigreturn` | ✅ | ✅ | **Full trap frame** on both arches via `signal/arch/` |
| Integrated tests #08, #44 | ✅ | ✅ | delivery + SIG_DFL/SIG_IGN (2026-05-19 log) |
| User-stack `rt_sigframe` | ❌ | ❌ | glibc needs layout + restorer |
| Path B (fork first return) | ⚠️ | ⚠️ | `linux_deliver_pending_signals` called on child bootstrap TF in `sys_fork` |
| Page-fault → SIGSEGV queue | ⚠️ | ⚠️ | `linux_page_fault_irq.c` (partial) |
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
- User handler: path A via `arch_syscall_set_user_return` + arg0
- **SA_RESETHAND**, **SA_NODEFER**, handler **mask** on entry
- Saves context via **`linux_signal_arch_save_context()`** (`signal/arch/signal_context_*.c`)
- SIG_IGN / invalid `< PAGE_SIZE` handler: clear thread **and** proc pending (no spurious warn)

### Return from handler — `signal_restore.c` + `sys_rt_sigreturn.c`

- **`linux_signal_arch_restore_context()`**: x86 via `arch_syscall_set_user_return`; aarch64 restores full `REGS[]`, `ELR`, `SPSR`, `SP_EL0`
- Restores blocked mask, alt-stack `SS_ONSTACK`

### Arch layout (2026-05 refactor)

```
include/linux_compat/signal/
  signal_context.h
  signal_restore_arch.h → signal_restore_arch_{x86_64,aarch64}.h
linux_layer/signal/
  signal_deliver.c          # arch-neutral Linux semantics
  signal_restore.c
  arch/signal_context_x86_64.c
  arch/signal_context_aarch64.c
```

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

1. `rt_sigaction` + `kill(self)` + handler calls `rt_sigreturn` — **#08, #44**
2. Blocked signal + `sigprocmask` unblock + delivery on next syscall
3. `fork` + child exit → parent `SIGCHLD` pending — **#07, #49**
4. **Paired** `make ARCH=x86_64 run` and `make ARCH=aarch64 run`; diff per [`CROSS_ARCH_VERIFICATION_LOG.md`](CROSS_ARCH_VERIFICATION_LOG.md)

```bash
make ARCH=x86_64 config && make ARCH=x86_64 user && make ARCH=x86_64 build && make ARCH=x86_64 run | tee x86_64_run.log
make ARCH=aarch64 config && make ARCH=aarch64 user && make ARCH=aarch64 build && make ARCH=aarch64 run | tee aarch64_run.log
```

---

## File index

| File | Role |
|------|------|
| `linux_layer/signal/signal_queue.c` | Layer A; `linux_signal_flush_pending()` |
| `linux_layer/signal/signal_deliver.c` | Layer B (arch-neutral) |
| `linux_layer/signal/signal_restore.c` | rt_sigreturn orchestration |
| `linux_layer/signal/arch/signal_context_*.c` | Arch save/restore |
| `linux_layer/signal/signal_init.c` | Disposition/mask init |
| `linux_layer/proc/sys_rt_sigaction.c` | Syscall; flush on SIG_IGN/SIG_DFL |
| `linux_layer/proc/sys_rt_sigreturn.c` | Syscall wrapper |
| `include/linux_compat/proc_compat.h` | `linux_signal_restore_t` + arch sub-struct |
