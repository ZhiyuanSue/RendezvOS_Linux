# AI Assist History

Purpose: durable, compact context of AI-assisted changes across long sessions.
Append one entry for each user-approved commit.

## Archives

- No archive yet.
- When rotation starts, list files here, e.g.
  - `doc/ai/archive/ASSIST_HISTORY_2026-03.md`

## Entry Rules (Mandatory)

- Append-only; do not rewrite old entries except factual corrections.
- One entry per commit (or tightly coupled commit batch).
- Keep each entry concise (target: 10-30 lines).
- If a new bug pattern was discovered, include `Pattern:` and confirm checklist update.
- If entries become too large:
  - keep this file as rolling index for recent 30-50 entries
  - move older entries to `doc/ai/archive/ASSIST_HISTORY_YYYY-MM.md` and leave links

## Entry Template

```md
## YYYY-MM-DD | <short title> | commit <sha>

- Scope: <modules/files touched>
- Why: <problem and motivation>
- Design decision(s):
  - <key trade-off 1>
  - <key trade-off 2>
- Data structure/API impact:
  - <public API change or "none">
  - <invariant changes>
- Failure-path strategy:
  - <path>: <strategy> -> <post-condition>
- Verification:
  - <lint/build/tests run>
  - <known gaps>
- Pattern: <new bug pattern, optional>
- Checklist update: <yes/no + section>
```

---

## 2026-04-01 | ipc/lifetime: refcount symmetry + exit intent flag + teardown hardening | commit c030dc8

- Scope: IPC request lifetime (`core/kernel/ipc/ipc.c`, `core/include/rendezvos/ipc/ipc.h`);
  exit/clean server (`linux_layer/syscall/thread_syscall.c`, `servers/clean_server.c`,
  scheduler `core/kernel/task/task_manager.c`, `core/include/rendezvos/task/tcb.h`);
  thread final free (`core/kernel/task/thread.c`, `core/kernel/task/tcb.c`,
  `core/include/common/dsa/list.h`); MSQ teardown (`core/include/common/dsa/ms_queue.h`,
  `core/kernel/ipc/message.c`); port teardown (`core/kernel/ipc/port.c`).
- Why: Fix a major refcount asymmetry (IPC request held `Thread_Base*` but only ref_put on free),
  prevent cross-CPU reaper deleting a still-executing thread (exit intent overwritten by IPC),
  and ensure last-ref free paths drain owned resources + defensively unlink from lists.
- Design decision(s):
  - Use monotonic flag (`THREAD_FLAG_EXIT_REQUESTED`) for exit intent instead of a dedicated
    status enum that can be overwritten by IPC states.
  - Centralize MSQ teardown via `msq_clean_queue` to avoid duplicated drain logic.
- Data structure/API impact:
  - Public: add `msq_clean_queue` helper; add `ipc_clean_port_thread_queue` for port teardown.
  - Invariants: final free must detach list nodes; wrapper objects must ref_get/ref_put symmetrically.
- Failure-path strategy:
  - `create_ipc_request` ref_get fails: Fail-fast -> free request and return NULL.
  - Thread/port/message teardown: Drain/Cleanup -> drain queues (incl. dummy), then free.
- Verification:
  - Blocked locally: cross toolchain missing (`aarch64-linux-gnu-gcc`), cannot build here.
  - Runtime smoke performed by maintainer during debug (reported “now runs through”).
- Pattern: refcount symmetry; exit intent flag; final free defensive unlink; MSQ double-drain hang.
- Checklist update: yes (`doc/ai/AI_CHECKLIST.md` Pattern Log).

## 2026-03-21 | nexus: `nexus_kernel_page_owner_cpu` only KERNEL_HEAP_REF rmap | commit <pending>

- Scope: `core/kernel/mm/nexus.c`, `doc/ai/INVARIANTS.md`, `doc/ai/AI_CHECKLIST.md`
  (Pattern Log), `doc/ai/ASSIST_HISTORY.md`.
- Why: User `VS_Common` rmap entries cannot define kmem whole-page owner; same PPN
  may have multiple user mappings or order-dependent walks.
- Checklist update: yes (INVARIANTS + Pattern Log).

## 2026-03-21 | kmalloc: drain `buffer_msq` + `kfree_page_msq` on all kalloc/kfree | commit <pending>

