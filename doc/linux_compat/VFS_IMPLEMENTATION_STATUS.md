# VFS Phase 4 — 实现状态（live）

> **Purpose**: 记录 **已写什么 / 缺什么 / 怎么验**，避免只靠翻代码。  
> **架构与验证门**: [`VFS_ARCHITECTURE.md`](VFS_ARCHITECTURE.md)  
> **IPC 细节**: [`VFS_SERVER_IPC.md`](VFS_SERVER_IPC.md)  
> **Last updated**: 2026-07-05

---

## 1. 演进步骤（对照架构 doc §5）

| Step | 内容 | 状态 |
|------|------|------|
| 0 | 文档 + cpio 接口/日志收束 | ✅ |
| 1 | `vfs_backend_ops` + `vfs_inode_t` | ✅ |
| 2 | `vfs_root_stat` / `vfs_root_readdir` + `vfs_kstat_t` | ✅（readdir 仅中端，syscall 未接） |
| 3 | `vfs_open.c` / `vfs_fd.c` / `vfs_rpc.c` + server RPC | ✅ |
| 4 | `sys_fs_impl.c` IPC 接线（open/read/close/fstat/lseek/statat/mkdirat） | ✅（fd 表在 **server** 侧，非 linux_layer） |
| 5 | write + unlinkat RPC | ✅（2026-07-05） |
| 6 | execve 从 vfs 读 ELF | ⬜ |
| 7 | getdents64 | ⬜ |
| 8 | 磁盘后端 | ⬜ |

**SMP**：`vfs_server` 整包 init + RPC 线程在 **CPU 1**（`NR_CPU>1`）；AP 在 `do_init_call` 后须持续 `schedule()`（core `smp.c`，由 maintainer 维护）。

---

## 2. 文件地图（`servers/fs/`）

| 文件 | 层 | 职责 |
|------|-----|------|
| `cpio_rofs.c/h` | 后端 | newc 只读；`lookup` / `read` / `visit` |
| `ramfs_layer.c/h` | 后端 | kmalloc 可写 overlay |
| `vfs_path.c/h` | 中端 | 路径规范化、`direct_child_name` |
| `vfs_backend_ops.c/h` | 中端 | per-backend I/O；`vfs_inode_t` |
| `vfs_root.c/h` | 中端 | overlay lookup/mkdir/create/unlink/read/write |
| `vfs_kstat.c/h` | 中端 | `struct kstat` 布局填充 |
| `vfs_open.c/h` | 前端 server | openat/read/write/lseek/fstat/statat/mkdirat/unlinkat |
| `vfs_fd.c/h` | 前端 server | **每 pid** fd 表（fd 3–31） |
| `vfs_rpc.c/h` | 前端 server | 从 `vfs_client_<pid>` 解析 pid |
| `vfs_server.c` | 前端 server | init + `ipc_rpc_server_loop` |

**`linux_layer/fs/`**

| 文件 | 职责 |
|------|------|
| `fs_ipc.c` | `vfs_ipc_request_response` 客户端 |
| `sys_fs_impl.c` | syscall → IPC（路径先 `load_from_user`） |
| `io/sys_write.c` | fd 1/2 stdio shim；**fd≥3** 在 `sys_fs_impl` 走 VFS WRITE RPC |

---

## 3. 已接 syscall ↔ RPC（2026-07-05）

| Syscall | RPC opcode | TLV fmt | Server 行为 |
|---------|------------|---------|-------------|
| `openat` | `KMSG_OP_VFS_OPEN` | `isiu` + `t` | `vfs_openat` → 返回 fd |
| `read` | `KMSG_OP_VFS_READ` | `ipp` + `t` | 读 fd → `linux_mm_store_to_user` |
| `write` (fd≥3) | `KMSG_OP_VFS_WRITE` | `ipp` + `t` | `load_from_user` → `vfs_root_write` |
| `close` | `KMSG_OP_VFS_CLOSE` | `i` + `t` | `vfs_fd_close` |
| `fstat` | `KMSG_OP_VFS_FSTAT` | `ip` + `t` | 填 `vfs_kstat_t` 到用户 buf |
| `newfstatat` | `KMSG_OP_VFS_NEWFSTATAT` | `ispu` + `t` | 路径 stat |
| `lseek` | `KMSG_OP_VFS_LSEEK` | `iqi` + `t` | fd seek |
| `mkdirat` | `KMSG_OP_VFS_MKDIRAT` | `isu` + `t` | ramfs mkdir |
| `unlinkat` | `KMSG_OP_VFS_UNLINKAT` | `isi` + `t` | unlink / whiteout |
| `getcwd` | `KMSG_OP_VFS_GETCWD` | `pu` + `t` | round-trip；compat 写 `"/"` |
| `pipe2` / `dup3` | — | — | RPC 仍 `-ENOSYS` |

路径：`AT_FDCWD` only；**cwd 固定 `/`**；相对路径、`./foo` 在 server 展开。

---

## 4. 已知缺口 / 陷阱

| 项 | 说明 |
|----|------|
| **`Stat` vs `kstat`** | oscomp `fstat` 用 `struct kstat`；ucore `ch6_file1` 用较小 `Stat` — 后者尚未单独适配 |
| **fd 表位置** | 在 **vfs_server** 按 pid，不在 linux_layer `fd_table.c`（Step 4 文档名保留，实现合并到 server） |
| **getdents64** | 中端 `vfs_root_readdir` 已有，syscall/RPC 未接 |
| **进程 exit** | 未 `vfs_fd_drop_pid`；依赖 slot 复用（bootstrap 可接受） |
| **验证** | V4–V7 需 maintainer `make run` + oscomp stdout；本文档不声称已验 |

---

## 5. 验证命令（maintainer）

```bash
make ARCH=x86_64 config user build run
# 52/52 harness 应 PASS；grep oscomp open/read/fstat/write 的 [PASS]
```

双架构通过后 append [`CROSS_ARCH_VERIFICATION_LOG.md`](CROSS_ARCH_VERIFICATION_LOG.md)。
