# execve 实现状态

> **Phase**: 3（程序执行）  
> **Last updated**: 2026-07-09  
> **Design**: [`SYSCALL_USER_RETURN_AND_EXECVE.md`](SYSCALL_USER_RETURN_AND_EXECVE.md)  
> **Roadmap**: [`SYSCALLS.md`](SYSCALLS.md) · **Index**: [`PROGRESS.md`](PROGRESS.md)

---

## Summary

| Area | x86_64 | aarch64 | Notes |
|------|--------|---------|-------|
| Syscall wired | ✅ | ✅ | `syscall_entry.c` → `sys_execve` |
| Phase 3a embedded ELF | ✅ | ✅ | Hardcoded `program_map` (5 names) |
| argv on user stack | ✅ | ✅ | `build_initial_stack` |
| aarch64 x0/x1 = argc/argv | ✅ | — | x86 relies on stack layout at `_start` |
| envp | ❌ | ❌ | `user_envp` ignored |
| auxv | ❌ | ❌ | No `AT_*` vector |
| de_thread before exec | ❌ | ❌ | Multi-thread exec unsafe |
| Full post-exec reset | ⚠️ | ⚠️ | Only brk/mmap_hint/pending; not dispositions/altstack |
| FS path (open + load) | ✅ | ✅ | CPIO slice + initramfs execve `#8` stdout PASS（2026-07-09） |
| shebang / PT_INTERP | ❌ | ❌ | Out of scope (no dynamic linking) |

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
- **aarch64 argc/argv in registers**: documented exception to “no `set_user_int_arg` on exec” in design doc — update ADR when stable.

---

## Next steps

1. envp + minimal auxv for static musl tests  
2. de_thread + complete signal/MM reset  
3. After VFS directory phase: shrink embedded `program_map` in `linux_exec_image.c`  
4. Verification gate entry in [`CROSS_ARCH_VERIFICATION_LOG.md`](CROSS_ARCH_VERIFICATION_LOG.md)
