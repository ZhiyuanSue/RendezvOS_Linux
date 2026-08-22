# wait4 ↔ SIGCHLD ↔ Layer B

**地位**：与 [`EXIT_CLEAN.md`](EXIT_CLEAN.md) 配套的权威交互模型。

- `EXIT_CLEAN`（v2）：谁删线程 / 谁发 `EXIT_NOTIFY` / `exit_state`；parent **本地** `linux_proc_reap`。
- **本文**：父进程在阻塞 wait、收尸返回、信号投递、再 wait 上的合法顺序与禁止项。

**实现纪律**：先改本文并达成一致，再改 `linux_layer/`。

---

## 1. 两个通道，两种职责（不可互相替代）

```text
                    ┌─────────────────────────────────────┐
  child exit        │  Channel R — Reap (EXIT_CLEAN v2) │
  ─────────────────►│  EXIT_NOTIFY → wait4 → linux_proc_reap (local) │
                    └─────────────────────────────────────┘
  child exit        ┌─────────────────────────────────────┐
  ─────────────────►│  Channel S — Signal (Layer A/B)     │
                    │  pending SIGCHLD → Layer B 投递      │
                    │  （不唤醒 wait 的 recv）               │
                    └─────────────────────────────────────┘
```

| | Channel R（收尸） | Channel S（信号） |
|--|------------------|-------------------|
| 真源消息 | `EXIT_NOTIFY` / `pending_exits` | `linux_queue_signal(SIGCHLD)` |
| 唤醒 wait `recv`？ | **是** | **否** |
| 用户可见结果 | `wait4` 返回 pid / status | handler 运行或 DFL/IGN |

---

## 2. 硬边界

1. **SIGCHLD 不得**令 `wait4` / `waitid` 在未完成收尸前返回 `-EINTR`。
2. **`WAIT_INTERRUPT`** 仅用于：
   - 非 SIGCHLD 的可中断信号；或
   - EXIT_NOTIFY 异步失败时的 `pending_exits` + poke（唤醒后 `try_pending`，不对 SIGCHLD 返回 `-EINTR`）。
3. Layer B 投递 SIGCHLD handler 时：
   - 优先 `SA_RESTORER`；
   - 否则使用**每进程 RX stub 页**（`rt_sigreturn`）；
   - **禁止**在 RW 用户栈上种 EXEC trampoline。

---

## 3. 合法时间线（简图）

```text
T1  child exit → EXIT_NOTIFY + SIGCHLD pending
T2  parent wait4 被 EXIT_NOTIFY 唤醒 → wstatus → linux_proc_reap → 返回 child pid
T3  syscall 出口：Layer B 可投递 pending（含 SIGCHLD）
T4  parent 再 wait(-1) → -ECHILD（Linux 合法，表示无未收尸子进程）
```

验收 smoke（Path B busybox ash）：

```text
sh -c 'echo SHELL_OK; /bin/busybox ls /bin; echo AFTER_LS'
```

期望：打印 `SHELL_OK`、目录列表、`AFTER_LS`，随后正常关机路径。

---

## 4. 历史坑（勿再误判）

| 误判 / 禁止项 | 正确归因 |
|---------------|----------|
| 用 SIGCHLD / `WAIT_INTERRUPT` 代替 EXIT_NOTIFY 唤醒 wait | Channel R 才是 wait 权威事件 |
| 在 RW 栈种 EXEC trampoline | WXN / mprotect；用 `SA_RESTORER` 或 RX stub |
| 把 ash 在 `wait→ECHILD` 后卡死归因于「wait 出口投递 SIGCHLD」 | **否证**：延后投递后 hang 仍在 |
| 上述 hang 的真因 | **core `clone_vspace` COW prep**：子 PTE 曾保留 WRITE，父已 RO → 子 `forkchild` 无故障写坏父共享 `.data`/`.bss`（含 ash `g_parsefile`）→ 父随后用户态死循环。修复：父子两侧 COW 页均 RO |

兼容层不承担该 core COW 契约；`linux_vspace.c` 注释仅描述期望。

---

## 5. 相关实现入口

| 主题 | 路径 |
|------|------|
| wait4 / EINTR 判定 | `linux_layer/proc/sys_wait.c`；`linux_signal_wait4_should_return_eintr` |
| wait 唤醒 / poke | `linux_layer/proc/proc_wait_ipc.c` |
| EXIT_NOTIFY 失败回退 | `servers/clean_server.c` |
| Layer B / RX stub | `linux_layer/signal/signal_deliver.c` |
| aarch64 clone ABI（tls / child_tid） | `linux_layer/syscall/syscall_entry.c` |
