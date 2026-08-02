# AI Change Checklist (Repository-wide)

This checklist provides **abstract patterns** for AI-assisted code changes in this repository.
**Primary target**: prevent hidden regressions in concurrency, lifetime, teardown, and failure paths.
**Philosophy**: prefer general patterns over specific rules; examples illustrate patterns but don't define them.

## Required Output Per Change

Before merge/commit, include a short note with:

1. **Invariants** (3-8 bullets)
2. **Failure paths** (alloc fail, partial update fail, concurrent invalidation)
3. **Lock/refcount order** (under lock vs after unlock)
4. **Teardown behavior** (require-empty vs drain-on-fini)
5. **Verification** (lint/build/test or explicit blocker)

If any section is missing, review is incomplete.

---

## Core Checklist

### 0) Abstraction / Non-Overfitting Policy (meta-rule)
- [ ] Each checklist entry describes a reusable *pattern* (symmetry, ownership, lock context, rollback strategy), not a one-off local implementation detail.
- [ ] If a rule mentions a concrete function/module name, it must be explicitly marked as an example, and the invariant/pattern must still be checkable without that name.
- [ ] Prefer 1 general rule that covers many cases over many narrow rules; if multiple bullets overlap, merge them and keep only the most actionable check.
- [ ] Every bullet must answer: "What can go wrong, and how do I check it?"
- [ ] When learning a new failure mode, update the closest existing category (invariants/teardown/failure/lookup/etc.) before adding a new category.
- [ ] **Layer boundary discipline:** core modules must not call into service/server modules (even if link-time symbols exist). Cross-layer startup uses initcall (`DEFINE_INIT` + per-CPU `do_init_call()`), and each service decides BSP-only vs per-CPU behavior internally (e.g., one global registration + per-CPU workers).

### 0b) Error-Reporting Layering (meta-rule)
- [ ] Lower layers return `error_t` (or equivalent status) and do not unconditionally print internal details.
- [ ] Higher layers decide whether/how to log (and can include context like request/thread/vspace ids).
- [ ] If an error is ignored on purpose, the code must document why and what remains safe.

### 0c) Low-Overhead Reviewability (meta-rule)
- [ ] Avoid extra temporary variables if the existing value can be reused for the decision and/or logging.
- [ ] Prefer linear control flow: do not refactor into additional scopes/identifiers without a correctness gain.

### 0d) Primary Error Preservation (meta-rule)
- [ ] If an earlier operation fails, do not overwrite its error status with later best-effort cleanup results.
- [ ] Cleanup errors must be either logged separately or aggregated, but the primary failure cause remains visible.

### 0e) Core reuse (meta-rule)
- [ ] Before new logic in `linux_layer/` or `servers/`, read `core/docs/USING_CORE.md` and `GUIDE.md` §6; reuse listed core APIs.
- [ ] Do not reimplement primitives already provided in core (IPC ports/kmsg, `copy_thread`, radix/vmm, arch syscall return helpers, etc.).
- [ ] Do not duplicate core usage guides in `doc/linux_compat/` or `doc/ai/`—extend `USING_CORE.md` via maintainer.
- [ ] If an API is missing from `GUIDE.md` §6: record the gap in `doc/ai/` or `doc/linux_compat/` and propose a core doc update.
- [ ] Do not add compat/Linux-specific documentation under `core/docs/`.
- [ ] Changes to `core/` still require explicit maintainer approval.

### 1) Data Structure Invariants

- [ ] **State clarity**: State fields have clear, single-purpose meanings.
- [ ] **Type consistency**: Sentinel values match their type's width and semantics.
- [ ] **Union discipline**: Only one union member is active at a time; assigning one member overwrites others. Use explicit tags or enums when union members need disambiguation.
- [ ] **Named constants**: Use named constants/macros for sentinel values; avoid undocumented numeric literals.
- [ ] **Overflow safety**: Capacity/index arithmetic has overflow checks before allocation.
- [ ] **Lifetime validity**: Token/cached handle invalidation is explicit on reuse or teardown.

### 2) Concurrency and Locking

- [ ] **Mutations under lock**: Shared mutations happen under the intended lock.
- [ ] **Lock order consistency**: Lock order is unchanged or explicitly documented.
- [ ] **Lockless access safety**: No lockless access assumes stability without design guarantees.
- [ ] **Heavy operations outside lock**: Potentially expensive operations run outside locks when possible.
- [ ] **Multi-queue fairness**: When multiple queues feed the same consumer, ensure all queues make progress (avoid starvation).
- [ ] **Per-CPU ownership**: Mutations to per-CPU data structures must synchronize with the owner CPU.
- [ ] **Lock API correctness**: Use lock APIs correctly (e.g., MCS lock waiter node must be per-acquirer, not shared).

