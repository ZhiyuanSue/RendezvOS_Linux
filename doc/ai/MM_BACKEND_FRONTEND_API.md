# MM 后端 / 前端 API 草约（同一 `VSpace` 内；与 L4 注册正交）

本文落实两件事：

1. **承认 L5 同时依赖 PMM、`map_handler`、radix** — 编排就是「申请/映射物理页 + radix 记录区间状态」的合成。  
2. **后端（匿名 / 文件）**只在**已绑定**的 `VSpace* + map_handler*` 上表达**页来源与释放**；**不**碰全局 vspace 注册（L4）。

以下为 **C 形态 API 草约**（名字可改；语义是约束核心）。

---

## 1. 设计是否合理？

**合理。** 理由：

- **L4（注册）** 是「地址空间对象在**全系统**的身份」；**后端** 是「**该对象地址空间内**某段映射的**内容策略**」— 分离后 nexus 里那种「又注册又当 VMA 后端」的泥团可以拆开。  
- **统一前端**：syscall / fault 只面对 `MMRegion` / `mm_region_*`，内部换 `MMBackend` 即可换匿名/文件子行为。  
- **注意点**：后端 **`provide_page` 内禁止**调用 `map` / `vmm_radix_tree_*` / 持 `vspace_lock`，否则与 L5 的锁序易绞死；**只允许** L1（以及文件自己的 cache 锁）等后端私有资源。

---

## 2. 后端接口（匿名与文件**共用**的 ops 表）

后端实例用不透明对象表示；**唯一多态入口**是 `const MMBackendOps *`。

```c
#include <rendezvos/error.h>
#include <rendezvos/mm/vmm.h>
#include <rendezvos/mm/map_handler.h>
#include <common/mm.h>

typedef struct MMBackend MMBackend;

/*
 * 单次 fault / demand-fill：给出「这一 4KiB 页」的物理 backing 与
 * 供 leaf_bind_range 使用的 ENTRY_FLAGS_t 词汇（须与后续 insert_range 一致）。
 * 不得调用 map / radix / vspace_register。
 */
typedef struct MMBackendPage {
        ppn_t ppn;
        ENTRY_FLAGS_t leaf_flags; /* sw bits stripped by L5 if needed */
} MMBackendPage;

typedef struct MMBackendOps {
        /* 区域销毁（munmap 全撤、或 close fd）：释放 ctx，不碰 radix/map */
        void (*destroy)(MMBackend *self);

        /*
         * 为 page_va（须 PAGE_SIZE 对齐且落在本 backend 绑定的 [base,base+len)）提供 backing。
         * write==true 表示写 fault：匿名可能 COW 分裂；文件可能读入可写页。
         * 失败：返回负 error_t，L5 不做 map/bind。
         */
        error_t (*provide_page)(MMBackend *self, vaddr page_va, bool write,
                                MMBackendPage *out);

        /*
         * L5 已完成 leaf_unbind（或等价）且已 unmap PTE 后调用：
         * 释放该范围内后端持有的物理页引用 / page cache / 文件页锁等。
         * 不得调用 radix；不得 map。
         */
        void (*release_pages)(MMBackend *self, vaddr start, size_t len_bytes);

        /*
         * 可选：MAP_POPULATE / readahead。L5 可在 insert_range 之后循环调用。
         * 默认 NULL = 仅按需 fault。
         */
        error_t (*populate_range)(MMBackend *self, vaddr start, size_t len_bytes);

} MMBackendOps;

struct MMBackend {
        const MMBackendOps *ops;
        /* anon: 可能无额外字段；file: inode+offset、page cache 句柄等 */
};
```

### 2.1 匿名后端子行为（实现同一 ops）

| 调用 | 典型行为 |
|------|----------|
| `provide_page` | `pmm_alloc` 零页或 COW 源；`leaf_flags` 带 LAZY/VALID/WRITE 等策略位 |
| `release_pages` | `pmm_free` 或仅减 ref（共享零页时） |
| `populate_range` | 循环 `provide_page` 或批量 `pmm_alloc`（由策略定） |

### 2.2 文件后端子行为

| 调用 | 典型行为 |
|------|----------|
| `provide_page` | 按 `page_va` 算 file offset → page cache `read_folio` / 预读；可能只读 `leaf_flags` |
| `release_pages` | `folio_put` / 解 mapping 与 cache 的 pin |
| `populate_range` | `readahead` 或同步读整段 |

