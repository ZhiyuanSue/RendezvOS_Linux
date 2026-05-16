# L4 对外接口（可落地清单）+ Radix 是否还应传 `VSpace*`

目标：**能直接照着写 `.c/.h`**，少抽象词。

---

## 1. L4 应对外收口的接口（建议命名与签名）

**前提**：`registry_root` 指 **`root_vspace`** 这类带 `root_manage_list_head` / `_vspace_rb_root` / `vspace_register_lock` 的对象；**`vs`** 指**被注册的用户表类地址空间**（`vspace_root_addr` 为 TTBR0 根物理页）。

```c
/* 在 registry_root 的 RB 里按 vspace_root_addr 查找；未找到返回 NULL。
 * 持 registry_root->vspace_register_lock 读临界（或与写同锁）。 */
VSpace *vspace_registry_lookup(VSpace *registry_root, paddr vspace_root_paddr);

/* 查重；成功则 RB insert + radix init（及策略上的 install 高半等）。
 * 内部：lock_cas(&registry_root->vspace_register_lock); …; unlock_cas。
 * handler：当前 CPU 的 map_handler（radix init / grow 要 map 元数据页）。 */
error_t vspace_registry_register(VSpace *registry_root,
                                 struct map_handler *handler,
                                 VSpace *vs);

/* radix_destroy + RB remove +（若 vs 槽来自池）free_vs_entry(registry_root, vs)。
 * del_vspace 路径在 tear PT 之前应调用。 */
void vspace_registry_unregister(VSpace *registry_root,
                                struct map_handler *handler,
                                VSpace *vs);
```

**可选**（有嵌入槽位池时）：

```c
/* 从 registry_root 管理页池拿一截「表类 VSpace」存储，并完成 register 前半段；
 * 若你始终 kmalloc new_vspace_structure，可不导出。 */
VSpace *vspace_registry_alloc_table_vs(VSpace *registry_root,
                                       struct map_handler *handler);
```

**线程安全**：所有触及 RB + `root_manage_list_head` + 池链表的，**已在 `vspace_register_lock` 下**；**不要**在持该锁时做长时间阻塞或再去拿无关 `vs` 的 `vspace_lock`（与 `get_free_vs_entry` 里 `map(root)` 的全序见 INVARIANTS）。

---

## 2. Radix：传 `VSpace*` 到底在干什么？要不要改成只传 `pmm + handler`？

### 2.1 现在 `vs` 在 radix 里实际承担的角色（从代码路径归纳）

| 从 `vs` 取用的东西 | 用途 |
|--------------------|------|
| `vs->root_radix` | L0 根指针 |
| `vs->pmm` | `pmm_alloc` / `free_level_table`、`radix_leaf_*_rmap` 用 `vs->pmm->zone` |
| 传给 **`map(vs, …)`** / **`unmap(vs, …)`** | radix **元数据页**、以及 **用户叶 PTE** 都挂在 **这份页表根** 上 |

所以：**不是「radix 逻辑依赖 VSpace 抽象」**，而是 **一次调用里需要三个具体能力：树根、PMM/zone、页表句柄**。`VSpace*` 只是这三者的**打包**。

### 2.2 你说的「层级耦合」指什么？

- **类型/头文件耦合**：`vmm_radix_tree.h` `#include "vmm.h"` 为了 `VSpace` —— 若改成前向声明 + 不透明 `typedef struct VSpace VSpace` 或 **`typedef struct RadixPtHost RadixPtHost`**，可减弱。  
- **生命周期耦合**：`vmm_radix_tree_init` 写 `vs->root_radix` —— **存储位置**在谁身上是数据模型问题；可以改成 **init 只返回 `Radix_entry_t*`，由 L4 赋值 `vs->root_radix`**，radix 源文件里**不写** `vs->root_radix` 字段，耦合再减一档。  
- **真·循环依赖**（radix 调 VSpace 方法、VSpace 又 inline 调 radix）：当前 **没有**；只有 **L4 先 register/init radix 再给别人用** 的**顺序**问题，用文档 + 初始化顺序解决即可。

### 2.3 是否应该改成「只依赖 pmm + map_handler」？

**不够。** 少了 **「哪棵树的根」**（`Radix_entry_t*`）和 **「PTE 写到哪套页表」**（`map` 的第一个参数）。`map_handler` 不带页表根地址；页表根在 **`VSpace::vspace_root_addr`**（或未来抽象成 `struct page_table*`）。

**推荐折中（可落地、解耦感最好）**：引入显式 **绑定结构**，把「从 `vs` 拆出来的三个指针」当参数传，**API 层不再暗示 radix 拥有 VSpace**：

```c
/* 仅数据：radix 算法只认这三样 + handler */
typedef struct RadixMapCtx {
        Radix_entry_t *root;   /* 当前树根；NULL 则非法 */
        struct pmm *pmm;
        VSpace *pt_vs;         /* 仅作 map/unmap/have_mapped 的第一个参数：页表宿主 */
} RadixMapCtx;

/* 从现有 VSpace 填 ctx（一行内联或 vmm 侧 helper） */
static inline void radix_ctx_from_vs(RadixMapCtx *out, VSpace *vs)
{
        out->root = vs->root_radix;
        out->pmm = vs->pmm;
        out->pt_vs = vs;
}
```

然后把 **对外 API** 改成例如：

```c
error_t vmm_radix_insert_range(struct map_handler *handler,
                               const RadixMapCtx *ctx,
                               vaddr page_vaddr, ENTRY_FLAGS_t flags,
                               size_t page_number);
```

**语义**：`ctx->root` 必须已由上层分配/赋值；`pt_vs` 仅用于 **`map`/`unmap`** 与 **zone 与 `vs->pmm` 一致** 的约束（通常 `pmm` 与 `pt_vs->pmm` 同指针）。

**高半 `vs_ptr` 仍写 `&root_vspace`**：可逐步改成 **`VSpace *reservation_owner`** 显式参数（insert 时传入），彻底去掉 radix 对全局 `root_vspace` 符号依赖（第二迭代）。

### 2.4 迁移成本（实话）

- **改签名**：所有 `vmm_radix_tree_*` 调用点要换；`handler` 仍在。  
- **收益**：头文件可不再 `#include "vmm.h"` 全量（若 `VSpace` 仅作不透明 `pt_vs`）；**心智上**「radix = root + pmm + 页表宿主」一眼清晰。  
- **若暂不改**：在头文件注释里写清 **「`vs` 仅提供 root_radix / pmm / map 目标三件套」**，也不算架构错误。

---

## 3. 和 L4 的衔接（谁赋 `root_radix`）

| 做法 | 说明 |
|------|------|
| **A** `vmm_radix_tree_init(handler, vs)` 内部写 `vs->root_radix` | 最少改动；L4 `register` 里调一次即可。 |
| **B** `init` 返回 `Radix_entry_t*`，L4 `vs->root_radix = ret` | radix `.c` 不碰 `VSpace` 布局，耦合更低。 |

推荐 **B + RadixMapCtx** 作为第二阶段的 refactor 目标。

---

## 4. 一页纸：你要落地的顺序

1. 在 **`vmm.h` / 新 `vmm_registry.h`** 声明 **`vspace_registry_lookup/register/unregister`**（上 §1）。  
2. **`thread_loader` / `del_vspace`** 从 nexus 改调上述三函数 + 现有 `new_vs_root`。  
3. （可选第二阶段）引入 **`RadixMapCtx`**，改 `vmm_radix_tree_*` 签名；`radix_ctx_from_vs` 过渡。
