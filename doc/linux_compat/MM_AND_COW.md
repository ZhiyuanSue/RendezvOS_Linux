# 内存：nexus 作为虚存真源 + COW + 页故障

## 1. 为何不需要独立 VMA 子系统

Linux **VMA** 解决的是：**按用户 VA 区间** 记录映射属性，并支持 **`munmap`/`mprotect` 的部分区间**（分裂、合并）。RendezvOS **nexus** 已在每个用户 `VS_Common` 下维护用户 VA 与物理页/nexus 节点的关系；**同一语义**可在 nexus 节点上扩展元数据（prot、`MAP_PRIVATE`/`SHARED`、COW 共享关系），无需第二棵并行树。若日后需要 **按文件 offset 反查** 等索引，再增加辅助结构。

## 2. nexus 需承载的 Linux 侧信息（实现时逐项补）

- **区间**：与现有 `nexus_node::addr`、长度 API 一致。
- **保护**：与 `region_flags` / PTE 属性对齐；`mprotect` 时 **分裂边界** 后改写。
- **匿名 vs 文件**：初期仅匿名；文件 `mmap` 预留 `file`/`offset` 字段或外部表。
- **COW**：`MAP_PRIVATE` + fork 后 **多 vspace 共享只读物理页**；写故障时 **分裂物理页** 并更新 nexus/rmap（与现有 `Page` refcount 衔接）。

## 3. `brk`

- **语义**：调整 **堆顶**；失败返回当前 brk。
- **实现要点**：`linux_proc_append` 中 `start_brk`/`brk`；从 `brk_old` 到 `brk_new` 用 `get_free_page` / `free_pages` + `map`/`unmap`，**堆范围**与 ELF program header 对齐（loader 已有信息可传入 append 初始化）。
- **文件**：`linux_layer` 新建 `sys_brk.c` 或并入 `linux_mm.c`；调用 core [`core/kernel/mm/nexus.c`](../../core/kernel/mm/nexus.c) 的 `get_free_page`、`free_pages`，[`map_handler.c`](../../core/kernel/mm/map_handler.c) 的 `map`/`unmap`。
- **锁**：`vs->vspace_lock` + `nexus_vspace_lock` 按 core 既有顺序。

## 4. `mmap` / `munmap` / `mprotect` / `mremap`

| Syscall | nexus 侧工作 |
|---------|----------------|
| `mmap` | 分配/插入区间；匿名页 `pmm_alloc` + `map`；处理 `MAP_FIXED` 与重叠策略 |
| `munmap` | **部分删除** → 分裂或截断 nexus 节点；`unmap` + `free_pages` |
| `mprotect` | 求交区间，分裂边界，更新 flags 与 PTE |
| `mremap` | P2：简实现或 `-ENOSYS`（在 `SYSCALLS.md` 标注） |

**Map_Handler**：所有 **页表遍历** 使用 **`&percpu(Map_Handler)`**（当前 CPU），即使 nexus 节点带 `handler->cpu_id`（见 `doc/ai/INVARIANTS.md`）。

## 5. COW 与页故障

- **目标**：fork 后子父共享 **只读** 用户页；写访问触发 **page fault**。
- **路径**：
  - **阶段 A**：在 [`core`](../../core) trap 路径中内联 **COW 解析**（最小闭环）。
  - **阶段 B**：trap **upcall** 到 **`cow_fault_handler` server**（内核线程），与现有 IPC 一致；server 调用 **窄 MM API** 完成分裂。
- **协议**（阶段 B）：消息含 `fault_vaddr`、`write`/`read`、`vs` 标识、`cpu_id`；回复 **重试** 或 **错误码**。

## 6. core 变更清单（仅通用接口，随实现勾选）

在 **不** 把 Linux 头文件引入 `core` 的前提下，可能需要：

- [ ] 导出或封装：**在指定 `VS_Common` 上** 的「分裂 nexus 区间」「按区间改 prot」辅助函数（若 `nexus.c` 内部 static 不足）。
- [ ] page fault 入口：**调用可注册回调**（弱符号或函数指针），由 linux_layer 注册 COW 逻辑。
- [ ] refcount：`fork` **dup 页表** 或 **共享只读 PTE** 时与 `Page` / buddy 一致性的审计。

具体符号名在实现 PR 中补齐，并同步 [`doc/ai/INVARIANTS.md`](../ai/INVARIANTS.md)。

## 7. fork 与 vspace

- **子进程**：新 `Tcb_Base` + 新 `VS_Common` + **复制或共享** L0…L3 策略（COW 通常为 **共享只读 + 引用计数**）。
- **执行 CPU**：若 nexus 删除/遍历仍与 **owner CPU** 有关，`MM_AND_COW.md` 的实现 PR 必须写明 **fork 在何 CPU 上调用**、是否 **IPI 到 owner** 或 **API 已 SMP 安全**。
