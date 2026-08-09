# Linux 兼容层 — 进展与追溯索引

> **Purpose**: 单一入口，把 **路线图 → 实现状态 → 验证证据 → 决策** 串起来。  
> **Last updated**: 2026-08-09（busybox bring-up 收尾；妥协账归档；见 [`NEXT_PLAN.md`](NEXT_PLAN.md)）

---

## 1. 文档怎么读（追溯链）

```text
SYSCALLS.md              阶段路线图（只看未来 + 顶表）
    ↓
PROGRESS.md (本文)       当前阶段、缺口汇总、下一步、§8 工作区盘点
    ↓
*_IMPLEMENTATION_STATUS  各子系统 live 状态（signal / wait / execve / time）
    ↓
CROSS_ARCH_VERIFICATION_LOG.md   配对运行证据（append-only）
    ↓
doc/ai/DECISIONS.md      非显然设计选择（ADR-lite）
```

**规则**

- 测例数字、stdout 细节 → **verification log**，不写进 roadmap。
- 实现细节 / 已知 gap → **status 文档**，roadmap 只写阶段目标。
- 历史阶段报告 → [`archive/README.md`](archive/README.md)（只读，指标可能过时）。
- 启动路径**怎么演进到今天** → [`BOOT_PATH_EVOLUTION.md`](BOOT_PATH_EVOLUTION.md)。
- busybox 收尾后的下一步 → [`NEXT_PLAN.md`](NEXT_PLAN.md)。
- ~~busybox 妥协 live 清单~~ → [`archive/BUSYBOX_BOOT_DEFERRALS.md`](archive/BUSYBOX_BOOT_DEFERRALS.md)（**已归档**）。
- **未提交工作区** → 本文 **§8**（提交前以 `git status` 为准）。

---

## 2. 阶段总览（当前共识）

