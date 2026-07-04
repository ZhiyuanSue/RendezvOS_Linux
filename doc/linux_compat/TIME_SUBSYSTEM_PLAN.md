# Time 子系统计划（linux_layer + core 契约）

> **Phase**: 3.5（VFS 之前）  
> **Last updated**: 2026-06-13  
> **Index**: [`PROGRESS.md`](PROGRESS.md)  
> **Core contract**: [`GOALS_AND_CORE_CONTRACT.md`](GOALS_AND_CORE_CONTRACT.md) §5 P2  
> **Signal cross-ref**: [`SIGNAL_IMPLEMENTATION_STATUS.md`](SIGNAL_IMPLEMENTATION_STATUS.md) §Layer A（sleep 唤醒）

---

## 0. 实施状态（2026-06）

**已落地**（compat-only，无 core 变更）：

| 能力 | 实现 |
|------|------|
| MONOTONIC / REALTIME 读 | `rendezvos_time_now()` + 启动 arch 快照（`linux_ktime.c`） |
| x86 REALTIME | `arch/time_arch_x86_64.c`：CMOS RTC → Unix epoch |
| aarch64 REALTIME | `arch/time_arch_aarch64.c`：DTB `arm,pl031` → RTCDR |
| 阻塞 sleep | `rendezvos_timer_event` + 每线程 `sleep_port` + `recv_msg`（`linux_time_sleep.c`） |
| EINTR | 信号向 `sleep_port` 投 CANCEL 唤醒 → `nanosleep` 返回 remainder（§10–§11） |
| Syscalls | `gettimeofday`, `times`（tms 存根）, `nanosleep`, `clock_nanosleep`, `clock_gettime`, `uname` |

**验证**：x86_64 / aarch64 harness **52/52 PASS**（#16–#20 通过）。

**未做**：`times` 真实 CPU 时间（待 core per-thread utime/stime）；`settimeofday`；vDSO。

---

## 1. 目标

支撑测例 **#16 gettimeofday**、**#17 sleep**、**#20 times**，并为后续 `clock_gettime` / `clock_nanosleep` 打基础。

**Harness 现状**: syscall NR 已定义于 `include/arch/*/syscall_ids.h`，但 `syscall_entry.c` **无分发** → `default` → 未实现。

---

## 2. core 已有能力（静态调研）

### 2.1 便携层

| 符号 | 位置 | 语义 |
|------|------|------|
| `jeffies` | `core/kernel/time/time.c` | 全局 tick 计数（100 Hz） |
| `SYS_TIME_MS_PER_INT` = 10 ms | `core/include/rendezvos/time.h` | tick 粒度 |
| `INT_PER_SECOND` = 100 | 同上 | |
| `udelay` / `mdelay` | `time.c` | **忙等**，非 sleep |
| `rendezvos_do_time_irq` | `time.c` | 定时器 IRQ → `jeffies++` |

### 2.2 x86_64

| 组件 | 位置 | 可用性 |
|------|------|--------|
| 8254 PIT + APIC timer | `core/arch/x86_64/time/`, `LocalAPIC.c` | 100 Hz IRQ ✅ |
| `get_rtc_time()` | `core/arch/x86_64/time/rtc.c` | **启动时读一次** CMOS，未维护 wall clock |
| TSC / APIC counter | `LocalAPIC.c` | 内部校准，**无** ns 级对外 API |

### 2.3 aarch64

| 组件 | 位置 | 可用性 |
|------|------|------|
| Generic timer (CNTP) | `core/arch/aarch64/time/generic_time.c` | 100 Hz IRQ ✅ |
| `get_phy_cnt()` / `get_virt_cnt()` | 同上 `.c` 内 | **未导出**到 `arch/aarch64/time.h` |

### 2.4 缺失（两架构共有）

- 单调 **纳秒**时钟统一 API（`ktime_get`-style）
- **定时阻塞** / timed wakeup（scheduler 无 timer wait queue）
- 进程 **CPU 时间** 字段（`times()` 需要 utime/stime）
- Wall clock **epoch 偏移**（set/gettimeofday 可调）

---

