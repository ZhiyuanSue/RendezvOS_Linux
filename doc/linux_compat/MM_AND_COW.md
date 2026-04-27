# 内存：nexus 作为虚存真源 + COW + 页故障

> **📅 阶段状态**：
> - ✅ **Phase 1 完成**：brk, mmap, munmap, mprotect, mremap, COW机制, fork地址空间复制
> - 📋 **后续阶段**：文件mmap, 更复杂的mremap场景

## 1. 为何不需要独立 VMA 子系统

Linux **VMA** 解决的是：**按用户 VA 区间** 记录映射属性，并支持 **`munmap`/`mprotect` 的部分区间**（分裂、合并）。RendezvOS **nexus** 已在每个用户 `VS_Common` 下维护用户 VA 与物理页/nexus 节点的关系；**同一语义**可在 nexus 节点上扩展元数据（prot、`MAP_PRIVATE`/`SHARED`、COW 共享关系），无需第二棵并行树。若日后需要 **按文件 offset 反查** 等索引，再增加辅助结构。

## 2. nexus 需承载的 Linux 侧信息（实现时逐项补）

- **区间**：与现有 `nexus_node::addr`、长度 API 一致。
- **保护**：与 `region_flags` / PTE 属性对齐；`mprotect` 时 **分裂边界** 后改写。
- **匿名 vs 文件**：初期仅匿名；文件 `mmap` 预留 `file`/`offset` 字段或外部表。
- **COW**：`MAP_PRIVATE` + fork 后 **多 vspace 共享只读物理页**；写故障时 **分裂物理页** 并更新 nexus/rmap（与现有 `Page` refcount 衔接）。

## 3. `brk`

> **状态**: ✅ Phase 1 完成

- **语义**：调整 **堆顶**；失败返回当前 brk。
- **实现要点**：`linux_proc_append` 中 `start_brk`/`brk`；从 `brk_old` 到 `brk_new` 用 `get_free_page` / `free_pages` + `map`/`unmap`，**堆范围**与 ELF program header 对齐（loader 已有信息可传入 append 初始化）。
- **实现文件**：`linux_layer/mm/sys_brk.c`
- **测试验证**: ✅ TEST 03/04, 04/04 PASS
- **锁**：`vs->vspace_lock` + `nexus_vspace_lock` 按 core 既有顺序。

## 4. `mmap` / `munmap` / `mprotect` / `mremap`

> **状态**: ✅ Phase 1 完成基础匿名映射

| Syscall | 状态 | nexus 侧工作 | 实现文件 |
|---------|------|----------------|----------|
| `mmap` | ✅ 完成 | 分配/插入区间；匿名页 `pmm_alloc` + `map`；处理 `MAP_FIXED` 与重叠策略 | `linux_layer/mm/sys_mmap.c` |
| `munmap` | ✅ 完成 | **部分删除** → 分裂或截断 nexus 节点；`unmap` + `free_pages` | `linux_layer/mm/sys_munmap.c` |
| `mprotect` | ✅ 完成 | 求交区间，分裂边界，更新 flags 与 PTE | `linux_layer/mm/sys_mprotect.c` |
| `mremap` | ✅ 完成 | 基础实现 | `linux_layer/mm/sys_mremap.c` |
| 文件mmap | 📋 后续阶段 | 需要VFS支持 | - |

**Map_Handler**：所有 **页表遍历** 使用 **`&percpu(Map_Handler)`**（当前 CPU），即使 nexus 节点带 `handler->cpu_id`（见 `doc/ai/INVARIANTS.md`）。

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

- [x] **导出或封装**：在指定 `VS_Common` 上的地址空间复制API（`copy_vspace`等）
- [x] **page fault 入口**：可注册回调（弱符号），由 linux_layer 注册 COW 逻辑
- [x] **refcount**：fork 共享只读 PTE 时与 `Page` / buddy 一致性已审计

**后续阶段可能需要**：
- [ ] 文件mmap相关的nexus扩展
- [ ] 更复杂的mremap场景支持

具体符号名在实现 PR 中补齐，并同步 [`doc/ai/INVARIANTS.md`](../ai/INVARIANTS.md)。

## 7. fork 与 vspace

> **状态**: ✅ Phase 1 完成

- **子进程**：新 `Tcb_Base` + 新 `VS_Common` + **复制或共享** L0…L3 策略（COW 通常为 **共享只读 + 引用计数**）。
- **实现文件**：
  - `linux_layer/proc/sys_fork.c` - fork系统调用实现
  - `linux_layer/mm/linux_vspace.c` - `linux_copy_vspace()` 地址空间COW复制
- **测试验证**: ✅ TEST 09/11 PASS, 用户态多进程测试通过
- **执行 CPU**：fork 在父进程 CPU 上调用，COW 复制已 SMP 安全
