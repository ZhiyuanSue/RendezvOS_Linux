# Exit / wait / clean_server 协议

本文件是 **exit ↔ clean_server ↔ wait4** 的权威模型。实现必须服从此协议；禁止用 `schedule` 空转“等 task 消失”掩盖异步竞态。

**Listen / client 端口名：** 见 [`PORT_NAMING.md`](PORT_NAMING.md)。现行：全局唯一 `clean_listen`；client `clean_cli_{pid}`。EXIT_NOTIFY 异步 worker 为 listen 派生的 one-shot（非通用 IPC pool）。

**项目角色：** 上层 compat（含本协议落地）由 AI 在 `linux_layer/` / `servers/` 实现；维护者指导与验收。

---

## IPC 默认：阻塞 rendezvous（非滥用 try_send）

- 协议路径一律 **`send_msg` / `recv_msg` 阻塞会合**。
- `ipc_try_send_msg` / `ipc_system_*` 仅用于 **不能调度的上下文**（时钟 IRQ 等）。
- 卡死先查 **等待环 / 角色选错 / 把不该进 pool 的消息丢进 pool**，禁止短超时丢协议消息。

---

## 架构（先定角色，再谈实现）

| 角色 | 谁 | 职责 |
|------|-----|------|
| **Exitor** | 退出中的用户线程 | `THREAD_REAP` → zombie → `schedule` |
| **Listen** | **唯一** `clean_listen` 线程（BSP） | `ipc_server_coop_loop`：`try_recv` + `poll`（今日仅 EXIT_NOTIFY worker **收尸**）+ 空则 `recv_msg`；`THREAD_REAP` zombie 等待 **inline** `schedule`（协议允许；勿 park，见下） |
| **EXIT_NOTIFY worker** | listen 按需 spawn 的 one-shot（过渡） | **仅**阻塞 `EXIT_NOTIFY`→父 `wait_port`；**目标**：listen 内对已阻塞父 `try_send`，失败再 park/fallback（去掉每退出一次 `gen_thread`） |
| **Parent** | 活父 | `wait4`：收 notify → `REAPED` → `TASK_REAP_SYNC` |

### 为何曾经反复 `send done` / 无 `enter`

旧实现把 **每条** `THREAD_REAP` 丢进通用 `ipc_server_recv_loop_per_msg_worker`，再叠加：

1. **每 CPU 一个 listen** 抢同一个 `clean_listen`，各自私有 pool；
2. listen `recv` 成功即让 client `send` 返回，再 `pending_push` 冒充派发成功；
3. worker 未跑到 handler → 永无 `THREAD_REAP enter`。

这与下面「pool 只为 EXIT_NOTIFY」矛盾。

**裁定：**

- `THREAD_REAP` / `TASK_REAP*` 在 **listen 上**处理；zombie 等待 **inline**（handshake §2）。
- 仅 `EXIT_NOTIFY` 异步（listen 不得堵在父 `wait_port` 上，否则无法收 `TASK_REAP_SYNC`）。
- **禁止**再为 clean 的 client 消息使用通用 per-msg worker pool。
- **禁止**把 `THREAD_REAP` zombie 等待 park 成 pending 却不保证随后 `delete_thread`+`EXIT_NOTIFY`：Path B `run_all` 下测例是 ash 的 **链路 A** 孩子，缺 `EXIT_NOTIFY` → 父 `wait4` 永挂（串口停在 `END test_*` 之后）。

---

## 两条链路（必须先选对）

| 条件 | 链路 | exit_state | 消息 |
|------|------|------------|------|
| **存在会 wait 的收尸方**：`ppid > 0` 且 `find_task(ppid)` | **A** | ZOMBIE(1) | `THREAD_REAP`(listen) → async `EXIT_NOTIFY` → wait4 → `TASK_REAP_SYNC`(listen) |
| **否则**（`ppid==0` / 无活父） | **B** | REAPED(2) | **仅** `THREAD_REAP`(listen 内联 `delete_thread` + `delete_task`) |

`proc_has_wait_reaper(pa)` **只**在链路 A 为 true。`ppid==0` **不是**链路 A。

### 链路 B 消息顺序

```
sys_exit:
  exit_state = REAPED(2)
  send THREAD_REAP          // 与 listen recv 会合 → send 返回
  zombie; schedule

Listen (`ipc_server_coop_loop`):
  THREAD_REAP enter
  wait until target zombie   // inline schedule + ready→zombie promote
  delete_thread
  if last && REAPED: claim + delete_task
```