- Scope: `core/kernel/mm/kmalloc.c`, `doc/ai/INVARIANTS.md`, `doc/ai/AI_CHECKLIST.md`
  (Pattern Log), `doc/ai/ASSIST_HISTORY.md`.
- Why: Page alloc path only drained page MSQ; merge into
  `mem_allocator_remote_frees` so object + page cross-CPU frees both run on
  every kalloc/kfree; remove redundant inner `clean_buffer_msq` calls.
- Checklist update: yes (INVARIANTS + Pattern Log).

## 2026-03-21 | ai: §7 — C idiom (`void* p`, string APIs) vs multi-role naming | commit <pending>

- Scope: `doc/ai/AI_CHECKLIST.md` (§7 + Pattern Log), `doc/ai/ASSIST_HISTORY.md`.
- Why: Single-letter params are acceptable when mirroring libc/allocator mental
  models; distinguish from opaque letters that hide role in one-type-many-roles
  APIs.
- Checklist update: yes (§7 + Pattern Log).

## 2026-03-21 | ai: abstract §7 / Pattern Log (multi-role naming); tighten INVARIANTS | commit <pending>

- Scope: `doc/ai/AI_CHECKLIST.md`, `doc/ai/INVARIANTS.md`, `doc/ai/ASSIST_HISTORY.md`,
  `core/include/rendezvos/mm/nexus.h` (comment), `core/kernel/mm/vmm.c`,
  `core/kernel/mm/nexus.c` (`com`→`user_vs` / `heap_ref`).
- Why: Checklist must state **patterns** (§0), not a mandatory nexus-specific
  spelling table; Pattern Log entries generalized where possible; INVARIANTS
  lead with multi-role aggregate rule; local names in recent MM code clarified.
- Checklist update: yes (§7 + Pattern Log wording).

## 2026-03-21 | nexus.h: rename inline params `n`→`nexus_node` + doc node roles | commit <pending>

- Scope: `core/include/rendezvos/mm/nexus.h`, `doc/ai/AI_CHECKLIST.md` (§7 + Pattern Log),
  `doc/ai/INVARIANTS.md`, `doc/ai/ASSIST_HISTORY.md`.
- Why: `struct nexus_node*` has multiple roles; single-letter parameters hurt
  readability and invite wrong invariants.
- Checklist update: yes (§7 + Pattern Log).

## 2026-03-21 | mm: `VS_Common` anonymous union (drop `payload` field name) | commit <pending>

- Scope: `core/include/rendezvos/mm/vmm.h`, `vmm.c`, `nexus.{h,c}`, `map_handler.c`,
  `task_manager.c`, `kmalloc.c`, `doc/ai/INVARIANTS.md`, `doc/ai/ASSIST_HISTORY.md`.
- Why: No domain need for a `payload` member; C11 anonymous union — access
  `vs->vspace_root_addr`, `vs->vs`, `vs->cpu_id`, etc., per `type`.
- Verification: build with cross-gcc when available.
- Checklist update: INVARIANTS only.

## 2026-03-21 | fix: `struct VS_Common*` self-reference in `vmm.h` + checklist §7 | commit <pending>

- Scope: `core/include/rendezvos/mm/vmm.h`, `doc/ai/AI_CHECKLIST.md` (§7 + Pattern Log),
  `doc/ai/ASSIST_HISTORY.md`.
- Why: `VS_Common* vs` inside `typedef struct VS_Common { ... } VS_Common` is not
  valid in standard C (typedef incomplete during struct body); gcc: unknown type,
  cascaded bogus pointer types in `nexus_node_vspace`.
- Verification: `make` when cross-gcc available.
- Checklist update: yes (§7 bullet + Pattern Log).

## 2026-03-21 | mm: drop `VSpace` typedef; single `VS_Common` table + heap-ref union | commit <pending>

- Scope: `core/include/rendezvos/mm/vmm.h`, `map_handler.{h,c}`, `vmm.c`, `nexus.{h,c}`,
  `kmalloc.c`, `tcb.h`, `thread_loader.{h,c}`, `task_manager.c`, tests, `LocalAPIC.c`,
  `doc/ai/INVARIANTS.md`, `doc/ai/ASSIST_HISTORY.md`.
