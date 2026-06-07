# Cross-Architecture Verification Log (linux_layer)

> **Purpose**: Record **paired** x86_64 / aarch64 runs after proc/signal/wait milestones.
> Harness PASS alone is insufficient — compare **stdout structure** and kernel-side noise.

**Last updated**: 2026-05-19 (maintainer runs: `aarch64_run.log`, `x86_64_run.log`)

---

## Run command (both arches)

```bash
make ARCH=x86_64 config && make ARCH=x86_64 user && make ARCH=x86_64 build
make ARCH=x86_64 run | tee x86_64_run.log

make ARCH=aarch64 config && make ARCH=aarch64 user && make ARCH=aarch64 build
make ARCH=aarch64 run | tee aarch64_run.log
```

Quick diff (replace script path as needed):

```bash
python3 scripts/compare_run_logs.py x86_64_run.log aarch64_run.log   # if present
# Or grep harness + key tests manually (see checklist below).
```

---

## 2026-05-19 — Phase 1/2 proc + signal + wait parity gate

### Global metrics

| Metric | x86_64 | aarch64 |
|--------|--------|---------|
| Harness | **52/52 PASS**, Failed 0/52 | **52/52 PASS**, Failed 0/52 |
| `NULL pointer` | 0 | 0 |
| `Unhandled page fault` | 0 | 0 |
| `[SIGNAL] deliver` / invalid handler | 0 | 0 |
| `[PROC] wait4` errors | 0 | 0 |
| `unimplemented id=124` (wrong sched_yield on x86) | 0 | 0 |
| stdout `[FAIL]` / Assert Fatal tests | 20 (same set) | 20 (same set) |

The 20 stdout failures are **shared backlog** (sleep, munmap, VFS/IO syscalls), not arch regressions.

### Key integrated tests (stdout parity)

| # | Test | x86_64 | aarch64 | Notes |
|---|------|--------|---------|-------|
| 07 | test_wait | PASS | PASS | `wait child success.` |
| 08 | signal delivery | PASS | PASS | handler + kill |
| 39 | test_clone | PASS | PASS | `Child says successfully!` |
| 41 | test_waitpid | PASS | PASS | no post-wait fault |
| 44 | SIG_DFL / SIG_IGN | PASS | PASS | Test 1–4 incl. `PASS: SIG_IGN still works` |
| 49 | test_fork_wait | PASS | PASS | 3 subtests + Summary; see below |

### #49 `test_fork_wait` structure (must match)

Both arches:

```
*** test_fork_wait_basic PASSED ***
*** test_wnohang PASSED ***
*** test_multiple_children PASSED ***
=== Test Summary ===
Total tests: 3
Passed: 3
Failed: 0
[TEST 49/52] PASS          ← MUST appear after Summary, not mid-test
```

**Reaped exit codes (multiple children)** — both:

```
Parent: Reaped PID=68, exit_code=10
Parent: Reaped PID=69, exit_code=20
Parent: Reaped PID=70, exit_code=30
```

WNOHANG subtest: both use `WNOHANG returned 0` → blocking wait → `Child exited normally`.

---

## Fixes validated in this gate

### Kernel / linux_layer

| Issue | Fix | Files |
|-------|-----|-------|
| aarch64 `rt_sigreturn` incomplete restore | Full EL0 trap frame save/restore | `linux_layer/signal/arch/signal_context_*.c`, `signal_restore_arch_*.h` |
| Signal arch `#ifdef` in deliver | Split to `linux_layer/signal/arch/` | `signal_deliver.c`, `signal_restore.c` |
| SIG_IGN stale pending + spurious warn | `linux_signal_flush_pending()`; special-handler helpers | `signal_queue.c`, `sys_rt_sigaction.c`, `signal_deliver.c` |
| Fork child inherits `test_cookie` → runner early PASS | `child_ta->test_cookie = 0` on fork/clone | `sys_fork.c`, `sys_clone.c` |
| aarch64 clone child user SP | `arch_set_thread_user_sp` on clone stack | `sys_clone.c` (earlier gate) |

### User payload

| Issue | Fix | File |
|-------|-----|------|
| `rt_sigaction` static `sigaction` corrupted after handler | Stack-local act + `memset` | `test_sig_dfl.c` |
| Fork loop shared stack `i` (all children exit 10) | `SPAWN_ONE_CHILD(0/1/2)` literals + `multiple_child_entry(idx)` | `test_fork_wait.c` |
| SMP printf garble in #49 | `write()` one-line helpers (no `snprintf` — minimal link) | `test_fork_wait.c` |
| x86 wrong yield syscall (124=getsid) | `SYS_sched_yield` (24 on x86) | `test_fork_wait.c` |

### core/ (maintainer-approved, earlier in arc)

- aarch64 `arch_syscall_get_user_return` reads **SP_EL0** via `mrs`, not stale `tf->SP`.

---

## Regression checklist (run after proc/signal/fork changes)

1. **Harness**: `Passed: 52/52`, `Failed: 0/52` on **both** arches.
2. **Kernel noise**: no NULL PF, no `[SIGNAL] deliver: invalid user handler`, no `[PROC] wait4` errors.
3. **#44**: `PASS: SIG_IGN still works` present; no `rt_sigaction(SIG_IGN) returned error`.
4. **#49 ordering**: `[TEST 49/52] PASS` **after** `=== Test Summary ===`.
5. **#49 exit codes**: reaped 10, 20, 30 (not three×10).
6. **Fork review**: any `copy_thread` append copy → clear `test_cookie` on child unless explicitly a runner main thread.

---

## Known acceptable differences

- **WNOHANG branch timing**: may take “child still running” vs “already exited” path depending on scheduling; both valid if subtest PASS.
- **SMP stdout ordering**: child “Running” lines may appear out of PID order on aarch64; lines must not be garbled after `my_write` fix.
- **Harness vs stdout**: 20 tests print `[FAIL]` while harness still PASS — documented backlog, not arch skew.

---

## Next verification targets (not in this gate)

- Phase 3 execve user-stack / musl (ongoing in `sys_execve.c`)
- Phase 4 VFS (#10–#51 stdout FAIL set)
- Path B signal delivery on fork bootstrap frame ([`SIGNAL_IMPLEMENTATION_STATUS.md`](SIGNAL_IMPLEMENTATION_STATUS.md))

---

## Related docs

- Roadmap: [`SYSCALLS.md`](SYSCALLS.md)
- Signal detail: [`SIGNAL_IMPLEMENTATION_STATUS.md`](SIGNAL_IMPLEMENTATION_STATUS.md)
- wait4 detail: [`WAIT4_IMPLEMENTATION_STATUS.md`](WAIT4_IMPLEMENTATION_STATUS.md)
- test_cookie / fork: [`BUGFIX_FORK_SYSCALL_STALE_USER_CONTEXT.md`](BUGFIX_FORK_SYSCALL_STALE_USER_CONTEXT.md) §B
- Decisions: [`../ai/DECISIONS.md`](../ai/DECISIONS.md)
