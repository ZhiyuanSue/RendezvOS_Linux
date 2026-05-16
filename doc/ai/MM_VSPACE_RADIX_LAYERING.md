# MM 分层：VSpace / Radix / Map handler / 前后端（对照实代码）

本文按**仓库里现有头文件与实现**（`vmm_radix_tree.h`、`map_handler.h`、`map_handler.c` 中 `map`/`unmap`，以及 `vmm.c` 里注册池 helper）重写分层：**每层向上接谁、向下用谁、有哪些真实符号、编排顺序与锁**。  
若实现演进，以头文件与 `.c` 为准，本文作「导航」而非第二真源。

---

## 总依赖（谁在上、谁在下）— 校正版

**先前图示容易让人误解**：好像 L6 在 L5「下面」、L5 只夹在 L4 与 L3 之间。**实际上 L5 的职责就是**：在**同一个 `VSpace*`** 里，把 **「物理页从哪来 / 何时释放」（L1 + 可选 L6）**、**「页表是否已装」（L2）**、**「radix 里该区间的 lazy/valid/rmap」（L3）** 按固定顺序串起来。也就是说 **L5 同时依赖并编排 L1、L2、L3**；**L6 不是 L3 的下层**，而是 **L5 在需要「页内容策略」时调用的插件**。

```
                    [L7  syscall / linux_layer]
                              │
                              ▼
        ┌─────────────────────────────────────────────┐
        │ L5 MM 编排（同一 VSpace 内的映射/区间）       │
        │ 依次/分支调用：L6 ops → L3 radix → L2 map   │
        │              → L1 PMM（及 rollback）        │
        └───────────────┬─────────────────────────────┘
                        │
        ┌───────┬───────┼───────┬───────┐
        ▼       ▼       ▼       ▼       ▼
     [L6]    [L3]    [L2]    [L1]    [L0]
    后端ops  Radix  map/   PMM/    arch
    匿名/文件 影子   unmap  zone

        [L4 全局 VSpace 注册 / 槽位池]  ← 进程级：谁有哪个 root_radix；
              ▲                         与「某段 mmap 用匿名还是文件」正交
              │
         L7 建进程 / L5 fault 里 lookup
```

- **L6（后端）**：**不**做 `vspace_register` / RB；只在**已选定的 `VSpace*`** 上回答「这一页 backing 怎么办 / 这段 VA 释放时我丢什么」。  
- **L5**：**唯一**应把 `insert_range`→`map`→`leaf_bind` 拼全的地方（外加 L6、L1 rollback）。  
- **L4 与 L6**：**正交** — L4 回答「当前 CPU 的 CR3 对应哪个 `VSpace*`」；L6 回答「这个 `VSpace` 里这段 region 的页从哪来」。

**更细的「后端 / 前端」API 草约**：见同目录 **`MM_BACKEND_FRONTEND_API.md`**。

---

## L5 接口面应有多宽？（对照注释掉的 `nexus.c`）

旧 `nexus.c`「屎山」主要来自：**同一文件里叠了**（1）每 vs 的 **VA 区间元数据**、（2）**全局 vspace 注册**、（3）**内核/用户取放页与 rmap/PTE 编排**、（4）**clone/mprotect/remap/query** 等策略。你现在把 **（1）** 放进 **radix**、**（2）** 放进 **`vmm` 注册池 + RB**，则 **L5（MM 编排器）** 应承接 **（3）（4）** 里所有「**在已知 `VSpace* + handler` 上、把 radix / map / PMM 按顺序拼起来**」的逻辑——**接口自然会多**，不是三五个 façade 能装完；但可以 **分族 + 分文件**（例如 `mm_kernel_heap.c`、`mm_user_utils.c`、`mm_fork.c`、`mm_mprotect.c`）避免再回到单文件巨石。

下表：**旧 nexus 符号（注释中）** → **新归属**；最后一列是 **建议落在 L5 的 façade 族**（名字可改）。

