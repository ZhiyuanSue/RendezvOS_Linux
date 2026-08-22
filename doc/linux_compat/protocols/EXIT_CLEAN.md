# Exit / wait / clean_server 协议（v2）

**权威模型**：thread-only core + 堆上 **`linux_proc_resource_t`（共享资源束 / zombie 壳）**。  
**禁止**再引入 core 进程对象。

端口名：[`PORT_NAMING.md`](PORT_NAMING.md) §4 — 全局 `clean_listen`；每 CPU 一条 clean coop 线程共用该 port。

---

## 1. 三种对象（职责分离）

| 对象 | 所有者 | 销毁时机 | 谁操作 |
|------|--------|----------|--------|
| **`Thread_Base`** | core refcount | `THREAD_REAP` → `delete_thread` | clean listen |
| **`VSpace`** | `thread->vs` ownership + 调度 extra | 末次 user `ref_put` → `del_vspace` | core |
| **`linux_proc_resource_t`** | compat refcount（alloc + 每 attach 一线程） | **`linux_proc_reap`**：`fini` + 末 `ref_put` | Link B：clean；Link A：**parent `wait4`** |

`linux_proc_resource_t` **不是** core TCB 替身，而是 **Linux 共享资源束**：pid、ppid、brk、fs/signal proc 状态、wait 元数据。若干 thread 经 append **`res`** 指针共享；**vs 不拥有**（仅 `proc->vs` 非拥有缓存）。

### core 契约（compat 必须遵守）

1. **`delete_thread`** 只摘调度并 `ref_put` thread；**`append fini` / detach 在末次 thread ref 的 `del_thread_structure` 里**（`delete_thread` 返回时 IPC/EBR 可能仍持有 ref）。
2. **clean listen 在 notify/reap 前必须让 `thread_number` 归零** — sync `linux_proc_detach_thread`；若 sys_exit 曾提示 `exit_last_thread` 且仍有附着线程，再 `linux_proc_wait_all_threads_detached`。
3. **末线程 notify 门控**：以 clean 侧 **`thread_number==0`** 为准（并置 `exit_last_thread=1`）；`sys_exit` 的 `exit_last_thread` 仅为提示，覆盖并发末两线程 exit 竞态。
4. **VSpace** 与资源束独立；`linux_proc_reap` **不放** vs。

---

## 2. refcount 角色（资源束）

```
linux_proc_alloc()           → refcount = 1（creator/壳）
linux_proc_attach_thread()   → ref_get（每线程一票）
linux_proc_detach_thread()   → ref_put（append fini）
clean listen 处理 THREAD_REAP → ref_get 工作引用；Link A notify 后 ref_put
parent wait4 收尸            → linux_proc_reap → fini + ref_put（释放 zombie 壳）
EXIT_NOTIFY payload          → proc*（不必 find_proc_by_pid）
```

| 阶段 | refcount 语义 |
|------|----------------|
| 运行中 | 1 + N（N = 附着线程数） |
| 末线程 exit 后、detach 完成 | 通常剩 1（zombie 壳，pid 仍在 registry） |
| `linux_proc_reap` | `fini` + `ref_put` → 0 释放堆对象 + unregister |

**Zombie 壳**：线程已全部 detach，但 pid/`exit_code` 仍供 `wait4` 读取，直到 parent（或 Link B clean）调用 `linux_proc_reap`。

---

## 3. exit_state（v2）

| 值 | 名 | 含义 |
|----|-----|------|
| 0 | RUNNING | 正常 |
| 1 | ZOMBIE | 已 exit；Link A 待发 notify，或 Link B 待 clean inline reap |
| 2 | NOTIFIED | Link A：EXIT_NOTIFY 已提交（防重复 notify） |
| 3 | CLAIMED | `linux_proc_reap` 认领中（防双 reap） |

sys_exit **一律**置 `ZOMBIE`。Link A/B 由 clean 在 `thread_number==0` 后 **`proc_has_wait_reaper(proc)`** 判定；父进程已死时 fallback 为 Link B。

parent `wait4` 从 `ZOMBIE` 或 `NOTIFIED` 直接 `linux_proc_reap`。

---

## 4. 两条链路

| 条件 | 链路 | sys_exit `exit_state` |
|------|------|------------------------|
| `proc_has_wait_reaper(proc)`（`ppid>0` 且活父） | **A** | ZOMBIE → clean 置 NOTIFIED → parent reap |
| 否则（orphan / 父已死 / `ppid==0`） | **B** | ZOMBIE；clean inline `linux_proc_reap` |

### Link A（有 parent wait）

