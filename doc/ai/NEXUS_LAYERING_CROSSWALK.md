# Nexus 分层对照表（重构参考）

本文是对 `core/kernel/mm/nexus.c` 现有函数的分层对照，目标是为后续“后端 CRUD 与前端原语组合解耦”提供直接可编辑的工作底稿。

## 分层定义（本表使用）

- **L0: Entry 内存支撑层**
  - nexus 自身元数据页/节点池管理（manager page / free list / node alloc/free）。
- **L1: 后端数据结构层（Range/Index CRUD）**
  - vspace 注册树、per-vspace range 树、kernel heap range 树；纯增删查改与区间结构操作。
- **L2: 页表/物理页后端适配层（PT+PMM+rmap）**
  - map/unmap/have_mapped 封装组合、rmap link/unlink、ppn/refcount 一致性。
- **L3: 前端原语组合层（对上层暴露）**
  - reserve/commit/uncommit/release、range flag update（含回滚）、fault-time leaf 修复、clone 策略组合。

---

## 函数对照表（按文件中大致出现顺序）

| 函数 | 建议层级 | 当前主要职责 | 主要越界/混杂点 | 建议重构落点 |
|---|---|---|---|---|
| `link_rmap_list` | L2 | 把 `nexus_node` 挂到 `Page.rmap_list` | 直接处理 2M/4K 混合策略判断 | 保持 L2；策略判断可抽成小 helper |
| `unlink_rmap_list` | L2 | 从 `Page.rmap_list` 摘节点 | 无明显越界 | 保持 L2 |
| `nexus_node_get_len` | L1 | 节点覆盖长度（4K/2M） | 无 | 保持 L1 |
| `nexus_node_get_pages` | L1 | 节点页数（1/512） | 无 | 保持 L1 |
| `nexus_node_set_len` | L1 | 设置 huge 属性 | 无 | 保持 L1 |
| `nexus_update_node` | L2 | 单 leaf 更新（PTE+region_flags+rmap+old_ppn 处理） | 带有 policy 位更新判断（`update_region_flags`） | 主体留 L2；策略判断参数由 L3 控制 |
| `nexus_rb_tree_insert` | L1 | 区间插入 | 无 | 保持 L1 |
| `nexus_rb_tree_vspace_insert` | L1 | vspace root 注册插入 | 无 | 保持 L1 |
| `nexus_rb_tree_remove` | L1 | 区间删除 | 无 | 保持 L1 |
| `nexus_rb_tree_vspace_remove` | L1 | vspace root 注册删除 | 无 | 保持 L1 |
| `nexus_rb_tree_vspace_search` | L1 | 查 vspace root 节点 | 无 | 保持 L1 |
| `nexus_rb_tree_search` | L1 | 按 VA 查覆盖节点 | 无 | 保持 L1 |
| `nexus_rb_tree_prev` | L1 | 前驱 | 无 | 保持 L1 |
| `nexus_rb_tree_next` | L1 | 后继 | 无 | 保持 L1 |
| `is_page_manage_node` | L0 | 区分管理节点 vs 普通节点 | 依赖复用字段语义，侵入全文件 | 保持 L0，减少外层直接依赖 |
| `nexus_init_manage_page` | L0 | 初始化管理页中的节点池 | 无 | 保持 L0 |
| `nexus_manage_pop` | L0 | 从管理池取节点 | 无 | 保持 L0 |
| `nexus_manage_push` | L0 | 归还节点到管理池 | 无 | 保持 L0 |
| `new_nexus_entry` | L0 | 分配一个 nexus node | 可能夹带 L1 初始化细节 | 分成 alloc 与 init 两步 |
| `delete_nexus_entry` | L0/L1 | 释放节点并从树/链表移除 | 同时处理结构删除与内存归还 | 拆成 L1 remove + L0 free |
| `init_vspace_nexus` | L0/L1 | 初始化 vspace 的 nexus 子树和管理页 | 初始化与注册混在一起 | 拆为 `init_pool` + `init_indices` |
| `init_nexus` | L0/L1 | 初始化 nexus_root（全局/每CPU） | 同时做内存、索引、vspace 关联 | 拆为 boot init 分步函数 |
| `nexus_create_vspace_root_node` | L1 | 创建并注册 vspace root node | 夹带部分 pool 逻辑 | 保持 L1，调用 L0 API |
| `nexus_delete_vspace` | L3 | 删除一个 vspace 的 nexus 视图 | 可能直接穿透到 L0/L2 | 作为 L3 组合保留，内部只调 L1/L2/L0 API |
| `nexus_migrate_vspace` | L3 | vspace 在不同 nexus_root 间迁移 | 同时触碰注册树与运行时状态 | 保持 L3 组合 |
| `_take_range` | L1/L3 | 通用 range 占位（含 kernel/user 差异） | 混合了策略分支（allow_2M） | 拆为 L1 `insert_range` + L3 policy wrapper |
| `_vspace_clone_copy_page` | L3 | clone-copy 子流程（copy page） | 直接触碰 map/pmm | 保持 L3，依赖 L2 API |
| `_vspace_clone_cow` | L3 | clone-cow 子流程（共享+RO+COW准备） | 组合逻辑与叶子细节耦合 | 保持 L3，叶子动作改调 L2/L3原语 |
| `nexus_update_flags_list_core` | L3 | range flags 批量更新核心（缓存/回滚） | cache_data 复用技巧侵入实现 | 保持 L3，回滚缓存对象可独立化 |
| `_vspace_update_user_leaf_flags` | L3 | 遍历用户叶子更新 flags | 依赖具体树遍历细节 | 保持 L3 |
| `vspace_clone` | L3 | 对外 clone 入口（copy/COW策略） | 体量大，分支多 | 拆 `clone_copy` / `clone_cow` / `clone_finalize` |
| `_kernel_get_free_page` | L3（kernel path） | kernel 路径 alloc+range+map+rmap | 一函数包含分配/映射/回滚全流程 | 拆成 reserve/commit 风格子步骤 |
| `user_fill_range` | L2/L3 | 对 reserve 区间分配并 map（commit） | 同时承担事务回滚 | 建议归 L3，内部调用 L2 leaf API |
| `_user_take_range` | L1 | user range 占位（reserve） | 依赖 flags 语义（policy侵入） | 保持 L1，policy 参数由 L3 传入 |
| `_unfill_range` | L2 | unmap + 物理页释放 + rmap 处理 | 与区间控制耦合 | 保持 L2 |
| `_kernel_free_pages` | L3（kernel path） | kernel 路径 unmap+release 组合 | 大量组合逻辑 | 拆分为 L3 wrapper + L2/L1 |
| `user_unfill_range` | L2/L3 | 用户 uncommit 入口 | 与范围迭代/事务耦合 | 归 L3，底层动作交 L2 |
| `_user_release_range` | L1 | 删除 VA 区间对应节点 | 包含部分策略分支（2M截断处理） | 保持 L1，策略报错逻辑上提 L3 |
| `nexus_get_vspace_node` | L1 | 从 `vs->_vspace_node` 拿 vspace node 并校验 | 无 | 保持 L1 |
| `nexus_query_user_semantics` | L3（查询原语） | 统一返回 nexus 语义 + 可选 PTE 状态 | PTE 查询依赖 handler（层间桥接） | 保持 L3 查询面 |
| `nexus_reserve_user_range` | L3 | reserve 对外原语 | 薄封装 | 保持 L3 |
| `nexus_commit_user_range` | L3 | commit 对外原语 | 薄封装 | 保持 L3 |
| `nexus_uncommit_user_range` | L3 | uncommit 对外原语 | 薄封装 | 保持 L3 |
| `nexus_release_user_range` | L3 | release 对外原语 | 薄封装 | 保持 L3 |
| `nexus_map_anon_zero_leaf` | L3/L2 | fault-time lazy anon populate（alloc+zero+map+rmap） | 包含 policy（guard判定）+ leaf动作 | 可拆：L3 判定 + L2 `populate_leaf_anon` |
| `get_free_page` | L3 | 旧接口：kernel/user 组合入口 | kernel/user 分支+事务处理 | 保持兼容，内部继续走 L3 原语 |
| `free_pages` | L3 | 旧接口：kernel/user 组合释放 | kernel/user 分支+事务处理 | 保持兼容，内部继续走 L3 原语 |
| `nexus_update_range_flags` | L3 | 区间 flags 更新与回滚总入口 | 与拆分/回滚强耦合 | 保持 L3 核心 API |
| `nexus_remap_user_leaf` | L3/L2 | fault-time COW remap 原语 | 既做查找/校验又做 leaf更新 | 可拆：L3 校验 + L2 leaf remap |
| `nexus_query_vaddr` | L1 | 查询 VA 对应 nexus 节点起点与 flags | 无 | 保持 L1 |
| `unfill_phy_page` | L2 | 物理页侧 unfill 辅助 | 与上层调用关系不直观 | 保持 L2，补清晰调用图 |
| `nexus_kernel_page_owner_cpu` | L2 | 按 rmap 推断 kernel page owner cpu | 无 | 保持 L2 |