| 旧 nexus（能力摘要） | radix | vmm 注册 | map_handler | **L5 编排（建议）** |
|----------------------|-------|----------|-------------|---------------------|
| `_take_range` / `insert_nexus_entry` / per-VA 树 | ✅ 已由 `insert_range` 等替代 | — | grow 时 `map` 已在 radix 内 | L5 只调 **公开 radix API**，不再自建树 |
| `_kernel_get_free_page` / `_kernel_free_pages` | `insert`→`map`→`bind` / 逆序 | — | `map`/`unmap` | **`mm_kheap_map_pages` / `mm_kheap_unmap_pages`**（或 kmem 直接调） |
| `_user_take_range` / `user_fill_range` / `user_unfill` / `_user_release_range` | `insert_range`、`leaf_bind` / `unbind`、`delete_range` | — | `map`/`unmap` | **`mm_user_reserve`**、**`mm_user_commit`**（fault 路径）、**`mm_user_release_span`**（跨洞 walk 见下行） |
| `get_free_page` / `free_pages`（内核/用户分发） | 见上 | — | 见上 | **薄分发** → 上两行 façade |
| `free_pages` 跨多个不连续「洞」的语义 | 按 VA 步进，对每叶调用 release | — | 每 VPN `unmap` | **`mm_user_release_span`**：循环 `query_range` / 或 L3 walk + `unbind`→`unmap`→`delete_range` |
| `nexus_create_vspace_root_node` / `delete_vspace` / RB vspace | — | ✅ `vmm` | `radix_init`/`destroy` 在注册路径调 | **L4 `vspace_register/unregister`**；L5 **不**实现 |
| `vspace_clone`（walk + copy/COW + 回滚） | 子 `vs` `insert_range`、父/子 `change_leaf_ppn`、`map`、`copy_page` | 子 `register` | `new_vs_root`、`map`、`unmap` | **`mm_vspace_fork_clone`**（大块；可拆 `_copy_eager` / `_cow_prep`） |
| `nexus_migrate_vspace` | 视新模型或废弃或变成「换 CPU nexus」→ 仅 registry | ✅ | — | **策略未定** 时单独 `mm_vspace_migrate.c` |
| `nexus_update_range_flags`（两阶段 + rollback） | `change_range_flag` + 可能需 **PTE 同步** | — | `have_mapped` + `map` 改 flags？或 arch | **`mm_mprotect_range`**（radix + PTE 双写 + 临时表回滚） |
| `nexus_remap_user_leaf` / `nexus_update_node` | `change_leaf_ppn` | — | `map` 新 PPN | **`mm_user_remap_leaf`**（薄封装 + 锁序） |
| `nexus_query_vaddr`（连续区间+flags） | `query_range` 仅 **4K 叶**；**连续区间长度**若指 VMA | — | `have_mapped` | **L7 VMA** 或 L5 **`mm_query_region`** 组合 **VMA + radix**；不要强迫 radix 冒充 VMA |
| `nexus_update_flags_list_core` / aux_list 批处理 | 无 aux_list；可 **临时 list 或数组** | — | 批 `map` | **`mm_range_flags_transaction`**（内部数据结构可重做） |
| `link_rmap_list` / unlink | — | — | — | 已在 **`leaf_bind`/`unbind`**；L5 **不**重复 |
| heap_ref / `init_nexus` per-CPU | — | 迁移到 **kinit / kmem** 策略 | — | **非 L5** 或 **极薄 bootstrap** |

### L5 建议按「族」开接口（避免单文件）

1. **内核堆 / kmem**：`mm_kheap_*` — 替代 `_kernel_get_free_page` / free；只碰 `root_vspace`（或策略选的 kernel vs）。  
2. **用户 VA 带模板**：`mm_user_utils_*` — reserve / lazy 单页物化 / release_span；内部固定 radix+map 顺序。  
3. **用户文件**（接 `MM_BACKEND_FRONTEND_API`）：`mm_user_file_*` 或统一在 `mm_region_*`。  
4. **属性变更**：`mm_mprotect_*` / `mm_madvise_*` — 替代 `nexus_update_range_flags`。  
5. **单页 remap / COW**：`mm_user_remap_leaf` / `mm_cow_*` — 替代 `nexus_remap_user_leaf` + 部分 `vspace_clone` 内页逻辑。  
6. **fork/clone**：`mm_vspace_fork_*` — 唯一应接近旧 `vspace_clone` 体量之处；可再分子模块。  
7. **查询**：`mm_query_range`（radix 薄封装）+ **`mm_query_vma`**（若保留 VMA 在 L7，则 L7 实现，L5 只提供 radix 部分）。  
8. **调试**：`mm_walk_user_radix`（可选）— 替代 `nexus_print` 一类。

### 结论

- **L5 不可能只有几个接口**——旧 nexus 里凡是 **「在单个 vs 上拼 radix + PTE + PMM + 回滚」** 的，都应迁到 **L5 多文件族**；**radix 与 vmm 已吃掉的部分不要再塞进 L5**。  
- **仍缺的主要块**（相对你当前仓库）：**`vspace_clone` 等价物**、**`nexus_update_range_flags` 等价物（mprotect）**、**`get_free_page`/`free_pages` 用户路径的完整 span 语义**、**`nexus_query_vaddr` 的替代（radix + VMA 分工）**、以及 **`nexus_migrate` 是否还要** 的策略决定。

