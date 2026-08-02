# linux_compat IPC RPC 框架

通用代码：`include/linux_compat/ipc/rpc.h`、`linux_layer/ipc/rpc.c`。

core 仍只提供 `send_msg` / `recv_msg` / `ipc_try_*` / `kmsg_create` / `ipc_serial`；**不在 core 增加 RPC 层**（避免改动 core/，且 reply 端口名约定属项目策略）。

**端口如何命名（必读）：** [`PORT_NAMING.md`](PORT_NAMING.md) — `service` + `cpu` + worker / client id；全局表禁止无结构撞名。

---

## 三种 server 模式

| 模式 | API | 示例 |
|------|-----|------|
| **合作式 one-way** | `ipc_server_coop_loop` + `poll_pending` | `clean_server` |
| **合作式 request–reply** | `ipc_rpc_coop_server_loop` + `ipc_rpc_coop_queue` | **VFS 迁移目标**（框架已就绪，server 未切） |
| **阻塞 request–reply** | `ipc_rpc_server_loop` | `vfs_*` / backends（过渡；**不是** worker 池） |

**禁止**通用 per-message OS worker pool（含已删除的 `ipc_server_recv_loop_per_msg_worker` / `ipc_server_recv_loop`）。

**clean_server**：`ipc_server_coop_loop`；`EXIT_NOTIFY` 暂 one-shot 线程。见 [`EXIT_CLEAN.md`](EXIT_CLEAN.md)。

---

## Coop 成熟度

| 能力 | 状态 | 说明 |
|------|------|------|
| 单 listen + `try_recv` / 空则 `recv_msg` | ✅ | `ipc_server_coop_loop` / `ipc_rpc_coop_server_loop` |
| `poll_pending` 推进 parked 收尾 | ✅ | `void` 回调；**不得**决定是否跳过 port |
| One-way 消息 inline 处理 | ✅ | clean：`THREAD_REAP` / `TASK_REAP*` |
| 阻塞点拆到独立线程（过渡） | ✅ 有限 | clean：`EXIT_NOTIFY` one-shot |
| **Request–reply coop**（accept → park → 稍后 reply） | ✅ 框架 | `ipc_rpc_coop_*`；VFS **尚未**改用 |
| Park **嵌套** `ipc_rpc_call_*`（VFS→backend） | ✅ 框架 | `ipc_rpc_coop_nested_call*` + `resume_fn` |
| Park **blocking reply** `send_msg` | ✅ 框架 | `NEED_REPLY` + `ipc_try_send_msg`（单 send 槽） |
| 通用 pending-job 队列 API | ✅ | `ipc_rpc_coop_queue` / `ipc_rpc_coop_job` |

**Listen 线程 send 队列约束（硬）：** `send_msg_queue` 是 FIFO，**不按目的 port 解复用**。coop 队列用 `send_owner` 保证**至多一条** in-flight `try_send` payload。多 job 的 reply 在槽位空闲前只保存 `(reply_port, result)`。

**VFS 迁 coop 仍需（server 侧，非框架）：**

1. 去掉/收窄 listen 全局可变状态：`vfs_req_cred`、`vfs_io_chunk` 等。  
2. 初期策略：IPC 等待点 park，namespace/handle/pcache **仍单飞**；重叠突变另加锁。  
3. Backends 可暂留 `ipc_rpc_server_loop`。  
4. **不要**恢复 per-msg OS worker pool。

---

## Request–reply 约定

1. 客户端在全局表注册 **reply port**（如 `vfs_cli_<pid>`；内核哨兵见 PORT_NAMING §6）。
2. 请求 TLV：业务参数 + 末尾 **`t`** = reply port 名字符串（框架自动追加）。
3. `kmsg.hdr.module` = **server port 的 `service_id`**（非硬编码常量）。
4. 响应：默认 `opcode=0` + `"q"`（单 `i64`）；VFS 使用 `KMSG_OP_VFS_RESP`。
5. Server：**阻塞路径**用 `ipc_rpc_reply`；**coop 路径**用 `ipc_rpc_coop_job_set_result` + `queue_poll`（内部 `ipc_try_send_msg`）。禁止对 live client 裸 `try_send` 且不 park。
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

## 服务端模板（阻塞 reply — 过渡）

```c
static void my_server_thread(void)
{
        ipc_rpc_server_loop(MY_PORT_NAME, my_service_id,
                            MY_RESP_OP, "q", my_handler);
}
```

---

## 服务端模板（coop reply — 迁移目标）

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
        Message_Port_t *nreply = /* per-job / per-tid reply port */;
        error_t e = ipc_rpc_coop_nested_call(job, BACKEND_PORT, nreply,
                                             BE_OP, "s", BE_RESP, "q", path);
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
        ipc_server_coop_loop(CLEAN_SERVER_PORT_NAME, on_msg, poll_pending, NULL);
}
```

---

## 为何放在 linux_compat 而非 core

- Reply 端口命名、`LINUX_*` 错误码、与 `proc_registry` 集成都属兼容层/servers 策略。
- core IPC 保持原语；若将来要零拷贝/超时/cancel，再在 core 提 **窄接口** 方案与你 review。
