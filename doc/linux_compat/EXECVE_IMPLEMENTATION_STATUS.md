# execve 实现状态

> **Phase**: 3（程序执行）  
> **Last updated**: 2026-07-27  
> **Design**: [`SYSCALL_USER_RETURN_AND_EXECVE.md`](SYSCALL_USER_RETURN_AND_EXECVE.md)  
> **Roadmap**: [`SYSCALLS.md`](SYSCALLS.md) · **Index**: [`PROGRESS.md`](PROGRESS.md)  
> **Busybox Path B 妥协**: [`BUSYBOX_BOOT_DEFERRALS.md`](BUSYBOX_BOOT_DEFERRALS.md) §P0

---

## Summary

| Area | x86_64 | aarch64 | Notes |
|------|--------|---------|-------|
| Syscall wired | ✅ | ✅ | `syscall_entry.c` → `sys_execve` |
| Phase 3a embedded ELF | ❌ | ❌ | **Removed** — exec load is cpio / VFS only |
| argv on user stack | ✅ | ✅ | `build_initial_stack` / Path B bootstrap |
| aarch64 x0/x1 at exec | ✅ | ✅ | **必须为 0 / 勿塞 argc**：glibc `_start` 把 x0 当 `rtld_fini`；argc/argv 只在栈上 |
| envp | ❌ | ❌ | `user_envp` ignored（syscall 路径） |
| auxv（syscall execve） | ⚠️ | ⚠️ | 与 Path B 共用 builder；缺 HWCAP/EXECFN/真随机 |
| auxv（busybox Path B spawn） | ⚠️ | ⚠️ | `/init`→busybox PID1 |
| de_thread before exec | ❌ | ❌ | Multi-thread exec unsafe |
| Full post-exec reset | ⚠️ | ⚠️ | pending + **caught→SIG_DFL** (2026-08-01); altstack/blocked via thread reinit; SIG_IGN kept |
| FS path (open + load) | ✅ | ✅ | CPIO slice + initramfs |
| shebang / PT_INTERP | ❌ | ❌ | Out of scope (no dynamic linking) |
| Boot orchestration | ✅ | ✅ | Path B `/init` + `run_all.sh`（非内核 for-manifest） |

---

## Verified tests (embedded path)

| Harness # | App | Log expectation |
|-----------|-----|-----------------|
| 03 | test_execve → test_echo | `execve success.` |
| 43 | test_echo via exec | `I am test_echo.` |
| 52 | test_execve_simple | `I am execve_target!` |

**FS exec verified**: `#8` / `#32` / `#33` harness + `#8` stdout（initramfs path）。  
**Not yet verified**: FS-only exec without embedded fallback removal; musl/glibc; multi-thread exec.

---

## Implemented (`linux_layer/proc/sys_execve.c`)

1. Copy filename + argv from user (`linux_mm_load_from_user`)
2. Resolve image: **CPIO middle layer** → **IPC VFS slice** → embedded ELF (`linux_exec_load_elf_slice`)
3. ELF header check **before** `vspace_clear`
4. TLB quiesce (`linux_exec_wait_remote_tlb_quiesce`)
5. `vspace_clear_user_mappings` + `load_elf_to_vs` (brk from `max_load_end`)
6. `generate_user_stack` + `build_initial_stack`
7. `linux_exec_reset_proc_state` (brk/mmap_hint + pending signals)
8. `arch_syscall_set_user_return` (Path A); skip signal deliver on success

---

## Gaps (by delivery stage)

| Stage | Content | Blocker |
|-------|---------|---------|
| **3a** | envp on stack | linux_layer only |
| **3b** | `de_thread` / exit_group semantics | policy in `GOALS_AND_CORE_CONTRACT.md` §3.1 |
| **3c** | auxv (`AT_PHDR`, `AT_ENTRY`, `AT_PAGESZ`, `AT_RANDOM`, …) | needs aux vector builder |
| **3c** | Full `linux_exec_reset_proc_state` | signal dispositions, altstack, blocked mask, thread pending |
| **3d** | Read ELF from VFS | ✅ static ELF64 via `linux_exec_load_elf_slice` + `vfs_exec_load.c` |
| **3d** | shebang, `PT_INTERP` | deferred (no dynamic linking) |

---

## Known design notes

- **Same PID**: exec must **not** call `register_process` again.
- **Failure before clear**: return `-errno`; old mappings kept.
- **Failure after clear**: `linux_exec_abort_unrecoverable` → fatal (no return to old user PC).
|- **aarch64 exec 入口寄存器**：与 Linux 一致，**不要** `set_user_int_arg(argc/argv)`。glibc `_start` 将入口 `x0` 存为 `rtld_fini`；`arch_syscall_set_user_return` 已写 `x0=0`。argc/argv 只通过用户栈传递。
- **x86_64 exec 入口寄存器**：`sysret` 会恢复 syscall 保存的 `%rdx`。必须在 `sys_execve` 成功路径上把 `syscall_ctx->rdx = 0`，否则旧 `envp` 指针被 `_start` 当成 `rtld_fini` 随后执行 → `RIP=CR2=<旧堆地址>`（ash→execve busybox 实测 `0xcdca70`）。

---

## Next steps

1. ✅ Boot: `/init`→busybox + `run_all.sh`；exec 去掉 embedded fallback  
2. envp + 正规化 auxv  
3. de_thread + complete signal/MM reset  
4. 可选：去掉 stub `link_app.o` 链接依赖  
5. Verification gate entry in [`CROSS_ARCH_VERIFICATION_LOG.md`](CROSS_ARCH_VERIFICATION_LOG.md)