---

## L0 — 架构 PTE / flags

- **符号**：`arch_set_L0_entry` …、`arch_decode_flags` / `arch_encode_flags`（`vmm.h`）。  
- **向上**：被 L2 `map_handler` 内部、`radix` 不直接调用。

---

## L1 — PMM、zone、Page rmap

- **符号**：`struct pmm` 的 `pmm_alloc` / `pmm_free`；`pmm_zone_lock`；`Page` 与 `zone_page_cursor_*`（radix rmap 路径使用）。  
- **Radix 头文件约定**：`radix_leaf_link_rmap` / `radix_leaf_unlink_rmap` 在 **`vs->pmm->zone` 锁**下操作 `Page::rmap_list`；典型顺序是 **先做 radix / 页表侧持锁工作，再拿 zone**（见 `vmm_radix_tree.h` I6 段文字），**不要**在持 zone 时去抢可能逆序的别的锁。

---

## L2 — `map_handler`：页表与辅助能力（实代码）

**头文件**：`core/include/rendezvos/mm/map_handler.h`  
**实现**：`core/kernel/mm/map_handler.c`

### 2.1 核心：`map` / `unmap` / `have_mapped`

| 符号 | 作用 |
|------|------|
| `map(VSpace* vs, ppn_t ppn, vpn_t vpn, int level, ENTRY_FLAGS_t eflags, struct map_handler* handler)` | 在 **`vs`** 的页表里安装/更新映射；**内部** `lock_mcs(&vs->vspace_lock, &handler->vspace_lock_node)` … `unlock_mcs`（见 `map_handler.c`）。 |
| `unmap(VSpace* vs, vpn_t vpn, u64 new_entry_addr, struct map_handler* handler)` | 拆除叶子映射，返回原 `ppn`（或错误码）；同样持 **MCS `vspace_lock`**。 |
| `have_mapped(VSpace* vs, vpn_t vpn, ENTRY_FLAGS_t* entry_flags_out, int* entry_level_out, struct map_handler* handler)` | 查询是否已映射及 flags/level；持同一 **MCS**。 |

**要点**：

- `VSpace::vspace_lock` 在类型上是 `spin_lock`（实为 **MCS 队列头指针**）；**每个正在改某 `vs` 页表的 CPU** 必须用 **`handler->vspace_lock_node`** 作为 MCS 节点（`spin_lock_t vspace_lock_node` 在 `struct map_handler` 里）。  
- `map`/`unmap` 还用 **`util_map`** 把 **`vs->vspace_root_addr`** 及各级表物理页映射到 **内核固定的 MAP 窗口**（`map_pages` + per-CPU `handler->map_vaddr[]`），再 walk/改 PTE —— 这是 **L2 内部细节**，L5 只调 `map`/`unmap` 即可。

### 2.2 根页与释放

| 符号 | 作用 |
|------|------|
| `new_vs_root(paddr old_vs_root_paddr, struct map_handler* handler)` | 分配新 L0 根；可选浅拷用户半 L0；**不**建 radix、**不**建 nexus。 |
| `vspace_free_user_pt` / `vspace_free_root_page` | 释放用户页表树 / 根页（与 `del_vspace` 路径配合）。 |

### 2.3 临时映射与拷贝（COW / clone 等）

| 符号 | 作用 |
|------|------|
| `map_handler_map_slot` / `map_handler_unmap_slot` | 把任意 `ppn` 映射到 **handler 的 slot 窗口**（与 `map` 的 4 个 slot 分工由调用方约定）。 |
| `map_handler_copy_data_range` / `map_handler_copy_page` | 不假设永久 KVA，用窗口做物理拷贝。 |

### 2.4 初始化

| 符号 | 作用 |
|------|------|
| `sys_init_map` / `init_map` | BSP/AP 建立 MAP 窗口与 `handler->pmm`；`sys_init_map` 里对内核高半 L0 有 **无锁** 初始化路径（注释写明仅 BSP）。 |

**L2 不负责**：VA 区间「lazy vs valid」语义 —— **L3**；文件读 —— **L6**。

---

## L3 — Radix（`vmm_radix_tree.h` / `.c`）

### 3.1 参数顺序（头文件白纸黑字）

