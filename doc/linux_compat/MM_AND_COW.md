# 内存：Radix Tree 作为虚存真源 + COW + 页故障

**Core API / 锁序 / 调用流程：** [`core/docs/USING_CORE.md`](../../core/docs/USING_CORE.md)、[`core/docs/memory.md`](../../core/docs/memory.md) §0–§0.7（本文件只写 **Linux 侧策略与实现状态**）。

> **📅 阶段状态**：
> - ✅ **Phase 1 完成**：brk, mmap, munmap, mprotect, mremap, COW机制, fork地址空间复制
> - 📋 **后续阶段**：文件mmap, 更复杂的mremap场景

## 1. 为何不需要独立 VMA 子系统

Linux **VMA** 解决的是：**按用户 VA 区间** 记录映射属性，并支持 **`munmap`/`mprotect` 的部分区间**（分裂、合并）。RendezvOS **Radix Tree** 已在每个用户 `VSpace` 下维护用户 VA 与物理页的映射关系；**同一语义**可在 Radix_node_t 上扩展元数据（flags、`MAP_PRIVATE`/`SHARED`、COW 共享关系），无需第二棵并行树。若日后需要 **按文件 offset 反查** 等索引，再增加辅助结构。

## 2. Radix Tree 需承载的 Linux 侧信息（实现时逐项补）

- **区间**：Radix tree 天然支持按 4K/2M 粒度查询，通过 `vmm_radix_tree_find_first_occupied_interval` 查找连续区间。
- **保护**：与 `Radix_node_t::flags` / PTE 属性对齐；`mprotect` 时通过 `mm_user_utils_set_range_flags` 批量更新。
- **匿名 vs 文件**：初期仅匿名；文件 `mmap` 预留后端接口（见 `MM_BACKEND_FRONTEND_API.md`）。
- **COW**：`MAP_PRIVATE` + fork 后 **多 vspace 共享只读物理页**；写故障时 **分裂物理页** 并更新 radix/rmap（与现有 `Page` refcount 衔接）。

## 3. `brk`

> **状态**: ✅ Phase 1 完成

- **语义**：调整 **堆顶**；失败返回当前 brk。
- **实现要点**：`linux_proc_append` 中 `start_brk`/`brk`；从 `brk_old` 到 `brk_new` 用 `mm_user_utils_set_range_and_fill` / `mm_user_utils_clean_range_and_unfill`，**堆范围**与 ELF program header 对齐（loader 已有信息可传入 append 初始化）。
- **实现文件**：`linux_layer/mm/sys_brk.c`
- **测试验证**: ✅ TEST 03/04, 04/04 PASS
- **锁**：按 core 既有顺序：L0 big lock → L2 band lock → PMM zone lock（rmap 操作）。

## 4. `mmap` / `munmap` / `mprotect` / `mremap`

> **状态**: ✅ Phase 1 完成基础匿名映射

| Syscall | 状态 | Radix Tree 侧工作 | 实现文件 |
|---------|------|------------------|----------|
| `mmap` | ✅ 完成 | 通过 `mm_user_utils_set_range_and_fill` 分配物理页、预留 radix 区间、映射页表、绑定叶子 | `linux_layer/mm/sys_mmap.c` |
| `munmap` | ✅ 完成 | 通过 `mm_user_utils_clean_range_and_unfill` 解绑、解映射、删除 radix 区间、释放物理页 | `linux_layer/mm/sys_munmap.c` |
| `mprotect` | ✅ 完成 | 通过 `mm_user_utils_set_range_flags` 批量更新 PTE 和 radix flags | `linux_layer/mm/sys_mprotect.c` |
| `mremap` | ✅ 完成 | 通过 `mm_user_utils_remap_page` 重映射单页 | `linux_layer/mm/sys_mremap.c` |
| 文件mmap | 📋 后续阶段 | 需要VFS支持 | - |

**Map_Handler**：所有 **页表遍历** 使用 **`&percpu(Map_Handler)`**（当前 CPU），见 `doc/ai/INVARIANTS.md`。

## 5. COW 与页故障

> **状态**: ✅ Phase 1 完成（采用阶段A：trap路径内联）

- **目标**：fork 后子父共享 **只读** 用户页；写访问触发 **page fault**。
- **实现方式**：采用**阶段 A** - 在 [`core`](../../core) trap 路径中内联 **COW 解析**（最小闭环）。
- **实现文件**：
  - `linux_layer/mm/linux_page_fault_irq.c` - COW页故障处理
  - `linux_layer/mm/linux_vspace.c` - 地址空间复制和COW分裂
- **测试验证**: ✅ fork + 多进程测试通过，COW机制工作正常
- **后续阶段**：阶段 B（trap **upcall** 到 **`cow_fault_handler` server**）暂不需要

## 6. core 变更清单（Phase 1 完成情况）

在 **不** 把 Linux 头文件引入 `core` 的前提下，Phase 1 已完成：

- [x] **导出 Radix Tree API**：`vmm_radix_tree_*` 系列，提供 INSERT/DELETE/QUERY_OR_CHANGE 三种语义。
- [x] **导出用户编排层**：`mm_user_utils_*` 系列，提供完整的"分配→映射→绑定"和"解绑→解映射→释放"流程。
- [x] **导出 vspace 复制 API**：`linux_copy_vspace()` 等，支持 fork 地址空间复制。
- [x] **page fault 入口**：可注册回调（弱符号），由 linux_layer 注册 COW 逻辑。
- [x] **两层锁机制**：L0 big lock + L2 per-band lock，多核扩展性优于单一 vspace-wide 锁。
- [x] **refcount**：fork 共享只读 PTE 时与 `Page` / buddy 一致性已审计。

**后续阶段可能需要**：
- [ ] 文件mmap相关的radix tree扩展
- [ ] 更复杂的mremap场景支持

具体符号名在实现 PR 中补齐，并同步 [`doc/ai/INVARIANTS.md`](../ai/INVARIANTS.md)。

## 7. fork 与 vspace

> **状态**: ✅ Phase 1 完成

- **子进程**：新 `Tcb_Base` + 新 `VSpace` + **复制或共享** L0…L3 策略（COW 通常为 **共享只读 + 引用计数**）。
- **实现文件**：
  - `linux_layer/proc/sys_fork.c` - fork系统调用实现
  - `linux_layer/mm/linux_vspace.c` - `linux_copy_vspace()` 地址空间COW复制
- **测试验证**: ✅ TEST 09/11 PASS, 用户态多进程测试通过
- **执行 CPU**：fork 在父进程 CPU 上调用，COW 复制已 SMP 安全
