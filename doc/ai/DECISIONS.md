# Decisions (ADR-lite)

Short log of important decisions to avoid re-litigating old design choices.
Format: Context / Decision / Consequences.

---

## 2026-08-16 | Multi-zone PMM: weak hook fills `mem_zones` directly

- Context: Need fixed-capacity multi-zone before buddy is up (no early allocator). A parallel `pmm_zone_config` / “slot” table duplicated `MemZone` fields. Initcall / compat is too late for `phy_mm_init`.
- Decision: `ZONE_NR_MAX` capacity + `nr_mem_zones` compact prefix. Weak **`configure_pmm_zones_hook`** (strong override in arch/early `.o`) writes `mem_zones[i].{lower_addr,upper_addr,pmm}` and `nr_mem_zones`; `pmm_configure_zones` validates / falls back to single NORMAL+buddy; `split_pmm_zones` only intersects with `m_regions`. No second boot-config struct.
- Consequences: Second pool (e.g. DMA) is an early strong hook + static `struct pmm`, not a late compat feature. Callers pick the zone’s `pmm` explicitly. See [`core/docs/memory.md`](../../core/docs/memory.md) §2.4.1, USING §3.8a, TODO_DONE #60.

---

## 2026-08-09 | CMDLINE policy lives in upper Makefile (not core)

- Context: Hybrid split kernel — core must not hardcode compat busybox boot argv (`sh /tests/run_all.sh`) into its Makefile/`configure.py`. Invading core with that default couples mechanism to upper-layer demo policy.
- Decision: **core** only accepts `CMDLINE=` (empty if omitted) and may pass it to QEMU `-append` / bootargs. **Root `Makefile`** injects `LINUX_BOOT_CMDLINE_DEFAULT` via `CONFIG_CMDLINE` / `RUN_CMDLINE`. Empty bootargs still fall back in `linux_boot.c` for compat PID1.
- Consequences: Rebuilding core alone without upper inject yields empty cmdline (compat default argv still applies at PID1). Do not reintroduce busybox strings into `core/Makefile`. See [`NEXT_PLAN.md`](../linux_compat/NEXT_PLAN.md) §1.

---

## 2026-08-09 | Busybox bring-up compromise ledger closed

- Context: `BUSYBOX_BOOT_DEFERRALS.md` tracked demo compromises; `/init` + `run_all` gate is good enough to stop treating that list as live P0.
- Decision: Archive to [`archive/BUSYBOX_BOOT_DEFERRALS.md`](../linux_compat/archive/BUSYBOX_BOOT_DEFERRALS.md). Live backlog → [`NEXT_PLAN.md`](../linux_compat/NEXT_PLAN.md). UART/console server is **extra**, not a busybox-close blocker.
- Consequences: Further work is Phase 5 / polish / uart_server design; do not reopen “busybox deferral” as the primary tracking doc.

---

## 2026-07-27 | VFS tables use growable page_slice (no fixed BSS)

- Context: cpio/ns raised to 2048 fixed BSS, readdir used 128KiB static/`names[][]` on stack, handles/ramfs/blkdev similarly hard-capped — demo-era bombs.
- Decision: `vfs_slice_table` packs records so none straddle pages; grow via `page_slice_set_size` + new pages (old KVAs stay valid). Soft max `VFS_SLICE_TABLE_SOFT_MAX`. Stack I/O uses one heap PAGE scratch. See [`VFS_DYNAMIC_STORAGE.md`](../linux_compat/VFS_DYNAMIC_STORAGE.md).
- Consequences: Rootfs growth no longer requires recompiling caps; OOM returns errors; namespace pointer links remain valid across grow.
- Follow-up (same day): **S2** — drop `vfs_ns_node.path[]`, rebuild with `vfs_ns_path_of` (cpio/ramfs/inode/fd path kept as keys/snapshots). **S3** — mount / backend registry / pcache slots also on `vfs_slice_table`.

---

## 2026-07-26 | Global port names encode (service, cpu, local_id)

