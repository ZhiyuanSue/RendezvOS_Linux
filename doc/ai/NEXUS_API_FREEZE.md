# Nexus 临时 API 冻结说明（重构期）

目的：你即将进行一次“基数树后端 + 大锁起步”的大重构。为了避免上层（`linux_layer/`）在重构过程中被迫跟着频繁改接口，这里把目前仓库里**新增/调整过**的 nexus 相关接口与语义做一次“临时冻结说明”。

原则：本文只描述“目前已经存在于仓库中的接口/flags 的语义约定”，并明确它们属于**待定稿**。等你重构跑通基本测例并把上层接回后，我们再统一收敛“长期稳定的最小 API 集”。

## 当前状态（git 层面）

- **`core/` 子模块处于 dirty 状态**：说明 core 内已有改动但尚未定稿合入。
- **兼容层当前的直接依赖变更点**：
  - `linux_layer/mm/linux_page_fault_irq.c` 在 lazy allocation 路径调用了 `nexus_map_anon_zero_leaf()`。

这意味着：重构期间，只要你能重新提供这里列出的“语义动作”（内部实现可大改），上层短期就可以冻结不动。

---

## 软件语义 flags（`core/include/common/mm.h`）

这些 bits 的定位是：**nexus 作为“区间语义真源”时的元数据位**，用于 fault / COW / guard / 未来 file-backed 等决策；它们**必须永远不进入硬件 PTE 编码**。

### `PAGE_ENTRY_REMAP`

- **类型**：software-only（必须在进入 arch PTE 编码前剥离）。
- **语义**：允许把已经存在的 leaf mapping 替换成另一个物理页（remap）。
- **典型使用**：COW split 时把同一 VA 的 PPN 改成新页。

### `PAGE_ENTRY_NEXUS_LAZY`

- **类型**：software-only。
- **语义**：该 VA 区间允许“延迟物理页提交”（reserve 后不立刻 commit，首次访问时由 fault 触发 populate）。

### `PAGE_ENTRY_NEXUS_COW`

- **类型**：software-only。
- **语义**：该 VA 区间带有 COW 语义（通常来自 fork/MAP_PRIVATE）；写访问需要走写故障处理。

### `PAGE_ENTRY_NEXUS_GUARD`

- **类型**：software-only。
- **语义**：guard page / 不可触碰区间；fault 处理应拒绝 populate。

### `PAGE_ENTRY_NEXUS_FILE`

- **类型**：software-only。
- **语义**：预留位，表示该区间未来可能是 file-backed。
- **当前约束**：core 目前并无 file 后端机制；此位**只允许作为“保留语义位”存在**，不能让 L2 leaf 动作直接依赖 file 细节。

### `entry_flags_rm_sw_flags(ENTRY_FLAGS_t f)`

- **语义**：从 `ENTRY_FLAGS_t` 中剥离所有 software-only bits，返回可以下发到 `map/unmap` 等页表编码路径的“硬件可接受 flags”。
- **要求**：任何最终会写 PTE 的路径，如果输入 flags 可能包含上述 software-only bits，都必须先调用该函数（或等价逻辑）。

---

## 临时新增的 nexus 公开原语（`core/include/rendezvos/mm/nexus.h`）

下面这些接口被视作“**重构期冻结**”的短期上层契约。你重构基数树后端时，可以内部完全换掉实现；如果你决定改签名（例如把 `first_entry_out` 改成 radix token/handle），建议同步更新本文作为“迁移映射”。

### 1) 语义查询：`nexus_query_user_semantics()`

```c
error_t nexus_query_user_semantics(VS_Common* vs,
                                  vaddr va,
                                  struct map_handler* handler,
                                  nexus_user_semantics_t* out);
```

- **目的**：给 fault handler / MM 子系统一个“一站式查询”：
  - **nexus 视角**：该 VA 是否在 nexus 区间内、区间起点、区间语义 flags
  - **（可选）PTE 视角**：当前 leaf 是否 present、PPN、flags、level
- **输入约束**：
  - `vs` 必须是 table vspace（用户态地址空间语义）。
  - `va` 可由实现按 4K 对齐语义处理。
  - `handler` 可为 `NULL`：此时只返回 nexus 语义，不触碰页表。
- **输出语义**：
  - `out->in_nexus=false` 表示 nexus 无覆盖（等价于 segfault 语义）。
  - `out->pte_present` 是 best-effort，仅当提供 `handler` 才尝试读取。