**Key principle**: Identify ownership (CPU, lock, component) before concurrent operations.

### 3) Refcount and Lifetime

- [ ] **Symmetry**: Lookup acquires ref, remove drops ref, destroy/fini doesn't leak.
- [ ] **Wrapper consistency**: If a wrapper holds a reference, constructor must acquire it before publishing; finalizer must release it.
- [ ] **Last-ref ownership**: Owned resources are freed only on the last ref, or API must handle multiple teardowns safely.
- [ ] **Structural isolation**: Before freeing an object, ensure it's detached from traversable structures.

**Key principle**: Reference counting requires symmetric acquire/release and clear ownership semantics.

### 4) Failure and Rollback

- [ ] **Deterministic failure**: Every failure path has deterministic, documented behavior.
- [ ] **Atomic updates**: Partial updates are either fully rolled back or fully committed (no inconsistent intermediate states).
- [ ] **Two-phase commits**: For complex state changes, build new state first, then atomically swap.
- [ ] **Failure safety**: On failure, pre-existing state remains valid and unchanged.

**Key principle**: Failures should never leave the system in an inconsistent or unrecoverable state.

### 5) Lookup/Cache Correctness

- [ ] **Collision safety**: Hash or key collisions cannot cause false positives/negatives.
- [ ] **Staleness detection**: Stale tokens or generation mismatches invalidate cached entries.
- [ ] **Consistency maintenance**: Cache operations keep metadata (occupancy, counts) consistent.

**Key principle**: Cached lookups must be as correct as uncached lookups, even with collisions or concurrent updates.

### 6) Teardown and Allocator Ownership

- [ ] **Policy clarity**: Teardown policy is explicit (require-empty vs drain).
- [ ] **Ownership consistency**: Allocator ownership is consistent across alloc/free/destroy operations.
- [ ] **Context validity**: Teardown runs in the correct context (CPU, address space, synchronization).
- [ ] **Defensive unlinking**: Before freeing, ensure objects are detached from traversable structures.
- [ ] **Intent preservation**: Teardown intent is represented in monotonic flags, not overwritable status.

**Key principle**: Teardown should be predictable, safe to call multiple times, and never leave dangling references.

### 7) API/Type Discipline

- [ ] **Global IPC port names:** any `register_port` / reply-port string must
  follow [`doc/linux_compat/protocols/PORT_NAMING.md`](../linux_compat/protocols/PORT_NAMING.md):
  encode `(service, cpu, local_id)` or client `(service, caller_id)` as
  `{svc}_c{cpu}`, `{svc}_c{cpu}_w{wid}`, `{svc}_cli_{id}` (global `{svc}_listen`
  only when the service protocol documents a singleton). **Check:** SMP worker
  names include both **service** and **cpu**; no new ad-hoc prefixes
  (`ipc_wk_*` without service, bare `foo_port` for per-CPU endpoints).
- [ ] Header/source signatures match exactly.
- [ ] Type width choices are intentional for target architectures.
- [ ] Comments match actual behavior (no stale comment drift).
- [ ] **Identifier scope vs type names (language-generic):** inner-scope names
  (parameters, locals) must not **reuse** a typename/token you still need for
  casts, `sizeof`, or reasoning about macro expansion. If type and value would
  spell the same token, the compiler binds the inner name as a **variable**;
  “cast-looking” syntax can become ill-formed or silently change meaning.
  **Check:** for each parameter, ask whether its name equals a typedef/tag/macro
  token used in the same function; rename the value if yes.
- [ ] **C self-referential struct fields:** while defining
  `typedef struct Tag { ... } Tag_t;`, the typedef name `Tag_t` is **not** in
  scope for member types until the `typedef` completes. A pointer to the same
  aggregate must be spelled `struct Tag *` (not `Tag_t *`) inside the struct
  body. Otherwise you get “unknown type name” and cascaded bogus pointer types.
  **Check:** any recursive pointer field uses the struct tag form.
- [ ] **Macros:** if a macro’s formal parameter name equals a common type token,
  treat call-site arguments as **operands** (simple variables or expressions
  without relying on a cast that reuses that token). Prefer fixing the macro’s
  parameter name when touching that header anyway.
- [ ] **Address-space / role vocabulary:** when multiple virtual or physical
  address kinds exist in one subsystem, names or comments must disambiguate
  **role** (e.g. kernel vs user VA) without coupling to one module’s typedef
  spellings—same idea as symmetric pair naming below.
