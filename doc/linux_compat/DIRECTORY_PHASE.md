# Phase 4b — 目录语义（chdir / openat / getdents64）

> **Status**: **已完成并归档**（2026-07-12）— 52/52 harness；后续变更见 [`VFS_EVOLUTION.md`](VFS_EVOLUTION.md)  
> **勿**按本文「ENOSYS / 未实现」实施；下文保留作历史记录。

---

## 1. 要不要做 Linux 式 inode？

**不需要新建一套 Linux `struct inode` / icache / dentry 树。**

项目里已有 **薄 inode 等价物**：

| Linux 概念 | 已有实现 | 位置 |
|------------|----------|------|
| inode 元数据 + 后端句柄 | `vfs_inode_t` | `servers/fs/vfs_backend_ops.h` |
| 路径 lookup | `vfs_root_lookup` | `vfs_root.c` |
| 目录枚举 | `vfs_root_readdir` | `vfs_root.c`（cpio visit + ramfs scan） |
| 打开文件状态 | `vfs_open_file_t`（offset、flags、绑定的 `vfs_inode_t`） | `vfs_fd.c` |
| stat 输出 | `vfs_kstat_t` / `vfs_dirent_t` | `vfs_kstat.h` |

目录阶段 **不扩 inode 子系统**，只做 **前端路径语义 + syscall/RPC 接线**：

1. **per-process cwd**（字符串或规范化绝对路径）
2. **`vfs_resolve_path(dirfd, rel)`** 支持 `AT_FDCWD` 与 **目录 fd**
3. **`getdents64`** 把 `vfs_root_readdir` 填成用户态 `struct linux_dirent64`
4. **`chdir`** 校验目标为目录并更新 cwd

这与 [`VFS_ARCHITECTURE.md`](VFS_ARCHITECTURE.md) §2.2 一致：bootstrap 阶段刻意 **不** 上 icache / page cache / mount 表。

---

## 2. 2026-07-09 run 暴露的缺口（双架构相同）

| 测例 | stdout | 根因（当前代码） |
|------|--------|------------------|
| `#3 chdir` | `-38` ENOSYS | `sys_chdir` 直接 `return -ENOSYS`（`sys_fs_impl.c`） |
| `#13 getdents` | open `-1` | `sys_getdents64` 未实现；测例需先 open 目录 |
| `#22 openat` | dir fd=3 后 openat `-1` | **`sys_openat` 忽略 `dirfd`**，只把 path 送 RPC；server 侧 `dirfd != AT_FDCWD` → `-EBADF` |
| `#6 dup` / `#7 dup2` | `-38` | fd 表 dup 未做（目录阶段可并行，非 inode） |
| `#19 mount` / `#46 umount` | `-38` | **刻意 deferred**（无块设备后端） |

已 PASS 的 FS 相关 stdout：`open` `read` `close` `fstat` `getcwd`(stub `/`) `mkdir` `unlink` `execve` `write`(fd≥3)。

---

## 3. 实施步骤（建议顺序）

### Step D1 — per-process cwd

| 项 | 建议 |
|----|------|
| 存储 | `linux_proc_append_t.cwd[VFS_PATH_MAX]`，初始 `"/"` |
| chdir | `sys_chdir` → RPC `KMSG_OP_VFS_CHDIR` 或 compat 内 lookup 目录后写 cwd |
| getcwd | 去掉 `fake_cwd`；从 append 或 RPC 读真实 cwd |
| 校验 | chdir 到非目录 → `-ENOTDIR`；不存在 → `-ENOENT` |

**不必** 为 cwd 单独建 inode；`vfs_root_lookup(path, &ino)` + `ino.is_dir` 即可。

### Step D2 — openat(dirfd) 路径展开

| 项 | 建议 |
|----|------|
| compat | `sys_openat` 把 **dirfd** 传入 RPC（扩展 `VFS_KMSG_FMT_OPEN` 或已有字段） |
| server | `vfs_resolve_path`：`AT_FDCWD` → 用 cwd；否则 `vfs_fd_get` → 目录 inode 的 path + `direct_child` 拼接 |
| 测例 | `#22 openat` 应绿 |

现有 server 代码：`dirfd != AT_FDCWD` 直接 `-EBADF`（`vfs_open.c`）— 需改为读 fd 表。

### Step D3 — getdents64

| 项 | 建议 |
|----|------|
| 中端 | 已有 `vfs_root_readdir(dirpath, index, &vfs_dirent_t)` |
| server | 新 RPC：`KMSG_OP_VFS_GETDENTS64`，fd 须为目录，按 `offset` 迭代 readdir |
| compat | `sys_getdents64` → IPC → `linux_dirent64` 序列写入 user buffer |
| 类型 | 用 `vfs_dirent_t.d_type`（`VFS_DT_DIR` / `VFS_DT_REG`） |

**不必** 引入 `struct file_operations->iterate_shared`；bootstrap 一次 readdir 排序即可（已实现 `vfs_readdir_sort`）。

### Step D4 — 验证门

双架构 `make run`，更新 verification log 中 FS stdout 矩阵：

- [ ] `#3 chdir` stdout `[PASS]`
- [ ] `#13 getdents` stdout `[PASS]`
- [ ] `#22 openat` stdout `[PASS]`
- [ ] harness 仍 52/52

### 并行（非 inode，Phase 4c）

- dup / dup2 / pipe2（fd 表）
- `vfs_fd_drop_pid` on exit
- mount/umount — 等 busybox demo 或块设备后端再议

---

## 4. 与 page_slice / exec 的关系

**无交叉。** 目录 syscall 走 VFS IPC + `vfs_inode_t`；内核加载 ELF/manifest 仍走 [`FILE_LOADING.md`](FILE_LOADING.md) 的 page_slice 路径。

execve 已从 initramfs 跑通（`#8` stdout：`execve success`），embedded `program_map` 可逐步缩小（见 [`EXECVE_IMPLEMENTATION_STATUS.md`](EXECVE_IMPLEMENTATION_STATUS.md)）。

---

## 5. 追溯

| 日期 | 事件 |
|------|------|
| 2026-07-09 | x86_64 + aarch64 52/52；FS bootstrap V4–V7 多项 stdout PASS；定目录阶段为 Step 7 首选 |
| 2026-07-05 | VFS Step 0–5 代码落地（见 VFS_IMPLEMENTATION_STATUS） |

改 VFS 目录相关代码时：**先更新本文 Step 状态 + VFS_IMPLEMENTATION_STATUS + verification log**。
