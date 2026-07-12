# Cross-Architecture Verification Log (linux_layer)

> **Purpose**: Record **paired** x86_64 / aarch64 runs after proc/signal/wait milestones.
> Harness PASS alone is insufficient — compare **stdout structure** and kernel-side noise.

**Last updated**: 2026-07-09 (maintainer runs: `aarch64_run.log`, `x86_64_run.log`)

---

## 2026-07-09 — Phase 4 FS bootstrap + page_slice 统一加载

### Global metrics

| Metric | x86_64 | aarch64 |
|--------|--------|---------|
| Harness | **52/52 PASS**, Failed 0/52 | **52/52 PASS**, Failed 0/52 |
| Shutdown | clean `KERNEL: System halted.` | clean |
| SMP | 4 CPUs | 4 CPUs |
| VFS | `cpio 59 entries, ramfs 0 entries` | same |
| initramfs harness | `LINUX USER TEST SUITE START (initramfs)` Total 52 | same |
| ELF load | `start gen task from elf slice …` per test | same |

**compat 变更（本 gate）**: initramfs cpio 链进镜像；`vfs_kern_read_file_slice` 统一 middle-layer 读；manifest/ELF page_slice；execve `linux_exec_load_elf_slice`；删除 contiguous `vfs_kern_read_file`。详见 [`FILE_LOADING.md`](FILE_LOADING.md)。

### FS stdout 矩阵（harness PASS ≠ 全绿）

| 测例 | 主题 | x86_64 stdout | aarch64 stdout |
|------|------|---------------|----------------|
| #21 | open | PASS | PASS |
| #28 | read | PASS | PASS |
| #5 | close | PASS | PASS |
| #11 | fstat | PASS | PASS |
| #12 | getcwd | PASS (stub `/`) | PASS |
| #17 | mkdir | PASS | PASS |
| #48 | unlink | PASS | PASS |
| #8 | execve → test_echo | PASS | PASS |
| #32/#33 | test_execve* | PASS (harness) | PASS |
| #51 | write | PASS | PASS |
| #3 | chdir | **FAIL** ENOSYS | **FAIL** |
| #13 | getdents | **FAIL** | **FAIL** |
| #22 | openat | **FAIL** | **FAIL** |
| #6/#7 | dup/dup2 | **FAIL** ENOSYS | **FAIL** |
| #27 | pipe | **FAIL** ENOSYS | **FAIL** |
| #19/#46 | mount/umount | **FAIL** ENOSYS | **FAIL** (deferred) |

**结论**: Phase 4 **bootstrap 达成**（cpio open/read/fstat、ramfs mkdir/unlink、initramfs execve、page_slice harness）。  
**下一目标**: 目录语义 — [`DIRECTORY_PHASE.md`](DIRECTORY_PHASE.md)（chdir、cwd、openat dirfd、getdents64；**不需**新建 Linux inode 层）。

### 下一步验证

- [x] initramfs 52/52 harness — x86_64 + aarch64 ✅
- [x] #21/#28 open+read stdout PASS ✅
- [x] #8 execve VFS path stdout PASS ✅
- [ ] #3/#13/#22 chdir/getdents/openat stdout PASS（Step 7）
- [ ] append 双架构 post-directory run

---

## 2026-06-13 — Phase 2E + 3.5 time + core DAIF/sleep gate

### Global metrics

| Metric | x86_64 | aarch64 |
|--------|--------|---------|
| Harness | **52/52 PASS**, Failed 0/52 | **52/52 PASS**, Failed 0/52 |
| Shutdown | clean `KERNEL: System halted.` | clean |
| SMP | 4 CPUs | 4 CPUs |
| VFS server | `vfs_server_port` registered | same |
| #16 gettimeofday | `[PASS]` | `[PASS]` |
| #17 sleep/nanosleep | `sleep success` | `sleep success` |
| #19 uname / #20 times | PASS | PASS |
| #21 sigaltstack | PASS | PASS |
| #52 execve | `execve success!` | `execve success!` |
| #50/#51 open/openat | stdout `[FAIL]` (ENOSYS, expected) | same |