---

## 当前最影响可维护性的“混杂点”清单

1. **组合层与后端层交叉太多**  
   典型：`_kernel_get_free_page`/`user_fill_range` 同时做“分配 + map + rmap + 回滚 + range 生命周期”。

2. **节点类型复用导致语义不透明**  
   `nexus_node` 复用 `manage_free_list` / `cache_data`，并靠 `is_page_manage_node()` 判定，跨层函数都在依赖这个细节。

3. **L3 API 与 L2动作边界不稳定**  
   如 `nexus_map_anon_zero_leaf`、`nexus_remap_user_leaf` 既像高层原语，又含大量 leaf 细节。

4. **回滚路径分散且耦合实现细节**  
   flags 批量更新、range commit 失败、kernel path map fail，各自维护回滚，缺少统一事务 helper。

---

## 建议的文件拆分骨架（可按需调整）

- `core/kernel/mm/nexus_pool.c`（L0）
  - manager page / node alloc/free / page-manage-node 判定
- `core/kernel/mm/nexus_tree.c`（L1）
  - vspace registry + range tree CRUD + split/truncate helpers
- `core/kernel/mm/nexus_leaf.c`（L2）
  - leaf map/unmap/remap + rmap link/unlink + unfill_phy_page
- `core/kernel/mm/nexus_ops.c`（L3）
  - reserve/commit/uncommit/release
  - update_range_flags + rollback orchestration
  - query_user_semantics
  - map_anon_zero_leaf / remap_user_leaf (对外原语)