| 阶段 | 状态 | 目标 | 状态文档 |
|------|------|------|----------|
| **0** | ✅ | syscall 框架 | — |
| **1** | ✅ | fork/exit/wait/brk/mmap | [`archive/PHASE1_SUMMARY.md`](archive/PHASE1_SUMMARY.md) |
| **2A–2D** | ✅ | clone + 信号 + 缺页 SIGSEGV | [`SIGNAL_IMPLEMENTATION_STATUS.md`](SIGNAL_IMPLEMENTATION_STATUS.md), [`WAIT4_IMPLEMENTATION_STATUS.md`](WAIT4_IMPLEMENTATION_STATUS.md) |
| **2E** | ✅ | sigaltstack / SA_ONSTACK (#21) | 2026-06 gate #21 PASS |
| **3** | 🔧 进行中 | execve（VFS 路径可用；PID1 已走 replace_image） | [`EXECVE_IMPLEMENTATION_STATUS.md`](EXECVE_IMPLEMENTATION_STATUS.md) · [`NEXT_PLAN.md`](NEXT_PLAN.md) |
| **3.5** | ✅ | time 子系统（gettimeofday/nanosleep/times/uname） | [`TIME_SUBSYSTEM_PLAN.md`](TIME_SUBSYSTEM_PLAN.md) |
| **4** | 🔧 **bootstrap ✅ / 目录与动态表 ✅** | VFS + initramfs | [`VFS_IMPLEMENTATION_STATUS.md`](VFS_IMPLEMENTATION_STATUS.md) · [`VFS_DYNAMIC_STORAGE.md`](VFS_DYNAMIC_STORAGE.md) |
| **5** | 📋 待做 | IPC/socket/rlimit 等 | [`SYSCALLS.md`](SYSCALLS.md) |

**Cross-arch gate (最新正式)**: 2026-07-09 — x86_64 + aarch64 **52/52 harness PASS**（**incbin / 内核 harness**）→ [`CROSS_ARCH_VERIFICATION_LOG.md`](CROSS_ARCH_VERIFICATION_LOG.md) §2026-07-09

**busybox / run_all（工作区）**: 2026-08-02 x86_64 `x86_64_run.log` → **`pass=52 fail=0`**（`#PF @ 0x57f485` 簇已消；与迁移 busybox 前 harness 全绿对齐）。aarch64 多核墙钟慢：偏 **idle busy-`schedule` × QEMU**（`SMP=1` 很快）；见 deferrals。

**RPC coop**: VFS/backends 已切 coop；**READ/WRITE** 嵌套 park（`vfs_coop.c`）；OPEN/namespace 等仍同步嵌套。见 [`protocols/IPC_RPC_FRAMEWORK.md`](protocols/IPC_RPC_FRAMEWORK.md)。

**core**: port ops gate 等若仍在子模块工作区，需维护者单独审阅。

---

## 3. 当前缺口（按优先级）

### P0 — #49 wait stdout（compat，已修）

| 现象 | 根因 | 状态 |
|------|------|------|
| #49 内三子测例 stdout FAIL（harness 仍 PASS） | `SIGCHLD` 默认动作误触发 wait4 EINTR，zombie 未 reap | **已修**；x86_64 + aarch64 post-fix **3/3 PASS** |

详见 [`WAIT4_IMPLEMENTATION_STATUS.md`](WAIT4_IMPLEMENTATION_STATUS.md)、verification log §2026-06-13。

### P0 — busybox bring-up（✅ 阶段关闭，2026-08-09）

| 项 | 状态 |
|----|------|
| `ls /bin` demo | ✅ |
| 用户态 `run_all.sh` 编排（替代内核 manifest 循环） | ✅ 默认；pack 生成显式 `run_one` |
| PID1：`linux_exec_replace_image("/init")` + 上层注入 cmdline | ✅ |
| IPC reply 会合楔死（VFS uninterruptible + ops gate + post-send 不弃 recv） | ✅ |
| fork/clone `#PF` @ `0x57f485`（status=139） | ✅ 2026-08-02 x86 `run_all` 52/52 |
| RPC coop + nested reply transfer | ✅ |
| 妥协 live 清单 | ✅ 已归档 → [`archive/BUSYBOX_BOOT_DEFERRALS.md`](archive/BUSYBOX_BOOT_DEFERRALS.md) |

后续（UART server、Phase 5 等）→ [`NEXT_PLAN.md`](NEXT_PLAN.md)。叙事 → [`BOOT_PATH_EVOLUTION.md`](BOOT_PATH_EVOLUTION.md)。

### P1 — execve 补完（Phase 3）

| 项 | 状态 |
|----|------|
| 内嵌 ELF + argv | ✅ #03/#43/#52 |
| initramfs / VFS execve | ✅ #8 stdout `execve success`（2026-07-09） |
| envp / auxv | ✅ busybox：`HWCAP`/`EXECFN`/`RANDOM`（伪随机）；envp 仍空 |
| de_thread + 完整 post-exec 清理 | ❌ |
| 缩小 embedded program_map / `_num_app` | ✅ **整套删除**（仅留 rootfs.cpio `.incbin`） |
| 内核 test harness → boot | ✅ `init/linux_boot.c` + `boot_wait.h` |

详见 [`EXECVE_IMPLEMENTATION_STATUS.md`](EXECVE_IMPLEMENTATION_STATUS.md)。

### P2 — VFS（Phase 4）

**Bootstrap ✅**（2026-07-09）：initramfs、open/read/close/fstat、mkdir/unlink、execve、page_slice 加载。

**目录 / fd（busybox 已用）**: chdir、cwd、openat、getdents64、dup/pipe、fcntl/poll stub 等已接线；stdout 全绿仍以测例为准。

**动态表 ✅**（2026-07-27）：定长 BSS/栈炸弹 → [`VFS_DYNAMIC_STORAGE.md`](VFS_DYNAMIC_STORAGE.md)（S0–S3）。

扩展性（readdir O(n²)、tombstone、ramfs body）不阻塞 busybox。

**Live 进度**：[`VFS_IMPLEMENTATION_STATUS.md`](VFS_IMPLEMENTATION_STATUS.md)。

### P3 — time 细节（非 FS 阻塞）

| 项 | 状态 |
|----|------|
| gettimeofday / nanosleep / clock_gettime / uname | ✅ #16–#20 |
| `times` 真实 CPU 时间 | stub |
| `settimeofday` | ❌（可选 polish） |
| vDSO | **暂缓**（无 FS / 动态链接，投入产出比低） |

---

## 4. 推荐实施顺序（maintainer）

```text
1–9. ~~busybox bring-up 主线~~ — ✅ 见 §3 P0；妥协账已归档
下一步（maintainer）→ NEXT_PLAN.md：
  - Phase 5 / 更多 syscall；execve 收尾；VFS 可选（/dev、shebang）
  - 额外：uart_server（与 core #46 / 直写串口权衡）
  - CMDLINE：仅上层 Makefile 注入策略；勿改 core 默认串
```

---

## 5. 各子系统文档索引

| 子系统 | 设计 | 状态 | 验证 |
|--------|------|------|------|
| Signal | [`SIGNAL_DELIVERY_TRAP_PATHS.md`](SIGNAL_DELIVERY_TRAP_PATHS.md) | [`SIGNAL_IMPLEMENTATION_STATUS.md`](SIGNAL_IMPLEMENTATION_STATUS.md) | log §2026-06-13 |
| Wait | [`DATA_MODEL.md`](DATA_MODEL.md) | [`WAIT4_IMPLEMENTATION_STATUS.md`](WAIT4_IMPLEMENTATION_STATUS.md) | log §#49 |
| Exec | [`SYSCALL_USER_RETURN_AND_EXECVE.md`](SYSCALL_USER_RETURN_AND_EXECVE.md) | [`EXECVE_IMPLEMENTATION_STATUS.md`](EXECVE_IMPLEMENTATION_STATUS.md) | log §2026-07-09 #8 execve |
| Time | [`TIME_SUBSYSTEM_PLAN.md`](TIME_SUBSYSTEM_PLAN.md) | §0 已落地 checklist | #16–#20 |
| VFS | [`VFS_ARCHITECTURE.md`](VFS_ARCHITECTURE.md) · [`VFS_DYNAMIC_STORAGE.md`](VFS_DYNAMIC_STORAGE.md) | [`VFS_IMPLEMENTATION_STATUS.md`](VFS_IMPLEMENTATION_STATUS.md) | log §2026-07-09；busybox gate |
| Busybox / boot | [`BOOT_PATH_EVOLUTION.md`](BOOT_PATH_EVOLUTION.md) · [`INITRAMFS_PLAN.md`](INITRAMFS_PLAN.md) | [`NEXT_PLAN.md`](NEXT_PLAN.md) · archive deferrals | x86 `run_all` **52/52**（2026-08-02） |
| IPC RPC | [`protocols/IPC_RPC_FRAMEWORK.md`](protocols/IPC_RPC_FRAMEWORK.md) | coop + nest reply transfer ✅ | — |
| MM | [`MM_AND_COW.md`](MM_AND_COW.md) | 分散在 Phase 1 文档 | #35 brk 等 |

---

## 6. 已归档 / 勿作 live 真源

- [`archive/BUSYBOX_BOOT_DEFERRALS.md`](archive/BUSYBOX_BOOT_DEFERRALS.md) — busybox bring-up 妥协账（**2026-08-09 关闭**）；live → [`NEXT_PLAN.md`](NEXT_PLAN.md)
- [`doc/ai/SYSCALL_IMPLEMENTATION_STATUS.md`](../ai/SYSCALL_IMPLEMENTATION_STATUS.md) — 2026-04-21，14 syscall，**已过时**
- [`doc/ai/SYSCALL_QUICK_REFERENCE.md`](../ai/SYSCALL_QUICK_REFERENCE.md) — 部分过时
- [`archive/PHASE2_SUMMARY.md`](archive/PHASE2_SUMMARY.md) — 16/16 ≠ 当前 52/52 harness

---

## 7. 更新 checklist（每次 gate 后）

- [ ] 更新对应 `*_IMPLEMENTATION_STATUS.md`
- [ ] 在 `CROSS_ARCH_VERIFICATION_LOG.md` **追加**一节（不覆盖旧记录）
- [ ] 非显然改动 → `doc/ai/DECISIONS.md`
- [ ] 合并后 → `doc/ai/ASSIST_HISTORY.md`
- [ ] 本文 §2–§3、**§8** 与 [`NEXT_PLAN.md`](NEXT_PLAN.md)
- [ ] `core/` 子模块变更单独审阅 / 提交（勿与 compat 混为一谈；勿塞上层 CMDLINE 策略）

---

## 8. 工作区盘点（未提交，2026-08-09）

> **以 `git status` 为准**。完整条目表见 [`NEXT_PLAN.md`](NEXT_PLAN.md) §1。  
> 协议：[`IPC_RPC_FRAMEWORK.md`](protocols/IPC_RPC_FRAMEWORK.md)。**不在此自动提交**。

### 8.1 本轮 compat 要点

| 主题 | 要点 |
|------|------|
| Nested reply transfer | TLV `@n` + `ipc_transfer_message`；request 仍 port rendezvous |
| CMDLINE | **上层** `Makefile` 注入默认；core 仅机制（空默认） |
| busybox 妥协账 | 已迁 [`archive/BUSYBOX_BOOT_DEFERRALS.md`](archive/BUSYBOX_BOOT_DEFERRALS.md) |
| 下一步 | [`NEXT_PLAN.md`](NEXT_PLAN.md)（含 UART server 额外项） |

### 8.2 验证快照

| 项 | 结果 |
|----|------|
| x86_64 busybox `run_all` | 曾 ✅ **`pass=52 fail=0`**（2026-08-02）；本波文档/IPC 整理后需维护者复跑再宣称 |
| aarch64 SMP 墙钟 | 日常 `SMP=1`；见 NEXT_PLAN |

### 8.3 建议下一刀

见 [`NEXT_PLAN.md`](NEXT_PLAN.md) §2–§3（主线 syscall / exec 收尾；额外 uart_server）。
