# Invariants (AI + Reviewer Reference)

Keep this file short and operational.
If a change breaks or modifies an invariant, update this file in the same commit.

## Port Table / Slots

- `Port_Table.lock` protects all table mutations (`slots`, `ht`, `live_ports`, freelist).
- `port_slots_lookup/resolve` success returns with one valid ref (`ref_get_not_zero`).
- `port_slots_free_slot` must bump `slot.gen` before slot reuse.
- Freelist empty sentinel must be type-consistent with `free_head` (`u64`).
- Token invalidation must be type-consistent with `port_table_slot_token_t.slot_index` (`u32`).
- `unregister` path must: remove hash mapping, unlink port, clear slot, decrement live count, free slot.
- `fini` must not leave registered ports silently alive.
- Rehash must be two-phase: build new table fully, then swap.

## Thread Port Cache

- Cache entry occupancy is determined by valid token state, not by stale string fields.
- Hash collision must not cause false-negative early return.
- Failed resolve of a candidate entry invalidates that entry only.
- Cache does not own a persistent port ref; each lookup/resolve acquires its own ref.

## Allocator Ownership

- Table-internal buffers (`slots`, `ht`) are allocated/freed by the same table allocator.
- Destroy path must keep allocator valid until table object free completes.

## Nexus / `nexus_node` address-space identity (`VS_Common`)

- **Multi-role aggregate:** when one `struct` type represents more than one logical
  role (different roots, containers, or records in the same subsystem), pointers
  of that type are **not** interchangeable by shape alone—naming and call-site
  context must reflect **role** (which tree, which lock, which lookup root).
- `nexus_node` holds `VS_Common* vs_common` only. `VS_Common` (`vmm.h`) is one
  struct: `type` (`enum vs_common_kind` as `u64`) + **anonymous** union (C11:
  members lifted to `VS_Common`). **Kernel heap ref:** `vs` points at the shared
  root `VS_Common` (table branch), `cpu_id` is the allocating CPU for kmem
  routing. **User / table branch:** `vspace_root_addr`, locks, `_vspace_node`
  (same union storage; never interpret without `type`).
  Never infer role from pointer truthiness. Per-CPU `nexus_kernel_heap_vs_common`
  holds the kernel-heap ref object; `new_vspace` returns a heap `VS_Common*`
  with `type == USER_VSPACE`, freed whole in `del_vspace`.
- `map` / `unmap` / `have_mapped` take a **table** `VS_Common*` (kernel
  `root_vspace` or user object from `new_vspace`), not the KERNEL_HEAP_REF
  wrapper.
- `nexus_delete_vspace(nexus_root, vs)` and `nexus_migrate_vspace` take the
  **per-CPU** `nexus_root` that owns `_vspace_rb_root`, not a per-vspace node.

## kmem / cross-CPU page `kfree`

- **Per-CPU drain:** each `kalloc` / `kfree` entry drains **both** cross-CPU
  queues on that allocator (`kfree_page_msq` for remote whole-page frees,
  `buffer_msq` for remote small-object frees), so neither backlog grows when only
  one allocation size is active.
- Whole-page `kfree` routes via `nexus_kernel_page_owner_cpu(kva)` to the owner
  CPU’s `kallocator` MSQ. Owner is **only** `cpu_id` on rmap nodes with
  `KERNEL_HEAP_REF` (per-CPU `nexus_kernel_heap_vs_common`); do not infer from
  global `root_vspace`. User rmap entries on the same PPN are ignored for this
  purpose.
- `Page.rmap_list` is PMM metadata: link/unlink and read-only walks that
  interpret the list run under the zone `pmm` MCS lock (`spin_ptr` +
  `percpu(pmm_spin_lock[zone_id])`). `unfill_phy_page` detaches **one** rmap
  entry under that lock (`list_del_init`), drops the lock, then unmaps — repeat
  until the list is empty — so lock order never inverts with
  `nexus_vspace_lock` (no fixed cap on how many mappings share a physical page).
