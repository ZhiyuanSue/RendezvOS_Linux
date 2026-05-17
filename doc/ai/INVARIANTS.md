# Invariants (AI + Reviewer Reference)

Keep this file short and operational.
If a change breaks or modifies an invariant, update this file in the same commit.

## Port table / name index

- `Port_Table.by_name.lock` protects all index mutations (`rows`, `ht`, `live`, freelist).
- `port_table_lookup` / `port_table_resolve_token` success returns with one valid ref (`ref_get_not_zero`).
- Row reuse must bump `row.gen` before the row is recycled.
- Freelist empty sentinel must be type-consistent with `free_head` (`u64`).
- Token invalidation must be type-consistent with `name_index_token_t` / `name_index_token_t` (`row_index` is `u32`).
- `unregister` path must: remove hash mapping, unlink value, clear row, decrement live count, recycle row.
- `name_index_fini` must not leave registered values silently alive.
- Rehash must be two-phase: build new table fully, then swap.

## Thread Port Cache

- Cache entry occupancy is determined by valid token state, not by stale string fields.
- Hash collision must not cause false-negative early return.
- Failed resolve of a candidate entry invalidates that entry only.
- Cache does not own a persistent port ref; each lookup/resolve acquires its own ref.

## Allocator Ownership

- Table-internal buffers (`slots`, `ht`) are allocated/freed by the same table allocator.
- Destroy path must keep allocator valid until table object free completes.

## Radix Tree / VSpace address-space metadata

- **Multi-role aggregate:** when one `struct` type represents more than one logical
  role (different roots, containers, or records in the same subsystem), pointers
  of that type are **not** interchangeable by shape alone—naming and call-site
  context must reflect **role** (which tree, which lock, which lookup root).
- **Radix_tree metadata:** Each `VSpace` has its own `root_radix` (4-level 512-way radix tree)
  for virtual address space metadata. `VSpace` (`vmm.h`) is one struct: `type`
  (`enum vs_common_kind` as `u64`) + **anonymous** union (C11: members lifted to `VSpace`).
  **Kernel heap ref:** `vs` points at the shared root `VSpace` (table branch), `cpu_id`
  is the allocating CPU for kmem routing.
- **`map` / `unmap` / `have_mapped`** take a **table** `VSpace*` (kernel
  `root_vspace` or user object from `new_vspace_structure`), not the KERNEL_HEAP_REF
  wrapper.
- **Kernel SMP:** mutations / walks of `root_vspace` page tables must hold
  `root_vspace.vspace_lock` (MCS, taken inside `map`/`unmap`/`have_mapped` via
  the per-CPU `map_handler` waiter node). **Radix Tree two-tier locking:** L0 big lock
  (512 GiB granularity) + L2 per-band lock (2 MiB granularity) provides better
  multi-core scalability than single vspace-wide locks.
- **Radix tree owner tracking:** Shared high-half (L0[256..511]) uses `tagged_ptr owner`
  field to track ownership (low-half = vs, high-half = &root_vspace). DELETE operations
  only clear leaves whose owner matches the caller.

## kmem / cross-CPU page `kfree`

- **Per-CPU drain:** each `kalloc` / `kfree` entry drains **both** cross-CPU
  queues on that allocator (`kfree_page_msq` for remote whole-page frees,
  `buffer_msq` for remote small-object frees), so neither backlog grows when only
  one allocation size is active.
- Whole-page `kfree` routes via radix owner lookup (`kmem_radix_kernel_heap_owner_cpu`
  in `kernel/mm/kmalloc.c`) to the owner CPU’s `kallocator` MSQ. Owner is **only**
  `cpu_id` on rmap nodes with `KERNEL_HEAP_REF` (per-CPU `nexus_kernel_heap_vs_common`,
  legacy symbol name in `vmm.h`); do not infer from global `root_vspace`. User rmap
  entries on the same PPN are ignored for this purpose.
- `Page.rmap_list` is PMM metadata: link/unlink and read-only walks that
  interpret the list run under the zone `pmm` MCS lock (`spin_ptr` +
  `percpu(pmm_spin_lock[zone_id])`). `unfill_phy_page` detaches **one** rmap
  entry under that lock (`list_del_init`), drops the lock, then unmaps — repeat
  until the list is empty — so lock order never inverts with
  radix tree range locks (no fixed cap on how many mappings share a physical page).
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

## Linux compatibility (syscall ABI)

- **User-visible errno discipline:** syscalls implemented under `linux_layer/`
  must not return RendezvOS internal error codes (e.g. `-E_RENDEZVOS == -1024`)
  to userspace. Return **Linux errno** (negative) or a Linux-defined value
  (e.g. `brk` returns new program break). Keep Linux errno constants in
  `include/linux_compat/errno.h`.

- **`write` before VFS:** Until a per-process fd table and VFS exist, `write`
  may handle **only stdout/stderr** (fd `1` and `2`) via the console/UART shim
  in `linux_layer/io/sys_write.c`. Other fds return `-EBADF`; do not hard-code
  console output only inside `syscall_entry.c`—keep `sys_write` as the
  extension point (see `doc/linux_compat/STDIO_SHIM.md`).

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

- **User `VSpace` teardown vs SMP (CR3 / `current_vspace`):** `delete_task` may
  call `del_vspace`, which tears down page tables. No CPU may still execute with
  that task’s page tables loaded: if another CPU faults while `CR3` (or the
  kernel’s `current_vspace` / map-handler view) still targets a `VSpace` that
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

