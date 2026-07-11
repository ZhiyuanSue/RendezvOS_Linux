# VFS Phase 4 — 实现状态（live）

> **Purpose**: 记录 **已写什么 / 缺什么 / 怎么验**，避免只靠翻代码。  
> **架构**: [`VFS_ARCHITECTURE.md`](VFS_ARCHITECTURE.md) · **fd 表**: [`FD_TABLE.md`](FD_TABLE.md)  
> **IPC**: [`VFS_SERVER_IPC.md`](VFS_SERVER_IPC.md)  
> **Last updated**: 2026-07-09（方案 B fd 表落地）

---

## 1. 演进步骤

| Step | 内容 | 状态 |
|------|------|------|
| 0–6b | cpio / RPC / exec / page_slice | ✅ |
| 7 | chdir + cwd + openat(dirfd) + getdents64 | 🟡 已接线 — 待 run 验证 |
| **7b** | **方案 B：compat per-pid fd 表 + server open handle** | 🟡 **已实现** — 待 run |
| 8 | pipe2 + exit 释放 handle | ⬜（dup/dup2 已在 compat） |
| 9 | 磁盘 / mount | ⬜ deferred |

---

## 2. 文件地图

**`servers/fs/`**

| 文件 | 职责 |
|------|------|
| `vfs_handle.c/h` | 全局 **open handle** 表（`struct file` 等价） |
| `vfs_open.c/h` | 路径 open + handle I/O |
| `vfs_root.c` / backends | 中端 inode / 数据（不变） |

**`linux_layer/fs/`**

| 文件 | 职责 |
|------|------|
| `linux_fd_table.c` | per-pid fd 表（`linux_proc_append_t.fs`） |
| `linux_vfs_path.c` | cwd + dirfd 路径展开 |
| `sys_fs_impl.c` | syscall 查表 → console / VFS IPC |
| `io/sys_write.c` | UART backend（console 条目调用） |

**已删除**：`vfs_fd.c/h`（bootstrap per-pid server fd 表）

---

## 3. Syscall ↔ 分发

| Syscall | 路径 |
|---------|------|
| `openat` / `*at` | compat 展开 abs path → RPC |
| `read/write/close/fstat/lseek/getdents64` | compat 查 fd → **handle** RPC |
| `getcwd` / `chdir` | **compat 本地 cwd**；chdir RPC 仅校验目录 |
| `write(1/2)` | 查表：`CONSOLE_*` → UART；`VFS` → IPC（dup2 后可重定向） |
| `dup` / `dup2` / `dup3` | **compat 本地** + `HANDLE_RETAIN` |
| `pipe2` | ❌ ENOSYS |

---

## 4. 验证

Maintainer 本地：

```bash
make ARCH=x86_64 config user build run | tee x86_64_run.log
make ARCH=aarch64 config user build run | tee aarch64_run.log
```

关注：`#3` `#6` `#7` `#12` `#13` `#22` `#51` stdout + harness 52/52。

**本环境未跑 cross-gcc 构建**（2026-07-09 assistant turn）。

---

## 5. 陷阱

| 项 | 说明 |
|----|------|
| OPEN RPC fmt | **`siu`**（abs path），不再是 `isiu` |
| I/O RPC 第一参数 | **handle id**，不是 compat fd |
| pid 用途 | 仅 user copy，**不**索引 fd |
| fork fd | `linux_fs_proc_fork` + retain；见 FD_TABLE §7 |
