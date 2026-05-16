# Radix 区间锁：`radix_range_lock_acquire` 五阶段方案（insert / delete 共用骨架）

本文描述 `core/kernel/mm/vmm_radix_tree.c` 中 **区间锁获取** 的五个阶段、**insert** 的 **pending 指针 + 延迟 VALID** 语义，以及与 **delete / query** 的差异。锁粒度与持锁时长可后续优化；此处以 **语义正确、无半提交可见** 为先。

---

## 全局约定

- **`Radix_entry_t::value`**：bit0 为 entry 自旋锁；bit1 为 **VALID**；bits 2..11 为子占用计数；高位为子对象 KVA（低 12 位为 0）。
- **对外可见的「已挂子」**：必须 **`VALID == 1` 且子指针非 0**。其它路径只可在 **本区间锁流程已持有的锁** 下，用 **`entry_child_kva`** 跟随 **pending**（`VALID==0` 且 ptr≠0）。
- **insert 的中间态（pending）**：已为子对象分配物理页并写入 **KVA**，但 **故意不置 VALID**。表示「本事务已占位，尚未提交」；失败回滚只释放在此状态下分配的页并把 word 清零，**无需** 撤销已写入的结构计数（计数仅在提交阶段发生）。
- **delete / query**：只承认 **VALID** 链；路径上任何一级 **`!VALID`** 视为不存在（与 pending 无关，因并发下其它事务的 pending 必须被对应 L0/L2 锁挡住——当前实现 insert 持全段相关 L0 锁时再改树）。

---

## Phase 1 — 锁 L0 片、保证 L1 表页存在（insert 可 grow）

- **遍历** VA 区间所跨的每个 **L0 下标** `i ∈ [l0_first, l0_last]`，**升序**对 `root[i]` **`radix_entry_lock`**。
- **insert**：若该 L0 槽尚无 **L1 表页**，则 **`radix_ensure_table_page_insert(..., RADIX_ENSURE_LEVEL_L1)`**（内部 **pmm_alloc**、`map`、清零），对该 **`l0e`** **`install_child_ptr_pending`**（写子 KVA，**VALID=0**，子计数=0）。若已有 **VALID** 或 **已有 pending 指针**，则跳过分配。
- **delete / query**：若 **`l0e` 无 VALID**，报错（区间不存在）；**不** grow。
- **失败回滚（insert，Phase 1 内）**：从当前 `i` 向前（降序）对已锁过的片尝试 `free_unused_child_table(..., RADIX_ENSURE_LEVEL_L1)`，再 **unlock** 已锁 L0 段。

---

## Phase 2 — 铺 L1 槽到 L2 表（insert 可 grow；不对 l1e 单独长期持锁）

- **对每个** 与 `[base,end)` 相交的 `(li, L1 下标 j)`：
  - **insert**：**`radix_ensure_table_page_insert(..., RADIX_ENSURE_LEVEL_L2)`**：若该 **`l1e`** 尚无子，则分配 L2 表页，对 **`l1e`** **`install_child_ptr_pending`**（**VALID=0**）。结构上的 **L0 fanout（子计数）不在此阶段修改**。（Phase 1 对 L1 表页用 **`RADIX_ENSURE_LEVEL_L1`**，同一函数。）
  - **delete / query**：要求对应 **`l1e` 已 VALID** 且子指针可用，否则报错。
- **不** 在此阶段更新 **L0/L1 结构计数**。

---

## Phase 3 — 真实「区间锁」：按 VA 升序锁每条 2Mi 行（L2 entry）、挂 L3、做叶级校验

- **对** `[band_first_2m, band_last_2m]` 内每个 2Mi 起点 **升序**（`radix_l2_band_walk_init(..., dir=+1, …)` / `radix_l2_band_walk_step`，`radix_l2_entry` 内已解析 L0→L1→L2）：
  - **`radix_entry_lock(l2e)`**（本条 2Mi 的区间锁）。
  - **insert**：**`radix_ensure_l3_for_band_insert`**：若该 **`l2e`** 尚无 L3，则分配 16KiB L3 数组并初始化，对 **`l2e`** **`install_child_ptr_pending`**（**VALID=0**）。若已有 **VALID** 或 **pending**，跳过。
  - **insert**：**`radix_validate_band_leaf_requirements(..., INSERT)`** — 通过 **`radix_l3_node`** 访问叶（内部允许 **pending l2e** 的 KVA）。
  - **delete / query**：要求 **`l2e` 已 VALID**；再按 kind 校验叶。
