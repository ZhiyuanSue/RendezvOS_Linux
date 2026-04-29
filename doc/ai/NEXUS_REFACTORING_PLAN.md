# Nexus重构计划（修正版）

## 架构理解修正

### 两个后端（不是三个）

基于重新理解，实际上是**两个后端**：

#### Backend-A: Nexus元数据后端（L0 + L1）
- **职责**: 管理虚拟地址区间的语义和元数据
- **数据结构**: 红黑树（区间）+ 节点池（nexus_node）
- **物理页使用**: L0内部使用PMM分配物理页存储nexus_node元数据
- **不关心**: 页表如何映射、物理页如何分配给用户

#### Backend-B: 页表/物理页适配层（L2）
- **职责**: 封装页表操作和物理页分配的复杂交互
- **封装内容**:
  - map_handler的map/unmap/have_mapped
  - PMM的pmm_alloc/pmm_free/pmm_change_pages_ref
  - rmap的link/unlink
- **关键**: 这一层把Backend-B和PMM的交互封装起来，给L3提供简单接口

### 层级定义（修正）

```
┌─────────────────────────────────────────┐
│  L4: Linux层策略                         │
│  fork, mmap, mprotect, brk, page fault  │
└──────────────┬──────────────────────────┘
               │
┌──────────────▼──────────────────────────┐
│  L3: Nexus前端原语层                     │
│  reserve/commit/uncommit/release         │
│  update_range_flags, remap_leaf         │
│  clone, query                            │
└──────┬─────────────┬────────────────────┘
       │             │
       │    ┌────────▼────────┐
       │    │ L1: 区间树      │
       │    │ (RB tree)       │
       │    │ insert/remove   │
       │    │ search/iterate  │
       │    └────────┬────────┘
       │             │
       │    ┌────────▼────────┐
       └────►│ L0: 节点池      │
            │ (Manager Pages) │
            │ alloc/free      │
            │ (内部用PMM)      │
            └─────────────────┘

       ┌──────────────────────────────┐
       │ L2: 页表/物理页适配层          │
       │ 封装：                        │
       │ - map_handler (map/unmap)    │
       │ - PMM (alloc/free/ref)       │
       │ - rmap (link/unlink)         │
       │ 提供接口：                     │
       │ - alloc_and_map              │
       │ - unmap_and_free             │
       │ - remap_leaf                │
       └──────────────────────────────┘
```

---

## 重构计划（三轮）

### 第一轮：建立清晰的层间接口

**目标**: 消除跨层直接调用，定义接口契约

#### 1.1 定义L0接口（节点池）

```c
// L0提供给L1/L2的接口
struct nexus_node* nexus_alloc_node(struct nexus_node* vspace_root);
void nexus_free_node(struct nexus_node* node, struct nexus_node* vspace_root);

// L0内部使用（不暴露）
static ppn_t nexus_alloc_manage_page(struct pmm* pmm);
static void nexus_free_manage_page(ppn_t ppn, struct pmm* pmm);
```

**重构动作**:
- `nexus_get_free_entry` → `nexus_alloc_node`（改名，明确语义）
- `nexus_free_entry` → `nexus_free_node`（改名）
- `free_manage_node_with_page` → `nexus_free_manage_page`（内部化）

#### 1.2 定义L1接口（区间树）

```c
// L1提供给L3的接口
error_t nexus_range_insert(
    struct nexus_node* vspace_node,
    vaddr start,
    vaddr end,
    ENTRY_FLAGS_t flags,
    struct nexus_node** first_node_out);

error_t nexus_range_remove(
    struct nexus_node* vspace_node,
    vaddr start,
    vaddr end);

struct nexus_node* nexus_range_find(
    struct nexus_node* vspace_node,
    vaddr addr);

struct nexus_node* nexus_range_next(struct nexus_node* node);
struct nexus_node* nexus_range_prev(struct nexus_node* node);
```

**重构动作**:
- 提取红黑树操作为纯函数
- `_take_range` 拆分为：调用L0分配节点 + 调用L1插入树
- `insert_nexus_entry` 和 `delete_nexus_entry` 合并到L1接口

#### 1.3 定义L2接口（页表/物理页适配）

```c
// L2提供给L3的接口
error_t nexus_pt_alloc_and_map(
    VS_Common* vs,
    vaddr va,
    ENTRY_FLAGS_t flags,
    struct nexus_node* nexus_node);

error_t nexus_pt_unmap_and_free(
    VS_Common* vs,
    vaddr va,
    struct nexus_node* nexus_node);

error_t nexus_pt_remap_leaf(
    VS_Common* vs,
    vaddr va,
    ppn_t new_ppn,
    ENTRY_FLAGS_t new_flags,
    struct nexus_node* nexus_node);

error_t nexus_pt_query_leaf(
    VS_Common* vs,
    vaddr va,
    ppn_t* ppn_out,
    ENTRY_FLAGS_t* flags_out,
    int* level_out);
```

