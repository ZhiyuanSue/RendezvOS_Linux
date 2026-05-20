# 架构：混合内核 + 可演进微内核

> **规划与 core 边界（维护者）：** [`GOALS_AND_CORE_CONTRACT.md`](GOALS_AND_CORE_CONTRACT.md)  
> **core API 用法：** [`core/docs/USING_CORE.md`](../../core/docs/USING_CORE.md)

## 1. 分层

```mermaid
flowchart LR
  subgraph payload [User payload]
    App[libc or static app]
  end
  subgraph linux_layer [linux_layer]
    Entry[syscall_entry.c]
    Proc[proc_compat registry wait]
    MM[linux_mm radix_tree]
  end
  subgraph core [core]
    Sched[schedule Task_Manager]
    VS[VSpace radix_tree map_handler]
    IPC[ports messages]
    Trap[trap syscall page_fault]
  end
  subgraph servers [Kernel servers optional]
    Clean[clean_server]
    Coord[proc_coordinator]
    PF[cow_fault_handler]
  end
  App --> Entry
  Entry --> Proc
  Entry --> MM
  Proc --> Sched
  Proc --> VS
  MM --> VS
  Proc --> IPC
  MM --> IPC
  Trap --> PF
  Coord --> IPC
  Clean --> IPC
```

- **linux_layer**：Linux 语义（`fork`/`wait`/`mmap` 标志解析、错误码、`brk` 游标等）。
- **core**：调度、IPC 原语、地址空间对象、radix tree、页表操作、trap 分发。
- **servers**：与现有 [`servers/clean_server.c`](../../servers/clean_server.c) 同构的内核线程 + 端口；按需新增。

## 2. 原则：能直接调 core 则不调 IPC

- **机制与调用顺序**（锁序、IPC 步骤、exec/fork 原语）：见 [`core/docs/USING_CORE.md`](../../core/docs/USING_CORE.md) 与 [`core/docs/memory.md`](../../core/docs/memory.md) §0 — **不在此重复**。
- **本层策略**：哪些 syscall 走直接 core、哪些走 server，见下文 §3 与 [`SYSCALLS.md`](SYSCALLS.md)。
- **何时用 IPC**：全局单序列策略（pid、父子表、wait 的可选集中化）、或避免锁序爆炸；处理线程不得是发起者自身。

## 3. IPC 串行化 vs 显式锁

| 手段 | 适用 |
|------|------|
| **单消费者 server + 请求/响应** | 全局进程登记、`fork` 记账（可选）、与 clean_server 类似的跨 CPU 回收协调的 **策略面** |
| **细粒度锁（core 已有）** | Radix tree 两层锁（L0 big lock + L2 per-band lock）、`vspace_lock`、`sched_lock`、PMM zone 锁 |
| **per-CPU 数据** | 当前线程/调度队列（见 `doc/ai/INVARIANTS.md`） |

**演进**：
- ✅ **Phase 1 完成**：用 `linux_layer` 内 **proc_registry**（基于core锁机制）维护进程注册表
- 📋 **阶段 2（可选）**：将 **同一数据结构的操作** 迁到 `proc_coordinator` 单线程，**消息格式**可预先按阶段 2 设计，减少二次重写

## 4. 与微内核的关系

- 今日：**策略在 linux_layer，原语在 core，部分工作在 servers**。
- 明日：`proc_coordinator` / `cow_fault_handler` 可迁 **用户态**，只要 **端口能力与消息布局** 稳定；core 只保留能力检查与映射操作。

## 5. SMP 与 vspace

- Radix tree 的 **两层锁机制** 支持多核扩展：L2 per-band lock（2 MiB 粒度）允许同一 vspace 内多线程并发操作不同区间。
- `map_handler` 的 **每 CPU `map_vaddr` 窗口** 约束仍然存在：在 **任意 CPU** 上遍历页表时须使用 **当前 CPU 的 `Map_Handler`**（见 `doc/ai/INVARIANTS.md`）。
- **fork/COW** 的 **执行** 通过 `linux_copy_vspace()` 和 `mm_user_utils_remap_page()` 实现，SMP 安全；`MM_AND_COW.md` 单独列「必须在 owner 上完成的步骤」。

## 6. 错误与可中断

- 阻塞类 syscall（`wait4`、将来 `pause` 等）需约定：与 `thread_status_block_*`、信号（若实现）之间的 **EINTR** 行为；在 `SYSCALLS.md` 按项勾选。