- [ ] Naming consistency as an auditability constraint:
  - For symmetric / dual operations (e.g., alloc<->free, map<->unmap,
    enqueue<->dequeue, lock<->unlock), keep the same identifier vocabulary
    for shared concepts across the pair (e.g., `entry_flags`, `table`,
    `handler`, `lock`).
  - **Single letters: C idiom vs opaque abuse:** not all one-letter parameters
    are wrong. When an API **mirrors familiar C / libc contracts**, short names
    are idiomatic and readable: `void *p` as “user pointer to a region” (same
    mental model as `malloc`/`free` / `realloc`), `s`/`n`/`dst`/`src` alongside
    `memcpy`/`strlen`/`strncpy`-style signatures. In-tree examples (not mandatory
    spellings): `free_pages(void *p, …)` / `user_unfill_range(void *p, …)` in
    `mm_user_utils.h`; `memcpy(void *dst_str, const void *src_str, size_t n)` in
    `common/string.h`. **Contrast:** if **one struct type** is reused for several
    **logical roles** in the same subsystem (tree root vs leaf vs bookkeeping),
    a bare `n` or `p` hides which lock or root applies—use **role-first** names
    there (`entry`, `tree_root`, `mapping_node`, …—examples only). If a
    one-letter temporary is unavoidable outside idiomatic APIs, scope it tightly
    and add a short comment.
  - **Tagged unions:** the wrapper field (if named) should not be an opaque
    single letter (`u`) when branches are role-specific; prefer branch identifiers
    aligned with the tag (e.g. `kernel_heap_ref` vs `user_vspace`), or anonymous
    union members with clear field names.
  - **Role-first names (allocators, CPUs, peers):** when several pointers are in
    play at once, name by **what it is for** (current CPU vs remote CPU, owner
    vs source vs target, generic `struct allocator` vs subsystem-specific view),
    not by abbreviation (`a`, `ka`, `tgt_*`). One stable name per role in a
    function beats renaming mid-scope. **Check:** could another reader tell
    which lock domain or teardown path each pointer belongs to from the name
    alone? (Concrete spellings in-tree are **examples**—e.g. kmem often uses
    `cpu_kallocator` for `percpu(kallocator)`; see `kmalloc.c` for mem-allocator
    views—not a mandatory identifier table.)
- [ ] **Redundant address parameters:** if an API takes both a pointer and a
  typed address (`vaddr`/`paddr`/…) for the **same** logical slot, and they
  cannot diverge by contract, pass **one** canonical value and derive the other
  inside the callee (e.g. `(vaddr)p` for page-base frees). Duplicate parameters
  invite drift and double-bookkeeping at call sites.

### 8) Validation

- [ ] Lints checked for modified files.
- [ ] Build/tests run where possible (or explicit blocker stated).
- [ ] Repo-wide usage checked for any symbol/function you remove (especially
  deprecated wrappers/macros) and public header API is preserved.
- [ ] Residual risks listed if verification is partial.

---

## Failure Path Strategy Template (Mandatory)

For each meaningful failure path, choose one strategy:

- **Rollback**: restore previous consistent state, return error.
- **Fail-fast**: stop, return error, no state mutation.
- **Drain/Cleanup**: continue safe cleanup, then return status.
- **Panic/Bug**: only for impossible corruption states.

Template:

```
Path: <function + failure point>
Strategy: <Rollback/Fail-fast/Drain/Panic>
Reason: <1-2 lines>
Post-condition: <what remains valid>
```

---

## Mandatory Update Mechanism

When a new bug pattern appears during review/debug:

1. Update this checklist in the same commit.
2. Append a new pattern entry in this file.
3. Append a short change summary in `doc/ai/ASSIST_HISTORY.md`.
4. Review is not complete unless steps 1-3 are done.

---

## Pattern Log (append-only)

- 2026-03: Rehash commit-order bug pattern:
  freeing/switching old table before full rebuild can leave inconsistent state on failure.
  Rule added under "Failure and Rollback" (two-phase rehash).

- 2026-03: Fini allocator lifetime bug pattern:
  nulling allocator in `fini` before final free(table) causes invalid free path.
  Rule added under "Teardown and Allocator Ownership".

- 2026-03: Typedef/tag shadowing by parameters (plus macro formal-parameter name
  collisions): reusing the type’s spelling as a value name breaks casts and
  obscures preprocessor expansion; fix by renaming the value and/or macro
  formal. Rule folded into "API/Type Discipline" (identifier scope + macros).

- 2026-03: Magic numeric sentinels for lookup/queue APIs (e.g. `-1` owner cpu,
  `0` dummy msq payload): define macros in the owning header/source and use
  them at returns and comparisons. Rule added under "Data Structure Invariants"
  (named sentinels).

- 2026-03: **C `union` last-writer alias:** assigning a second member of the
  same union overwrites the first (same storage). Example bug: set `kref` then
  `uvs = NULL` leaves `kref` cleared. Rule: only write the active member; do not
  “clear” the other branch in the same sequence. Checklist: §1 (data structure
  invariants) + Pattern Log.