**重构动作**:
- 提取`map + pmm_alloc + rmap_link`的组合为`nexus_pt_alloc_and_map`
- 提取`unmap + pmm_free + rmap_unlink`的组合为`nexus_pt_unmap_and_free`
- `nexus_update_node`拆分为：页表操作（L2）和事务逻辑（L3）

#### 1.4 更新L3接口（保持稳定）

```c
// L3给Linux层的接口（保持不变，内部实现改变）
error_t nexus_reserve_user_range(...);
error_t nexus_commit_user_range(...);
error_t nexus_uncommit_user_range(...);
error_t nexus_release_user_range(...);
error_t nexus_update_range_flags(...);
error_t nexus_remap_user_leaf(...);
error_t nexus_query_vaddr(...);
error_t vspace_clone(...);
```

---

### 第二轮：重构函数实现

**目标**: 每个函数<100行，单一职责，使用第一轮定义的接口

#### 2.1 重构复杂函数（按优先级）

**优先级1: `get_free_page` / `free_pages`**
```c
void* get_free_page(size_t page_num, vaddr target_vaddr,
                    struct nexus_node* nexus_root, VS_Common* vs,
                    ENTRY_FLAGS_t flags) {
    if (内核) {
        return _kernel_get_free_page(page_num, nexus_root);
    } else {
        // L3: 只调用L1和L2接口
        struct nexus_node* first_node;
        vaddr addr;
        error_t e = nexus_range_insert(vs, target_vaddr,
                                      target_vaddr + page_num * PAGE_SIZE,
                                      flags, &first_node);
        if (e) return NULL;

        // L2: 分配物理页并映射
        struct nexus_node* node = first_node;
        for (int i = 0; i < page_num; i++) {
            e = nexus_pt_alloc_and_map(vs, node->addr, flags, node);
            if (e) {
                // 回滚
                nexus_pt_unmap_and_free(vs, node->addr, node);
                nexus_range_remove(vs, addr, addr + i * PAGE_SIZE);
                return NULL;
            }
            node = nexus_range_next(node);
        }

        return (void*)addr;
    }
}
```

**优先级2: `nexus_update_range_flags`**
```c
error_t nexus_update_range_flags(...) {
    // Phase 1: 收集节点（L1）
    struct list_entry update_list;
    INIT_LIST_HEAD(&update_list);

    for (vaddr cur = start_addr; cur < end_addr;) {
        struct nexus_node* node = nexus_range_find(vs, cur);
        // ... 添加到update_list
        cur = node->addr + nexus_node_get_len(node);
    }

    // Phase 2: 批量更新（L2）
    error_t e = nexus_pt_batch_update(vs, &update_list, mode, set_mask, clear_mask);

    if (e) {
        // Phase 3: 回滚（L2）
        nexus_pt_batch_rollback(vs, &update_list);
    }

    return e;
}
```

**优先级3: `vspace_clone`**
```c
error_t vspace_clone(...) {
    // Step 1: 创建基础设施（L1）
    error_t e = vspace_clone_create_infrastructure(
                    src_vs, &dst_vs, &dst_nexus, nexus_root);
    if (e) goto fail;

    // Step 2: 根据flag选择策略
    if (flags & VSPACE_CLONE_F_COPY_PAGES) {
        e = vspace_clone_copy_mappings(src_vs, dst_vs, dst_nexus);
    } else {
        e = vspace_clone_setup_cow(src_vs, dst_vs, dst_nexus);
    }

    if (e) goto fail_cleanup;
    return REND_SUCCESS;

fail_cleanup:
    nexus_delete_vspace(nexus_root, dst_vs);
fail:
    return e;
}
```

#### 2.2 提取L2实现

创建`nexus_pt.c`，实现L2接口：

```c
// L2: alloc_and_map实现
error_t nexus_pt_alloc_and_map(VS_Common* vs, vaddr va,
                               ENTRY_FLAGS_t flags,
                               struct nexus_node* nexus_node) {
    struct pmm* pmm = vs->pmm;
    struct map_handler* handler = &percpu(Map_Handler);

    // 1. 分配物理页
    size_t alloced;
    ppn_t ppn = pmm->pmm_alloc(pmm, 1, &alloced);
    if (invalid_ppn(ppn) || alloced != 1)
        return -E_RENDEZVOS;

    // 2. 映射页表
    error_t e = map(vs, ppn, VPN(va), 3, flags, handler);
    if (e != REND_SUCCESS) {
        pmm->pmm_free(pmm, ppn, 1);
        return e;
    }

    // 3. 维护rmap
    link_rmap_list(pmm->zone, ppn, nexus_node);

    return REND_SUCCESS;
}

// L2: unmap_and_free实现
error_t nexus_pt_unmap_and_free(VS_Common* vs, vaddr va,
                               struct nexus_node* nexus_node) {
    struct pmm* pmm = vs->pmm;
    struct map_handler* handler = &percpu(Map_Handler);

    // 1. 解映射页表
    ppn_t ppn = unmap(vs, VPN(va), 0, handler);
    if (invalid_ppn(ppn))
        return -E_RENDEZVOS;

    // 2. 维护rmap
    unlink_rmap_list(pmm->zone, ppn, nexus_node);

    // 3. 释放物理页
    return pmm->pmm_free(pmm, ppn, 1);
}
```