### 2) 用户区间两阶段原语（reserve/commit/uncommit/release）

#### `nexus_reserve_user_range()`

```c
error_t nexus_reserve_user_range(size_t page_num,
                                vaddr target_vaddr,
                                struct nexus_node* nexus_root,
                                VS_Common* vs,
                                ENTRY_FLAGS_t region_flags,
                                struct nexus_node** first_entry_out,
                                vaddr* addr_out);
```

- **语义**：在 nexus 中为一个用户 VA 区间“占位/登记语义”（建立元数据节点），但不一定立刻建立 leaf PTE。
- **返回**：
  - `addr_out`：实际分配到的起始 VA（可能与 hint 不同）。
  - `first_entry_out`：提交阶段的入口（当前实现依赖 RB-tree 结构；重构后可替换为 token/handle）。

#### `nexus_commit_user_range()`

```c
error_t nexus_commit_user_range(struct nexus_node* first_entry,
                               int page_num,
                               VS_Common* vs);
```

- **语义**：为此前 reserve 的区间分配物理页并建立 leaf PTE（populate）。
- **约束**：失败时必须可回滚（至少不泄露物理页、不留下半映射）。

#### `nexus_uncommit_user_range()`

```c
error_t nexus_uncommit_user_range(vaddr addr, int page_num, VS_Common* vs);
```

- **语义**：撤销 leaf PTE 并释放物理页（允许保留区间元数据，以便后续再次 commit 或 lazy fault）。

#### `nexus_release_user_range()`

```c
error_t nexus_release_user_range(vaddr addr, int page_num, VS_Common* vs);
```

- **语义**：删除/释放 nexus 中的区间元数据（并处理必要的 split/truncate）。
- **典型使用**：munmap 删除区间。

> 备注：这四个原语等价于把旧 `get_free_page/free_pages` 的“元数据 + PTE + 回滚”拆成两阶段。即使你把后端换成 radix，也建议保留这个语义层次（便于后续引入 file-backed、swap 等后端能力）。

### 3) fault-time populate：`nexus_map_anon_zero_leaf()`

```c
error_t nexus_map_anon_zero_leaf(VS_Common* vs,
                                vaddr va,
                                struct map_handler* handler,
                                ENTRY_FLAGS_t leaf_flags);
```

- **定位**：core 机制原语，用于“lazy anon 页的首次访问补页”。
- **语义**：
  - 若该页已 present：成功返回（幂等）。
  - 若 nexus 中存在该 VA 的元数据节点且允许 populate：
    - 分配 1 个物理页
    - 清零（anon 语义，避免信息泄漏）
    - 建立 4K leaf 映射（flags 需剥离 software-only bits）
    - 维护 rmap
  - 若该页是 guard：拒绝（错误返回）。
- **输入约束**：
  - `vs` 必须是 table vspace。
  - `va` 必须是用户区间（拒绝 kernel VA）。
  - `handler` 必须非空。
- **注意**：该接口当前只表达 anon-zero populate。未来 file-backed 等后端应通过“后端能力/ops 注入”演进，而不是把 file 逻辑侵入此函数。

---

## 兼容层（linux_layer）当前依赖点

### `linux_layer/mm/linux_page_fault_irq.c`

- **依赖**：lazy allocation 路径调用 `nexus_map_anon_zero_leaf(vs, aligned, handler, nflags)`。
- **上层预期**：只要 nexus 认为该页在区间内且允许访问，该调用会把 4K leaf 补齐；失败则 linux_layer 走 fatal fault。

---

## 冻结期间的约定（给闭关重构用）

1. **短期冻结**：上层（`linux_layer/`）在你重构期间尽量不改；core 内部可任意重写，只要能重新提供这些“语义动作”。  
2. **签名可变但语义不变**：如果你把 `first_entry_out` 改成 radix token（推荐），可以改签名，但请同步更新本文作为“迁移映射”。  
3. **software-only flags 必须可存储但不得下发到 PTE**：无论 RB-tree 还是 radix，都必须保留 `entry_flags_rm_sw_flags` 等价机制。  
4. **锁与 rmap 不变式优先**：任何重构都不得破坏 `doc/ai/INVARIANTS.md` 中关于 rmap/PMM 锁顺序与 MCS `me` 约束。  