- **本阶段结束**：所有相关 **overlap / 存在性** 判断已完成；**insert** 仍 **未** 对任何新结点置 **VALID**，**未** 做 **L0/L1 结构计数** 提交。

---

## Phase 4 — insert：提交 VALID + 结构计数；delete：仅 L1 带级结构减量

### insert（仅当 Phase 3 全部成功）

在 **仍持有** Phase 1 锁住的 **整段 L0**、且 Phase 3 **仍持有** 所有已交叉 **l2e** 的前提下：

1. **提交 L0 → L1 表**：对每个 crossed **`l0e`**，若 **`!VALID && ptr≠0`**，则 **`commit_valid`**（仅置 **VALID**，不改编码计数以外的字段）。
2. **提交 L1 → L2 表**：对每个 crossed **`l1e`**，若 **`!VALID && ptr≠0`**，则 **`commit_valid`**，并对父 **`l0e`** **结构子计数 +1**（表示多一个 **已发布 L2 表** 的 L1 槽）。
3. **提交 L2 → L3**：对 crossed 的每个 **`l2e`**（按 band  walk），若 **`!VALID && ptr≠0`**，则 **`commit_valid`**（在 **已持该 l2e 锁** 下用 nolock 变体），并对父 **`l1e`** **结构子计数 +1**（多一条 **已发布 L3** 的 2Mi 行）。

**说明**：结构增量由 **「提交时是否从 pending 变为 VALID」** 推导，**不需要** 栈上为计数单独保存大头尾位图；与「头尾+中间」范式一致的是 **几何区间上的闭式** 可由 **pending 扫描** 等价实现（本次实现采用 **按层提交 + 按槽判断 pending**）。

### delete

- **`radix_apply_l1_struct_delete_bands`**（与现实现一致）：按 2Mi 带对 **将空** 的带做 **L1 结构减量**。
- **不在此 phase 做 shrink**；元数据表释放在 **`radix_range_lock_acquire` 的 Phase 5** 内完成。`vmm_radix_tree_delete_range` 仅 `acquire`/`release`，不再做二次 shrink；**业务 VA 的 PTE** 由上层拆除。`leaf_unbind(handler, vs, va, ppn)`（`ppn` 与 `leaf_bind` 一致）在 PMM zone 锁下从 `Page::rmap_list` 摘除该叶子的 rmap 后恢复 LAZY 影子，**不** `unmap` 叶子 VA。

---

## Phase 5 — 释放全部 L0 锁；在仅持 L2 行锁下调整「页数」占用

- **`radix_unlock_l0_span(root, l0_first, l0_last)`**。
- **insert**：对每个 crossed 2Mi 带，**`radix_entry_adj_occ_nolock(l2e, +band_page_delta)`**（行内 **4Ki 槽** 占用）。
- **delete**：**`-band_page_delta`**。
- **query/change（不改变映射规模）**：不调 L2 页计数。

成功返回后，**调用方**仍持有 Phase 3 获取的 **所有 l2e** 锁，直到 **`radix_range_lock_release`** 按降序 VA 释放。

---

## insert 失败回滚（摘要）

- **Phase 3 及以后失败**：先 **按降序 unlock 已持有的 l2e**；再 **`radix_insert_undo_new_l3_bands`**（**`sbi` 降序**；释放 **ptr≠0** 且 **`entry_decode_count(l2e)==0`** 的 L3，不扫整 band 叶；计数与 Phase 5 一致时即与「带保留的 band」互斥）；再 **`radix_insert_undo_empty_l2_l1_for_range`**（**`li` 从高到低**，片内用同一 **`radix_l1_range_walk`**，`dir=-1` 降序 `j`）；每个 `l0e` 仍尝试 `L1` 级 `free_unused`，与 Phase 2 升序铺表对称）；最后 **unlock L0**。
- **因从未进入 Phase 4 提交**，**不存在**「结构计数已加却要回滚」的问题。

---

## 与 `vmm_radix_tree.h` 文档的关系

若 header 中 **I0–I7** 与本文冲突，以 **「对外仅视 VALID 为已发布」** 为准；**pending** 为 **insert 区间事务** 内部态，**不得**被无锁读者当作正式路径跟随（读路径须 **`entry_valid` 为真** 再跟指针）。