- **带 `handler` 的 API**：参数顺序为 **`handler` 在前，`VSpace* vs` 在后**（如 `insert_range`、`leaf_bind_range`、`delete_range`、`change_leaf_ppn`…）。  
- **仅 `vs`**：`vmm_radix_tree_change_range_flag`、`vmm_radix_tree_query_range` —— **无 `map_handler`**，且不 grow 表。

### 3.2 生命周期与高半

| 符号 | 说明 |
|------|------|
| `vmm_radix_tree_init(handler, vs)` | 分配 L0，**内部用 `map()`** 把 radix 元数据页接到内核线性映射并清零；`root_radix` 挂在 `VSpace` 上（通过 nexus 字段的历史路径在迁移中可改，以代码为准）。 |
| `vmm_radix_tree_destroy(handler, vs)` | 低半 teardown、unmap 元数据、`vs_ptr==vs` 的 rmap 清理；**共享高半 L1 不释放**（头文件 I5 段）。 |
| `vmm_radix_tree_bootstrap_shared_kernel_high_half(handler, vs)` | **一次**（如 BSP）：分配 slab + `map`。 |
| `vmm_radix_tree_install_shared_kernel_high_half(handler, vs)` | 每个 `vs` 把 L0[256..511] 指到共享 L1；与 `delete_range`/bind 的 **`vs_ptr`** 策略配套。 |

### 3.3 区间语义（与 L5 编排直接相关）

| 符号 | `radix_range_lock` kind（摘要） |
|------|----------------------------------|
| `vmm_radix_tree_insert_range` | **INSERT**：可 grow 路径；叶上 **LAZY**；`vs_ptr` 低半 `vs`、高半 **`&root_vspace`**。 |
| `vmm_radix_tree_leaf_bind_range` / `leaf_unbind_range` | **QUERY_OR_CHANGE**：**不**在 acquire 里清 L3；bind/unbind 改叶 flags + rmap。 |
| `vmm_radix_tree_delete_range` | **DELETE**：清 **radix 侧 reservation**（Phase4 `radix_node_clear` + Phase5 count/表回收在 **`radix_range_lock_acquire` 内** 完成）。**不负责** PTE、`Page` rmap —— 由调用方在进 `delete_range` 前按项目约定做完 **`leaf_unbind_range` → `unmap`**（见 `vmm_radix_tree.h` 与 L5 编排）。 |
| `change_leaf_ppn` / `change_leaf_ppn_flag` | QUERY_OR_CHANGE；**不调 `map`**；改 shadow + rmap。 |
| `change_range_flag` / `query_range` | 无 handler；只改/读 flags。 |

### 3.4 头文件推荐的端到端顺序（用户 PTE + shadow）

1. `vmm_radix_tree_init`（+ 视情况 bootstrap / install 高半）  
2. **`insert_range`**（LAZY，尚无用户 PTE）  
3. **`map(vs, …)`** 为用户 VA 安装 PTE（**L2**，持 `vspace_lock`）  
4. **`leaf_bind_range`**（VALID + `radix_leaf_link_rmap`，与 `insert_range` 的 flags 词汇对齐）  

卸载：**radix 只管元数据区间**；全路径由 **L5** 编排。项目约定（与头文件「caller 负责 PTE」一致的具体顺序）：**`leaf_unbind_range`（解 rmap + VALID→LAZY）→ `unmap`（拆 PTE）→ `delete_range`（撕掉 LAZY reservation / 回收 radix 结构）**；必要时再 `change_*` / 后端 `release`。这不是 radix「缺实现」，而是**模块边界**。

### 3.5 与 L2 的耦合（实现事实）

- **`insert_range` / `destroy` 等 grow/teardown** 会在 radix 内部 **`map(vs, …)`** 元数据页 —— 即 **L3 路径会嵌套调用 L2**，且 radix 侧还持有 **L0/L1/L2 行上的 bit 锁**（头文件 I2–I4）。  
- **L5 编排**时不要在持 **与 `map` 可能逆序** 的其它全局锁时进入 radix（你们已定 **`vspace_register_lock` 在 `vspace_lock` 外且先 register 再 `map`** 时，与 `get_free_vs_entry` 一致）。

---

## L4 — `VSpace` + 注册池（`vmm.h` + `vmm.c` helper）

- **符号（当前多为 `static`）**：`get_free_vs_entry` / `free_vs_entry` / `free_manage_node_with_page`、`vspace_rb_tree_insert/remove/search`；锁 **`lock_cas(&root_vs->vspace_register_lock)`**。  
- **与 L2**：`get_free_vs_entry` 在持 **register_lock** 下 **`map(root_vs, …)`** → **嵌套 `vspace_lock`**；顺序为 **先 register 后 vspace**。  
- **待收口对外**：`vspace_register` / `lookup` / `unregister`（前文所述），供 L5/L7 替换 nexus RB 查询。

