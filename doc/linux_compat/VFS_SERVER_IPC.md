# VFS Server IPC（全局单例 + request–reply）

实现文件：

- `servers/fs/vfs_server.c` — handler + `ipc_rpc_server_loop`
- `linux_layer/fs/fs_ipc.c` — `vfs_ipc_request_response` → `ipc_rpc_call_va`
- `include/linux_compat/fs/vfs_protocol.h` — VFS opcode / TLV
- **接线表（live）**: [`VFS_IMPLEMENTATION_STATUS.md`](VFS_IMPLEMENTATION_STATUS.md) §3
- **框架**：[`IPC_RPC_FRAMEWORK.md`](IPC_RPC_FRAMEWORK.md)

参考：`doc/ai/IPC_MESSAGE.md`、`servers/clean_server.c`、`linux_layer/proc/sys_wait.c`。

---

## 1. 拓扑

```text
用户 syscall 线程                    vfs_server 内核线程
      |                              (SMP: CPU 1 when NR_CPU>1)
      |  send_msg(vfs_server_port)         | recv_msg(vfs_server_port)
      |------------------------------------->|
      |                                      | 处理（pid 从 reply port 名解析）
      |  recv_msg(vfs_client_<pid>)          | send_msg(vfs_client_<pid>)
      |<-------------------------------------|
```

- **服务端口**（全局唯一）：`vfs_server_port`。
- **回复端口**（每进程一个）：`vfs_client_<pid>`，首次 syscall 时 `create_message_port` + `register_port`。
- **用户缓冲区**：read/fstat/write 等由 **server** 通过 `find_task_by_pid` + `linux_mm_*_user` 访问（非 syscall 线程上下文）。

---

## 2. kmsg 约定

| 字段 | 规则 |
|------|------|
| `hdr.module` | **必须**为 `vfs_server_port->service_id`（`service_id_from_name` 哈希，**不是**固定 `2`） |
| `hdr.opcode` | 请求：各 `KMSG_OP_VFS_*`；响应：统一 `KMSG_OP_VFS_RESP` |
| 请求 payload | `ipc_serial` TLV + **末尾** `t` = 回复端口名字符串 |
| 响应 payload | `VFS_KMSG_FMT_RESP` = `"q"`（单个 `i64`） |

---

## 3. 已定义 opcode / TLV（2026-07-05）

| Opcode | fmt（不含 `t`） | 说明 |
|--------|-----------------|------|
| `KMSG_OP_VFS_OPEN` (1) | `isiu` | dirfd, path, flags, mode → 返回 fd |
| `KMSG_OP_VFS_CLOSE` (2) | `i` | fd |
| `KMSG_OP_VFS_READ` (3) | `ipp` | fd, user_buf, count |
| `KMSG_OP_VFS_WRITE` (4) | `ipp` | fd, user_buf, count |
| `KMSG_OP_VFS_FSTAT` (5) | `ip` | fd, user stat buf |
| `KMSG_OP_VFS_LSEEK` (7) | `iqi` | fd, offset (i64), whence |
| `KMSG_OP_VFS_GETCWD` (8) | `pu` | user_buf, size |
| `KMSG_OP_VFS_MKDIRAT` (12) | `isu` | dirfd, path, mode |
| `KMSG_OP_VFS_UNLINKAT` (13) | `isi` | dirfd, path, flags |
| `KMSG_OP_VFS_NEWFSTATAT` (14) | `ispu` | dirfd, path, stat buf, flags |
| `KMSG_OP_VFS_DUP3` / `PIPE2` | — | 仍 `-ENOSYS` |

**哪些已接线**：见 [`VFS_IMPLEMENTATION_STATUS.md`](VFS_IMPLEMENTATION_STATUS.md) §3。

---

## 4. 客户端流程（`vfs_ipc_request_response`）

1. `vfs_get_or_create_client_port()`
2. `thread_lookup_port(VFS_SERVER_PORT_NAME)`
3. `ipc_kmsg_create_request(...)` — encode fmt + append reply port `t`
4. `send_msg` → `recv_msg` → decode `"q"`

路径类 syscall：**客户端** `linux_mm_load_from_user` 到内核栈，TLV 传 `s`（字符串）。

---

## 5. 服务端流程

1. `ipc_rpc_server_loop` → `vfs_rpc_handler`
2. decode `VFS_KMSG_FMT_* "t"`
3. `vfs_rpc_client_pid(reply_port, &pid)`
4. dispatch → `vfs_open.c` / `vfs_fd.c` / `vfs_root.c`
5. `ipc_rpc_reply_best_effort`（失败也必须回复）

---

## 6. 审查 / 测试

- 构建：`make ARCH=x86_64 config user build run`
- 52/52 harness 回归 + oscomp FS stdout（open/read/fstat/write）
- 记录：[`VFS_IMPLEMENTATION_STATUS.md`](VFS_IMPLEMENTATION_STATUS.md) §5
