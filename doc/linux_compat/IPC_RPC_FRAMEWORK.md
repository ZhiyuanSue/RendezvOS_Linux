# linux_compat IPC RPC 框架

通用代码：`include/linux_compat/ipc/rpc.h`、`linux_layer/ipc/rpc.c`。

core 仍只提供 `send_msg` / `recv_msg` / `kmsg_create` / `ipc_serial`；**不在 core 增加 RPC 层**（避免改动 core/，且 reply 端口名约定属项目策略）。

---

## 两种 server 模式

| 模式 | API | 示例 |
|------|-----|------|
| **Request–reply** | `ipc_rpc_server_loop` + handler 返回 `i64` | `vfs_server` |
| **One-way** | `ipc_server_recv_loop` + 自行处理 | （同步 handler） |
| **One-way + per-msg worker** | `ipc_server_recv_loop_per_msg_worker` | `clean_server` |
| **Request–reply + per-msg worker** | `ipc_rpc_server_loop_per_msg_worker` | （VFS 可迁） |

### Per-message worker（过渡方案）

Dispatcher 只 `recv_msg` 并 `gen_thread_from_func` 拉起 worker；handler（含阻塞 `send_msg`，如 `EXIT_NOTIFY`）在 worker 上跑。worker 结束后 `THREAD_FLAG_EXIT_REQUESTED` → `schedule` 变 zombie，由 **dispatcher 直接 `delete_thread`** 回收（不走 clean IPC，避免递归）。

**Name 生命周期**：`gen_thread_from_func` 不拷贝 name，而 `del_thread_structure` 会对 `thread->name` 做 `m_free`。worker 名必须是堆分配，禁止传静态字符串（否则回收时堆损坏）。

后续可换成线程池 / 有栈协程，API 保持 listen loop 形态。

---

## Request–reply 约定

1. 客户端在全局表注册 **reply port**（如 `vfs_client_<pid>`）。
2. 请求 TLV：业务参数 + 末尾 **`t`** = reply port 名字符串（框架自动追加）。
3. `kmsg.hdr.module` = **server port 的 `service_id`**（非硬编码常量）。
4. 响应：默认 `opcode=0` + `"q"`（单 `i64`）；VFS 使用 `KMSG_OP_VFS_RESP`。
5. 失败时 server **必须** `ipc_rpc_reply_best_effort`（避免 client 卡在 `recv_msg`）。
6. **Signal EINTR**：`ipc_rpc_call*` 在 `recv_msg(reply_port)` 上阻塞；可投递信号时 `signal_queue` 向 reply port 投 `KMSG_OP_IPC_RECV_INTERRUPT`（见 `include/linux_compat/ipc/block_wake.h`），RPC 返回 `-LINUX_EINTR`。无需 VFS server 配合。

---

## 客户端模板

```c
Message_Port_t* reply = ipc_rpc_port_lookup_or_create("my_client_12");
Message_Port_t* srv = thread_lookup_port("my_server_port");
i64 ret = ipc_rpc_call(srv, reply, MY_OP, "pu", user_ptr, size);
ref_put(...);
```

或按名：

```c
i64 ret = ipc_rpc_call_named("my_server_port", reply, MY_OP, "pu", ptr, size);
```

VFS 封装：`vfs_ipc_request_response()` → `ipc_rpc_call_va(..., KMSG_OP_VFS_RESP, "q", ap)`。

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

static void my_init(void) {
        Message_Port_t* p = create_message_port(MY_PORT_NAME);
        register_port(global_port_table, p);
        my_service_id = p->service_id;
        gen_thread_from_func(..., my_server_thread, ...);
}
DEFINE_INIT(my_init);
```

---

## 服务端模板（one-way + per-msg worker）

```c
static void on_msg(Message_t* msg, u16 service_id) {
        (void)service_id;
        /* may block on send_msg; listen loop stays free */
}

void clean_server_thread(void) {
        ipc_server_recv_loop_per_msg_worker(CLEAN_SERVER_PORT_NAME, on_msg);
}
```

---

## 为何放在 linux_compat 而非 core

- Reply 端口命名、`LINUX_*` 错误码、与 `proc_registry` 集成都属兼容层/servers 策略。
- core IPC 保持原语；若将来要零拷贝/超时/cancel，再在 core 提 **窄接口** 方案与你 review。

---

## 相关文档

- [`doc/ai/IPC_MESSAGE.md`](../ai/IPC_MESSAGE.md)
- [`VFS_SERVER_IPC.md`](VFS_SERVER_IPC.md)