---

## L5 — MM 前端编排（替换 nexus 里大块逻辑）

**向下**：只调 **真实存在的** L4/L3/L2/L1 API，顺序遵守 **§3.4** 与 **map_handler** 锁语义。

### 5.1 内核 `_kernel_get_free_page` 一类（连续 KVA + 用户半/内核半策略按调用点）

1. L1：`pmm_alloc`  
2. L3：`radix_range_lock` 语义由 **`insert_range`** 包在内；对目标 `vs`（常为 `&root_vspace`）`insert_range(handler, vs, kva, flags, n)`  
3. L2：对 `[kva, kva+n*PAGE)` 每页 **`map(vs, ppn+i, vpn+i, 3, …, handler)`**（若用 2M 再按策略走 `level==2`）  
4. L3：`leaf_bind_range(handler, vs, kva, ppn_first, n, leaf_flags)`  
5. 失败：按头文件 rollback 语义（bind 失败回滚 lazy + unlink rmap）+ **`unmap`** + **`pmm_free`** 等  

（若路径上还持 **L4 register_lock**，应保证 **不**与步骤 3 的 **`vspace_lock`** 逆序；当前 `get_free_vs_entry` 采用 **先 register 后 map** 已对齐。）

### 5.2 用户 fault（匿名零页）

1. L4：`vspace_lookup_root`（待实现）→ `vs` + `percpu(Map_Handler)`  
2. L6：`fault_page` → `ppn` + `leaf_flags`  
3. L3：若无 reservation → `insert_range`；若已有 → `change_*`  
4. L2：`map(vs, …)`  
5. L3：`leaf_bind_range`  
6. L0：TLBI 策略由架构路径处理  

### 5.3 卸载 / munmap

- **`mm_munmap_range`**：按约定 **`leaf_unbind_range` → `unmap` → `delete_range`**，再加 **L6 `release_pages`**；radix **不**替调用方 unbind/unmap。

---

## L6 / L7 — 后端与 syscall

（与前版相同：**ops 不进 radix**；L7 只选后端与系统调用语义。）

---

## 与「不要 nexus」的对照（结合实符号）

| 旧 nexus 职责 | 新归属 |
|---------------|--------|
| `_take_range` / per-VA 元数据 | **L3** `insert_range` + `leaf_*` + `change_*` / `query_range` |
| `_kernel_get_free_page`（PMM + 元数据 + map + rmap） | **L5** 编排 **`pmm_alloc` + `insert_range` + `map` + `leaf_bind_range`** |
| 按 CR3/root paddr 找 `VSpace*` | **L4** RB（`vspace_rb_tree_search`）+ 对外 `lookup` |
| PTE 修改 | **L2** `map`/`unmap`/`have_mapped` + **MCS `vspace_lock` + `handler->vspace_lock_node`** |
| 匿名 / 文件 | **L6**；经 **L5** 调 L3/L2 |

---

## 实现顺序建议（更新）

1. **补全或绕过 `vmm_radix_tree_delete_range`**，否则「munmap 全删 slot」缺一角。  
2. **L4 对外 `register` / `lookup` / `unregister`**。  
3. **L5 一条竖线**：`root_vspace` + `insert_range` → `map` → `leaf_bind_range`（与头文件一致）。  
4. fault + L6 匿名，再 L6 文件。

---

## 一句话（更新）

**L2 = PTE；L3 = radix 只做 VA 区间元数据（insert/bind/unbind/delete 等），不管「页从哪来 / PTE 谁先拆」；L4 = 注册池；L5 = 把 L6 与 L3/L2/L1 按约定顺序拼起来；卸载时 `unbind`→`unmap`→`delete_range` 是约定，不是 radix 模块欠账。**

---

## 各层接口手册（语义 · 向下调用谁 · 编排）

下面每张表：**语义** = 调用方应满足的契约；**向下** = 本接口内部或调用方紧接着应调用的下层；**向上** = 典型调用者；**编排** = 与上下层拼起来时的顺序提示。

---

### L0 — 架构（节选）

