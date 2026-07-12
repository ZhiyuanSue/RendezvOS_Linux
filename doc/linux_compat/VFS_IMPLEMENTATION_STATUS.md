# VFS Phase 4 — 实现状态（live）

> **Purpose**: 已写什么 / 缺什么 / 怎么验  
> **架构**: [`VFS_ARCHITECTURE.md`](VFS_ARCHITECTURE.md) · **演进**: [`VFS_EVOLUTION.md`](VFS_EVOLUTION.md)  
> **fd 表**: [`FD_TABLE.md`](FD_TABLE.md) · **IPC**: [`VFS_SERVER_IPC.md`](VFS_SERVER_IPC.md)  
> **Last updated**: 2026-07-12

---

## 1. 基线

| 项 | 状态 |
|----|------|
| x86_64 user harness | **52/52**（2026-07-12 前次基线；fd 表栈修复待 re-run） |
| aarch64 user harness | **52/52** |
| 命名空间 | 树形 `vfs_namespace` |
| fd 表 | page_slice 容器 + server handle（方案 B） |
| mount | 登记 + lookup 覆盖；**未**切换后端 |
| page cache | cpio 小文件读缓存 |

---

## 2. 文件地图

**`servers/fs/`**

| 文件 | 职责 |
|------|------|
| `vfs_server.c` | RPC dispatch |
| `vfs_namespace.c` | **树形** dentry、lookup/readdir/rename/link |
| `vfs_mount.c` | mount 登记、namespace mount_covered |
| `vfs_page_cache.c` | cpio 读缓存 |
| `vfs_perm.c` | request cred + owner mode |
| `vfs_handle.c` / `vfs_open.c` | open handle 表 + I/O |
| `vfs_root.c` | init + 薄封装 → namespace |
| `vfs_backend_ops.c` | cpio/ramfs 字节 I/O |
| `ramfs_layer.c`, `cpio_rofs.c` | 平坦后端存储 |
| `vfs_kern_load.c` | 内核侧 read→slice（manifest/exec） |

**已删除**：`vfs_fd.c`, `servers/fs/vfs_path.c`（path 合并到 compat）

**`linux_layer/fs/`**

| 文件 | 职责 |
|------|------|
| `linux_fd_table.c` | per-process fd 表（**page_slice**） |
| `vfs_path.c` | **唯一** path normalize/join/equal |
| `linux_vfs_path.c` | cwd + dirfd → abs |
| `sys_fs_impl.c` | syscall → 查表 / IPC |
| `vfs_exec_load.c` | exec IPC 读 ELF |
| `vfs_root_bootstrap.c` | compat 早期 init |

**`linux_layer/mm/`**

| 文件 | 职责 |
|------|------|
| `linux_page_slice_file.c` | kva → page_slice（ingest） |

---

## 3. Syscall ↔ 分发

| Syscall | 路径 |
|---------|------|
| `openat` / `*at` | compat abs path → RPC |
| `read/write/close/fstat/lseek/getdents64` | compat fd → **handle** RPC |
| `getcwd` / `chdir` | compat 本地 cwd（slice hdr） |
| `dup` / `dup2` | compat + RETAIN |
| `pipe2` | compat 本地 pipe 表 + fd 槽 |
| `mount` / `umount2` | RPC + mount 登记 |
| `renameat` / `linkat` | RPC → namespace |

**Deprecated RPC**：`GETCWD`（compat 本地）；`DUP3`/`PIPE2` server stub（compat 实现）。

---

## 4. 验证

```bash
make ARCH=x86_64 config user build run | tee x86_64_run.log
make ARCH=aarch64 config user build run | tee aarch64_run.log
```

---

## 5. 已知陷阱

| 项 | 说明 |
|----|------|
| OPEN RPC | **`siu`** abs path → handle |
| I/O RPC | 第一参数是 **handle**，不是 compat fd |
| fd 表 hdr | **勿**在 kstack 上大 struct；见 FD_TABLE §4 |
| exit vs exec | exit **release only**；exec **reset** slice |
| 测例 cookie | THREAD_REAP 早于 TASK_REAP；runner 等 pid 消失 |
| fd 表 SMP | 多线程同进程改表无锁；见 EVOLUTION §SMP |

---

## 6. 下一批（代码）

见 [`VFS_EVOLUTION.md`](VFS_EVOLUTION.md) §PageCache、§下一批：

1. page cache → page_slice + kern/user 读统一（PC-2~5）
2. mount 后端切换（P3-5）
3. fd 表 spinlock（多线程前）
4. statx / symlink