## 3. 对外接口设计（linux_layer 视角）

建议在 `linux_layer/time/` 增加 **compat 时间门面**，syscall 只调门面，不直接摸 arch：

```text
include/linux_compat/time/
  linux_time_types.h    # timeval, timespec, tms, CLOCK_*, utsname
  linux_time_arch.h     # linux_time_arch_boot_realtime_us()
  linux_ktime.h         # 便携门面 + sleep API
  linux_time_sleep.h    # wake_for_signal / port_teardown（signal、exit 用）

linux_layer/time/
  linux_ktime.c         # 时钟读 + resolve_deadline
  linux_time_sleep.c    # timer + IPC 阻塞 sleep
  arch/
    time_arch_x86_64.c
    time_arch_aarch64.c
  sys_gettimeofday.c
  sys_times.c
  sys_clock_gettime.c
  sys_nanosleep.c
  sys_uname.c
```

与 `linux_layer/signal/arch/` 相同模式：**便携头 + 便携 `.c` + `arch/` 子目录**。

### 3.1 门面 API（`linux_ktime.h`）

```c
/* Monotonic ns since boot — primary source for sleep + CLOCK_MONOTONIC */
u64 linux_time_monotonic_ns(void);

/* Wall clock ns since Unix epoch — may be coarse until RTC sync */
u64 linux_time_realtime_ns(void);

/* Optional: boot snapshot for REALTIME without core change (x86 RTC once) */
void linux_time_init_from_arch(void);   /* called from linux initcall */
```

### 3.2 Syscall 映射（最小集）

| Syscall | x86_64 NR | aarch64 NR | 第一版语义 |
|---------|-----------|------------|------------|
| `gettimeofday` | 96 | 169 | REALTIME → `linux_time_realtime_ns()` |
| `times` | 100 | 153 | utime/stime 来自 per-task 计数（初值 0） |
| `nanosleep` | 35 | 101 | 阻塞至 monotonic + req（见 §4） |
| `clock_gettime` | 228 | 113 | `CLOCK_MONOTONIC` + `CLOCK_REALTIME` 最小集 |