Exitor **不**另发 one-way `TASK_REAP`。

### 链路 A 消息顺序

```
Exitor --THREAD_REAP--> Listen
  |                     |-- wait zombie, delete_thread
  |                     |-- if live parent: spawn EXIT_NOTIFY --wait_port--> Parent
  |                     |-- else parent gone: demote REAPED + delete_task (→B)
  | zombie              |-- (listen 立刻回到 recv / try_recv)
Parent wait4:
  recv EXIT_NOTIFY
  REAPED
  TASK_REAP_SYNC --> Listen (inline delete_task + reply)
```

多孩子：多个 EXIT_NOTIFY worker 可同时堵在父 `wait_port`；listen 仍可收 `TASK_REAP_SYNC` / 其它 `THREAD_REAP`。

**Path B 验收误判**：`/init`→`sh /tests/run_all.sh` 时，内核 cookie 只盯 `/init`；单个 `/tests/*` 退出走链路 A。串口有 `END test_*` 却无下一测例 → 先查 ash `wait4` / `EXIT_NOTIFY`，不要先怪 harness cookie。

---

## 与 SIGCHLD 的边界

**完整交互模型（时间线、Layer B、二次 wait→ECHILD、卡死归因）见 [`WAIT_AND_SIGCHLD.md`](WAIT_AND_SIGCHLD.md)。**  
下文仅保留 EXIT_CLEAN 必需的硬边界，避免两处文档漂移。

子进程退出同时产生两件事，**角色不同，禁止互相替代**：

| 通道 | 谁发 | 作用 | 是否唤醒 `wait4` 的 `recv` |
|------|------|------|---------------------------|
| **`EXIT_NOTIFY`** | EXIT_NOTIFY worker → 父 `wait_port` | **wait 的权威事件**；父据此 REAPED + `TASK_REAP_SYNC` | **是** |
| **`SIGCHLD` pending** | `sys_exit` → `linux_queue_signal(parent)` | 信号语义；层 B 在 syscall 返回前投递 | **否** |

- SIGCHLD **不得**令 wait4 在未完成收尸前 `-EINTR`（见 `WAIT_AND_SIGCHLD.md` §2）。  
- `WAIT_INTERRUPT`：非 SIGCHLD 的 EINTR，或 EXIT_NOTIFY 异步失败时的 poke（同文 §6）。  
- Layer B / restorer / RX stub：见同文 §5；**禁止** RW 栈 EXEC trampoline。

### EXIT_NOTIFY 异步失败回退

`clean_async_exit_notify` 若 alloc/spawn 失败：向父 `pending_exits` 推送 + `linux_proc_wait_poke`。禁止 listen 同步堵在 `wait_port` 上。

---

## exit_state

| 值 | 名字 | 含义 |
|----|------|------|
| 0 | RUNNING | 正常运行 |
| 1 | ZOMBIE | 链路 A：可被 wait4 收集 |
| 2 | REAPED | wait4 已提交，或链路 B 退出自标 |
| 3 | TASK_CLAIMED | 恰好一方拥有 `delete_task` |

`exit_notify_sent`：EXIT_NOTIFY 至多一次（仅链路 A）。

---

## 角色与消息

| 消息 | 方向 | 语义 | 阻塞？ |
|------|------|------|--------|
| `THREAD_REAP` | exitor → clean_listen | listen 内联：`delete_thread`；链路 B 可接 `delete_task`；链路 A 末线程 → async EXIT_NOTIFY | 至 **listen recv**（随后 listen 在同线程处理，可 `schedule` 等 zombie） |
| `EXIT_NOTIFY` | notify-worker → 活父 wait_port | 链路 A | 是（故不能在 listen 上做） |
| `TASK_REAP` | （遗留）→ clean_listen | 认领后 `delete_task` | one-way；链路 B 退出不用 |
| `TASK_REAP_SYNC` | wait4 → clean_listen + reply | 认领后 `delete_task` | RPC 全程阻塞；**listen 内联** |

---

## 为何必须有 EXIT_NOTIFY 异步（不是通用 THREAD_REAP pool）

链路 A 下 `EXIT_NOTIFY` 合法阻塞在父 `wait_port`。若 listen 自己发：

- 父接着要 `TASK_REAP_SYNC` → 同一 `clean_listen`；
- listen 堵在 EXIT_NOTIFY → **死锁**。

故：**只有 EXIT_NOTIFY 离开 listen**。`THREAD_REAP` 本身（等 zombie / `delete_thread`）用 `schedule` 即可，不必进 pool。

