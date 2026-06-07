# execve 实现状态

> **Phase**: 3（程序执行）  
> **Last updated**: 2026-05-19  
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
| FS path (open + load) | ❌ | ❌ | #18 `test_execve_basic` blocked by open |
| shebang / PT_INTERP | ❌ | ❌ | — |

---

## Verified tests (embedded path)

| Harness # | App | Log expectation |
|-----------|-----|-----------------|
| 03 | test_execve → test_echo | `execve success.` |
| 43 | test_echo via exec | `I am test_echo.` |
| 52 | test_execve_simple | `I am execve_target!` |

**Not yet verified**: FS-based exec (#18), musl/glibc dynamic binaries, multi-thread exec.

---

## Implemented (`linux_layer/proc/sys_execve.c`)

1. Copy filename + argv from user (`linux_mm_load_from_user`)
2. Resolve embedded ELF by basename (`find_embedded_elf_by_name`)
3. ELF header check **before** `vspace_clear`
4. TLB quiesce (`linux_exec_wait_remote_tlb_quiesce`)
5. `vspace_clear_user_mappings` + `load_elf_to_vs`
6. `generate_user_stack` + `build_initial_stack`
7. `linux_exec_reset_proc_state` (partial)
8. `arch_syscall_set_user_return` (Path A); skip signal deliver on success

---

## Gaps (by delivery stage)

| Stage | Content | Blocker |
|-------|---------|---------|
| **3a** | envp on stack | linux_layer only |
| **3b** | `de_thread` / exit_group semantics | policy in `GOALS_AND_CORE_CONTRACT.md` §3.1 |
| **3c** | auxv (`AT_PHDR`, `AT_ENTRY`, `AT_PAGESZ`, `AT_RANDOM`, …) | needs aux vector builder |
| **3c** | Full `linux_exec_reset_proc_state` | signal dispositions, altstack, blocked mask, thread pending |
| **3d** | Read ELF from VFS | **Phase 4 VFS** |
| **3d** | shebang, `PT_INTERP` | VFS + loader |

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
3. After VFS: path resolution + FS read + extend program_map removal  
4. Verification gate entry in [`CROSS_ARCH_VERIFICATION_LOG.md`](CROSS_ARCH_VERIFICATION_LOG.md)