```
Exitor                         Listen (clean)                    Parent
  sys_exit: ZOMBIE, exit_last_thread
  THREAD_REAP ───────────────► recv
  zombie; schedule             wait exitor zombie
                               delete_thread
                               linux_proc_detach_thread(exitor)  [sync]
                               (if exit_last_thread && tn>0: wait detach)
                               EXIT_NOTIFY + proc* ────────► wait4 recv
                               (listen 回到 recv)                 wstatus
                                                                linux_proc_reap (ref_put)
```

- **单向**消息：`THREAD_REAP`、`EXIT_NOTIFY`（payload **`pid + proc* + exit_code`**，含 ref handoff）。
- parent 用消息中的 **`proc*`** 收尸，**`linux_proc_reap` = fini + ref_put**（无 parent→clean RPC）。

### Link B（无 wait reaper）

```
Exitor: ZOMBIE → THREAD_REAP → zombie; schedule
Listen: delete_thread → sync detach → (if exit_last_thread && tn>0: wait detach)
        → !proc_has_wait_reaper → linux_proc_reap
```
（Link B 同样在 `delete_thread` 后 **sync detach**；多线程时 `wait_all_threads_detached` 兜底。）

---

## 5. 消息表（v2）

| 消息 | 方向 | 作用 |
|------|------|------|
| `THREAD_REAP` | exitor → `clean_listen` | rendezvous；listen 内联 `delete_thread` + 后续 |
| `EXIT_NOTIFY` | listen try → 父 `wait_port` | Link A：唤醒 wait + **传递 proc ref**（`"q p i"`） |

EXIT_NOTIFY：`ipc_system_try_deliver` + park；**禁止** listen 阻塞 `send_msg(wait_port)`。

---

## 6. 角色

| 角色 | 职责 |
|------|------|
| Exitor | `THREAD_REAP` → zombie → `schedule` 直到 `delete_thread` |
| Listen | coop loop；THREAD_REAP inline；EXIT_NOTIFY try+park |
| Parent | `wait4`：EXIT_NOTIFY → 解码 **`proc*`** → wstatus → **`linux_proc_reap`（ref_put）** |

### 禁止项（仍有效）

- `ppid==0` 走 Link A。
- THREAD_REAP 进 worker pool / pending 撒谎。
- listen 阻塞 EXIT_NOTIFY。
- **parent / wait4 等 `thread_number==0`**（clean 已 detach 后再 notify；parent 只 reap）。
- SIGCHLD 代替 EXIT_NOTIFY 唤醒 wait4。

---

## 7. 代码落点

| 组件 | 路径 |
|------|------|
| 协议 / `proc_has_wait_reaper` | 本文；`sys_proc_registry.c` |
| THREAD_REAP 客户端 | `linux_layer/proc/clean_ipc.c` |
| Listen | `servers/clean_server.c` |
| sys_exit | `linux_layer/syscall/thread_syscall.c` |
| wait4 | `linux_layer/proc/sys_wait.c` |
| proc reap / wait detach | `linux_layer/proc/linux_proc.c` |
| EXIT_NOTIFY / wait_port | `linux_layer/proc/proc_wait_ipc.c` |

---

## 8. 与 SIGCHLD

见 [`WAIT_AND_SIGCHLD.md`](WAIT_AND_SIGCHLD.md)。**EXIT_NOTIFY** 唤醒 wait4；SIGCHLD 仅 pending，不得令 wait4 误 EINTR。

---

## 附录 A — v1 → v2 与实现对照（迁移清单）

以下为 **v2 要求** vs **迁移前实现**；实现应逐项收敛到 v2。

| # | v2 要求 | 迁移前 / 不符合点 | 目标改法 |
|---|---------|-------------------|----------|
| A1 | Link A 无 TASK_REAP_SYNC | `wait4_finish_reap` → `linux_clean_task_reap_sync` | parent 直接 `linux_proc_reap` |
| A2 | notify 前 detach 完成 | clean 在 `delete_thread` 后立即 notify | **`delete_thread` 后同步 `linux_proc_detach_thread`**；多线程时 `wait_all_threads_detached` 兜底 |
| A3 | 末线程 notify 门控 | 曾仅用 `exit_last_thread` 快照 | **clean：`thread_number==0`** + 置 `exit_last_thread` |
| A4 | parent 不等 detach | 曾 `wait4` 内 wait detach → 与 clean recv 死锁 | parent 只 reap；detach 仅在 clean 等 |
| A5 | `linux_proc_reap` 认领 ZOMBIE/NOTIFIED | 仅 REAPED+CLAIMED 在 clean_claim | **`linux_proc_reap` 内 CAS → CLAIMED** |
| A6 | init orphan reap | v1 kernel_port + init reaper queue | **Link B**：clean inline `linux_proc_reap` |
| A7 | 无 v1 clean opcode | `KMSG_OP_CLEAN_TASK_REAP*` | **已删除**；仅 `THREAD_REAP` |
| A8 | wait ECHILD 语义 | 仅查 ZOMBIE → 子仍运行就 ECHILD | **RUNNING 或 ZOMBIE** 可 block |
| A9 | 文档 / DATA_MODEL / SIGCHLD | 仍写 TASK_REAP_SYNC | 同步更新（v2 已更新） |

