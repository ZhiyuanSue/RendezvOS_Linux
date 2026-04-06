# AI Change Checklist (Repository-wide)

This checklist is for AI-assisted code changes in this repository.
Primary target: prevent hidden regressions in concurrency, lifetime, teardown, and failure paths.

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

### 1) Data Structure Invariants

- [ ] State fields have clear meaning (`used`, `gen`, `free_head`, `live_count`).
- [ ] Sentinel values are type-consistent (`U64_MAX` for `u64` index invalid).
- [ ] **Unions:** only one member is active at a time; assigning member B after
  member A overwrites A (same address). Do not write a “cleanup” or NULL to a
  second member after setting the intended member unless that is deliberately
  switching the active member.
- [ ] **Union of same-width pointers:** two `void*`/`uintptr_t`-sized fields in a
  union cannot be distinguished by “is non-NULL?” — both roles use non-NULL
  addresses. Use an explicit tag bit, enum, or separate field.
- [ ] **Named sentinels:** API return values that mean “invalid / not found”
  (e.g. owner id, queue dummy marker) use a **macro or enum** in a public header;
  call sites must not compare against undocumented numeric literals (`-1`, `0`, …).
- [ ] Sentinel/constant names match their actual semantics (e.g., `free_head`
  sentinel is not reused as token/cache invalidation).
- [ ] Capacity/index arithmetic has overflow checks before allocation math.
- [ ] Token/cached handle invalidation is explicit (e.g., generation bump on free).

### 2) Concurrency and Locking

- [ ] Shared mutations happen under the intended lock.
- [ ] Lock order is unchanged or explicitly documented when changed.
- [ ] No lockless access assumes stability unless guaranteed by design.
- [ ] Potentially heavy free paths run outside lock when possible.
- [ ] **Multiple inbound work queues** (e.g. cross-CPU completion / MSQ pairs on one
  CPU): if only one queue is drained from allocation/free hot paths, the other
  can **starve**. Prefer one documented drain entry (fixed order or fair batching)
  so all queues make progress under skewed load.
- [ ] **Per-CPU scheduler / `Task_Manager` lists:** `sched_thread_list` /
  `sched_task_list` are owned by one CPU’s `Task_Manager`. Mutations
  (`list_del_init`, `add_*_to_manager`, `current_thread` updates) must not race
  the owner’s `schedule()` walking those lists—use an
  explicit per-TM lock, run the detach on the **owner CPU** (IPI / work item),
  or prove the thread is never concurrently scheduled. **Check:** does teardown
  run on the same CPU as `thread->tm` / `task->tm`?
- [ ] **MCS lock `me` pointer:** for `lock_mcs` / `unlock_mcs`, the second
  argument is the **per-acquirer** waiter node (see `spin_lock.h`). It must be
  `percpu(...)` on the executing CPU, not `per_cpu(..., handler->cpu_id)` or
  any other CPU’s per-CPU slot.

### 3) Refcount and Lifetime

- [ ] Lookup/resolve acquires a valid ref on success.
- [ ] Remove/unregister drops ownership ref exactly once.
- [ ] Destroy/fini does not silently leak live objects.
- [ ] Final free path does not mutate index structures unexpectedly.
- [ ] **Wrapper holds pointer:** if a wrapper stores `T*` and its finalizer does
  `ref_put(T)`, then the bind/constructor path must `ref_get_not_zero(T)` before
  publishing that pointer (symmetry); on ref_get failure, fail-fast without
  publishing a half-initialized wrapper.
- [ ] **Last-ref rule:** owned heap resources (queues, buffers, stacks) must be
  drained/freed only on the **last ref** path, or the API must be explicitly
  queue-aware/guarded so calling teardown twice is safe.

### 4) Failure and Rollback

- [ ] Every allocation failure has deterministic behavior.
- [ ] Partial updates are fully rolled back or fully committed.
- [ ] Rehash/grow uses two-phase commit:
  - build new structure first
  - swap pointers only after full success
- [ ] On failure, old structure remains valid and unchanged.

### 5) Lookup/Cache Correctness

- [ ] Hash collision cannot cause false-positive return.
- [ ] Hash collision cannot cause false-negative early return.
- [ ] Stale token/generation mismatch invalidates cache entry correctly.
- [ ] Cache replacement/eviction keeps occupancy/count consistent.

### 6) Teardown and Allocator Ownership

- [ ] `fini` policy is explicit: require empty or drain.
- [ ] Allocator ownership is consistent across alloc/grow/free/free(table).
- [ ] Destroy path does not null allocator before final free(table).
- [ ] Teardown must use the recorded owner metadata and the correct
  synchronization context (no "best-effort" fallbacks).
- [ ] If teardown relies on a per-CPU/per-thread scratch window (e.g.,
  self-mapping region used to edit frames), it must run in the context
  that owns that window.
- [ ] **Final free defensive unlink:** before freeing an object, ensure it is no
  longer linked in any traversable structure (task thread list, scheduler ring,
  hash/table, MSQ, etc.). If still linked, unlink under correct lock/context (or
  last-chance `list_del_init` + diagnostics) to prevent list walks from touching
  freed memory.
- [ ] **Intent vs state overwrite:** if a “teardown intent” signal can be
  overwritten by unrelated state transitions (IPC blocking, etc.), represent
  intent in a monotonic flag/field, not only as a transient status enum.

### 7) API/Type Discipline

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
    `nexus.h`; `memcpy(void *dst_str, const void *src_str, size_t n)` in
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
  breaks lookup/teardown. *(In-tree example: MM nexus delete vs per-vspace node.)*
  Checklist: §5 + Pattern Log.

- 2026-03: **Union of two pointers, truthiness bug:** `if (vs.kref)` is true for
  both kernel `kref*` and user `VSpace*` stored in the same word — second case
  mis-reads `VSpace*` as `kernel_address_space_ref*` (`kref->vs` becomes
  `vspace_root_addr`). Fix: read `vs_common->type` first, then only the active
  branch fields on `VS_Common` (`vs`/`cpu_id` vs table fields). Checklist:
  §1 + Pattern Log.

- 2026-03: **`Page.rmap_list` vs lock-free kmem queues:** small-object free may
  use MSQs without touching PMM lists, but `rmap_list` link/unlink/scan must
  synchronize with the zone `pmm` lock. Do not walk `rmap_list` lock-free while
  other CPUs link/unlink. If unmap needs `nexus_vspace_lock`, detach rmap
  entries under `pmm` lock one at a time, then unmap without holding `pmm`
  (see `unfill_phy_page`). Checklist: §2 + Pattern Log.

- 2026-03: **Typedef incomplete during struct body (`VS_Common*` in
  `typedef struct VS_Common`):** using the typedef alias for a pointer to the
  struct being defined is ill-formed in standard C; use `struct VS_Common *`.
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
