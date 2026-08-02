# linux_compat IPC RPC 框架

通用代码：`include/linux_compat/ipc/rpc.h`、`linux_layer/ipc/rpc.c`。

core 仍只提供 `send_msg` / `recv_msg` / `kmsg_create` / `ipc_serial`；**不在 core 增加 RPC 层**（避免改动 core/，且 reply 端口名约定属项目策略）。

**端口如何命名（必读）：** [`PORT_NAMING.md`](PORT_NAMING.md) — `service` + `cpu` + worker / client id；全局表禁止无结构撞名。

---

## 两种 server 模式

| 模式 | API | 示例 |
|------|-----|------|
| **合作式 one-way** | `ipc_server_coop_loop` + `poll_pending` | `clean_server` |
| **阻塞 request–reply** | `ipc_rpc_server_loop`（同线程 recv→handler→reply） | `vfs_*`（过渡；**不是** worker 池） |

**禁止**通用 per-message OS worker pool（含已删除的 `ipc_server_recv_loop_per_msg_worker` / `ipc_server_recv_loop`）。  
`ipc_rpc_server_loop` 仍存在只因为 VFS 尚未把 reply/嵌套 RPC park 进 coop；它**不会**起线程分发。

**clean_server**：`ipc_server_coop_loop`；`EXIT_NOTIFY` 暂 one-shot 线程。见 [`EXIT_CLEAN.md`](EXIT_CLEAN.md)。

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

### 5.1 Rendezvous 时序（与 §5 不冲突；对齐 core 原语）

合法交错（任一方可先到达 reply port）：

```text
Client                         Server (listen)
──────                         ────────────────
send_msg(server) ───────────►  recv_msg(listen)   # 请求会合
                               handler()
recv_msg(reply)  ◄───────────  send_msg(reply)    # 应答会合
```

- **允许** server 在 client 进入 `recv_msg(reply)` **之前** 就进入 `send_msg(reply)`（server 入队 `block_on_send` 等待）。
- **禁止** 用直调 backend / 绕过 RPC 代替上述会合（与架构分层冲突）。
- **core 不变量**（实现必须满足，否则会出现 `REPLY send` 后无 `SRV done` 的楔死）：
  1. 从 port `thread_queue` **出队** 的 waiter，要么完成 transfer 并唤醒对端，要么在放弃时 **显式唤醒**（不得静默丢弃后让对端永睡）。
  2. `recv_msg`/`send_msg` 在 `ipc_transfer_message` 返回 `-E_REND_NO_MSG` / 需换对端时，必须 **释放当前 request、唤醒已出队对端（`THREAD_FLAG_IPC_XFER_FAIL`）、并重新 `try_match`/重试**；不得静默丢弃对端，也不得对同一 request 死循环 `continue`。对端被 `XFER_FAIL` 唤醒后不得假 SUCCESS，须重入会合。
  3. `try_match` 若因 `status != block_on_{send,receive}` 丢弃队列项，被丢弃线程若仍停在对应 block 状态，必须被唤醒（否则已离队却无人 `ready`）。

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

内核侧 VFS↔backend：`vfs_cli_k_t<tid>`（每调用方线程）/ `vfs_cli_k_reg_<fstype>`（见 `vfs_backend_ipc.c`）。

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
        ipc_server_coop_loop(CLEAN_SERVER_PORT_NAME, on_msg, poll_pending, NULL);
}
```

---

## 为何放在 linux_compat 而非 core

- Reply 端口命名、`LINUX_*` 错误码、与 `proc_registry` 集成都属兼容层/servers 策略。
- core IPC 保持原语；若将来要零拷贝/超时/cancel，再在 core 提 **窄接口** 方案与你 review。