---

### 第三轮：文件拆分

**目标**: 每个文件<500行，清晰的模块边界

#### 文件组织

```
core/kernel/mm/nexus/
├── nexus.h                    # 公共接口（L3）
├── nexus.c                    # L3主要实现
├── nexus_priv.h              # 内部接口（L0/L1/L2）
├── nexus_allocator.c         # L0: 节点池
├── nexus_range.c             # L1: 区间树
├── nexus_pt.c                # L2: 页表/物理页适配
└── README.md                 # 分层说明
```

#### nexus.h（L3接口）
```c
// 给Linux层的公共接口
void* get_free_page(...);
error_t free_pages(...);
error_t nexus_reserve_user_range(...);
error_t nexus_commit_user_range(...);
error_t nexus_update_range_flags(...);
error_t nexus_remap_user_leaf(...);
error_t vspace_clone(...);
error_t nexus_query_vaddr(...);
// ...
```

#### nexus_priv.h（内部接口）
```c
// L0接口（给L1/L2使用）
struct nexus_node* nexus_alloc_node(struct nexus_node* vspace_root);
void nexus_free_node(struct nexus_node* node, struct nexus_node* vspace_root);

// L1接口（给L3使用）
error_t nexus_range_insert(...);
error_t nexus_range_remove(...);
struct nexus_node* nexus_range_find(...);

// L2接口（给L3使用）
error_t nexus_pt_alloc_and_map(...);
error_t nexus_pt_unmap_and_free(...);
error_t nexus_pt_remap_leaf(...);
```

#### nexus_allocator.c（L0）
```c
// 实现：
// - nexus_alloc_node
// - nexus_free_node
// - nexus_init_manage_page
// - is_page_manage_node
// 行数：约300行
```

#### nexus_range.c（L1）
```c
// 实现：
// - 红黑树操作（insert/remove/search/next/prev）
// - nexus_range_insert/remove/find
// - _take_range（拆分后）
// 行数：约400行
```

#### nexus_pt.c（L2）
```c
// 实现：
// - nexus_pt_alloc_and_map
// - nexus_pt_unmap_and_free
// - nexus_pt_remap_leaf
// - nexus_pt_query_leaf
// - link_rmap_list/unlink_rmap_list
// - unfill_phy_page/nexus_kernel_page_owner_cpu
// 行数：约400行
```

#### nexus.c（L3）
```c
// 实现：
// - get_free_page/free_pages
// - nexus_reserve/commit/uncommit/release
// - nexus_update_range_flags
// - nexus_remap_user_leaf
// - vspace_clone
// - nexus_query_vaddr/nexus_query_user_semantics
// - nexus_create/delete/migrate_vspace
// 行数：约600行
```

---

## 验证和测试

### 每轮完成后的验证

**第一轮验证**:
- [ ] 所有接口定义完成
- [ ] 接口语义文档化
- [ ] 现有函数能映射到新接口

**第二轮验证**:
- [ ] 所有函数<100行
- [ ] 没有跨层直接调用（L3不直接调用PMM/map_handler）
- [ ] 所有测试通过
- [ ] 性能无明显退化

**第三轮验证**:
- [ ] 每个文件<500行
- [ ] 模块边界清晰
- [ ] 头文件依赖清晰
- [ ] 所有测试通过

### 测试策略

1. **单元测试**: 为L0/L1/L2编写独立测试
2. **集成测试**: 确保Linux层所有功能正常
3. **性能测试**: 确保没有明显退化
4. **压力测试**: 多核并发、大量映射

---

## 时间估计

- **第一轮**: 定义接口和契约（2-3天）
- **第二轮**: 重构函数实现（1-2周）
- **第三轮**: 文件拆分（3-5天）

**总计**: 约3-4周

---

## 风险和缓解

### 风险1: 接口设计不完善
**缓解**: 第一轮充分review，确保接口覆盖所有使用场景

### 风险2: 重构引入bug
**缓解**: 每个小步骤都运行测试，使用git bisect快速定位

### 风险3: 性能退化
**缓解**: 性能基准测试，关键路径优化

### 风险4: 时间超期
**缓解**: 按优先级分阶段，核心功能优先