---

## 握手要点

1. Exitor：`send(THREAD_REAP)` 与 listen `recv` 会合 → 再 zombie。  
2. Listen：同线程跑 handler；等 zombie 时 `schedule`（exitor 才能 zombie）。  
3. EXIT_NOTIFY：async one-shot；listen 不阻塞。  
4. TASK_REAP_SYNC：listen 内联；reply 用 **`ipc_rpc_reply`（blocking）**。  
5. 全局 **一个** `clean_listen` 线程（BSP）；禁止每 CPU 再挂一个抢同一端口的 listen。  
6. VFS / backend 等普通 RPC **同样**用 **`ipc_rpc_reply`（blocking rendezvous）**；遗弃 client 靠 reply-port teardown 唤醒 `block_on_send`。禁止用裸 `try_send`「best-effort」冒充协议（会与 client 尚未 `recv` 竞态）。

---

## Listen 上仍允许的 `schedule`（非 poll 空转）

| 位置 | 为何允许 | 禁止改成 |
|------|----------|----------|
| `clean_wait_exitor_zombie` | THREAD_REAP 会合后 exitor 可能尚未 zombie；ready→zombie promote，否则 `schedule` | 把整个 THREAD_REAP park 进 pending 却不保证随后 `EXIT_NOTIFY`（ash `wait4` 挂） |
| `clean_claim_and_delete_task` | 等 `thread_number==0` / 防双 claim | 父侧空转等 pid 消失 |
| EXIT_NOTIFY one-shot 末尾 | worker 自标 zombie 后等 listen `delete_thread` | 在 listen 上同步 `send` `wait_port` |

`poll_pending` / `clean_poll_exit_notify_jobs`：**禁止** `schedule()`。

## 相对「单线程轮询多请求进度」的差距

| 目标 | 今日 |
|------|------|
| poll 推进多个 **协议 FSM** | 仅收尸已 `finished` 的 EXIT_NOTIFY 线程 |
| EXIT_NOTIFY 不 `gen_thread` | 仍 one-shot（正确性优先于少线程） |
| 框架级 pending API | 无；job list 留在 `clean_server.c` |

收紧原则：先保证 Link A/B 与 ash `run_all`；再做 EXIT_NOTIFY `try_send`+park。**不要**为「像 VFS 一样」把 THREAD_REAP 丢回 worker 池。

---

## 明确禁止

- 把 `ppid==0` 当成链路 A（对 kernel_port 发 EXIT_NOTIFY）。  
- 把 `THREAD_REAP` 丢进通用 per-msg worker pool / pending 冒充 handoff。  
- 每 CPU 多个 listen 抢同一个 `clean_listen`。  
- 协议路径短超时 `try_send` 丢 reply。  
- wait4 在 one-way `TASK_REAP` 后空转等 pid 消失。  
- 链路 B 从 exitor 再发 one-way `TASK_REAP`。  
- listen 上同步 `EXIT_NOTIFY`（与 `TASK_REAP_SYNC` 死锁）。  
- 未认领并发 `delete_task`。  
- **用 SIGCHLD / `WAIT_INTERRUPT` 代替 `EXIT_NOTIFY` 唤醒 wait4，或因 SIGCHLD pending 对 wait4 返回 `-EINTR`（未收 EXIT_NOTIFY 即离开）。**  
  （例外：EXIT_NOTIFY 异步失败时，`pending_exits` + poke 用的 `WAIT_INTERRUPT` 只唤醒并 `try_pending`，不因此对 SIGCHLD 返回 `-EINTR`。）
- **在 RW 用户栈上种 EXEC 信号 trampoline**（与 WXN / absolute mprotect 冲突）。

---

## 代码落点

| 组件 | 路径 |
|------|------|
| 协议 / `proc_has_wait_reaper` | 本文；`sys_proc_registry.c` |
| 客户端 | `linux_layer/proc/clean_ipc.c` |
| Server | `servers/clean_server.c`（`ipc_server_coop_loop` + async EXIT_NOTIFY） |
| exit / SIGCHLD queue | `linux_layer/syscall/thread_syscall.c` |
| wait4 / EINTR 判定 | `linux_layer/proc/sys_wait.c`；`linux_signal_wait4_should_return_eintr` |
| wait 唤醒 | `linux_layer/proc/proc_wait_ipc.c`（EXIT_NOTIFY / WAIT_INTERRUPT） |
| Coop / 过渡 reply loop | `linux_layer/ipc/rpc.c`（**无** per-msg worker pool） |
