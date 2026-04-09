# Linux 兼容层设计文档

本目录描述在 **方案 A（混合内核）** 下，以 [`linux_layer/`](../../linux_layer/) 为主、**不侵入 [`core/`](../../core/) 的 Linux 专用逻辑** 的前提下，实现基础 Linux syscall 兼容的路径。实现时 **按需** 向 `core` 提交 **通用、可文档化** 的窄接口（见 [`MM_AND_COW.md`](MM_AND_COW.md)）。

## 文档索引

| 文档 | 内容 |
|------|------|
| [`ARCHITECTURE.md`](ARCHITECTURE.md) | 边界、数据流、IPC 与锁的分工、与微内核演进 |
| [`DATA_MODEL.md`](DATA_MODEL.md) | 进程/线程组、append 区、`pid`/`tid`、登记簿演进 |
| [`MM_AND_COW.md`](MM_AND_COW.md) | nexus 作为虚存真源、COW、页故障、core 变更清单 |
| [`SYSCALLS.md`](SYSCALLS.md) | **推荐实现顺序**、逐步文件/函数清单、独立 server 条目 |
| [`STDIO_SHIM.md`](STDIO_SHIM.md) | **无 VFS 阶段**：`write(1|2, …)` 控制台 shim、与后续 fd 表/VFS 的衔接 |
| [`USER_TESTS.md`](USER_TESTS.md) | 用户态 ELF 测例：single/smp 分层、case 同步边界、输出乱序期望 |

## 与仓库规范的关系

- 运行时不变式与 SMP 注意点：[`doc/ai/INVARIANTS.md`](../ai/INVARIANTS.md)
- 协作流程与检查项：[`doc/ai/README.md`](../ai/README.md)、[`doc/ai/AI_CHECKLIST.md`](../ai/AI_CHECKLIST.md)

## 叙述一致性说明（避免矛盾）

1. **虚存**：不另建 Linux 式 VMA 链表；**用户 VA 区间以 nexus 为真源**（见 `MM_AND_COW.md`）。
2. **进程元数据与 wait**：**阶段 1** 可在 `linux_layer` 内用 **登记簿 + 显式锁** 实现 `wait`/`exit` 交互；**阶段 2** 可将同一登记逻辑 **迁入 `proc_coordinator` 线程**，通过 IPC 串行化，与 [`ARCHITECTURE.md`](ARCHITECTURE.md) 中的 IPC 域一致，二者不是互斥叙述。
3. **直接调 core**：syscall 热路径上 **能安全直接调用** 的 nexus/map/调度 API **应直接调用**；仅 **全局策略/多 CPU 易死锁** 的块用 IPC server（见 `ARCHITECTURE.md`）。

后续只改文档时，优先更新本 README 的「一致性说明」以免与正文漂移。
