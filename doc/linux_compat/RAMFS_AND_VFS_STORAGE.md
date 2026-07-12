# Ramfs 与 VFS 存储层（Phase 4 后端笔记）

> **Status**: 后端实现笔记；**架构 / 对齐 / 验证以 [`VFS_ARCHITECTURE.md`](VFS_ARCHITECTURE.md) 为准**  
> **Index**: [`INITRAMFS_PLAN.md`](INITRAMFS_PLAN.md) · [`VFS_SERVER_IPC.md`](VFS_SERVER_IPC.md)

---

## 1. 快速结论

| 问题 | 答案 |
|------|------|
| initramfs | 内存中的文件内容；**cpio 只读底 + ramfs 可写 overlay** |
| 写 cpio？ | 不能；写只进 ramfs |
| inode / page cache？ | Phase 4 用 **entry + buffer** 承担职责；见架构 doc §2 |
| core | 仅 `percpu(kallocator)` |

---

## 2. 分层（摘要）

见 [`VFS_ARCHITECTURE.md`](VFS_ARCHITECTURE.md) §1 三层图。

演进待办见 [`VFS_EVOLUTION.md`](VFS_EVOLUTION.md)。

**查找顺序**（`vfs_namespace_lookup`）：树 walk → 节点 `deleted` 过滤 → ramfs 或 cpio 填 `vfs_inode_t`。

---

## 3. 模块与 cpio 接口

| 文件 | 层 | 职责 |
|------|-----|------|
| `cpio_rofs.c` | 后端 | 只读；**公开 API 见架构 doc §4** |
| `ramfs_layer.c` | 后端 | 可写 kmalloc（**无** path/deleted 状态） |
| `vfs_namespace.c` | 中端 | 路径真源：cpio 目录 + ramfs 覆盖 + deleted |
| `vfs_root.c` | 中端 | 薄 facade → namespace |
| `vfs_path.c` | 中端 | 路径规范化 |
| `vfs_open.c` / `vfs_fd.c` | 前端 server | open/fd/RPC（见实现状态 doc） |
| `vfs_server.c` | 前端 server | RPC loop |

cpio **不**再提供 boot dump；成功时仅 `vfs_root_init` 一行计数日志。

---

## 4. ramfs 限制（bootstrap）

| 项 | 值 |
|----|-----|
| 最大条目 | 128 |
| 单文件最大 | 256 KiB |
| 并发 | vfs_server 单线程，无锁 |

---

## 5. 实施状态

见 **[`VFS_IMPLEMENTATION_STATUS.md`](VFS_IMPLEMENTATION_STATUS.md)**（live 快照，改代码同步更新）。

```text
✅ namespace 树 + openat/getdents/chdir + page cache + mount 登记
⬜ mount 覆盖 lookup / linkat / uid 权限
```