- Why: Unify page-table fields and kernel-heap ref in one `VS_Common` (anonymous
  union: `vs`/`cpu_id` vs table fields). All former `VSpace*` APIs are `VS_Common*`.
- Verification: full build not run here (cross `gcc` may be missing).
- Checklist update: INVARIANTS only (no new pattern).

## 2026-03-21 | Naming: `cpu_kallocator` + kmalloc locals | commit <pending>

- Scope: `core/kernel/task/thread.c`, `tcb.c`, `port.c`, `task_manager.c`,
  `core/kernel/mm/vmm.c`, `core/kernel/mm/kmalloc.c`, `doc/ai/AI_CHECKLIST.md`.
- Why: Unify `percpu(kallocator)` locals to `cpu_kallocator` (not `ka`/`a`/`ma`);
  `kmalloc.c` uses `k_allocator_p`, `owner_mem_allocator`, `src_cpu_kallocator`.
- Checklist update: yes (§7).

## 2026-03-21 | Review: kmem cross-CPU pages + `rmap_list` locking + owner_cpu | commit <pending>

- Scope: `core/kernel/mm/nexus.c`, `core/kernel/mm/kmalloc.c`, `doc/ai/INVARIANTS.md`,
  `doc/ai/AI_CHECKLIST.md`.
- Why: Align whole-page `kfree` routing (`nexus_kernel_page_owner_cpu`) with
  per-CPU kernel heap identity (`payload.kernel_heap_ref.cpu_id`); protect
  `Page.rmap_list` with zone `pmm` MCS lock on link/unlink/scan; `unfill_phy_page`
  snapshots rmap under lock then unmaps (avoids pmm+nexus lock order inversion).
  `_unfill_range` / user-fill rollback: unlink rmap before `pmm_free` under same
  lock where applicable.
- Verification: not run (cross `gcc` unavailable here).
- Checklist update: yes (INVARIANTS + Pattern Log §2).

## 2026-03-21 | mm: `VS_Common` in `vmm.h` (replaces `nexus_vs_common`) | commit <pending>

- Scope: `core/include/rendezvos/mm/vmm.h`, `core/include/rendezvos/mm/nexus.h`,
  `core/kernel/mm/vmm.c`, `core/kernel/mm/nexus.c`, `doc/ai/INVARIANTS.md`,
  `doc/ai/ASSIST_HISTORY.md`.
- Why: one typedef `VS_Common` (`type` + `payload`) with `VSpace` and
  `kernel_address_space_ref` lives next to vspace helpers; `nexus_node.vs_common`
  stays a pointer. Enum `vs_common_kind` / `VS_COMMON_*`; helper
  `vs_common_from_user_vspace`. Per-CPU `DEFINE_PER_CPU(VS_Common,
  nexus_kernel_heap_vs_common)`.
- Verification: build run if toolchain available.
- Checklist update: INVARIANTS only (no new pattern).

## 2026-03-21 | thread_loader: correct nexus_delete_vspace root + delete_task detach | commit <pending>

- Scope: `core/kernel/task/thread_loader.c`, `core/kernel/task/tcb.c`,
  `core/kernel/mm/nexus.c`, `core/include/rendezvos/mm/nexus.h`, `doc/ai/*`.
- Why: ELF / clean-server teardown logged “no such a vspace in nexus” and task
  manager errors; `nexus_delete_vspace` was called with the per-vspace node
  from `nexus_create_vspace_root_node` instead of `percpu(nexus_root)`.
- Design decision(s): `delete_task` skips `del_task_from_manager` when `tm` is
  NULL (task never added). `nexus_delete_vspace` null-check before
  `ROUND_DOWN(vspace_node)`.
- Reverted: `nexus_node_vspace` KERNEL_VIRT_OFFSET heuristic (masking); rely on
  union fix + correct nexus API.
- Verification: not run here.
- Pattern: Nexus API wrong tree root argument (Pattern Log + INVARIANTS).
- Checklist update: yes.

## 2026-03-21 | Nexus `vs` union fix + virt_mm_init kinit errors | commit <pending>

- Scope: `core/kernel/mm/nexus.c`, `core/kernel/mm/vmm.c`, `doc/ai/AI_CHECKLIST.md`,
  `doc/ai/INVARIANTS.md`.
- Why: boot failed with `get free page fail` / kmalloc bootstrap; root nexus
  `vs.kref` was cleared by a subsequent `vs.uvs = NULL` (same union storage).