- 2026-03: **Wrong root in hierarchical delete/lookup:** search/remove APIs that
  key off a value inserted under a **canonical tree root** must be given the
  same root pointer used at insert time; passing an interior or secondary root
  breaks lookup/teardown. *(In-tree example: MM radix tree delete vs per-vspace node.)*
  Checklist: §5 + Pattern Log.

- 2026-03: **Union of two pointers, truthiness bug:** `if (vs.kref)` is true for
  both kernel `kref*` and user `VSpace*` stored in the same word — second case
  mis-reads `VSpace*` as `kernel_address_space_ref*` (`kref->vs` becomes
  `vspace_root_addr`). Fix: read `vs_common->type` first, then only the active
  branch fields on `VSpace` (`vs`/`cpu_id` vs table fields). Checklist:
  §1 + Pattern Log.

- 2026-03: **`Page.rmap_list` vs lock-free kmem queues:** small-object free may
  use MSQs without touching PMM lists, but `rmap_list` link/unlink/scan must
  synchronize with the zone `pmm` lock. Do not walk `rmap_list` lock-free while
  other CPUs link/unlink. If unmap needs radix tree range locks, detach rmap
  entries under `pmm` lock one at a time, then unmap without holding `pmm`
  (see `unfill_phy_page`). Checklist: §2 + Pattern Log.

- 2026-03: **Typedef incomplete during struct body (`VSpace*` in
  `typedef struct VSpace`):** using the typedef alias for a pointer to the
  struct being defined is ill-formed in standard C; use `struct VSpace *`.
  Symptom: `unknown type name` on the alias, then wrong return/assignment types
  on union members. Checklist: §7 + Pattern Log.

- 2026-03: **Multi-role type, single-letter parameters:** one struct tag reused
  for several logical roles in a subsystem; short parameter names in headers/inlines
  obscure which root/lock/domain applies. Prefer role-vocabulary names at call
  sites and in public helper signatures. Checklist: §7 + Pattern Log.

- 2026-03: **C idiom `p`/`s`/`n` vs role-hiding letters:** `void *p` (malloc-like),
  string/buffer APIs matching `string.h` shapes—single letters aid recognition.
  Do not conflate with multi-role pointers where the name must carry role.
  Checklist: §7 + Pattern Log.

- 2026-03: **Dual MSQ / work queues for cross-CPU frees:** if two queues carry
  “foreign completion” work onto the same CPU (e.g. page vs object path),
  draining only one from alloc/free hot paths starves the other. Prefer one
  entry point that drains both in a fixed order. Checklist: §2 (concurrency) +
  Pattern Log.

- 2026-03: **Duplicate pointer + address for one slot:** APIs that accept both
  `void*` and `vaddr` for the same page/object key should use one parameter and
  cast internally when both representations are needed—avoids redundant locals
  and mismatched pairs at call sites. Checklist: §7 + Pattern Log.

- 2026-03: **Layering violation (core calls server):** low-level core modules
  must not directly invoke service/server entry points (even if link-time symbols
  exist). Fix by running per-CPU `do_init_call()` on secondary CPUs, and making
  each server init decide BSP-only global registration vs per-CPU worker spawn.
  Checklist: §0 (layer boundary discipline) + Pattern Log.

- 2026-03: **Cross-CPU teardown vs per-CPU `Task_Manager`:** freeing or unlinking
  a `Thread_Base` / `Tcb_Base` from another CPU’s scheduler lists without
  synchronization races `schedule()` on the owner CPU. Fix: owner-CPU execution,
  per-TM lock, or quiesce scheduler. Checklist: §2 + `INVARIANTS.md` (Task_Manager).

- 2026-03: **MCS `me` must be current CPU:** `lock_mcs(&pmm->spin_ptr, me)` —
  `me` must be `percpu(pmm_spin_lock[z])` on the executing CPU. Using
  `per_cpu(pmm_spin_lock[z], handler->cpu_id)` lets two CPUs share one waiter
  node (`me`), corrupting the MCS queue and `rmap_list` / PMM invariants. See
  `INVARIANTS.md` (kmem / rmap). Checklist: §2 + Pattern Log.

- 2026-03: **Rmap multi-role, wrong owner:** when one physical page’s reverse map
  lists several logical roles (e.g. kernel vs user paths), do not use a
  non-authoritative entry to infer **subsystem-specific** ownership (e.g. kmem
  CPU from a user mapping). Filter by the role that matches the invariant.
  Checklist: §1 + Pattern Log.

- 2026-04: **Wrapper refcount symmetry:** finalizer `ref_put(T)` implies creator
  must `ref_get_not_zero(T)` at bind time. Checklist: §3.

- 2026-04: **Intent survives state changes:** teardown/exit intent must not be
  representable only by a status enum that IPC can overwrite. Checklist: §6.

- 2026-04: **Final free defensive unlink:** last ref must not free a node still
  linked in any list/ring. Checklist: §6 + §2.