- Context: Repeated `register_port` collisions and “port not found” on exit/clean paths; ad-hoc names (`ipc_wk_*`, bare `*_server_port`) do not scale on SMP with per-CPU pools.
- Decision: Canonical grammar in [`doc/linux_compat/protocols/PORT_NAMING.md`](../linux_compat/protocols/PORT_NAMING.md): identity is `(service, cpu, local_id)` (or client `(service, caller_id)`); strings are `{svc}_c{cpu}`, `{svc}_c{cpu}_w{wid}`, `{svc}_cli_{id}`; global listen only as documented exception `{svc}_listen`.
- Consequences: New ports must follow the grammar; migrate legacy names in lockstep with clients; centralize format helpers; worker names always include **service** and **cpu**.

---

## 2026-07-26 | Link A vs B: only live parent is a wait reaper

- Context: Treating `ppid==0` as link A forced `EXIT_NOTIFY(kernel_port)` on every test exit, pinned per-CPU clean workers, then listen accepted `THREAD_REAP` but could not dispatch (`send done`, no `enter`). Contradicted link B in the same protocol doc.
- Decision: `proc_has_wait_reaper` is true **only** if `ppid>0` and the parent task exists. Init-adopted / orphan exits use link B (`REAPED` + **only** `THREAD_REAP`; listen claims `delete_task` when last thread). `EXIT_NOTIFY` only to live parent `wait_port`. See [`protocols/EXIT_CLEAN.md`](../linux_compat/protocols/EXIT_CLEAN.md).
- Consequences: Test/harness exits no longer force EXIT_NOTIFY to kernel_port; clone/wait4 (live parent) still uses link A + async EXIT_NOTIFY.

---

## 2026-07-26 | RPC reply is blocking rendezvous (not try_send)

- Context: Splitting “blocking for TASK_REAP_SYNC / best-effort try_send for VFS” looked like it protected VFS from abandoned clients, but bare `try_send` races live clients still entering `recv` (boot: shared `vfs_backend_caller` → `reply best-effort failed`). Shared singleton reply ports also violate PORT_NAMING under concurrent backend register.
- Decision: All live request–reply servers use **`ipc_rpc_reply` / blocking `send_msg`**. Abandoned clients release reply ports on teardown; core wakes `block_on_send`. Kernel VFS clients use unique `vfs_cli_k_*` names. Removed unused generic `per_msg_worker` pool from `rpc.c`.
- Consequences: Protocol matches rendezvous IPC; no silent drop of live replies; docs no longer prescribe VFS best-effort.

---

## 2026-07-26 | clean_server: THREAD_REAP on listen; EXIT_NOTIFY async only

- Context: Routing every `THREAD_REAP` through a generic `per_msg_worker` pool, plus one listen thread per CPU on the same `clean_listen`, produced repeated `send THREAD_REAP done` with no `THREAD_REAP enter` (pending / handoff lies).
- Decision (partial, later corrected): Inline THREAD_REAP / TASK_REAP* on listen; one-shot **only** for EXIT_NOTIFY. No framework-wide worker pool. Temporarily collapsed to one BSP `clean_listen` — that collapse was a misread of the design (see 2026-08-02 entry below).
- Consequences: Pool/handoff bug fixed; BSP-only listen was an unintended bottleneck.

---

## 2026-08-02 | clean_server: shared `clean_listen` + per-CPU threads

- Context: BSP-only one thread starved AP reapers. A mistaken follow-up used per-CPU ports `clean_c{cpu}` + owner_cpu routing, which **prevents** cross-CPU cleanup (core0 reap handled on core1) — that contradicts the design.
- Decision: **One global port** `clean_listen` (PORT_NAMING §4). **One coop thread per CPU**, all `recv` that port. Clients always send to `clean_listen`. Keep THREAD_REAP inline; **no** per-msg worker pool. Do not collapse to BSP-only; do not split into per-CPU listen names.
- Consequences: Cross-CPU reap restored; AP threads participate; docs match.

---

## 2026-08-02 | EXIT_NOTIFY: listen try_deliver + park (drop gen_thread)

- Context: One-shot EXIT_NOTIFY threads were a transitional escape from blocking listen on `wait_port`. Coop already had try/park; clean still spawned workers.
- Decision: `linux_proc_try_post_exit_notify` via `ipc_system_try_deliver` (avoids listen `send_msg_queue`). AGAIN → per-CPU park list; `ipc_server_coop_loop` poll returns true → `schedule` instead of blocking `recv_msg`. Alloc/hard fail → pending_exits+poke. No EXIT_NOTIFY `gen_thread`.
- Consequences: Same-thread coop FSM; TASK_REAP_SYNC can still be accepted on that CPU; fewer threads under ash `run_all`.

