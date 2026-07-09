# Linux 兼容层设计文档

在 **方案 A（混合内核）** 下，以 [`linux_layer/`](../../linux_layer/) 实现 Linux syscall 语义；**不侵入 [`core/`](../../core/)**。

**如何使用 core（API/调用顺序）：** 仅 [`core/docs/USING_CORE.md`](../../core/docs/USING_CORE.md)（本目录不再维护 core 使用说明）。

**仓库文档总入口：** [`doc/README.md`](../README.md)

---

## 必读（canonical）

| 文档 | 内容 |
|------|------|
| [`PROGRESS.md`](PROGRESS.md) | **进展索引**：阶段状态、缺口、文档追溯链 |
| [`GOALS_AND_CORE_CONTRACT.md`](GOALS_AND_CORE_CONTRACT.md) | **总目标、compat 政策、对 core 契约、维护者审阅包** |
| [`ARCHITECTURE.md`](ARCHITECTURE.md) | 边界、数据流、IPC vs 直接调 core |
| [`CODE_STRUCTURE.md`](CODE_STRUCTURE.md) | **目录与模块职责（现行）** |
| [`FILE_LOADING.md`](FILE_LOADING.md) | **page_slice 统一文件加载（CPIO / IPC / embedded）** |
| [`DATA_MODEL.md`](DATA_MODEL.md) | 进程/线程、`pid`/`tid`、登记簿 |
| [`SYSCALLS.md`](SYSCALLS.md) | **实现顺序**与文件清单 |
| [`MM_AND_COW.md`](MM_AND_COW.md) | Radix 虚存真源、COW、页故障 |
| [`IPC_RPC_FRAMEWORK.md`](IPC_RPC_FRAMEWORK.md) | RPC / one-way server 模板 |
| [`../ai/IPC_MESSAGE.md`](../ai/IPC_MESSAGE.md) | kmsg envelope、TLV、reply port `t` |
| [`../../include/linux_compat/ipc/clean_protocol.h`](../../include/linux_compat/ipc/clean_protocol.h) | clean_server、exit 等 compat opcode（见同目录 `exit_protocol.h`、`fs/vfs_protocol.h`） |
| [`SYSCALL_USER_RETURN_AND_EXECVE.md`](SYSCALL_USER_RETURN_AND_EXECVE.md) | Path A 返回、exec 接线 |
| [`SIGNAL_IMPLEMENTATION_STATUS.md`](SIGNAL_IMPLEMENTATION_STATUS.md) | 信号实现状态（持续更新） |
| [`EXECVE_IMPLEMENTATION_STATUS.md`](EXECVE_IMPLEMENTATION_STATUS.md) | execve 实现状态（Phase 3） |
| [`TIME_SUBSYSTEM_PLAN.md`](TIME_SUBSYSTEM_PLAN.md) | 时间子系统计划（Phase 3.5） |
| [`CROSS_ARCH_VERIFICATION_LOG.md`](CROSS_ARCH_VERIFICATION_LOG.md) | 双架构配对验证日志 |
| [`SIGNAL_DELIVERY_TRAP_PATHS.md`](SIGNAL_DELIVERY_TRAP_PATHS.md) | trap 路径投递 |
| [`IPC_BASED_SIGNAL_DESIGN.md`](IPC_BASED_SIGNAL_DESIGN.md) | IPC 辅助 vs pending（勿用信号服务器替代 trap） |
| [`VFS_SERVER_IPC.md`](VFS_SERVER_IPC.md) | vfs_server 协议 |
| [`VFS_ARCHITECTURE.md`](VFS_ARCHITECTURE.md) | **VFS 三层、Linux 对齐、演进与验证门** |
| [`VFS_IMPLEMENTATION_STATUS.md`](VFS_IMPLEMENTATION_STATUS.md) | **Phase 4 live：已写什么 / RPC 表 / 缺口** |
| [`INITRAMFS_PLAN.md`](INITRAMFS_PLAN.md) | cpio initramfs 方案 |
| [`ROOTFS.md`](ROOTFS.md) | **rootfs/ 目录、Git 策略、fixtures vs generated** |
| [`RAMFS_AND_VFS_STORAGE.md`](RAMFS_AND_VFS_STORAGE.md) | ramfs/存储后端笔记 |
| [`USER_TESTS.md`](USER_TESTS.md) | 用户态测例 |
| [`STDIO_SHIM.md`](STDIO_SHIM.md) | 无 VFS 阶段 console shim |
| [`LINUX_COMPAT_CODING_STYLE.md`](LINUX_COMPAT_CODING_STYLE.md) | 代码风格 |

---

## 参考（bugfix / 专项设计）

| 文档 | 内容 |
|------|------|
| [`BUGFIX_MAP_INTERMEDIATE_PTE_FLAGS.md`](BUGFIX_MAP_INTERMEDIATE_PTE_FLAGS.md) | 中间页表项 flags |
| [`BUGFIX_FORK_SYSCALL_STALE_USER_CONTEXT.md`](BUGFIX_FORK_SYSCALL_STALE_USER_CONTEXT.md) | fork 陈旧 user SP/TLS |
| [`COPY_VSPACE_DESIGN.md`](COPY_VSPACE_DESIGN.md) | 地址空间复制 |
| [`WAIT4_IMPLEMENTATION_STATUS.md`](WAIT4_IMPLEMENTATION_STATUS.md) | wait4 状态 |
| [`CORE_MODIFICATION_STRATEGY.md`](CORE_MODIFICATION_STRATEGY.md) | 提议 core 变更策略 |
| [`CORE_MODIFICATION_BRK_FIX.md`](CORE_MODIFICATION_BRK_FIX.md) | brk 相关 core 变更 |

与 **必读** 冲突时，以必读为准。

---

## 已归档

阶段报告、可行性分析、已被上述 canonical 文档取代的草稿 → [`archive/README.md`](archive/README.md)

---

## 协作规范

- 运行时不变式：[`doc/ai/INVARIANTS.md`](../ai/INVARIANTS.md)
- 审查清单：[`doc/ai/AI_CHECKLIST.md`](../ai/AI_CHECKLIST.md)

## 一致性说明

1. **虚存**：用户 VA 以 Radix 为真源（`MM_AND_COW.md`），不另建 Linux 式 VMA 链表。
2. **wait/exit**：可先登记簿 + 锁，再迁入 `proc_coordinator` IPC（`ARCHITECTURE.md`）。
3. **调 core**：热路径能直接调 radix/mm/调度则直接调；仅全局序列化用 server。

更新文档时同步维护本 README 的 canonical / 参考 分区。