- 2026-04: **MSQ drain is single-shot (or guarded):** avoid double-drain of the
  same queue lifetime; prefer one shared drain helper. The MSQ dummy’s final
  `ref_put` happens inside `msq_dequeue`’s empty-queue path—do not `ref_put` the
  dummy again in a “delete dummy” tail. Checklist: §3 + §6.

- 2026-04: **`kmsg_t` wire length vs flexible array:** use
  `offsetof(kmsg_t, payload)` for the header-prefix size when allocating,
  passing `data_len` to `create_message_data`, and validating
  `payload_len` in `kmsg_from_msg`; do not assume `sizeof(kmsg_t)` equals that
  prefix on all toolchains. Slim header has no `version` field: changing
  `kmsg_hdr_t` requires bumping `KMSG_MAGIC` and updating all encoders/decoders.
  Checklist: §7 + `doc/ai/IPC_MESSAGE.md`.

- 2026-04: **IPC serialization format vs `va_list`:** pack and unpack must use the same
  format string and argument types/order; mismatches are undefined behavior (like
  `printf`). Prefer one shared `fmt` literal per `(module, opcode)` for client
  and server. Checklist: §7 + `doc/ai/IPC_MESSAGE.md`.

- 2026-04: **`kmsg` payload entry point:** use `kmsg_create(module, opcode, fmt,
  ...)` only; it uses `ipc_serial_measure_va` + `ipc_serial_encode_into_va` into the
  allocated `kmsg` (no extra TLV allocation + copy). Checklist: §7 +
  `doc/ai/IPC_MESSAGE.md`.

- 2026-07-26: **Global port name collisions on SMP:** listen/worker/client
  strings shared one flat table without structure → `register_port` fail /
  wrong endpoint / exit path “port not found”. **Rule:** identity first
  `(service, cpu, local_id)`, then `PORT_NAMING.md` grammar. Checklist: §7 +
  `doc/linux_compat/protocols/PORT_NAMING.md`.

- 2026-07-26: **RPC reply = blocking rendezvous:** do not use bare
  `try_send` on live request–reply (races client entering recv). Abandoned
  clients: reply-port teardown wakes server. Unique `vfs_cli_k_*` for kernel
  VFS clients — never share `vfs_backend_caller`. Checklist: §2 +
  `protocols/IPC_RPC_FRAMEWORK.md` + `PORT_NAMING.md`.

- 2026-07-26: **clean_server role split:** `THREAD_REAP` on single
  `clean_listen` (`ipc_server_coop_loop`); async only for EXIT_NOTIFY.
  Checklist: §2 + `protocols/EXIT_CLEAN.md`.

- 2026-04: **Field repurposing with union + type-safe caching (vmm_radix_tree_change_range_flags):**
  - **Pattern**: When repurposing struct fields as temporary cache, use union with
    correct target types, document safety conditions, and ensure symmetric cleanup.
  - **Example**: `union { struct list_entry manage_free_list; struct { ppn_t cached_ppn;
    ENTRY_FLAGS_t cached_flags; } cache_data; };`
  - **Safety analysis**: (1) manage_free_list only used by manager nodes,
    (2) vspace lock prevents concurrent is_page_manage_node() checks,
    (3) cleanup sets both fields to 0 (not INIT_LIST_HEAD) for NULL checks,
    (4) restore before lock release.
  - **Performance gains**: Avoid repeated have_mapped calls, enable full rollback.
  - **Type safety**: Using `ppn_t`/`ENTRY_FLAGS_t` instead of forced `struct list_entry*`
    casts eliminates undefined behavior and improves debuggability.
  - **Atomicity**: Full rollback on failure (all-or-nothing) prevents inconsistent state.
  - Checklist: §1 (union type safety + field repurposing) + §4 (atomic updates) +
  `doc/ai/CODE_QUALITY_PATTERNS.md`.

- 2026-07-29: **wait4 vs SIGCHLD (EXIT_CLEAN gap):**
  - Protocol intent: EXIT_NOTIFY wakes wait; SIGCHLD is pending-only. Omitting
    that boundary caused `WAIT_INTERRUPT`/`-EINTR` before EXIT_NOTIFY.
  - Unsafe follow-up: RW-stack EXEC trampoline for sigreturn. Rule: SIGCHLD
    never EINTR wait4; Layer B uses `SA_RESTORER` or per-process RX stub;
    EXIT_NOTIFY spawn failure → `pending_exits` + poke.
  - Checklist: §0 + `protocols/EXIT_CLEAN.md` / `WAIT_AND_SIGCHLD.md`.