### v1 根因摘要（为何必须 v2）

1. **TCB 时代**：`delete_task` 晚于 `delete_thread`，`thread_number==0` 与 notify 同时发生。  
2. **thread 模型**：detach 随 `fini` 延迟（EBR/IPC ref）；在 `delete_thread` 后立刻看 `thread_number==0` 会漏 EXIT_NOTIFY。  
3. **TASK_REAP_SYNC**：parent RPC 回 clean listen，与 listen `recv` **争用同一 port**；parent 若再 wait detach 则 **经典死锁**。

v2 原则：**clean 管 thread 物理删除 + notify；parent 管 zombie 壳 reap；一条 RPC 回去收尸的设计与 thread-only core 不兼容。**

---

## 附录 C — 设计意图（资源束 + ref 传递）与分阶段落地

### C.1 你的目标模型（thread-only core 下）

`linux_proc_resource_t` **保留**，但语义是 **共享资源束**（pid / brk / fs / signal / wait 元数据），**不是** core 进程对象替身：

```text
若干 Thread ──attach/ref_get──► linux_proc_resource_t (refcount)
                                      │
                    VSpace ◄── thread->vs（各线程持有；proc->vs 仅非拥有缓存）
```

**末线程 exit 时序（Link A）**：

```text
Exitor                Clean (listen)                         Parent (thread)
  sys_exit              │
  THREAD_REAP ─────────►│ ref_get_not_zero(proc)  [工作引用]
                        │ delete_thread
                        │ linux_proc_detach_thread(exitor)  [sync]
                        │ (if exit_last_thread && tn>0: wait detach)
                        │ （资源束上 thread 票已归零；壳 refcount≈1）
                        │ EXIT_NOTIFY + 传递 proc* ────────► recv
                        │ ref_put(工作引用)                      读 wstatus
                        │                                       linux_proc_reap（无 RPC）
```

要点：

1. **单向消息**：`THREAD_REAP`、`EXIT_NOTIFY` only。
2. **Clean** 在 notify 前完成 thread 物理删除 + detach 等待；Link A **不** inline `linux_proc_reap`。
3. **Parent** 收到 notify 后使用消息中的 **`proc*`** 读 status 并 **`linux_proc_reap`（fini + ref_put）** — 不用 `find_proc_by_pid`，不用 RPC。
4. **VSpace** 与资源束独立；懒切换策略不变。

### C.2 实现对照（当前代码）

| 项 | 状态 |
|----|------|
| 去掉 v1 `KMSG_OP_CLEAN_TASK_REAP*` / wire fmt | ✅ |
| clean：`delete_thread` 后 **同步 `linux_proc_detach_thread`** | ✅ |
| clean：`wait_all_threads_detached` 仅作多线程兜底 | ✅ |
| `exit_last_thread` + clean `thread_number==0` 门控 | ✅ |
| 并发末两线程 exit：`tn==0` 兜底 notify | ✅ |
| EXIT_NOTIFY **`"q p i"`** + proc* 传递（无 registry 查 pid） | ✅ |
| parent **`wait4` 解码 `proc*`** → **`linux_proc_reap`（ref_put）** | ✅ |
| `pending_exits` 持有 **`proc*`** handoff | ✅ |
| `linux_proc_resource_t` 类型 rename | ✅ |
| 移除 v1：`find_zombie_child*`、blocking `post_exit_notify`、kernel_port init reaper | ✅ |
| `exit_state`：`NOTIFIED` 替代 `exit_notify_sent`；Link B 由 `proc_has_wait_reaper` 判定 | ✅ |

### C.3 附录 A 实现状态

| # | 要求 | 代码状态 |
|---|------|----------|
| A1–A9 | 见附录 A 表 | ✅ |
| B1–B3 ref 传递 | 见 C.2 | ✅ |
| B4 类型 rename | 已完成 | ✅ |

---

## 附录 B — 历史（旧 core 进程对象）

旧 core：`vs` 在 TCB 上，`delete_task` 才放 vs / notify。已删除；compat 不得假设该顺序。