---

## 2026-08-02 | Single-thread server = cooperative event loop (not blocking handler)

- Context: VFS/clean need one listen thread. Blocking handlers wedge everyone. The old per-msg worker pool was already gone, but dead APIs remained (`ipc_server_recv_loop`, unused `ipc_port_name_worker*`) and `ipc_rpc_server_loop` comments looked like a dispatch/pool model.
- Decision: **`ipc_server_coop_loop`** in `rpc.c` (preferred). **`ipc_rpc_server_loop`** kept only as transitional same-thread blocking request–reply for VFS/backends — it does **not** spawn workers. Deleted unused `ipc_server_recv_loop` and worker port-name helpers. Core rendezvous fix separate and required. **THREAD_REAP zombie wait stays inline** (EXIT_CLEAN handshake); parking it without guaranteed `EXIT_NOTIFY` hung ash `wait4` after the first Path B `run_all` test (Link A). Coop `poll_pending` only advances EXIT_NOTIFY worker teardown (no `schedule` inside poll).
- Consequences: Less API surface; docs match code; VFS migration to coop is explicit follow-up.

---

## 2026-08-02 | VFS coop is the direction; reply-aware framework landed

- Context: clean_server proves one-way coop. VFS still blocks listen on nested backend RPC and client reply.
- Decision: Land **`ipc_rpc_coop_*`**. Listen send queue serializes with **`send_owner`**. No per-msg worker pools.
- Consequences: Framework ready for VFS/backends.

---

## 2026-08-02 | VFS + backends on `ipc_rpc_coop_server_loop` (phase 1)

- Context: All four FS IPC servers used blocking `ipc_rpc_server_loop`.
- Decision: backends → leaf coop; vfs_listen → `ipc_rpc_coop_server_loop`.
- Consequences: Client reply parks on all four.

---

## 2026-08-02 | VFS READ/WRITE nested coop park (phase 2)

- Context: Phase 1 still blocked vfs_listen for the whole backend RTT on every I/O.
- Decision: **`vfs_coop.c` / `vfs_coop_path.c`** — listen is fully coop: nested park for
  READ/WRITE and path ops (incl. READLINK/MOUNT/GETDENTS); local one-shots
  (close/lseek/fstat/umount/backend_register) finish inline. Leaf backends use
  `IPC_RPC_COOP_REPLIED` + blocking reply. Nested reply ports `vfs_cli_k_j<seq>`.
  Kern `vfs_lookup_path` / `vfs_backend_{lookup,mkdir,unlink}` remain sync for
  exec/load and namespace rollback only.
- Consequences: Concurrent client replies / other listens can progress while a READ/WRITE waits on backend; OPEN/namespace still sync-nested.

---

## 2026-08-02 | Leaf backend reply = blocking (`IPC_RPC_COOP_REPLIED`)

- Context: After backends moved to coop `NEED_REPLY`+`try_send`, boot hung at `exec /init`. Nested VFS uses `try_recv` only (no RECV waiter on the nested reply port); leaf `try_send` never enqueues a SEND waiter either → poll livelock.
- Decision: Leaf backends complete with **blocking `ipc_rpc_reply`** and return **`IPC_RPC_COOP_REPLIED`** (framework releases job; no second try_send). Keep `NEED_REPLY`+`try_send` only when the peer **blocking-`recv_msg`** (user/kern RPC clients).
- Consequences: Nested READ/WRITE and blocking `vfs_backend_ipc_call` (kern load / sync OPEN) rendezvous again; leaf may block in reply (acceptable — no nested work on that thread).
- Superseded (reply path): 2026-08-06 nest reply transfer.

---

## 2026-08-06 | Nested VFS reply = `ipc_transfer_message` (request still port)