- `core/kernel/mm/nexus_clone.c`（L3）
  - vspace_clone 及 copy/cow 子流程
- `core/kernel/mm/nexus_compat.c`（L3）
  - `get_free_page/free_pages` 旧接口兼容封装

---

## 建议先做的“低风险重构顺序”

1. **先抽 L1（tree CRUD）和 L0（pool）**：不改语义，先搬函数与静态 helper。  
2. **再抽 L3 公开原语与 L2 leaf 动作**：保持 API 不变，减少穿透调用。  
3. **最后处理 clone 与大回滚路径**：这两块最复杂，放最后避免同时引入多类风险。  

---

## 备注

- 本表是“按当前实现状态”给出的映射，不是最终架构定论。  
- 建议你在 review 过程中，把“必须长期稳定的对上层语义接口”先圈出来，再决定内部如何继续分拆。  
- 如果后续引入 file-backed 语义，优先扩展 L3/L1 的语义结构，不要直接把文件细节侵入 L2 leaf 动作。  

---

## 最小稳定接口集（建议先冻结）

下面这组接口建议作为“前端语义面”长期稳定，内部如何拆文件/优化锁/改后端都尽量不影响它们：

- 地址空间区间生命周期
  - `nexus_reserve_user_range`
  - `nexus_commit_user_range`
  - `nexus_uncommit_user_range`
  - `nexus_release_user_range`
- 查询与故障修复原语
  - `nexus_query_user_semantics`
  - `nexus_map_anon_zero_leaf`
  - `nexus_remap_user_leaf`
- 属性变更
  - `nexus_update_range_flags`
- 兼容层保留口（逐步弱化）
  - `get_free_page`
  - `free_pages`

设计原则：

1. 上层只依赖“语义动作”（reserve/commit/...），不直接依赖树结构或页表细节。  
2. 允许 L2/L1 内部替换实现（例如换锁、换索引），只要行为契约不变。  
3. 新后端（如 file-backed）优先通过扩展参数/策略对象注入，不新增平行 API 洪泛。  