- 2026-07-30: **fork COW child PTE must be RO (core):**
  - Symptom: ash printed `SHELL_OK` + `ls` then hung (no `AFTER_LS`); Channel R
    fine. Misdiagnosis: SIGCHLD-on-wait-exit (defer did not help).
  - Root cause: `clone_vspace` COW prep left **child PTE writable** while parent
    was RO → child mutated shared `.data`/`.bss` without fault. Fix in core:
    both sides map COW pages RO. Do not paper over with compat debug/defer.
  - Checklist: §0 + `protocols/WAIT_AND_SIGCHLD.md` §4; core COW contract.

- 2026-07-30: **execve must zero rtld_fini register (x86 rdx / aarch64 x0):**
  - Symptom (x86 ash→`execve` busybox): `#PF` with `RIP=CR2=<old envp>` (e.g.
    `0xcdca70`), error `e=0x14` (user insn fetch). `AFTER_LS` still prints.
  - Cause: Path A `sysret` restores syscall arg regs; glibc `_start` does
    `mov %rdx,%r9` (rtld_fini) / aarch64 uses `x0`. Leaving pre-exec envp/filename
    there → later call into unmapped VA. aarch64 already gets `x0=0` via
    `set_user_return`; x86 needs explicit `syscall_ctx->rdx = 0` after it.
  - Checklist: §0 + `SYSCALL_USER_RETURN_AND_EXECVE.md` §5.1.

- 2026-07-31: **Path-dependent syscalls (`sh -c` vs `sh script`):**
  - Symptom: Stage A `sh /tests/boot_smoke.sh` → `unimplemented id=72` /
    ENOSYS before any script output; earlier `sh -c '…'` ash smoke worked.
  - Cause: script path opens the file and uses `fcntl(F_SETFD, CLOEXEC)`;
    `-c` does not. id 72 is `__NR_fcntl` on **x86_64** (aarch64 `__NR_fcntl`
    is 25). Dispatch via arch `syscall_ids.h` + shared `sys_fcntl`.
  - Checklist: §0; `FD_TABLE.md` fd_flags/open_flags.

- 2026-07-31: **x86-64 kernel must use `-mno-red-zone`:**
  - Symptom: Path B boot → `#PF` in `vfs_path_collapse` (`mov (%r11,%r9),%bl`),
    CR2 = garbage `raw + comp_off[]`; e=0 (kernel not-present).
  - Cause: leaf `comp_off`/`comp_len` lived in SysV **red zone** (below RSP);
    IRQ/trap entry clobbers it → wild offset. aarch64 has no red zone.
  - Fix: bake `-mno-red-zone` into `core/script/config/config_x86_64.json`
    (flows to `Makefile.env` `CFLAGS`; root `linux_layer` already compiles with
    `$(CFLAGS)`). `vfs_path_stack_anchor` keeps collapse non-leaf as belt.
    Not needed on aarch64/riscv/loongarch (no SysV red zone; flag is x86-only).
  - Checklist: §0 + stack discipline.

- 2026-07-31: **poll must block via IPC, never busy-return 0 on timeout < 0:**
  - Symptom: after `=== run_all start ===`, apparent hang; DUMP mostly
    `round_robin_schedule`, occasional RIP at `syscall` with
    `RAX=__NR_poll(7)`, `RSI=nfds=1`, `RDX=timeout=-1`.
  - Cause: stub returned 0 when nothing ready (including infinite timeout).
    Ash retries `poll` forever. Related: CONSOLE_IN always-POLLIN +
    `read`→0 is the same busy-loop family.
  - Architecture: local readiness scan → if wait needed, `recv_msg` on
    per-thread `sleep_port` (timer EXPIRE / signal CANCEL / future UART|pipe
    wake kmsg) — same model as `linux_time_sleep.c`. Until UART RX exists,
    console-only infinite wait synthesizes `POLLHUP` (no producer to wake).
  - Checklist: §0 + `TIME_SUBSYSTEM_PLAN.md` §10; do not invent a parallel
    wait queue outside IPC.

- 2026-07-31: **Bring-up orchestration ≠ ash `while read` + nested VFS READ:**
  - Symptom: after `=== run_all start ===`, idle in `schedule`; syscall trail
    `write` → `newfstatat` → `openat` → `poll(1,-1)` then stuck; samples in
    `vfs_read_handle` / `vfs_backend_ipc_call` / `vfs_pcache_fill_slice_from_kva`
    (not a tight poll spin).
  - Model: ash always `poll`+byte-`read`s. Console without UART is an **EOF
    device** (`read`→0, `poll(POLLIN)`→`POLLHUP`). File reads go
    user→vfs_server→cpio nested IPC; page_slice populate on that path hung.
  - Fix: (1) pack-time expand `run_all.sh` (explicit `run_one`, no manifest
    line-read); (2) console EOF poll semantics; (3) cpio READ = direct blob
    copy; (4) nested backend RPC uninterruptible.
  - Checklist: §0; do not paper over by returning 0 from infinite poll.