- Context: Nested reply via `vfs_cli_k_j*` + leaf blocking `send_msg` worked but was asymmetric port rendezvous. §8.3 already allows known-peer transfer. Pure transfer does **not** wake `recv_msg` port waiters — so request→leaf must stay on listen port; reply→VFS is safe because VFS is in coop `NESTED_RECV` (parked poll drains `recv_msg_queue`).
- Decision: Nested TLV `t` = `@n<cookie>` (not a registered port). Leaf `enqueue` + `ipc_transfer_message` to `vfs_server_thread_get()`. VFS coop loop drains nest-resp kmsgs (`IPC_RPC_NEST_RESP_OPCODE` / `"qq"`). Sync `vfs_backend_ipc_call` unchanged (real reply port + blocking reply). Do **not** invent port-wait wake in linux_layer. Also delete unused transitional **`ipc_rpc_server_loop`** / `ipc_rpc_server_handler_t` (all FS already on coop).
- Consequences: Drop per-job `vfs_cli_k_j*` for coop nested; request-side direct deliver deferred until leaf idle model or core wake API.

---

## 2026-08-03 | VFS path ops nested FSM (phase 3); coop is the direction

- Context: Tests passed after leaf blocking reply. Question: is coop appropriate? OPEN/MKDIR/LOOKUP still sync-nested and can wedge listen for a backend RTT.
- Decision: **Keep coop.** Single listen must park nested work (`try_send`/`try_recv` + schedule) so other clients progress. Split namespace into **prepare (local) / nested RPC / commit**; `vfs_coop_path.c` FSM for OPEN, STAT/CHDIR/FACCESSAT, MKDIR, UNLINK, then **RENAME/LINK/GETDENTS**. Kern `vfs_lookup_path` may still block. Leaf backends stay `IPC_RPC_COOP_REPLIED`.
- Consequences: Listen no longer blocks on common path-op / dirent backend RTT; **readlink / mount / umount** remain follow-ups (or accept rare sync).

---

## 2026-04 | Stdout/stderr `write` shim without VFS

- Context: User tests and minimal libc need `write` on fd 1/2 before an fd table and filesystem exist.
- Decision: Implement `sys_write` in `linux_layer/io/sys_write.c` as a **console/UART backend** for fd 1 and 2 only; reserve future dispatch through a per-process fd table to real files (documented in `doc/linux_compat/STDIO_SHIM.md`).
- Consequences: No `read`/stdin in this step; user pointer copy is not yet full `copy_from_user` (documented).

---

## 2026-07-09 | Per-process fd table in compat (scheme B)

- Context: Bootstrap put fd numbers on vfs_server; broke Linux dup2/stdout redirect and mixed process semantics into FS server.
- Decision: **`linux_proc_append_t.fs`** holds fd 0–31; vfs_server holds **`vfs_handle_t`** open-file state only. IPC carries abs paths (open) or handle ids (I/O). Console write stays local unless fd redirected to VFS.
- Consequences: Removed `vfs_fd.c`; OPEN fmt `siu`; getcwd local. See `doc/linux_compat/FD_TABLE.md`.

---

- Context: read-mostly IPC port lookup; RB-based path was removed.
- Decision: use dynamic slot array + OA hash + per-slot generation token.
- Consequences:
  - simpler hot lookup path and token-based cache resolve.
  - requires careful rehash rollback discipline and teardown handling.

## 2026-04 | Generic string-keyed index named `name_index` (not “named_slot_table”)

- Context: the reusable backend is “index string name → stored value (+ row generation for cache tokens)”; the old name sounded like an implementation detail (“slot”) rather than the role.
- Decision: expose it as `name_index` (`name_index_t`, `name_index_token_t` with `row_index` / `row_gen`); port table embeds `name_index_t by_name`.
- Consequences:
  - clearer mental model at call sites; internal storage remains a row array + open-addressing hash.
  - `name_index_token_t` stays as a typedef alias for `name_index_token_t` (IPC-facing name unchanged).

## 2026-03 | Thread cache stores hash + token, no name copy

- Context: avoid duplicate string compare/storage in thread cache entries.
- Decision: remove `name_copy`; lookup resolves all same-hash candidates via token.
- Consequences:
  - saves per-thread cache memory.
  - collision handling relies on trying all same-hash candidates.

## 2026-03 | Kernel IPC messages use a common envelope (`kmsg`)

- Context: multiple in-kernel server threads communicate via lock-free IPC; ad-hoc per-service payload structs make validation/versioning hard.
- Decision: reserve a kernel message `msg_type` and wrap payloads in a versioned `kmsg` header with `kind + payload_len`.
- Consequences:
  - message receivers can validate `magic/version/len` before casting.
  - new services add new `kind` values without changing the base IPC layer.