---

## 引入 file 后端：是否更方便？

结论：**会更方便，但前提是先把“统一后端能力接口”抽象好**。  
如果直接在现有函数内到处加 `if (is_file_backed)`，会更乱；如果把差异收敛到后端 ops，前端原语几乎不用改。

具体收益：

- `nexus` 继续做“虚拟地址语义真源”，而“页内容来源”由后端决定（anon/file/swap/dev）。  
- page fault 路径从“写死 anon 分配”变成“调用后端 fill/prefault 策略”，扩展新类型更自然。  
- COW、脏页回写、缺页补页等可按后端实现差异化，不污染 L1 树管理逻辑。  

主要成本：

- 需要定义 page 生命周期状态机（not-present/present/dirty/writeback/...）。  
- 需要为 file-backed 建立统一的 page cache / object 引用规则（哪层持有、何时 drop）。  
- 锁顺序与并发模型要先定（vspace lock 与 backend object lock 的顺序）。  

---

## 是否会演变成“统一后端能力 + 前端注入后端”？

建议答案：**是，而且应该主动朝这个方向设计**。  
推荐将 L3 原语固定，把“页来源与提交动作”下沉到可替换后端能力表（ops/vtable）。

### 建议的后端能力面（草案）

```c
struct nexus_backend_ops {
    /* 区间级：建立/销毁后端对象引用（例如 anon_object/file_object） */
    error_t (*range_bind)(struct nexus_range_ctx *rctx);
    error_t (*range_unbind)(struct nexus_range_ctx *rctx);

    /* 页级：缺页时填充一个 leaf（读缺页/执行缺页） */
    error_t (*leaf_populate)(struct nexus_fault_ctx *fctx);

    /* 页级：写故障（COW 或后端自定义写入策略） */
    error_t (*leaf_write_fault)(struct nexus_fault_ctx *fctx);

    /* 页级：回收/回写前回调（可选） */
    error_t (*leaf_evict_prepare)(struct nexus_leaf_ctx *lctx);
};
```

对应的前端调用关系：

1. `reserve`：L1 建 range + `ops->range_bind`  
2. `commit` 或 fault-time populate：L3 驱动 + `ops->leaf_populate`  
3. 写故障：L3 校验共享/权限 + `ops->leaf_write_fault`  
4. `uncommit/release`：L3 迭代 leaf + `ops->leaf_evict_prepare` + L2 unmap + `ops->range_unbind`

这样做后，前端基本不需要知道“这是 anon 还是 file”，只看 range 上的 backend 类型与上下文。

---

## 如何避免“再次侵入式修改 nexus”

1. **先把 backend 类型固化到 range 元数据，而不是散落到 flags 位判断。**  
2. **L3 只做流程编排，不做后端分支细节。**所有后端分歧都在 `backend_ops`。  
3. **L2 保持最小职责：map/unmap/rmap/refcount 一致性。**不要让 L2 认识 file inode/pagecache。  
4. **定义统一 fault 上下文结构体**（访问类型、权限、目标 VA、当前 leaf 状态、backend 私有句柄）。  
5. **把“可回滚事务”抽成公共 helper**，避免每个后端复制回滚代码。  

---

## 演进路线（尽量低风险）

1. 第一步：先引入 `backend_type + backend_private` 到 range 元数据，仅支持 `ANON`。  
2. 第二步：把 `nexus_map_anon_zero_leaf` 内部改为调用 `backend_ops->leaf_populate`（anon 实现不变）。  
3. 第三步：接入 `FILE` 后端最小版本（只读映射 + 读缺页），先不做写回。  
4. 第四步：补 `write_fault` 与 dirty 跟踪，再逐步引入回写策略。  

---

## 对你问题的直接回答

- 这种分层下，引入 file 后端会不会更方便？  
  - **会**，因为前端流程不变，新增的是后端实现而不是全局分支。  
- 会不会演变成统一后端能力？  
  - **会，而且这是最稳的长期路线**。  
- 能否做到“前端只传不同后端，不侵入式改 nexus”？  
  - **可以**，前提是先固定 L3 语义接口与 `backend_ops` 契约，并约束后端差异只能出现在 ops 层。  

