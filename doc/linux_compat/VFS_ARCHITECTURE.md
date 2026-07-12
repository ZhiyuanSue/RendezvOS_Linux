# VFS 架构、Linux 对齐与逐步验证

> **Status**: 定稿（2026-07-12）  
> **演进待办**: [`VFS_EVOLUTION.md`](VFS_EVOLUTION.md)（**Page cache 统一、SMP、mount** 以该文档为准）  
> **Live 索引**: [`VFS_IMPLEMENTATION_STATUS.md`](VFS_IMPLEMENTATION_STATUS.md)

---

## 1. 三层模型

```text
┌─────────────────────────────────────────────────────────────┐
│ 前端 compat — linux_layer/fs/                               │
│  • per-process fd 表（page_slice 容器）→ server handle      │
│  • cwd、dirfd 路径展开；syscall → IPC                       │
│  • console / pipe 本地；VFS 走 RPC                          │
└───────────────────────────────┬─────────────────────────────┘
                                │ handle + abs path
┌───────────────────────────────▼─────────────────────────────┐
│ 前端 server — vfs_open.c, vfs_handle.c, vfs_server.c        │
│  • 全局 open handle（offset、flags、refcnt）                  │
└───────────────────────────────┬─────────────────────────────┘
                                │ inode 原语
┌───────────────────────────────▼─────────────────────────────┐
│ 中端 — vfs_namespace.c, vfs_page_cache.c, vfs_mount.c, …    │
│  • 树形 namespace（路径真源）                               │
│  • cpio 按需 page cache；ramfs 可写存储                     │
│  • lookup / readdir / rename / link / perm                   │
└───────────────┬─────────────────────┬───────────────────────┘
                │                     │
         cpio_rofs (incbin)      ramfs_layer (kmalloc)
```

**边界纪律**

| 层 | 关心 | 不关心 |
|----|------|--------|
| compat | fd、路径展开、IPC、Linux errno | cpio 魔数 |
| server handle | offset、refcnt、open flags | 路径 walk |
| namespace | 路径→节点、deleted/mount_covered | compat fd 编号 |
| 后端 | 字节在哪（镜像内指针 / kmalloc） | syscall 语义 |

---

## 2. 与 Linux 对齐

### 2.1 必须对齐

Syscall 号、errno、openat 标志、initramfs newc、stat 字段、read/write 语义 — 见 [`VFS_IMPLEMENTATION_STATUS.md`](VFS_IMPLEMENTATION_STATUS.md)。

### 2.2 已实现（2026-07-12）

| 项 | 实现 |
|----|------|
| per-process cwd | fd 表 page_slice hdr |
| chdir / getcwd / openat(dirfd) / getdents64 | compat + namespace |
| dup / dup2 / pipe2 | compat 本地 + RETAIN |
| mount / umount2 | 登记 + lookup 覆盖 |
| renameat / linkat | namespace + ramfs |
| 权限 | `vfs_perm.c` owner 位；root 绕过 |
| page cache | cpio 读按需 kmalloc（**待** 迁 page_slice，见 EVOLUTION §PageCache） |

### 2.3 可简化 / 待补

| Linux | 现状 | 待办 |
|-------|------|------|
| icache / dentry 哈希 | 树 + sibling 链表 | 大规模目录再优化 |
| page cache address_space | kmalloc 小 cache + **独立** kern load | **PC-2~5 统一** |
| bind mount / 多 fs 类型 | mount 登记 only | P3-4/5 |
| symlink | 未实现 | P4-4 |
| 完整 uid/gid | bootstrap owner 位 | P4-3 |

---

## 3. page_slice 与 page cache（概念）

**两个不同问题**：

1. **fd 表 page_slice**（compat）：存 cwd + fd 槽；**不是**文件内容。见 [`FD_TABLE.md`](FD_TABLE.md)。
2. **文件内容 page_slice**（server，目标态）：page cache 应用 core `page_slice` 存 cpio/ramfs 文件 bytes；启动 **只** 建 namespace/cpio **元数据**，读 miss 时再填充。

当前：`vfs_kern_read_file_slice` **未**走 page cache — 测例 spawn 每 ELF 全量直拷，与 PC-4 待统一。

---

## 4. 代码地图

| 层 | 文件 |
|----|------|
| compat fd | `linux_fd_table.c`, `linux_vfs_path.c`, `sys_fs_impl.c` |
| compat ingest | `vfs_kern_load.c`（linux_layer 包装）, `linux_page_slice_file.c` |
| server | `vfs_namespace.c`, `vfs_page_cache.c`, `vfs_handle.c`, `vfs_open.c`, `vfs_server.c` |
| 后端 | `cpio_rofs.c`, `ramfs_layer.c` |
| 路径 | **`linux_layer/fs/vfs_path.c`**（唯一实现） |

**已删除**：`vfs_fd.c`, `servers/fs/vfs_path.c`。

**SMP**：`vfs_server` + 存储后端线程均在 **`VFS_SERVICE_CPU_ID`（core 1）**；cpio/namespace 数据 init 在 BSP `DEFINE_INIT_LEVEL(…,3)`。

---

## 5. cpio 向上接口

消费者：`vfs_namespace.c`（populate）、`vfs_backend_ops.c`（lookup/read）。

```c
error_t cpio_rofs_init(const void *image, u64 image_len);
bool    cpio_rofs_lookup(const char *path, cpio_rofs_stat_t *out);
i64     cpio_rofs_read(const cpio_rofs_stat_t *st, u64 offset, void *buf, u64 len);
void    cpio_rofs_visit(cpio_rofs_visit_fn fn, void *ctx);  /* 启动：namespace 建树 only */
```

- `visit`：**不**读文件 payload 进 cache；只 materialize 树节点。
- 文件字节仍在 incbin 镜像内；首次 read 才进 page cache。

---

## 6. 演进步骤（摘要）

| Step | 内容 | 状态 |
|------|------|------|
| 0–6 | cpio、RPC、exec、ramfs | ✅ |
| 7 | 目录 syscall | ✅ |
| 7b | 方案 B fd 表 + **page_slice 容器** | ✅ |
| 8 | dup/pipe/exit release | ✅ |
| 9 | mount + namespace 树 + page cache | 🚧 |
| **10** | **page cache → page_slice + kern 读统一** | ⬜ PC-2~5 |
| 11 | 块设备 / bind mount | ⬜ |

历史 Step 明细与 DIRECTORY 阶段见 [`DIRECTORY_PHASE.md`](DIRECTORY_PHASE.md)（**已归档**）。

---

## 7. 相关文档

- [`VFS_EVOLUTION.md`](VFS_EVOLUTION.md) — 待办与 Page cache 统一  
- [`RAMFS_AND_VFS_STORAGE.md`](RAMFS_AND_VFS_STORAGE.md)  
- [`FD_TABLE.md`](FD_TABLE.md)  
- [`FILE_LOADING.md`](FILE_LOADING.md)
