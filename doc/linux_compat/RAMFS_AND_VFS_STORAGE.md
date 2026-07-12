# Ramfs 与 VFS 存储层

> **Status**: 2026-07-12  
> **架构**: [`VFS_ARCHITECTURE.md`](VFS_ARCHITECTURE.md) · **演进**: [`VFS_EVOLUTION.md`](VFS_EVOLUTION.md)

---

## 1. 快速结论

| 问题 | 答案 |
|------|------|
| initramfs | incbin cpio 只读 + ramfs 可写 overlay |
| 写 cpio？ | 不能；写 ramfs |
| 启动读文件？ | **只**解析 cpio + namespace **元数据**；文件体 **按需** read/cache |
| page cache | cpio 读路径；**待** 改为 page_slice 后端（PC-2） |
| kern load | **暂** bypass cache；与 page cache **待统一**（PC-4） |

---

## 2. 查找顺序

`vfs_namespace_lookup`：树 walk → `deleted` / `mount_covered` → 填 `vfs_inode_t`（cpio 指针或 ramfs 缓冲）。

---

## 3. 模块表

| 文件 | 层 | 职责 |
|------|-----|------|
| `cpio_rofs.c` | 后端 | 解析 cpio；lookup/read；`data` 指向镜像 |
| `ramfs_layer.c` | 后端 | 可写 kmalloc 字节 |
| `vfs_namespace.c` | 中端 | **路径真源**；启动 visit 建树 |
| `vfs_page_cache.c` | 中端 | cpio 读 cache（kmalloc；→ page_slice） |
| `vfs_mount.c` | 中端 | mount 登记 + cover |
| `vfs_perm.c` | 中端 | owner mode |
| `vfs_root.c` | 中端 | init facade |
| `vfs_backend_ops.c` | 中端 | read/write 分发 |
| `vfs_handle.c` / `vfs_open.c` | server 前端 | handle 表 + open |
| `vfs_kern_load.c` | 内核 ingest | read→slice（**待** 接 cache） |
| `linux_layer/fs/vfs_path.c` | compat+server 共享 | path normalize |

**已删除**：`vfs_fd.c`, `servers/fs/vfs_path.c`。

---

## 4. ramfs 限制

| 项 | 值 |
|----|-----|
| 最大条目 | 128 |
| 单文件最大 | 256 KiB |
| vfs_server | 单线程 RPC |

---

## 5. 实施状态

```text
✅ namespace 树 + openat/getdents/chdir + mount 登记/覆盖
✅ page cache（kmalloc 版）+ 写失效
✅ rename/link + bootstrap perm
⬜ page cache → page_slice（PC-2）
⬜ kern load 与 read 统一（PC-4）
⬜ mount 后端切换（P3-5）
```

Live 细节：[`VFS_IMPLEMENTATION_STATUS.md`](VFS_IMPLEMENTATION_STATUS.md)。