- **MCS waiter node (`me`):** the second argument to `lock_mcs` / `unlock_mcs` must
  be **this CPU’s** `percpu(pmm_spin_lock[zone_id])`, never
  `per_cpu(pmm_spin_lock[zone_id], handler->cpu_id)`. The global queue head is
  `pmm_ptr->spin_ptr`; `me` is per **acquirer** CPU. Reusing another CPU’s
  per‑CPU slot as `me` on a different CPU corrupts the MCS queue, breaks mutual
  exclusion on `rmap_list` / buddy metadata, and can surface as page faults in
  `list_add_head` / `list_del_init`.

## Task_Manager / teardown (SMP)

- `Task_Manager` is **per CPU** (`percpu(core_tm)`). `thread->tm` / `task->tm`
  point at the manager that owns `sched_thread_list` / `sched_task_list` for
  that thread/task.
- `schedule()` walks `sched_thread_list` without a lock. Any path that
  **removes** a thread/task from those lists or mutates `current_thread` must
  not run concurrently with the **owner CPU’s** scheduler
  on the same lists—unless a dedicated lock or owner-CPU-only execution is
  established.
- Default `sys_exit` sends cleanup work to **global clean server port** (looked up
  via `thread_lookup_port("clean_server_port")`). Clean server threads on each CPU
  receive messages from the shared port via `recv_msg`. A design that tears down
  another CPU’s thread/task from a remote CPU must explicitly synchronize with that
  CPU’s `Task_Manager` (and any IPC/port references), not rely on kmem routing alone.

- **Teardown split:** logical unlink (task list + scheduler ring) happens before
  dropping the last ref; final free drains owned resources and frees the object.

- **Intent survives IPC:** exit/teardown intent uses a monotonic flag (not a
  transient status) so IPC/blocking cannot erase it; owner CPU proves quiescence
  via `thread_status_zombie` before reaping.

- **No freed nodes in traversable structures:** before freeing an object, it must
  be detached from any list/ring/queue that other code may traverse.

- **`current_thread` / `belong_tcb` / vspace:** The runnable identity is
  `current_thread`; the logical task is **`get_cpu_current_task()`** =
  `current_thread->belong_tcb` when set, else `root_task` (covers threads
  detached from a task but still current briefly). There is no separate
  `Task_Manager::current_task` field. After each successful switch to a
  **user** thread, `schedule` updates CR3 / `current_vspace` when
  `prev_tcb != next_tcb`; when the **next** thread is **kernel-only**, if the
  **previous** thread was user, drop the active vspace ref and point
  `current_vspace` at `root_vspace`. Without dropping user vspace on kernel
  idle, kernel code can keep a user CR3. `gen_thread_from_func` attaches new
  kernel threads to `root_task` when present, else `get_cpu_current_task()`.

- **User `VS_Common` teardown vs SMP (CR3 / `current_vspace`):** `delete_task` may
  call `del_vspace`, which tears down page tables. No CPU may still execute with
  that task’s page tables loaded: if another CPU faults while `CR3` (or the
  kernel’s `current_vspace` / map-handler view) still targets a `VS_Common` that
  is already being freed, you can get `CR2 ≈ RIP` and recursive `#PF` / triple
  fault. Moving only the clean-server CPU to `root_task` is **not** sufficient;
  cross-CPU quiescence (e.g. remote threads stopped, IPI + switch all CPUs to a
  safe kernel vspace, or a vspace refcount / deferred free) must be designed
  explicitly.

## Maintenance Rule

- Any bug that reveals a missing invariant here must add a new bullet in the same fix commit.
- **Identifier / naming discipline** (compile-time auditability, not a runtime
  invariant): see `AI_CHECKLIST.md` §7 (API/Type Discipline)—especially shadowing
  and macro-token collisions.

