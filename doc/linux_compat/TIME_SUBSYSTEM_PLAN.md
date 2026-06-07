# Time 子系统计划（linux_layer + core 契约）

> **Phase**: 3.5（VFS 之前）  
> **Last updated**: 2026-05-19  
> **Index**: [`PROGRESS.md`](PROGRESS.md)  
> **Core contract**: [`GOALS_AND_CORE_CONTRACT.md`](GOALS_AND_CORE_CONTRACT.md) §5 P2

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
  linux_time.h          # uapi: timeval, timespec, clockid_t, tms
  linux_ktime.h         # kernel-side: ktime_t, linux_time_now_*()

linux_layer/time/
  linux_ktime.c         # 读时钟（委托 core 或 jeffies 回退）
  linux_time_sleep.c    # nanosleep / clock_nanosleep
  sys_gettimeofday.c
  sys_times.c
  sys_clock_gettime.c   # phase 3.5b
  sys_nanosleep.c
```

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

## 7. 文件清单（计划新增）

| 文件 | 职责 |
|------|------|
| `include/linux_compat/time/linux_time_types.h` | timeval, timespec, tms, CLOCK_* |
| `include/linux_compat/time/linux_ktime.h` | 门面对外 |
| `linux_layer/time/linux_ktime.c` | 时钟读 |
| `linux_layer/time/linux_time_sleep.c` | sleep 公共逻辑 |
| `linux_layer/time/sys_gettimeofday.c` | |
| `linux_layer/time/sys_times.c` | |
| `linux_layer/time/sys_nanosleep.c` | |
| `linux_layer/time/sys_clock_gettime.c` | 可选 T2 |
| `linux_layer/syscall/syscall_entry.c` | 新增 case |

Makefile: 确认 `linux_layer/*/*/*.c` 或等价规则已覆盖 `linux_layer/time/*.c`。

---

## 8. 验证

- **Minimum**: #16/#17/#20 stdout 无 ERROR；双架构各跑一遍 harness
- **Record**: [`CROSS_ARCH_VERIFICATION_LOG.md`](CROSS_ARCH_VERIFICATION_LOG.md) 新 gate 节
- **TEST_MATRIX**: 新增 §I time changes（待补）

---

## 9. 风险

| 风险 | 缓解 |
|------|------|
| 档 A 10 ms sleep 测例超时/ flaky | T1 用 schedule+yield；尽快推档 B |
| aarch64 无 RTC → REALTIME=0 | 文档化；测例若只比 monotonic 则 OK |
| nanosleep + signal EINTR | 接 `linux_deliver_pending_signals` 取消 sleep |
| core 变更被拒 | 档 A 可长期支撑“能跑通测例”，非生产级 |
