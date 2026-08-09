# linux_compat IPC RPC 框架

通用代码：`include/linux_compat/ipc/rpc.h`、`linux_layer/ipc/rpc.c`。

core 仍只提供 `send_msg` / `recv_msg` / `ipc_try_*` / `kmsg_create` / `ipc_serial`；**不在 core 增加 RPC 层**（避免改动 core/，且 reply 端口名约定属项目策略）。

**端口如何命名（必读）：** [`PORT_NAMING.md`](PORT_NAMING.md) — `service` + `cpu` + worker / client id；全局表禁止无结构撞名。

---

## 两种 server 模式

| 模式 | API | 示例 |
|------|-----|------|
| **合作式 one-way** | `ipc_server_coop_loop` + `poll_pending` | `clean_server` |
| **合作式 request–reply** | `ipc_rpc_coop_server_loop` + `ipc_rpc_coop_queue` | `vfs_listen` + cpio/ramfs/blkdev |

**禁止**通用 per-message OS worker pool（含已删除的 `ipc_server_recv_loop_per_msg_worker` / `ipc_server_recv_loop` / `ipc_rpc_server_loop`）。

**clean_server**：`ipc_server_coop_loop`；`EXIT_NOTIFY` = try_deliver + park。见 [`EXIT_CLEAN.md`](EXIT_CLEAN.md)。

---

## Coop 成熟度

| 能力 | 状态 | 说明 |
|------|------|------|
| 单 listen + `try_recv` / 空则 `recv_msg` | ✅ | `ipc_server_coop_loop` / `ipc_rpc_coop_server_loop` |
| `poll_pending` 推进 parked 收尾 | ✅ | 返回 `bool`：仍有 park 则 `schedule` 而非堵 `recv_msg` |
| One-way 消息 inline 处理 | ✅ | clean：`THREAD_REAP` / `TASK_REAP*` |
| EXIT_NOTIFY try+park | ✅ | clean listen；无 `gen_thread` |
| **Request–reply coop**（accept → park reply） | ✅ | VFS + backends 已切 `ipc_rpc_coop_server_loop` |
| Park **嵌套** VFS→backend | ✅ path + RW | `vfs_coop` / `vfs_coop_path`：OPEN/LOOKUP-like/MKDIR/UNLINK + READ/WRITE；rename/link/getdents/mount 仍同步 |
| Park **client reply**（listen→blocking client） | ✅ | `NEED_REPLY` + `ipc_try_send_msg`（单 send 槽；client 会 `recv_msg` 挂 wait） |
| Leaf backend → nested VFS reply | ✅ | **`enqueue` + `ipc_transfer_message`** 到 VFS listen（`@n<cookie>`）；sync 仍阻塞 `ipc_rpc_reply` |
| 通用 pending-job 队列 API | ✅ | `ipc_rpc_coop_queue` / `ipc_rpc_coop_job` |

**硬约束：`try_send` ↔ `try_recv` 不能两边都不挂 port wait。**  
对 **port rendezvous** 仍成立。Nested reply 已改为 §8.3 直投：leaf `ipc_transfer_message` → VFS `recv_msg_queue`；VFS 在 `NESTED_RECV` 时因 coop `parked` 只 `try_recv`+`schedule` 并 **drain** 该队列——**不**依赖 reply-port `try_recv`，也 **不**发明 port-wait 唤醒。

**不要**对仍堵在 `recv_msg(listen)` 的线程做纯直投当请求投递：transfer **不会**把 `block_on_receive` 设为 ready。故 nested **request** 仍走 backend listen port。

**Listen 线程 send 队列约束（硬）：** `send_msg_queue` 是 FIFO，**不按目的 port 解复用**。coop 队列用 `send_owner` 保证**至多一条** in-flight `try_send` payload。多 job 的 reply 在槽位空闲前只保存 `(reply_port, result)`。

**VFS 仍待：**

1. RENAME/LINK/GETDENTS/MOUNT/READLINK 仍同步嵌套 → 同 prepare/commit + FSM。  
2. `vfs_req_cred` 在剩余 sync opcode 路径仍是 listen 全局；coop path/RW 用 per-job cookie。  
3. namespace/handle/pcache **仍单飞**直至加锁。  
4. **不要**恢复 per-msg OS worker pool。