| 接口（示例族） | 语义 | 向下 | 向上 |
|----------------|------|------|------|
| `arch_set_L{0,1,2,3}_entry` | 写 PTE 原始字；调用方保证目标 KVA 已映射到可写 walk 窗口 | 硬件/内存 | **L2** `map_handler` 内部 |
| `arch_decode_flags` / `arch_encode_flags` | `ENTRY_FLAGS_t` ↔ 架构 PFLAGS | L0 常量/位布局 | **L2**、**L5**（组 leaf_flags） |

**编排**：L5/L2 不直接散落 `arch_*`；集中在 `map_handler` 与 radix 已用路径即可。

---

### L1 — PMM / zone / Page

| 接口 | 语义 | 向下 | 向上 |
|------|------|------|------|
| `pmm->pmm_alloc(pmm, n, &out_n)` | 从 zone 拿连续或不连续物理页；`out_n` 必须检查 | 伙伴/zone 实现 | **L2**（`new_vs_root`）、**L5**（填页）、**L4**（`get_free_vs_entry` 管理页） |
| `pmm->pmm_free(pmm, ppn, n)` | 归还物理页 | zone | **L2** teardown、**L5** rollback、**L4** `free_manage_node_with_page` |
| `pmm_zone_lock` / `unlock` | 保护 `Page::rmap_list` 等 | 无 | **L3** `radix_leaf_*_rmap`、其它 rmap 路径 |
| `zone_page_cursor_*` / `Page` | 通过 `ppn` 定位 `Page` | L1 | **L3** rmap |

**编排**：与 radix 头文件一致 — **不要在持 zone 时再去拿可能与 `vspace_lock` 逆序的锁**；通常 **radix 叶级逻辑在进 zone 前已结束其它锁序**（以 `vmm_radix_tree.h` I6 为准）。

---

### L2 — `map_handler`（页表 + 窗口 + 根）

| 接口 | 语义 | 向下 | 向上 |
|------|------|------|------|
| `init_map(handler, cpu_id, pmm)` | 填 `handler->pmm`、分配 4 个 slot 物理页与 `map_vaddr[]` | L1 `pmm_alloc` | **virt_mm_init** / AP 启动 |
| `sys_init_map(pmm)` | BSP：把 MAP 窗口接到**当前**内核根；高半 L0 补槽（注释：仅 BSP、无锁） | L0 `arch_set_*`、L1 | **BSP `virt_mm_init`** |
| `map(vs, ppn, vpn, level, eflags, handler)` | 在 `vs` 页表里建/改映射；**整段持 MCS `vspace_lock`**；内部 `util_map` 根与各级表 | L0、L1（若分配中间表页则 `pmm_alloc`） | **L5**、**L3**（radix grow/metadata）、**L4**（`get_free_vs_entry`） |
| `unmap(vs, vpn, new_entry_addr, handler)` | 拆叶映射；返回原 `ppn` 或错误 | L0 | **L5**、**L3** teardown、**L4** `free_manage_node_with_page` |
| `have_mapped(vs, vpn, flags_out, level_out, handler)` | 查询；不修改映射 | L0 | **L5** fault 快速路径、调试 |
| `new_vs_root(old_root, handler)` | 新 L0；用户半空或浅拷；**不**建 radix | L1、L0 | **L7/L5** 建进程 |
| `vspace_free_user_pt` / `vspace_free_root_page` | 拆用户树/根页 | `unmap`、L1 | **L5** `del_vspace` 路径 |
| `map_handler_map_slot` / `unmap_slot` | 临时把某 `ppn` 映射到 handler 窗口 | L0（`util_map`） | **L5** COW、`copy_*` |
| `map_handler_copy_data_range` / `copy_page` | 物理拷贝 | `map_slot`、memcpy | **L5** fork/COW |

**编排（L5 调 L2）**：

- 任意 **`map`/`unmap`** 前：本 CPU **`handler` 已 `init_map`**；`vs->vspace_root_addr` 有效（用户根已分配）。  
- **不要**在持 **L4 `register_lock`** 时再去拿另一 `vs` 的 **`vspace_lock`** 以免死锁；若必须扩展，固定全序。  
- 与 **L3**：用户路径常见为 **`insert_range` → `map` → `leaf_bind_range`**（`leaf_bind` 不调用 `map`）。

---

### L3 — `vmm_radix_tree_*`（每 `vs` 的 VA 影子）

