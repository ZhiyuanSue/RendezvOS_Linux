# VFS Server IPC（全局单例 + request–reply）

实现文件：

- `servers/fs/vfs_server.c` — handler + `ipc_rpc_server_loop`
- `linux_layer/fs/fs_ipc.c` — `vfs_ipc_request_response`
- `include/linux_compat/fs/vfs_protocol.h` — opcode / TLV
- **fd 模型**: [`FD_TABLE.md`](FD_TABLE.md)

---

## 1. 拓扑

```text
用户 syscall (linux_layer)              vfs_server 线程
  查 linux_proc_append_t.fs                  |
  路径展开 / fd→handle                      | recv RPC
  IPC(handle 或 abs path)  ---------------->|
  recv reply <------------------------------|
```

- **pid**：从 `vfs_client_<pid>` 解析，仅用于 **user 内存访问**。
- **不再**用 pid 索引 server 侧 fd 表。

---

## 2. Opcode / TLV（方案 B）

| Opcode | fmt | 说明 |
|--------|-----|------|
| `OPEN` (1) | **`siu`** | abs path, flags, mode → 返回 **handle** |
| `CLOSE` (2) | `i` | handle |
| `HANDLE_RETAIN` (16) | `i` | handle refcnt++（dup/fork） |
| `READ/WRITE` (3/4) | `ipp` | handle, user_buf, count |
| `FSTAT` (5) | `ip` | handle, stat buf |
| `LSEEK` (7) | `iqi` | handle, offset, whence |
| `CHDIR` (9) | `s` | abs path — **仅校验目录** |
| `MKDIRAT` (12) | **`su`** | abs path, mode |
| `UNLINKAT` (13) | **`si`** | abs path, flags |
| `NEWFSTATAT` (14) | **`spu`** | abs path, stat buf, flags |
| `GETDENTS64` (15) | `ipp` | handle, dirp, count |
| `GETCWD` (8) | — | **deprecated**（compat 本地） |

响应：统一 `KMSG_OP_VFS_RESP` / `"q"`。

---

## 3. 相关文档

- [`FD_TABLE.md`](FD_TABLE.md)
- [`VFS_IMPLEMENTATION_STATUS.md`](VFS_IMPLEMENTATION_STATUS.md)
- [`IPC_RPC_FRAMEWORK.md`](IPC_RPC_FRAMEWORK.md)
