# linux_compat IPC RPC 框架

通用代码：`include/linux_compat/ipc/rpc.h`、`linux_layer/ipc/rpc.c`。

core 仍只提供 `send_msg` / `recv_msg` / `kmsg_create` / `ipc_serial`；**不在 core 增加 RPC 层**（避免改动 core/，且 reply 端口名约定属项目策略）。

**端口如何命名（必读）：** [`PORT_NAMING.md`](PORT_NAMING.md) — `service` + `cpu` + worker / client id；全局表禁止无结构撞名。

---

## 两种 server 模式

| 模式 | API | 示例 |
|------|-----|------|
| **Request–reply** | `ipc_rpc_server_loop` + handler 返回 `i64` | `vfs_server`、backends |
| **One-way** | `ipc_server_recv_loop` + 自行处理 | `clean_server` listen |

没有通用 **per-message worker pool**。需要把个别长阻塞操作踢出 listen 时，由该服务自己 spawn one-shot（如 clean 的 EXIT_NOTIFY），不要再发明一套全局 pool。

**clean_server**：client 消息（`THREAD_REAP` / `TASK_REAP*`）走 **`ipc_server_recv_loop` 内联**；仅 `EXIT_NOTIFY` 异步。协议见 [`EXIT_CLEAN.md`](EXIT_CLEAN.md)。

---

## Request–reply 约定

1. 客户端在全局表注册 **reply port**（如 `vfs_cli_<pid>`；内核哨兵见 PORT_NAMING §6）。
2. 请求 TLV：业务参数 + 末尾 **`t`** = reply port 名字符串（框架自动追加）。
3. `kmsg.hdr.module` = **server port 的 `service_id`**（非硬编码常量）。
4. 响应：默认 `opcode=0` + `"q"`（单 `i64`）；VFS 使用 `KMSG_OP_VFS_RESP`。
5. Server **必须** `ipc_rpc_reply`（阻塞 `send_msg`）。Client 在 `ipc_rpc_call*` 的 `recv` 上会合；裸 `try_send` 会与「server 先于 client recv」竞态（曾出现 `reply best-effort failed port='vfs_backend_caller'`）。
6. 遗弃 client：进程 teardown 调 `unregister_port(reply)` → 名表摘掉后 port **ops gate** 进入 `CLOSING`，等所有 `port_ops_begin` 临界段结束（阻塞路径在 `schedule` 前 `port_ops_end`），再 `port_clean_thread_queue` 唤醒 `block_on_send`；`send_msg` 见 `THREAD_FLAG_IPC_PORT_CLOSED` → `-E_REND_PORT_CLOSED` 并丢弃未送出 payload；`ipc_rpc_send_reply` 视为已处理，listen 继续。`recv` 侧另收 `KMSG_OP_SYSTEM_PORT_CLOSED` 或同 flag。
7. **Signal EINTR（仅 commit 前）**：`ipc_rpc_call*`（interruptible）仅在 **`send_msg(server)` 之前** 若有可投递信号则返回 `-LINUX_EINTR`。请求已交给 server 后必须等 reply（或 reply-port close）；`KMSG_OP_IPC_RECV_INTERRUPT` 只作 wake，**不得**弃 recv。
8. **不可中断 RPC**：`ipc_rpc_call_*_uninterruptible`（`TASK_REAP_SYNC`、**VFS 客户端**、VFS→backend）在 send 前也不因信号返回。单线程 listen 的 reply rendezvous 不能容忍 client 弃 recv。

---

## 客户端模板

```c
Message_Port_t* reply = ipc_rpc_port_lookup_or_create("vfs_cli_12");
Message_Port_t* srv = thread_lookup_port(VFS_SERVER_PORT_NAME);
i64 ret = ipc_rpc_call(srv, reply, MY_OP, "pu", user_ptr, size);
ref_put(...);
```

或按名：

```c
i64 ret = ipc_rpc_call_named(VFS_SERVER_PORT_NAME, reply, MY_OP, "pu", ptr, size);
```

VFS 封装：`vfs_ipc_request_response()` → `ipc_rpc_call_va_uninterruptible(..., KMSG_OP_VFS_RESP, "q", ap)`。

内核侧 VFS↔backend：`vfs_cli_k_srv` / `vfs_cli_k_reg_<fstype>`（见 `vfs_backend_ipc.c`）。

---

## 服务端模板（reply）

```c
static i64 my_handler(u16 opcode, const kmsg_t* km, char** reply_port_out)
{
        switch (opcode) { ... ipc_serial_decode(..., "....t", ..., reply_port_out); }
}

static void my_server_thread(void)
{
        ipc_rpc_server_loop(MY_PORT_NAME, my_service_id,
                            MY_RESP_OP, "q", my_handler);
}
```

`ipc_rpc_server_loop` 对每次 handler 结果调用 **`ipc_rpc_reply`（blocking）**。

---

## 服务端模板（one-way）

```c
static void on_msg(Message_t* msg, u16 service_id) {
        (void)service_id;
        /* clean: handle inline; may schedule() while waiting */
}

void clean_server_thread(void) {
        ipc_server_recv_loop(CLEAN_SERVER_PORT_NAME, on_msg);
}
```

---

## 为何放在 linux_compat 而非 core

- Reply 端口命名、`LINUX_*` 错误码、与 `proc_registry` 集成都属兼容层/servers 策略。
- core IPC 保持原语；若将来要零拷贝/超时/cancel，再在 core 提 **窄接口** 方案与你 review。