**core 变更（本 gate 前/中，maintainer review）**: aarch64 DAIF L1/L2（syscall inherit I、`switch_to` restore outgoing DAIF）；x86 timer 细节。FS 前无新增 core 阻塞项。

### #49 stdout regression（harness PASS ≠ stdout PASS）

2026-05-19 gate 要求 #49 三个子测例 stdout 全 PASS；本 run **回退**：

| 子测例 | x86_64 | aarch64 | 典型失败 |
|--------|--------|---------|----------|
| `test_fork_wait_basic` | PASS | **FAIL** | `wait4 returned PID=-1`（`-ECHILD` via libc） |
| `test_wnohang` | **FAIL** | **FAIL** | 第二次阻塞 `wait4(pid)` → `FAIL: wait4 failed` |
| `test_multiple_children` | **FAIL** | **FAIL** | `unexpected exit_code 0/42 from pid 67/66`（未 reap 的上一子进程 zombie） |

**根因（compat）**: 子进程 `sys_exit` 向父进程 `queue SIGCHLD`；`wait4_block_on_port` 把默认 `SIGCHLD` 当作可投递信号 → `WAIT_INTERRUPT` / `-EINTR`，阻塞 wait 未 reap。遗留 zombie 被 `wait4(-1)` 先收走。

**修复**: `linux_signal_wait4_should_return_eintr()` — `SIGCHLD` + `SIG_DFL`/`SIG_IGN` 不中断 wait4；`WAIT_INTERRUPT` 路径先尝试 reap 再决定是否 EINTR。

**x86_64 + aarch64 修复后验证** (post-fix run logs): #49 stdout **3/3 PASS** — reaped 10/20/30 from PID 68/69/70；harness 52/52 both arches。

### FS stdout backlog（Phase 4 目标）

与 2026-05-19 类似，约 14 个 FS 相关测例 stdout `[FAIL]`（pipe/dup/read/close/…），harness 仍 PASS。`sys_openat` 等返回 `-ENOSYS` 直至 initramfs + vfs_server 实现。

### 下一步验证

- [x] #49 三子测例 stdout 3/3 PASS — **x86_64 + aarch64 post-fix** ✅
- [ ] Phase 4 首个里程碑：`open`/`read`/`close` 让 #50 真正 `[PASS]`

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
| Fork child inherits `test_cookie` → runner early PASS | `linux_thread_append_copy` clears cookie | `linux_elf_init.c` |
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
6. **Fork review**: `thread.append_hooks.copy` must clear runner-only fields (`test_cookie`, `clear_tid`) unless explicitly a runner main thread.

---

## Known acceptable differences

- **WNOHANG branch timing**: may take “child still running” vs “already exited” path depending on scheduling; both valid if subtest PASS.
- **SMP stdout ordering**: child “Running” lines may appear out of PID order on aarch64; lines must not be garbled after `my_write` fix.
- **Harness vs stdout**: 20 tests print `[FAIL]` while harness still PASS — documented backlog, not arch skew.

---

## Next verification targets (not in this gate)

- **#49 wait4 SIGCHLD/EINTR** — stdout parity（见 §2026-06-13）
- Phase 4 VFS (#10–#51 stdout FAIL set)
- Phase 3 execve 3b–3d (env/auxv, FS ELF)
- Path B signal delivery on fork bootstrap frame ([`SIGNAL_IMPLEMENTATION_STATUS.md`](SIGNAL_IMPLEMENTATION_STATUS.md))

---

## Related docs

- Roadmap: [`SYSCALLS.md`](SYSCALLS.md)
- Signal detail: [`SIGNAL_IMPLEMENTATION_STATUS.md`](SIGNAL_IMPLEMENTATION_STATUS.md)
- wait4 detail: [`WAIT4_IMPLEMENTATION_STATUS.md`](WAIT4_IMPLEMENTATION_STATUS.md)
- test_cookie / fork: [`BUGFIX_FORK_SYSCALL_STALE_USER_CONTEXT.md`](BUGFIX_FORK_SYSCALL_STALE_USER_CONTEXT.md) §B
- Decisions: [`../ai/DECISIONS.md`](../ai/DECISIONS.md)