---

## 3. 前端接口（L5：对 syscall / fault 的统一面）

前端持有一个 **「区域」** 对象：绑定 **`VSpace*` + `map_handler*` + VA 区间 + `MMBackend*`**。**radix 的 insert/bind/unbind 顺序只在这里出现**。

```c
typedef struct MMRegion MMRegion;

/*
 * 创建逻辑区域：不注册全局 vspace。
 * - 典型 mmap：va_base,len 已对齐策略；backend 已构造好。
 * - lazy_flags：传给 vmm_radix_tree_insert_range 的 flags（含 PAGE_ENTRY_LAZY 等）。
 * 成功：radix 上已有 LAZY  reservation；PTE 尚未装或按 eager 策略由 populate 决定。
 */
error_t mm_region_create(MMRegion **out,
                           VSpace *vs,
                           struct map_handler *handler,
                           vaddr va_base,
                           size_t len_bytes,
                           ENTRY_FLAGS_t lazy_flags,
                           MMBackend *backend);

/* fault 入口：arch / syscall 间接调用 */
error_t mm_region_fault(MMRegion *reg, vaddr va, bool write);

/* 与 munmap 对齐：先 radix unbind + unmap，再 backend release */
error_t mm_region_unmap(MMRegion *reg, vaddr start, size_t len_bytes);

void mm_region_destroy(MMRegion *reg); /* 整段去掉：按 unbind→unmap→delete_range + backend destroy */
```

### 3.1 `mm_region_fault` 内部编排（成功路径，与 `vmm_radix_tree.h` 一致）

1. `vmm_radix_tree_query_range`（可选：判断是否已 VALID）  
2. 若尚未 LAZY reservation：`vmm_radix_tree_insert_range(handler, vs, …)`（仅当策略允许 fault  grow；否则 L7 已在 `create` 里 insert 满）  
3. **`backend->ops->provide_page`** → `ppn` + `leaf_flags`  
4. **`map(vs, ppn, vpn, 3, pte_flags_from(leaf_flags), handler)`**  
5. **`vmm_radix_tree_leaf_bind_range(handler, vs, page_va, ppn, 1, leaf_flags)`**  
6. 失败：按 radix 头文件 rollback（bind 失败则 unmap + lazy 恢复）+ **不**调用 `release_pages` 除非已明确提交过 backing 策略（由实现定）

### 3.2 `mm_region_unmap` 编排

1. `vmm_radix_tree_leaf_unbind_range`（若当前为 VALID）  
2. `unmap(vs, vpn, …)` 每页  
3. `vmm_radix_tree_delete_range`（仅 radix 元数据 / reservation；**不**替代 unbind/unmap）  
4. **`backend->ops->release_pages`**

### 3.3 `mm_region_create` 编排（lazy 整段先占 radix）

1. `vmm_radix_tree_insert_range(handler, vs, va_base, lazy_flags, n_pages)`  
2. 若 `backend->ops->populate_range` 非 NULL 且 syscall 要求 populate → 循环 `provide_page` 或调用 `populate_range`（内部仍应遵守 insert→map→bind）

---

## 4. 与 L4 的衔接（lookup 不进后端）

| 场景 | 谁调什么 |
|------|----------|
| fault 在**不知 `vs`** 时 | **L4** `vspace_lookup_root(registry, cr3_paddr)` → `vs`，再 `mm_region_fault(reg,…)`（`reg` 内已含该 `vs`） |
| 建进程 | **L7** → **L4** `register`；**mmap** → **L5** `mm_region_create` 选不同 `MMBackend` |

---

## 5. 小结

| 层 | 回答的问题 |
|----|------------|
| **L4** | 「这个 CR3 / root_paddr **是**哪个 `VSpace*`？」 |
| **L6 后端** | 「**这个** `VSpace` 里、**这段** region、**这一页** backing 从哪来 / 怎么释放？」 |
| **L5 前端** | 「在同一 `vs` 上如何把 **L6 的答案** 落成 **L3+L2+L1** 且可回滚？」 |

这样 **图与职责** 与你说的一致：**L5 依赖 map_handler + PMM + radix；编排就是三者 + 可选后端**；**后端不碰注册**，只服务**同一地址空间内的申请/映射语义与释放**。
