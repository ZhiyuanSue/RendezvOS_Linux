# Nexus 历史文档归档

> **状态**：📜 已废弃 - Radix Tree 重构完成后归档  
> **归档时间**：2026年5月（Radix Tree重构完成）  
> **原因**：core/内存管理已从nexus红黑树重构为4级radix tree

## 📁 归档文档列表

| 文档 | 原用途 | 现状 |
|------|--------|------|
| `NEXUS_API_FREEZE.md` | Nexus API冻结说明 | 历史参考 |
| `NEXUS_INTERFACE_USAGE.md` | Nexus接口使用指南 | [`core/docs/memory.md`](../../../core/docs/memory.md) |
| `NEXUS_LAYERING_CROSSWALK.md` | Nexus分层对照表 | 已被 `MM_VSPACE_RADIX_LAYERING.md` 替代 |
| `NEXUS_LAYERING_TABLE.md` | Nexus层次表 | 历史参考 |
| `NEXUS_REFACTORING_ANALYSIS.md` | Nexus重构分析 | 重构完成归档 |
| `NEXUS_REFACTORING_PLAN.md` | Nexus重构计划 | 重构完成归档 |

## 🔍 历史背景

**Nexus架构**（2024-2025）：
- 基于红黑树的每CPU虚拟页分配器
- 每个CPU一个nexus_root，每个VSpace一个vspace根节点
- nexus_vspace_lock保护单VSpace操作
- 按页记录映射，不做VMA区间合并/分裂

**重构为Radix Tree**（2025-2026）：
- 4级512路radix tree，与x86-64页表结构对齐
- 两层锁：L0 big lock + L2 per-band lock
- Range-based APIs：INSERT/DELETE/QUERY_OR_CHANGE
- 更好的多核扩展性

## 📚 当前文档

**新的Radix Tree文档**：
- `core/docs/memory.md` - 内存系统设计（已更新第4节）
- `core/docs/memory.md` §0.7, `core/docs/USING_CORE.md` - radix / MM caller docs
- `doc/ai/MM_VSPACE_RADIX_LAYERING.md` - 分层架构详解
- `doc/ai/RADIX_RANGE_LOCK_FIVE_PHASES.md` - Range lock五阶段详解
- `doc/ai/MM_BACKEND_FRONTEND_API.md` - 前后端API设计

**Linux兼容层文档**：
- `doc/linux_compat/MM_AND_COW.md` - 内存与COW设计（已更新）
- `doc/linux_compat/ARCHITECTURE.md` - 架构总览（已更新）

## ⚠️ 使用注意

这些文档**仅供参考**，当前系统使用Radix Tree架构：
- 代码实现：`core/kernel/mm/vmm_radix_tree.c`
- 接口头文件：`core/include/rendezvos/mm/vmm_radix_tree.h`
- 编排层：`core/kernel/mm/mm_user_utils.c`

**不要**在新代码中引用Nexus概念，所有新开发应使用Radix Tree API。

---

**归档负责人**：Claude AI  
**最后更新**：2026-05-16