- Design decision(s):
  - kernel path sets only `vs.kref`; user path sets only `vs.uvs`.
  - `virt_mm_init` returns error if `init_nexus` or `kinit` fails; BSP
    `init_vspace` passes `NULL` until nexus exists.
- Failure-path strategy:
  - `_kernel_get_free_page` guard: fail-fast + `pr_error` with which clause
    failed (diagnostics).
- Verification:
  - build not run (cross `x86_64-linux-gnu-gcc` missing in agent env).
- Pattern: C union double-write clears prior member (Pattern Log + §1 bullet).
- Checklist update: yes.

## 2026-03-20 | Named sentinels for nexus owner cpu + kfree_page dummy | commit <pending>

- Scope: `core/include/rendezvos/mm/nexus.h`, `core/kernel/mm/nexus.c`,
  `core/kernel/mm/kmalloc.c`, `doc/ai/AI_CHECKLIST.md`.
- Why: avoid raw `-1` / `0` as undocumented API sentinels; align with checklist.
- Verification: grep for remaining owner-cpu `-1` in this path (none).
- Pattern: magic sentinel without macro (Pattern Log + §1 bullet).
- Checklist update: yes.

## 2026-03-20 | Root AGENTS.md for Cursor Agent discovery | commit <pending>

- Scope: `AGENTS.md` (new), `doc/ai/README.md` (one-line cross-link).
- Why: give Agent automatic entry to `doc/ai/` workflow without manual `@` each session.
- Verification: n/a (docs).
- Pattern: n/a.
- Checklist update: no.

## 2026-03-20 | AI docs: naming lesson merged into checklist | commit <pending>

- Scope: `doc/ai/README.md`, `doc/ai/AI_CHECKLIST.md`, `doc/ai/INVARIANTS.md`,
  `doc/ai/RED_TEAM_REVIEW.md`, `doc/ai/TEST_MATRIX.md`; removed `doc/ai/NAMING_PATTERNS.md`.
- Why: align with mandatory mechanism (patterns live in checklist + Pattern Log);
  keep lessons abstract and avoid duplicate, example-heavy topic files.
- Design decision(s):
  - single authoritative checkable source: `AI_CHECKLIST.md` §7 + Pattern Log.
  - other `doc/ai/*.md` only cross-reference or add matrix/red-team hooks.
- Data structure/API impact: none (documentation only).
- Failure-path strategy: n/a.
- Verification: manual consistency read of `doc/ai/` tree.
- Pattern: typedef/tag shadowing + macro formal-name collision (documented in Pattern Log).
- Checklist update: yes (§7 bullets + Pattern Log; TEST_MATRIX §F; RED_TEAM prompt).

## 2026-03-20 | Port table slots hardening | commit <pending>

- Scope: `core/kernel/ipc/port_table_slots.c`, `core/kernel/ipc/port.c`,
  `core/include/rendezvos/ipc/port.h`, `core/include/rendezvos/task/tcb.h`,
  `core/kernel/task/thread.c`.
- Why: stabilize slot+hash backend, tighten lifecycle/teardown safety, improve cache memory/use pattern.
- Design decision(s):
  - keep global lock + OA hash for read-mostly port lookup workload.
  - use token `(slot_index, slot_gen)` for cache resolve and ABA resistance.
  - remove per-entry `name_copy` in thread cache; resolve by hash candidates + token validation.
- Data structure/API impact:
  - port table indices/capacities/token index use 64-bit widths.
  - thread cache entry removed `name_copy`; now stores `name_hash + slot_tok`.
- Failure-path strategy:
  - rehash build failure: rollback (old hash remains valid; new hash freed).
  - register partial failure (slot allocated, ht insert failed): rollback slot to freelist.
  - fini on non-empty table: drain (unregister + ref_put until empty).
- Verification:
  - lints on touched files clean.
  - runtime tests not executed in this turn (needs local run).
- Pattern:
  - rehash swap-before-build bug class.
  - allocator-lifetime in fini/destroy bug class.
- Checklist update: yes (`AI_CHECKLIST.md`: Failure/Rollback + Teardown sections).

## 2026-03-21 | kfree page/free API tightening | commit d65b984