| 接口 | 语义 | 向下（内部或契约） | 向上 |
|------|------|---------------------|------|
| `vmm_radix_tree_init(handler, vs)` | 建 `root_radix` L0；**内部 `map` 元数据页** | **L2** `map`、L1 | **L4 register** 成功后、**L5** 新用户空间 |
| `bootstrap_shared_kernel_high_half(handler, vs)` | 全局一次：共享高半 L1 slab + `map` | L2、L1 | **BSP** |
| `install_shared_kernel_high_half(handler, vs)` | 每 `vs` 挂 L0[256..511] 到共享 L1 | L2（更新 L0 项） | **L4/L5** 每个用户 `vs`（若策略需要） |
| `insert_range(handler, vs, va, flags, n)` | **INSERT** 锁；叶 **LAZY**；`vs_ptr` 按高/低半规则；grow 时 **`map`  radix 表页** | **L2**、L1 | **L5** reserve、**L4** 不直接 |
| `leaf_bind_range(handler, vs, va, ppn_first, n, leaf_flags)` | **QUERY_OR_CHANGE**；要求 PTE 已装好、PPN 连续；**VALID + rmap**；不写 `vs_ptr` | **L1** zone（rmap） | **L5** commit |
| `leaf_unbind_range(handler, vs, va, ppn_first, n)` | VALID→LAZY；unlink rmap；不写 `vs_ptr` | L1 | **L5** 软卸载 |
| `leaf_bind` / `leaf_unbind` | 单页 = `*_range(..., 1)` | 同上 | **L5** |
| `delete_range(handler, vs, va, n)` | **DELETE**：清 reservation；**叶级清除在 `radix_range_lock_acquire` 内**；**调用方须已 `leaf_unbind`+`unmap`**，radix **不**碰 PTE/rmap | （无直接 L1，除非 Phase5 `free_level_table` 触 PMM） | **L5** munmap 最后一步 |
| `change_leaf_ppn` / `change_leaf_ppn_flag` | 改绑 PPN / 标志；**不调 `map`** | L1 rmap | **L5** COW、remap |
| `change_range_flag(vs, va, flags, n)` | 仅改叶 flags；**无 handler**、不 grow | 无 L2 | **L5** mprotect 类 |
| `query_range(vs, va, out_flags)` | 读叶 flags；**无 handler** | 无 | **L5** fault、调试 |
| `destroy(handler, vs)` | 低半 radix teardown；**内部 `unmap` 元数据**；高半共享不释 | **L2**、L1 | **L4 unregister**、**L5** |

**编排（标准「装页」）**：

1. `insert_range`（LAZY）  
2. `map`（每个用户 VPN，或按大页策略）  
3. `leaf_bind_range`（与 step1 同一 `leaf_flags` 词汇）

**编排（标准「拆页」— radix 边界）**：

1. **`leaf_unbind_range`**（VALID→LAZY，rmap 与 radix 叶一致）  
2. **`unmap`**（PTE，L2）  
3. **`delete_range`**（radix：去掉 LAZY reservation / 回收元数据页；**仅管虚拟区间记录**）  
4. 后端 **`release_pages`**（L6）若需要  

（若头文件某句写「caller 先拆 PTE」，以你们全项目统一顺序为准：**先 unbind 再 unmap** 与 rmap/PPN 语义一致；文档此处以 **L5 约定** 为准。）

---

### L4 — `VSpace` 注册与池（现状 + 建议对外）

| 接口 | 语义 | 向下 | 向上 |
|------|------|------|------|
| `get_free_vs_entry(root_vs)` | 持 **`register_lock`**；可能 `pmm_alloc` + **`map(root_vs,…)`** 管理页 + 从页内 freelist 摘 **一个嵌入 `VSpace` 槽** | **L2**、L1 | **建议仅** `vspace_register` 内部（若走池分配表类对象） |
| `free_vs_entry(vs, root_vs)` | 还槽；可能触发整管理页 `unmap`+`pmm_free` | **L2**、L1 | **unregister**、错误回滚 |
| `vspace_rb_tree_insert/remove/search` | RB；**调用方已持 `register_lock`** | 无 | **register/unregister/lookup** |
| **`vspace_lookup_root(root, paddr)`**（建议） | `search` + 可选 ref 规则 | L4 RB | **L5** fault、shootdown |
| **`vspace_register(root, vs)`**（建议） | 持锁：查重 → `insert` → `radix_init`（+ 可选 install 高半） | **L3**、**L2**（init 内） | **L7** 建进程 |
| **`vspace_unregister(root, vs)`**（建议） | `destroy` → `remove` → `free_vs_entry`（若适用） | **L3**、L4 池、**L2** | **L7** 删进程 |

**编排**：**`lock_cas(&root->vspace_register_lock)`** 包住 RB + 池链表；**内**若调 `vmm_radix_tree_init`，init 会 **`map`** → **嵌套 `root->vspace_lock`**；故 **全工程** 约定 **先 register_lock，后不在持 register_lock 时去等别的 vs 的 vspace_lock**（或固定更细顺序）。