- 2026-08-01: **THREAD_REAP zombie wait vs ready-but-not-scheduled exitor:**
  - Symptom: intermittent hang after first `run_all` test exit; last line often
    `clear_tid write failed` (red herring — warn before `THREAD_REAP` send).
  - Cause: listen waits for `thread_status_zombie` while exitor may already be
    `ready` (recv completed send) but not yet scheduled to store zombie;
    single `clean_listen` livelocks in that wait.
  - Fix: after IPC unfinished states clear, promote `ready` → zombie in
    `clean_handle_thread_reap`; drop noisy clear_tid warn.
  - Checklist: §0 + EXIT_CLEAN (send returns before exitor stores zombie).

- 2026-08-01: **VFS client RPC must be uninterruptible (rendezvous wedge):**
  - Symptom: suite progresses; hang at `/tests/oscomp_munmap` after
    `[vfs-be] … LOOKUP … leave ret=0`; DUMP mostly `schedule` (blocked).
  - Cause: `vfs_ipc_request_response` used interruptible `ipc_rpc_call_va`.
    After send, `-EINTR` abandons `recv` on `vfs_cli_<pid>` while VFS listen
    (single-threaded) blocks forever in `send_msg(reply)` — same class as
    `TASK_REAP_SYNC` / nested backend. Prior fork/clone noise makes SIGCHLD
    more likely mid-OPEN.
  - Fix: `ipc_rpc_call_va_uninterruptible` in `fs_ipc.c`.
  - Checklist: §0 + IPC_RPC_FRAMEWORK §8; single-threaded reply rendezvous
    cannot tolerate client abandon without port teardown.

- 2026-08-01: **unregister_port must wake waiters (not only final free):**
  - Symptom: same FS hang after uninterruptible VFS; DUMP schedule-only after
    INFO; intermittent around process death / `#PF` teardown.
  - Cause: `port_clean_thread_queue` only ran at refcount→0, while
    `ipc_rpc_send_reply` holds a lookup ref across `send_msg` — unregister
    did not wake `block_on_send`. Late enqueue after clean was also possible.
  - Fix: per-port ops gate (`port_ops_begin/end`, `ops_life`/`ops_count`).
    Unregister: CLOSING → wait inflight==0 → `port_clean` → CLOSED. Blocking
    send/recv `end` before `schedule`; wake checks `PORT_CLOSED` (+ drop
    orphan send). RPC reply treats `-E_REND_PORT_CLOSED` as handled.
  - Checklist: §0 + IPC_RPC_FRAMEWORK §6; BUSYBOX_BOOT_DEFERRALS IPC P0 §2.

- 2026-08-01: **RPC / EXIT_NOTIFY must not abandon after commit or drop notify:**
  - Symptom: intermittent FS wedge or parent stuck in wait4 after child exit.
  - Cause: (1) interruptible RPC returned `-EINTR` after `send_msg(server)`
    while listen blocked in `send_msg(reply)`; (2) reply/EXIT_NOTIFY gave up
    on OOM or treated `PORT_CLOSED` as hard failure without fallback.
  - Fix: EINTR only before request commit; post-commit drain interrupt and
    wait; `ipc_rpc_send_reply` / `post_exit_notify` retry alloc; EXIT_NOTIFY
    worker falls back to `pending_exits`+poke; `PORT_CLOSED` = handled.
  - Checklist: §0 + IPC_RPC_FRAMEWORK §7–8; EXIT_CLEAN; BUSYBOX IPC P0 §3–4.

- 2026-08-01: **execve must reset caught signal dispositions (not only pending):**
  - Symptom: after ash `execve /tests/{clone,fork,exit}`, child runs OK then
    parent `#PF pc=far=0x57f485` (`present=0`, `rdi=SIGCHLD`); VA is busybox
    `.text`, outside the test ELF (`LOAD` ends ~`0x4037b1`).
  - Cause: `linux_signal_proc_reset` cleared pending/`sigreturn_page` but left
    catchers from the previous image; SIGCHLD delivery jumped to stale handler.
  - Fix: on exec reset, caught handlers → `SIG_DFL` (keep `SIG_IGN`); do not
    chase with post-fork COW PTE reinstall bandaids.
  - Checklist: §5 (lifecycle across image replace) + Pattern Log.

- 2026-08-01: **VFS backend reply port must be per-thread (not shared `srv`):**
  - Symptom: hang at `/tests/oscomp_munmap` after ash `copy_thread`, before any
    test printf; DUMP mostly `schedule`, brief `ipc_port_try_match`.
  - Cause: `vfs_backend_ipc_call` used one `vfs_cli_k_srv` for all callers.
    `vfs_server` (nested backend) and syscall-context `vfs_kern_read_file_slice`
    (execve) can overlap when listen blocks in `recv_msg` → reply misdelivery /
    rendezvous wedge.
  - Fix: reply tag `t<tid>` per caller; TRACE_IPC_WEDGE / TRACE_VFS_IO breadcrumbs.
  - Checklist: §0 (single-threaded listen ≠ single global reply port).