## 2026-03 | 64-bit index/capacity/token fields in port table path

- Context: target architectures are 64-bit; alignment/consistency favored.
- Decision: use 64-bit widths for slot index/capacity/token index on port table path.
- Consequences:
  - more uniform type model and fewer width-conversion footguns.
  - hash bucket storage uses more memory than 32-bit variant.

---
## 2026-03 | When to forward work to server threads

- Context: core provides primitives (thread/memory/IPC/page-table basics), but certain “business semantics + resource ownership / lifecycle constraints” must not be executed directly on the caller stack without additional safety.
- Decision: forward work to a top-level server thread only if at least one condition holds:
  - caller context resources may be unavailable (example: `exit` teardown must run after switching to a safe vspace/stack).
  - strict serialization is required for a shared/unique resource (example: network/NIC queue ownership).
  - complex multi-step rollback/lifecycle correctness is better centralized into a single executor.
- Consequences:
  - core stays focused on primitives with stronger low-level invariants.
  - server threads become the place for strong ordering and lifecycle correctness.

---
## 2026-03 | Current user mappings are 4K-only (so COW targets 4K first)

- Context: radix tree 用户态分配路径固定调用 4K 页映射并且 `user_fill_range()` 只 `map(..., level=3)`；当前未维护用户态 2M/huge 节点数据结构。
- Decision: implement fork+COW minimal closure for 4K first; extend to 2M/huge only after the user-side 2M mapping data-path is introduced.
- Consequences:
  - avoids unnecessary huge-page COW complexity early.
  - keeps page-table teardown/reclamation scoped to the common 4K path.

---
## 2026-03 | Hybrid kernel: core primitives + server threads via IPC

- Context:
  - `core/` implements the minimal kernel core: scheduling/thread management, memory primitives (PMM/Nexus/map/unmap), and fast IPC/port-based object lookup.
  - Higher-level “Linux compatibility” and syscall semantics are implemented as in-kernel `servers/` (and other kernel-mode services).
  - Syscalls in `linux_layer/` typically do not execute heavy cleanup directly; they send requests via IPC ports to the corresponding server thread.
  - Some teardown actions (e.g. `exit` cleanup) must be performed outside the caller’s unsafe execution context (stack/vspace hazards), similar to the motivation behind `clean_server`.
- Decision:
  - Keep `core/` as a set of reusable, tightly-scoped primitives with strong lifetime/teardown invariants.
  - Centralize business semantics and resource-ownership ordering into server threads that operate through IPC message passing.
  - Use server forwarding when (1) the caller context may be invalid or unsafe for the action, or (2) serialization over a shared/unique resource is required.
- Consequences:
  - Pros:
    - Clear layering: fewer places where low-level memory/page-table invariants are broken by high-level syscall semantics.
    - Stronger lifetime correctness: teardown and refcount ownership can be centralized and audited in one service.
    - Better performance predictability: hot IPC/data paths stay in `core`, while expensive/complex operations move to servers.
    - Matches your “one syscall service thread owns the hard part” design, reducing multi-core contention on unique resources.
  - Cons / risks:
    - Requires very explicit core<->server contracts (what core guarantees vs what server must guarantee).
    - Teardown correctness becomes a cross-module invariant; missing a single reclamation step (e.g. vspace page-table frames) causes leaks.
    - Debugging complexity increases: a failure can manifest far from the code that triggered it (via message passing).
    - Future features like `fork+COW` increase the surface area of correctness: page refcounting and rmap coherence must remain consistent under server-driven lifetimes.
  - Targeted improvements for future work:
    - Define a small set of “ownership + teardown” invariants that every server must respect when driving `core` primitives.
    - For fork/COW, formalize how radix tree nodes and rmap entries are duplicated so exit cleanup never mis-frees shared physical pages.
    - Expand tests to cover server-driven teardown paths and memory reclamation regressions under concurrent workloads.

---

## 2026-05 | Signal context: arch files under `linux_layer/signal/arch/`