`test_sleep` (#17) 日志显示 **NR 96 = gettimeofday**（x86）；测例可能同时需要 **nanosleep** — 实现时需对照 user payload 源码确认。

---

## 4. 实现路径（两档）

### 档 A — compat-only bootstrap（**无需 core 变更**，精度差）

| 能力 | 实现 |
|------|------|
| MONOTONIC | `jeffies * 10_000_000` ns |
| REALTIME | x86: 启动 RTC 快照 + jeffies delta；aarch64: boot=0 或设备树占位 |
| nanosleep | 在 `jeffies` 上 **schedule 循环**（10 ms 粒度），非忙等 |
| times | 返回 0；`sysconf(_SC_CLK_TCK)` 用 100 |
| EINTR | signal 打断 sleep 时返回 remainder（接 signal 路径） |

**优点**: 快速解锁 #16/#17/#20 stdout。  
**缺点**: 10 ms 粒度；REALTIME 不准；sleep 占调度槽。

### 档 B — core P2 增量（**需 maintainer 批准**，正确语义）

按 `GOALS_AND_CORE_CONTRACT.md` P2 提案：

| core 增量 | 用途 |
|-----------|------|
| `u64 rendezvos_ktime_monotonic_ns(void)` | 统一单调源（x86: TSC/APIC；aarch64: CNTVCT） |
| `void rendezvos_ktime_init(void)` | boot 校准、频率 |
| `error_t thread_sleep_until_ns(Thread_Base *, u64 deadline_ns)` | 真 sleep + timer IRQ 唤醒 |
| 可选：`struct task_cputime` in TCB append | `times()` |

linux_layer 档 B  syscall 层 **接口不变**，只替换 `linux_ktime.c` 后端。

---

## 5. 推荐分步计划

| Step | 内容 | core? | 验证 |
|------|------|-------|------|
| **T0** | uapi 头 + `syscall_entry` 分发 stub | 否 | 编译 |
| **T1** | 档 A：`gettimeofday` + `times` + jeffies sleep | 否 | #16/#20；#17 部分 |
| **T2** | `clock_gettime(MONOTONIC/REALTIME)` 共用 backend | 否 | libc 基础 |
| **T3** | 向 maintainer 提交 **档 B core 方案**（API + SMP + signal EINTR） | **提案** | — |
| **T4** | 档 B 落地后：细粒度 nanosleep + CPU time | 是 | #17 精确 sleep |
| **T5** | `uname` stub（#19）可与 T1 并行 | 否 | #19 |

**与 VFS 关系**: time 不依赖 FS；**应在 Phase 4 之前**完成 T1–T2，减少 harness stdout 噪音。

---

## 6. core 变更提案提纲（供 review，未实施）

```
1. rendezvos/time.h
   + u64 rendezvos_ktime_monotonic_ns(void);
   + u64 rendezvos_ktime_realtime_ns(void);  /* optional boot offset */

2. arch/*/time/
   x86_64: export TSC or APIC tick → ns (calibrated at boot)
   aarch64: export CNTVCT_EL0 via header; document CNTFRQ

3. task/
   + thread timer wait: TCB flag + sorted per-CPU timer queue OR
     reuse jeffies compare in schedule() path
   + thread_sleep_until_ns() — block, wakeup on IRQ if deadline passed

4. SMP: jeffies update already has TODO lock; ktime read must be coherent
```

**不在此计划内**: POSIX timer (`setitimer`)、`alarm`、高精度 itimer 信号投递。

---

## 7. 文件清单（已新增）

| 文件 | 职责 |
|------|------|
| `include/linux_compat/time/linux_time_types.h` | uapi 类型 |
| `include/linux_compat/time/linux_time_arch.h` | arch 快照 API |
| `include/linux_compat/time/linux_ktime.h` | 门面对外 |
| `include/linux_compat/time/linux_time_sleep.h` | sleep 与 signal/exit 交叉 API |
| `linux_layer/time/linux_ktime.c` | 时钟读 + deadline |
| `linux_layer/time/linux_time_sleep.c` | 阻塞 sleep |
| `linux_layer/time/arch/time_arch_*.c` | 每 arch REALTIME 快照 |
| `linux_layer/time/sys_*.c` | syscall 入口 |
| `linux_layer/syscall/syscall_entry.c` | 分发 |
| `include/linux_compat/proc_compat.h` | `sleep_port` / `sleep_timer_token`（thread append） |

Makefile: `linux_layer/*/*.c` 与 `linux_layer/*/*/*.c` 已覆盖 `time/` 与 `time/arch/`。

---

## 8. 验证

- **Minimum**: #16/#17/#20 stdout 无 ERROR；双架构各跑一遍 harness
- **Record**: [`CROSS_ARCH_VERIFICATION_LOG.md`](CROSS_ARCH_VERIFICATION_LOG.md) 新 gate 节
- **TEST_MATRIX**: 新增 §I time changes（待补）

---

## 9. 风险

| 风险 | 缓解 |
|------|------|
| x86 RTC 非 UTC | 文档化；测例通常只比单调性 |
| aarch64 无 PL031 → REALTIME=0 | DTB 探测失败回退 0 |
| `times()` tms 全 0 | 待 core utime/stime + wait 聚合 cutime |
| `post_cancel` 投递失败 | 极端路径等到 timer；可后续加重试 |
| core 变更被拒 | 当前 timer+IPC 路径已满足 #17 |

---

## 10. 阻塞 sleep 架构（已实现）

### 10.1 模型

与 core `rendezvos_timer_event` **model B** 一致（见 `core/docs/ipc.md` §10、`single_timer_test.c`）：

1. 每线程懒创建 IPC 端口 `linux_sleep_<tid>`，存入 `linux_thread_append_t.sleep_port`。
2. `rendezvos_timer_event_init(0, sleep_port, token)` + `add(expires_at)`，`expires_at` 在 `arch_timer_read()` 域。
3. `recv_msg(sleep_port)` 真阻塞（`thread_status_block_on_receive`）。
4. 定时器 IRQ → `KMSG_OP_SYSTEM_TIMER_EXPIRE` → `ipc_system_try_deliver(sleep_port)` → 唤醒。
5. 循环直至 `rendezvos_time_now() >= deadline`；`sys_exit` 时 `linux_time_sleep_port_teardown()` → `unregister_port`。

`sleep_armed`：已移除。阻塞语义由 core `thread_status_block_on_receive` + `Thread_Base.port_ptr` 表达；compat 仅在 `port_ptr == sleep_port` 时向该 port 投 CANCEL。

### 10.2 Linux `nanosleep` 语义

| 事件 | 行为 |
|------|------|
| 时间到 | syscall 返回 0 |
| 可投递信号 | syscall 返回 `-EINTR`，`rem` 填剩余时间 |
| 信号 handler 返回后 | **不**自动续睡；用户态用 `rem` 再调 `nanosleep` |
| `SA_RESTART` | 不重试 `nanosleep`（与 Linux 一致） |

### 10.3 port 上的消息类型

| opcode | 来源 | sleeper 处理 |
|--------|------|----------------|
| `TIMER_EXPIRE` | 定时器 IRQ | 若未到 deadline 则继续循环 |
| `TIMER_CANCEL` | 信号唤醒 / 本地 disarm | 返回 EINTR + remainder |
| 错 token | 陈旧消息 | `disarm` 后重 arm |

---

## 11. 与信号子系统的协调（非「信号走 port」）

### 11.1 信号主路径未改

Phase 2B 仍是两层（见 [`IPC_BASED_SIGNAL_DESIGN.md`](IPC_BASED_SIGNAL_DESIGN.md)）：

- **层 A（产生）**：`linux_queue_signal` → 写 proc/thread `pending`；**不**把信号 payload 放进 IPC。
- **层 B（投递）**：`syscall_entry` 返回用户态前 `linux_deliver_pending_signals(tf)` → handler / 默认动作。

设计文档明确 **反对**「每线程 `signal_port` + `recv_msg` 收信号」作为主路径。

### 11.2 与 core IPC 阻塞状态的关系

`recv_msg` / `send_msg` 在等不到对端时（`core/kernel/ipc/ipc.c`）：

| 调用 | 阻塞前 `thread_set_status` | 挂到 port 上 |
|------|---------------------------|--------------|
| `recv_msg` | `thread_status_block_on_receive` | `port_ptr = port` |
| `send_msg` | `thread_status_block_on_send` | `port_ptr = port` |

成功配对后 core 才把对端从 `block_on_*` 改回 `ready`。**compat 不得**对 `block_on_receive` / `block_on_send` 线程直接 `thread_set_status(ready)`，否则破坏 port 队列匹配（与 `wait4` 父进程阻塞同理）。

`signal_queue_on_thread_helper` 逻辑：

```text
pending += sig
if status == block_on_receive && port_ptr == sleep_port
    → linux_time_sleep_wake_for_signal()
else if status == block_on_receive && port_ptr == wait_port_<pid>  (thread_lookup)
    → linux_proc_wait_wake_for_signal()
else if status == block_on_receive && other port_ptr
    → linux_ipc_recv_wake_for_signal()   // RPC reply ports, …
else if status != running
    → thread_set_status(ready)
```

**不再使用** compat 层 `sleep_armed` 标志——避免与 core status 重复。

### 11.3 与 `wait4` 的类比

| 场景 | 阻塞点 | 唤醒消息 | 消息是否「就是」事件本身 |
|------|--------|----------|--------------------------|
| `wait4` | `recv_msg(wait_port)` | `EXIT_NOTIFY` / `WAIT_INTERRUPT` | 否；reap 看 `exit_state` |
| `nanosleep` | `recv_msg(sleep_port)` | `TIMER_EXPIRE` / `TIMER_CANCEL` | EXPIRE=到时；CANCEL=打断 sleep |
| `ipc_rpc_call` | `recv_msg(reply_port)` | `IPC_RECV_INTERRUPT` | 否；返回 `-EINTR` |
| 信号（通用） | 任意 / ready 队列 | （无 port） | pending 位图 + defer 投递 |

**结论**：port 仅用于 **打断特定阻塞 syscall**，不是信号传递通道。