- 2026-08-01: **Backend reply rendezvous wedge (do not bypass with local dispatch):**
  - Symptom: execve LOOKUP log stops after `[rpc] REPLY send … vfs_cli_k_t<tid>`
    (no `SRV done`); same LOOKUP often succeeds on another tid.
  - Rejected: in-process `vfs_backend_dispatch` short-circuit — breaks VFS→backend
    RPC architecture; may only be used as a temporary A/B to localize the bug.
  - Protocol (unchanged): blocking `send_msg(reply)` ↔ `recv_msg(reply)`; server
    may arrive first. See IPC_RPC_FRAMEWORK §5.1.
  - Evidence (rpc-stall): both `recv_enter` and `reply_enter` then neither leave
    on `vfs_cli_k_t*` READ — wedge inside rendezvous (not “client never recv”).
  - Core fix (symmetric, §5.1 “dequeue ⇒ complete or wake”):
    (1) `recv_msg`/`try_recv` on `-E_REND_NO_MSG` wake sender `XFER_FAIL` →
    `send_msg` retries; (2) `send_msg`/`try_send` on `-E_REND_NO_MSG` wake
    receiver `XFER_FAIL` → `recv_msg` retries (was missing — silent drop of
    dequeued `block_on_receive`); (3) stale `try_match` wakes with
    `PORT_CLOSED`. Same class: early boot `vfs_cli_*` READLINKAT and
    munmap nested `vfs_cli_k_t*` — intermittent either side first.
  - Framework: `ipc_server_coop_loop` for single-thread servers; no worker pool.
  - Checklist: §0 + IPC_RPC_FRAMEWORK §5.1.

- 2026-08-02: **Park THREAD_REAP → Path B run_all stuck after first test:**
  - Symptom: `END test_brk` then silence (no next `=== /tests/… ===`).
  - Misread: harness cookie / Link B. Truth: `/init`→ash `run_all` children are
    **Link A**; missing `EXIT_NOTIFY` leaves parent `wait4` forever.
  - Cause: parking THREAD_REAP zombie wait without guaranteed
    `delete_thread`+`EXIT_NOTIFY`; also `schedule()` inside coop poll.
  - Fix: zombie wait stays inline (EXIT_CLEAN); Link A only if live parent
    else demote REAPED+`delete_task`; poll only non-blocking EXIT_NOTIFY
    worker teardown.
  - Checklist: §0 + EXIT_CLEAN Link A/B.

- 2026-08-02: **IPC `port_ptr` claim-after-enqueue → stale hold / ghost wait:**
  - Symptom: nested RPC or `wait4` hang; probes showed `enqueue cas_fail`
    (held prior reply port) then `try_match drop_port_ptr` (no wake).
  - Cause: request visible before `CAS(NULL→port)`; peer matched with
    `port_ptr==NULL` then local CAS wrote a post-match stale hold. Also
    enqueue-fail path `store NULL` then unconditional `CAS(NULL→port)`.
  - Invariant: claim `port_ptr` before queue visibility; enqueue fail rolls
    back with `store NULL`; `try_match` succeeds only if identity is `port`
    (NULL is not success). Blocking send/recv ⇒ at most one wait identity.
  - Claim CAS fail must `store NULL` (stale poison) so caller AGAIN can
    proceed; otherwise same-CPU send can spin and never schedule powerd
    (core suite PASS but no `[powerd] shutdown request`).
  - Core-test footgun (2026-08-02 log): `BSP_test`/`AP_test` used
    `thread_set_status(init_thread, ready)` while init was in
    `recv_msg(kernel_port)`. That bypasses `try_match` clear →
    `STALE_AFTER_WAIT` / `claim_fail` busy loop (no `schedule`) → powerd
    starved. Fix: do not force-ready IPC-blocked waiters; shutdown only via
    `rendezvos_request_poweroff()` → `send_msg(powerd)`.
  - Checklist: §2 (lockless linearization) + §5 (wait lifecycle).

- 2026-08-02: **Drop core↔compat test phase gate; simplify poweroff:**
  - Core-only (`RENDEZVOS_TEST`): `BSP_test` ends with
    `rendezvos_request_poweroff()` (no `CORE_AUTO_POWEROFF` feature).
  - Full root: undef `RENDEZVOS_TEST`; `RENDEZVOS_ROOT_AUTO_POWEROFF`
    after `/init` cookie wait in `linux_boot`.
  - Removed unused `core_test_phase_*` and multi-slot / legacy manifest
    boot branch; one boot wait cookie for PID1.
