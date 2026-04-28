# Bug：逐级建页表时误用叶子映射的 flags

## 适用范围

- **层级**：主要在 [`core/kernel/mm/map_handler.c`](../../core/kernel/mm/map_handler.c) 的 `map()`——为 **VS_Common**（用户多级页表根）逐级走向 **最终叶子**（2M 或 4K）时，若需要 **新分配** 中间级别的页目录表。
- **架构**：x86_64 等使用多级 PTE 的实现；语义是「**表指针**」与「**叶子映射**」对 flags 编码的要求不同。

## 症状（为何会坏）

在中间级（示意：L0→L1→L2→……）为新页表目录安装表项时，若错误地使用 **与用户页相同的 `eflags`**（例如某一具体 `prot`/`COW`/remap 相关的组合），可能出现：

- 中间条目被编成 **无效的“叶子语义”**，或带了 **Huge / final** 等与“指向下一级表”矛盾的组合；
- 后续 walk、`have_mapped()`、或对同一 VA 的再次映射表现出 **间歇性映射失败**、走错层级、或与 nexus/Linux 层预期不一致；
- 问题往往只在 **需要先分配中间表** 的路径上触发，因此对「已有页表骨架」的简单场景可能不漏。

本质是：**多级页表中，非末端项必须是“合法的下一级表指针语义”，不能照搬本次 `map(..., eflags)` 写给叶子的 flags。**

## 根本原因

- **`map(..., level, eflags)`** 的 **`eflags`** 描述的是 **最终安装到映射末级的一项**（2M huge 或 4K PTE）的属性。
- **新分配的中间页表目录页**在安装到上一级时，应使用 **独立于本次叶子请求**、且符合架构规定的 **non-final / table-pointer** 标志集合（例如：`VALID`、`READ`、`WRITE`、`USER`、`EXEC` 等由 `arch_decode_flags(level, …)` 解释的一套“可走表”语义），而不能把调用方传来的 **叶子 efags** 直接套在中间级上。

## 修复思路（工程上怎么做）

在 `map()` 内为「首次分配某级页表容器」的路径单独约定一组 **中间层必须 flags**，例如文档化名为 `nonfinal_must_flags`，并通过对应层号的 `arch_decode_flags(level_nonfinal, nonfinal_must_flags)` 生成 `ARCH_PFLAGS`，再调用 `arch_set_Lx_entry(...)`。

叶子路径仍使用请求传入的 **`eflags`**（并配合 `PAGE_ENTRY_HUGE`、`allow_remap` 等原有逻辑），与中建表分流。

这样既保证 **逐级 walk** 合法，又让 **最后一次** 映射仍反映 Linux/nexus 侧的真实保护与 COW 位。

## 与兼容层文档的关系

该问题发生在 **core 页表_walk 原语**；Linux 兼容层大量通过 **`get_free_page` / `map` / `have_mapped`** 驱动用户映射。若在 core 修了中间层语义，上层 **mmap/mprotect/COW/fork** 的稳定性会一并受益。故在本目录记录，便于兼容层开发与回归时对齐「多级 PT 的中间项 ≠ 叶子项」这一类不变式。

## 参考锚点（实现位置）

[`core/kernel/mm/map_handler.c`](../../core/kernel/mm/map_handler.c) 内 `map()`：`nonfinal_must_flags` 及 `arch_decode_flags` 用于新分配 **L1/L2（及架构所需其它中间级）** 表指针条目的分支。