- Scope: `core/kernel/mm/kmalloc.c`, `core/kernel/mm/nexus.c`, `core/include/rendezvos/mm/kmalloc.h`, `doc/ai/AI_CHECKLIST.md` (§7 + Pattern Log).
- Why: `page_vaddr` duplicated `(vaddr)p` at every call site; `pv` in `kfree` only
  mirrored `p` for the same lookups.
- Design decision(s):
  - `kfree_page_local(k_allocator, p)` uses `(vaddr)p` for `page_chunk` RB lookup.
  - keep allocator boundary at callback surface: implementation `kalloc`/`kfree` are file-local (`static`) and no longer exported by header symbol.
  - `unfill_phy_page` relink-on-failure uses a single goto exit to avoid duplicated lock/relink/error blocks.
- Data structure/API impact:
  - no public `kalloc(...)` declaration in `kmalloc.h`; external callers use `allocator::m_alloc/m_free`.
  - invariant unchanged: page-aligned whole-page kfree uses page base as RB key.
- Failure-path strategy: unchanged (`missing pcn` → same errors/fallback as before).
- Verification:
  - `make ARCH=x86_64 build` not run: toolchain `x86_64-linux-gnu-gcc` missing in env.
- Pattern: duplicate pointer + typed address for one logical key (checklist §7).
- Checklist update: yes (§7 bullet + Pattern Log).

## 2026-03-21 | Cross-CPU teardown risk review (Task_Manager) | commit 53adc0b

- Scope: `doc/ai/AI_CHECKLIST.md` (§2 + Pattern Log), `doc/ai/INVARIANTS.md` (Task_Manager).
- Why: kmem/nexus routing for remote `kfree`/`del_vspace` does not by itself
  serialize with per-CPU scheduler lists or IPC references when cleanup is
  initiated from another CPU.
- Design decision(s):
  - document invariant: `thread->tm`/`task->tm` list mutations vs owner `schedule()`.
  - prefer owner-CPU detach or per-`Task_Manager` lock over ad hoc cross-CPU free.
- Data structure/API impact: documentation only in this commit batch.
- Failure-path strategy: n/a (review).
- Verification: reasoning + code path review (`clean_server.c`, `tcb.c`,
  `task_manager.c`, `thread_syscall.c`); full build not re-run here.
- Pattern: cross-CPU teardown vs per-CPU `Task_Manager` (§2 + Pattern Log +
  INVARIANTS).
- Checklist update: yes.

## 2026-03-22 | Fix MCS pmm lock waiter node (rmap_list corruption) | commit 826f00f

- Scope: `core/kernel/mm/nexus.c`, `core/kernel/mm/map_handler.c`,
  `doc/ai/AI_CHECKLIST.md`, `doc/ai/INVARIANTS.md`, `doc/ai/ASSIST_HISTORY.md`.
- Why: `lock_mcs` second arg (`me`) must be **current CPU** `percpu(pmm_spin_lock[zone])`.
  Using `per_cpu(..., handler->cpu_id)` allowed two CPUs to share one MCS node
  (`me`), corrupting the queue and breaking exclusion on `Page.rmap_list` (symptom:
  #PF in `list_add_head` / `list_del_init`).
- Design decision(s):
  - keep global head `pmm_ptr->spin_ptr`; only `me` is per **executing** CPU.
- Failure-path strategy: n/a (lock API correctness).
- Verification: code review + grep for remaining `per_cpu(pmm_spin_lock`); build
  not run (toolchain may be absent in CI).
- Pattern: MCS `me` must be current CPU (checklist §2 + Pattern Log + INVARIANTS).
- Checklist update: yes.

## 2026-03-22 | Workflow: verification + commit gates for assistants | commit <pending>

- Scope: `doc/ai/README.md`, `AGENTS.md`, `doc/ai/ASSIST_HISTORY.md` (this entry).
- Why: assistants must not treat unverified code as correct or commit without
  maintainer confirmation; align docs with that expectation.
- Design decision(s):
  - default: diff / uncommitted changes; commit only when the maintainer asks
    or explicitly approves the batch.
  - verification must be recorded (or “not run”) before calling a change done.
- Data structure/API impact: documentation / process only.
- Failure-path strategy: n/a.
- Verification: doc consistency read; no code execution required for this entry.
- Pattern: n/a (process).
- Checklist update: no (workflow meta; checklist already has §8 validation).