- Context: `rt_sigreturn` on aarch64 requires restoring the **full** EL0 syscall trap frame (`REGS[]`, `SPSR`, `SP_EL0`), not only PC/SP/x0. Inline `#ifdef` in deliver/restore was hard to maintain; x86 benefits from symmetric full-frame save for robustness.
- Decision: Add `linux_signal_arch_save_context` / `linux_signal_arch_restore_context` in `linux_layer/signal/arch/signal_context_{x86_64,aarch64}.c`; keep `signal_deliver.c` / `signal_restore.c` arch-neutral. Extend `linux_signal_restore_t` with `linux_signal_restore_arch_t` selected by header.
- Consequences:
  - Makefile wildcard `linux_layer/*/*/*.c` builds both arch files; each `.c` guarded by `#if defined(_AARCH64_)` / `_X86_64_`.
  - No core/ change required for this split.
  - Future arches add one pair of files + one restore arch header.

---

## 2026-08 | Do not advertise `HWCAP_CPUID` without EL0 MRS emulation

- Context: aarch64 busybox after PID1 exec merge trapped at `mrs x0, midr_el1` (ESR unknown/IL). QEMU log: branch from glibc after seeing AT_HWCAP bit 11.
- Decision: `AT_HWCAP` advertises only features usable without kernel traps we do not handle (`FP|ASIMD|EVTSTRM`). Omit `HWCAP_CPUID` until core implements ID-register MRS emulation (Linux `do_el0_undef` path).
- Consequences: No new arch forks in boot/exec paths; one auxv constant. x86 was unaffected (`AT_HWCAP=0`).

## 2026-08 | Kernel boot is not a test harness (`test_*` → boot)

- Context: Default boot is Path B `/init` → busybox `run_all.sh`. `linux_layer/tests/` and `test_runner.h` / `test_cookie` were leftover harness names blocking a clean `execve("/init")` path.
- Decision: Move launcher to `linux_layer/init/linux_boot.c`; API `linux_boot_notify_exit` / `boot_wait.h`; field `boot_wait_cookie`. Delete kernel manifest loader, `elf_read_test`, and `test_*.h`. Keep `rootfs/tests/` as **userspace** suite dir.
- Consequences: Prep for exec-based PID1; docs updated (`USER_TESTS.md`, `CODE_STRUCTURE.md`).

## 2026-05 | Fork/clone children must not inherit boot wait cookie

- Context: Boot wait used `linux_thread_append_t` cookie (then `test_cookie`) via `clean_server` → notify. Fork children inherited the cookie and **prematurely completed** the wait while the parent was still in `wait4`.
- Decision: **`linux_thread_append_copy`** clears `boot_wait_cookie` (formerly `test_cookie`) and `clear_tid` on every `copy_thread` child. Only Path-B `/init` keeps the cookie (set in `linux_boot.c` after spawn).
- Consequences:
  - Documented in [`APPEND_HOOKS.md`](linux_compat/APPEND_HOOKS.md) §5, [`BUGFIX_FORK_SYSCALL_STALE_USER_CONTEXT.md`](linux_compat/BUGFIX_FORK_SYSCALL_STALE_USER_CONTEXT.md) §B, and [`CROSS_ARCH_VERIFICATION_LOG.md`](linux_compat/CROSS_ARCH_VERIFICATION_LOG.md) checklist.
  - Any new `copy_thread` consumer must keep `thread.copy` clearing boot-wait-only fields.

---

## 2026-07 | Append lifecycle via hook tables (init / copy / fini)

- Context: Linux proc/thread state lived in core append bytes; separate `elf_init_handler` params and `copy_thread` append memcpy caused drift and fork bugs.
- Decision: Core exposes `task_append_hooks_t` / `thread_append_hooks_t` (`append_info_len` + init/copy/fini). Compat defines static `linux_*_append_hooks`; ELF first load uses `thread.init`; fork/clone use `task.copy` + `thread.copy`; teardown uses `fini`.
- Consequences:
  - Canonical compat doc: [`APPEND_HOOKS.md`](linux_compat/APPEND_HOOKS.md).
  - `gen_task_from_elf` / `new_task_structure` / `create_thread` take hook table pointers only.
  - `linux_task_append_clone()` encapsulates CLONE_VM vs fork signal/fs policy for `sys_clone`.

---

## Add New Decision Template

```md
## YYYY-MM | <title>

- Context: <what pressure/problem existed>
- Decision: <what was chosen>
- Consequences:
  - <good>
  - <trade-off/risk>
```