---

### L5 — MM 编排器（建议模块：`mm_front.c` 或分散在 fault/kmem）

| 接口（建议族） | 语义 | 向下 | 向上 |
|----------------|------|------|------|
| `mm_kernel_map_pages(va, n, flags)` | 内核线性区：§3.4 顺序 + rollback | L1→L3→L2→L3 | **kmem**、slab |
| `mm_user_fault(vs, va, write, backend_ops, ctx)` | fault：`query`/`insert`/`map`/`bind` 或 `change_*` | **L6**→L3→L2→L3 | **arch fault**、**L7** |
| `mm_munmap_range(vs, va, n)` | 卸映射：`unbind`→`unmap`→`delete_range`→后端 release | L3、L2、L6 | **L7** |
| `mm_brk` / `mm_mmap` / … | syscall 语义翻译为多次上列原语 | L4/L3/L2/L6 | **L7** |

**编排原则**：每个 façade **文档化锁序**；**失败回滚**与 radix 头文件「bind rollback」一致；**不在 L5 里藏 RB 细节** — 用 L4 的 `lookup/register`。

---

### L6 — 内存后端（匿名 / 文件）

**以 `MM_BACKEND_FRONTEND_API.md` 为准**：共享 **`MMBackendOps`**（`provide_page` / `release_pages` / 可选 `populate_range` + `destroy`）；**不调 radix / map / register**。

| 接口（草约名） | 语义 | 向下 | 向上 |
|----------------|------|------|------|
| `provide_page` | 产出 **一页** `ppn` + **leaf_bind** 用 `ENTRY_FLAGS_t` | **L1**、page cache / 块设备 | **L5** `mm_region_fault` |
| `release_pages` | 在 L5 **已** unbind+unmap 后释放 backing | L1、cache | **L5** `mm_region_unmap` / `destroy` |
| `populate_range`（可选） | MAP_POPULATE / readahead | 同 provide | **L5** `mm_region_create` |

**编排**：L5 **先** `provide_page`，**再** `map` → `leaf_bind_range`；失败则 radix rollback，**不**调用 `release_pages` 除非已明确占用 backing（按实现约定）。

---

### L7 — syscall / linux_layer

| 接口（示例） | 语义 | 向下 | 向上 |
|--------------|------|------|------|
| `sys_mmap` / `sys_munmap` / `sys_brk` / … | 用户契约、参数检查、VMA 链表（若你有） | **L5** façade + **L6** ctx | 用户态 |
| `clone` / `exec` 相关 | 建/换 `vs`、**`register`**、dup 表 | **L4**、**L2** `new_vs_root`、**L5** | 进程子系统 |

**编排**：L7 **不**直接 `pmm_alloc` / `vspace_rb_tree_insert`；只调 **L5/L4 公共 façade**。

---

### 跨层编排速查（一张表）

| 场景 | 顺序（成功路径） |
|------|------------------|
| 新用户地址空间 | `new_vs_root`（L2）→ `set_vspace_root_addr` → **`vspace_register`**（L4：radix `init` + RB）→（策略）`install_shared_high_half`（L3） |
| 首次 touch 匿名页 | `lookup`（L4）→ `fault_page`（L6）→ `insert_range`→`map`→`leaf_bind_range`（L3/L2） |
| munmap（全路径） | `leaf_unbind_range`（L3）→ `unmap`（L2）→ `delete_range`（L3）→ 后端 `release_pages`（L6） |
| COW 分裂 | L6 或 L5 分配新页 → `map_handler_copy_page`（L2）→ `change_leaf_ppn`（L3）→ 必要时 `unmap` 旧（顺序以 INVARIANTS 为准） |

---

### 锁序速查（与实现一致）

1. **`vspace_register_lock`**：仅保护 L4 RB + 管理页池 +（当前）`get_free_vs_entry` 内 `map(root)`。  
2. **`vspace_lock`（MCS）**：**仅**在 **`map`/`unmap`/`have_mapped`** 内、用 **`handler->vspace_lock_node`**。  
3. **Radix 区间锁**：在 **`insert_range` / `leaf_bind_range` / …** 内部；grow 时 **嵌套 `map`**。  
4. **`pmm_zone_lock`**：在 **rmap** 链更新时；**晚于** radix 头文件建议的其它锁序。  

**禁止**：在持 **zone** 时去 **`lock_mcs(vspace_lock)`**（除非全项目证明无环）。
