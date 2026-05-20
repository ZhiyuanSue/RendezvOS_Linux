# VFS Server IPC（全局单例 + request–reply）

实现文件：

- `servers/fs/vfs_server.c` — handler + `ipc_rpc_server_loop`
- `linux_layer/fs/fs_ipc.c` — `vfs_ipc_request_response` → `ipc_rpc_call_va`
- `include/linux_compat/fs/vfs_protocol.h` — VFS opcode / TLV
- **框架**：[`IPC_RPC_FRAMEWORK.md`](IPC_RPC_FRAMEWORK.md)

参考：`doc/ai/IPC_MESSAGE.md`、`servers/clean_server.c`、`linux_layer/proc/sys_wait.c`。

---

## 1. 拓扑

```text
用户 syscall 线程                    vfs_server 内核线程 (全局)
      |                                      |
      |  send_msg(vfs_server_port)         | recv_msg(vfs_server_port)
      |------------------------------------->|
      |                                      | 处理
      |  recv_msg(vfs_client_<pid>)          | send_msg(vfs_client_<pid>)
      |<-------------------------------------|
```

- **服务端口**（全局唯一）：`vfs_server_port`，`DEFINE_INIT` 在 BSP 创建并 `register_port`。
- **回复端口**（每进程一个）：`vfs_client_<pid>`，首次 syscall 时 `create_message_port` + `register_port`。

---

## 2. kmsg 约定

| 字段 | 规则 |
|------|------|
| `hdr.module` | **必须**为 `vfs_server_port->service_id`（`service_id_from_name` 哈希，**不是**固定 `2`） |
| `hdr.opcode` | 请求：各 `KMSG_OP_VFS_*`；响应：统一 `KMSG_OP_VFS_RESP` |
| 请求 payload | `ipc_serial` TLV + **末尾** `t` = 回复端口名字符串 |
| 响应 payload | `VFS_KMSG_FMT_RESP` = `"q"`（单个 `i64`） |

---

## 3. 客户端流程（`vfs_ipc_request_response`）

1. `vfs_get_or_create_client_port()`
2. `thread_lookup_port(VFS_SERVER_PORT_NAME)`
3. `vfs_kmsg_create_request(service_id, opcode, fmt, client_port->name, ap)`  
   - 先 `ipc_serial_encode_into_va` 编码 `fmt`  
   - 再 `vfs_payload_append_reply_port` 追加 `t`  
4. `enqueue_msg_for_send` + `send_msg(vfs_server_port)`（阻塞至 server `recv_msg`）
5. `recv_msg(client_port)` + `dequeue_recv_msg` + 解码 `"q"`

**不要**把 `va_list` 传给 `kmsg_create(...)` — `kmsg_create` 只接受 `...` 变参。

---

## 4. 服务端流程（`vfs_server_thread`）

1. `recv_msg(vfs_server_port)`
2. `dequeue_recv_msg` 处理队列中全部消息
3. 按 opcode 用 `VFS_KMSG_FMT_* "t"` 解码（例如 GETCWD 为 `"put"`）
4. `vfs_send_reply(reply_port_name, result)` → `kmsg_create` + `send_msg`

**失败也必须回复**（避免客户端卡在 `recv_msg`）：

- 正常路径与 decode/module 错误均调用 `vfs_reply_best_effort`。
- 若 TLV 解码未得到端口名，用 `vfs_payload_last_reply_port()` 扫描最后一个 `t`。
- 仍无法解析回复端口时打 error 日志（极少数畸形包；客户端可能仍阻塞，core 无 IPC 超时）。

**客户端**：

- 每次 RPC 前 `vfs_drain_client_recv_queue()`，避免把上一次残留响应当成本次结果。
- `send_msg` 失败则**不**调用 `recv_msg`。
- `recv_msg` / 响应校验失败时 drain 并返回 `-EIO` / `-EINVAL`。

---

## 5. 与 GLM 草稿的主要差异（审阅结论）

| 问题 | 修复 |
|------|------|
| `kmsg_create(..., ap, reply_port)` | 非法变参；改为 measure/encode + 追加 `t` TLV |
| `KMSG_MOD_VFS 2u` | 与真实 `service_id` 不一致；改用 `port->service_id` |
| `sys_getcwd` 未走 IPC | 已接 `vfs_ipc_request_response` 作 round-trip 测例 |
| `vfs_server_service_id` 重复定义 | 已去掉重复 |
| 响应 opcode 与请求相同 | 响应改为 `KMSG_OP_VFS_RESP` |
| `vfs.h` 重复宏 | 收敛到 `vfs_protocol.h` |

---

## 6. 后续 syscall 接入模板

```c
return vfs_ipc_request_response(KMSG_OP_VFS_CLOSE, VFS_KMSG_FMT_CLOSE, (u32)fd);
```

路径类 syscall 在客户端先把用户指针拷到内核缓冲区，TLV 用 `s`/`p` 传内核地址或内联字符串。

---

## 7. 审查 / 测试

- `getcwd`：IPC 成功返回 `0` 后由 compat 写入 `"/"`（server 尚未读用户缓冲区）。
- 构建：`make ARCH=x86_64 config user build run`，观察 `[VFS] registered` 与 getcwd 不挂死。
- SMP：server 仅 BSP 创建；客户端端口全局注册，多 CPU lookup 同名端口。