---

## Request–reply 约定

1. 客户端在全局表注册 **reply port**（如 `vfs_cli_<pid>`；内核哨兵见 PORT_NAMING §6）。
2. 请求 TLV：业务参数 + 末尾 **`t`** = reply port 名字符串（框架自动追加）。
3. `kmsg.hdr.module` = **server port 的 `service_id`**（非硬编码常量）。
4. 响应：默认 `opcode=0` + `"q"`（单 `i64`）；VFS 使用 `KMSG_OP_VFS_RESP`。
5. Server：**阻塞路径**用 `ipc_rpc_reply`；**coop→blocking client** 用 `set_result` + `try_send` park；**leaf→nested VFS** 用 `ipc_rpc_nest_reply_transfer`（TLV `t`=`@n<cookie>`）并返回 `IPC_RPC_COOP_REPLIED`。禁止对 live client 裸 `try_send` 且不 park。
6. 遗弃 client：unregister → ops gate → `PORT_CLOSED`；`try_send`/`send_msg` 视为已处理。
7. **Signal EINTR（仅 commit 前）**：interruptible `ipc_rpc_call*` 仅在 `send_msg(server)` 前可 `-EINTR`。
8. **不可中断 RPC**：`ipc_rpc_call_*_uninterruptible`（`TASK_REAP_SYNC`、VFS 客户端、VFS→backend）。

### 5.1 Rendezvous 时序

合法交错（任一方可先到达 reply port）：

```text
Client                         Server (listen)
──────                         ────────────────
send_msg(server) ───────────►  recv_msg(listen)   # 请求会合
                               handler()
recv_msg(reply)  ◄───────────  send_msg / try_send(reply)
```

- **允许** server 在 client 进入 `recv_msg(reply)` **之前** 就进入 send（block 或 try+park）。
- Compat：**禁止**无 port-wait 身份的 `schedule()` 空转；coop poll 亦然。
- Reply：`NO_MSG`/`AGAIN` → rebuild/再 try；`SUCCESS`/`PORT_CLOSED` 后禁止再发。

---

## 客户端模板

```c
Message_Port_t* reply = ipc_rpc_port_lookup_or_create("vfs_cli_12");
i64 ret = ipc_rpc_call_named(VFS_SERVER_PORT_NAME, reply, MY_OP, "pu", ptr, size);
ref_put(...);
```

VFS 封装：`vfs_ipc_request_response()` → uninterruptible call。

---

## 服务端模板（coop request–reply）

```c
static ipc_rpc_coop_queue_t g_q;

static void on_resume(ipc_rpc_coop_job_t *job, i64 nested, void *ctx)
{
        (void)ctx;
        /* finish FS work using nested; then: */
        ipc_rpc_coop_job_set_result(job, nested);
}

static ipc_rpc_coop_disp_t on_req(ipc_rpc_coop_job_t *job, u16 op,
                                  const kmsg_t *km, i64 *result_out)
{
        error_t e = ipc_rpc_coop_nested_call(job, BACKEND_PORT, BE_OP, "s",
                                             path);
        if (e == REND_SUCCESS || e == -E_REND_AGAIN)
                return IPC_RPC_COOP_PARKED;
        *result_out = -LINUX_EIO;
        return IPC_RPC_COOP_HANDLED;
}

void vfs_coop_thread(void)
{
        ipc_rpc_coop_queue_init(&g_q);
        ipc_rpc_coop_queue_set_resume(&g_q, on_resume, NULL);
        ipc_rpc_coop_server_loop(VFS_SERVER_PORT_NAME, sid,
                                 KMSG_OP_VFS_RESP, "q",
                                 on_req, &g_q, NULL, NULL);
}
```

---

## 服务端模板（one-way）

```c
void clean_server_thread(void) {
        /* Every CPU runs this; all recv the same global listen. */
        ipc_server_coop_loop(CLEAN_SERVER_PORT_NAME, on_msg, poll_pending, NULL);
}
```

---

## 为何放在 linux_compat 而非 core

- Reply 端口命名、`LINUX_*` 错误码、与 `proc_registry` 集成都属兼容层/servers 策略。
- core IPC 保持原语；若将来要零拷贝/超时/cancel，再在 core 提 **窄接口** 方案与你 review。
