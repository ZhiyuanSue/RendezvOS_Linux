# Linux 兼容层 — 进展与追溯索引

> **Purpose**: 单一入口，把 **路线图 → 实现状态 → 验证证据 → 决策** 串起来。  
> **Last updated**: 2026-08-02（busybox `run_all` 52/52；RPC coop 框架落地）

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
- busybox demo 妥协与回收清单 → [`BUSYBOX_BOOT_DEFERRALS.md`](BUSYBOX_BOOT_DEFERRALS.md)。
- **未提交工作区** → 本文 **§8**（提交前以 `git status` 为准）。

---

## 2. 阶段总览（当前共识）

| 阶段 | 状态 | 目标 | 状态文档 |
|------|------|------|----------|
| **0** | ✅ | syscall 框架 | — |
| **1** | ✅ | fork/exit/wait/brk/mmap | [`archive/PHASE1_SUMMARY.md`](archive/PHASE1_SUMMARY.md) |
| **2A–2D** | ✅ | clone + 信号 + 缺页 SIGSEGV | [`SIGNAL_IMPLEMENTATION_STATUS.md`](SIGNAL_IMPLEMENTATION_STATUS.md), [`WAIT4_IMPLEMENTATION_STATUS.md`](WAIT4_IMPLEMENTATION_STATUS.md) |
| **2E** | ✅ | sigaltstack / SA_ONSTACK (#21) | 2026-06 gate #21 PASS |
| **3** | 🔧 进行中 | execve（VFS 路径可用；Path B demo 仍妥协） | [`EXECVE_IMPLEMENTATION_STATUS.md`](EXECVE_IMPLEMENTATION_STATUS.md) · [`BUSYBOX_BOOT_DEFERRALS.md`](BUSYBOX_BOOT_DEFERRALS.md) |
| **3.5** | ✅ | time 子系统（gettimeofday/nanosleep/times/uname） | [`TIME_SUBSYSTEM_PLAN.md`](TIME_SUBSYSTEM_PLAN.md) |
| **4** | 🔧 **bootstrap ✅ / 目录与动态表 ✅** | VFS + initramfs | [`VFS_IMPLEMENTATION_STATUS.md`](VFS_IMPLEMENTATION_STATUS.md) · [`VFS_DYNAMIC_STORAGE.md`](VFS_DYNAMIC_STORAGE.md) |
| **5** | 📋 待做 | IPC/socket/rlimit 等 | [`SYSCALLS.md`](SYSCALLS.md) |

**Cross-arch gate (最新正式)**: 2026-07-09 — x86_64 + aarch64 **52/52 harness PASS**（**incbin / 内核 harness**）→ [`CROSS_ARCH_VERIFICATION_LOG.md`](CROSS_ARCH_VERIFICATION_LOG.md) §2026-07-09

**busybox / run_all（工作区）**: 2026-08-02 x86_64 `x86_64_run.log` → **`pass=52 fail=0`**（`#PF @ 0x57f485` 簇已消；与迁移 busybox 前 harness 全绿对齐）。aarch64 多核墙钟慢：偏 **idle busy-`schedule` × QEMU**（`SMP=1` 很快）；见 deferrals。

**RPC coop 框架**: `ipc_rpc_coop_*` 已落地（park reply / nested）；**VFS 仍用** `ipc_rpc_server_loop`（下一刀迁 server）。见 [`protocols/IPC_RPC_FRAMEWORK.md`](protocols/IPC_RPC_FRAMEWORK.md)。

**core**: port ops gate 等若仍在子模块工作区，需维护者单独审阅。

---

## 3. 当前缺口（按优先级）

### P0 — #49 wait stdout（compat，已修）

| 现象 | 根因 | 状态 |
|------|------|------|
| #49 内三子测例 stdout FAIL（harness 仍 PASS） | `SIGCHLD` 默认动作误触发 wait4 EINTR，zombie 未 reap | **已修**；x86_64 + aarch64 post-fix **3/3 PASS** |

详见 [`WAIT4_IMPLEMENTATION_STATUS.md`](WAIT4_IMPLEMENTATION_STATUS.md)、verification log §2026-06-13。

### P0 — busybox Path B / execve 正规化

| 项 | 状态 |
|----|------|
| `ls /bin` demo | ✅ |
| 用户态 `run_all.sh` 编排（替代内核 manifest 循环） | ✅ 默认；pack 生成显式 `run_one` |
| spawn 仍 `gen_task_from_elf` + Path B 二次 bootstrap | ⬜ 应改 `execve("/init")` / cmdline |
| IPC reply 会合楔死（VFS uninterruptible + ops gate + post-send 不弃 recv） | ✅ |
| fork/clone `#PF` @ `0x57f485`（status=139） | ✅ 2026-08-02 x86 `run_all` 52/52（`linux_signal_proc_reset`） |
| RPC reply-aware coop 框架 | ✅ API；⬜ VFS/backends 改用 `ipc_rpc_coop_server_loop` |

详见 [`BOOT_PATH_EVOLUTION.md`](BOOT_PATH_EVOLUTION.md)、[`BUSYBOX_BOOT_DEFERRALS.md`](BUSYBOX_BOOT_DEFERRALS.md)。

### P1 — execve 补完（Phase 3）

| 项 | 状态 |
|----|------|
| 内嵌 ELF + argv | ✅ #03/#43/#52 |
| initramfs / VFS execve | ✅ #8 stdout `execve success`（2026-07-09） |
| envp / auxv | ✅ busybox：`HWCAP`/`EXECFN`/`RANDOM`（伪随机）；envp 仍空 |
| de_thread + 完整 post-exec 清理 | ❌ |
| 缩小 embedded program_map / `_num_app` | ✅ **整套删除**（仅留 rootfs.cpio `.incbin`） |

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
1. ~~#49 wait4 SIGCHLD/EINTR 修复~~ — ✅
2. ~~VFS Phase 4 bootstrap（initramfs, open/read, execve, page_slice）~~ — ✅ 2026-07-09
3. ~~busybox ls /bin demo~~ — ✅ 2026-07-13+
4. ~~VFS 定长表 → vfs_slice_table（S0–S3）~~ — ✅ 2026-07-27
5. ~~run_all 编排 + IPC 楔死修复~~ — ✅
6. ~~fork/clone `#PF` 0x57f485~~ — ✅ x86 52/52
7. ~~RPC coop 框架（park reply/nested）~~ — ✅；下一刀：**VFS listen 迁** `ipc_rpc_coop_server_loop`
8. cmdline + `execve("/init")`（需 core bootargs）；VFS→coop listen；aarch64 日常 `SMP=1`
9. ~~pathname/auxv；清除 link_app/`_num_app` 整套~~ — ✅ 2026-08-02
```

---

## 5. 各子系统文档索引

| 子系统 | 设计 | 状态 | 验证 |
|--------|------|------|------|
| Signal | [`SIGNAL_DELIVERY_TRAP_PATHS.md`](SIGNAL_DELIVERY_TRAP_PATHS.md) | [`SIGNAL_IMPLEMENTATION_STATUS.md`](SIGNAL_IMPLEMENTATION_STATUS.md) | log §2026-06-13 |
| Wait | [`DATA_MODEL.md`](DATA_MODEL.md) | [`WAIT4_IMPLEMENTATION_STATUS.md`](WAIT4_IMPLEMENTATION_STATUS.md) | log §#49 |
| Exec | [`SYSCALL_USER_RETURN_AND_EXECVE.md`](SYSCALL_USER_RETURN_AND_EXECVE.md) | [`EXECVE_IMPLEMENTATION_STATUS.md`](EXECVE_IMPLEMENTATION_STATUS.md) | log §2026-07-09 #8 execve |
| Time | [`TIME_SUBSYSTEM_PLAN.md`](TIME_SUBSYSTEM_PLAN.md) | §0 已落地 checklist | #16–#20 |
| VFS | [`VFS_ARCHITECTURE.md`](VFS_ARCHITECTURE.md) · [`VFS_DYNAMIC_STORAGE.md`](VFS_DYNAMIC_STORAGE.md) | [`VFS_IMPLEMENTATION_STATUS.md`](VFS_IMPLEMENTATION_STATUS.md) | log §2026-07-09；busybox §deferrals |
| Busybox / boot | [`BOOT_PATH_EVOLUTION.md`](BOOT_PATH_EVOLUTION.md) · [`INITRAMFS_PLAN.md`](INITRAMFS_PLAN.md) | [`BUSYBOX_BOOT_DEFERRALS.md`](BUSYBOX_BOOT_DEFERRALS.md) | x86 `run_all` **52/52**（2026-08-02） |
| IPC RPC | [`protocols/IPC_RPC_FRAMEWORK.md`](protocols/IPC_RPC_FRAMEWORK.md) | coop API ✅；VFS 迁移 ⬜ | — |
| MM | [`MM_AND_COW.md`](MM_AND_COW.md) | 分散在 Phase 1 文档 | #35 brk 等 |

---

## 6. 已归档 / 勿作 live 真源

- [`doc/ai/SYSCALL_IMPLEMENTATION_STATUS.md`](../ai/SYSCALL_IMPLEMENTATION_STATUS.md) — 2026-04-21，14 syscall，**已过时**
- [`doc/ai/SYSCALL_QUICK_REFERENCE.md`](../ai/SYSCALL_QUICK_REFERENCE.md) — 部分过时
- [`archive/PHASE2_SUMMARY.md`](archive/PHASE2_SUMMARY.md) — 16/16 ≠ 当前 52/52 harness

---

## 7. 更新 checklist（每次 gate 后）

- [ ] 更新对应 `*_IMPLEMENTATION_STATUS.md`
- [ ] 在 `CROSS_ARCH_VERIFICATION_LOG.md` **追加**一节（不覆盖旧记录）
- [ ] 非显然改动 → `doc/ai/DECISIONS.md`
- [ ] 合并后 → `doc/ai/ASSIST_HISTORY.md`
- [ ] 本文 §2–§3、**§8** 与 deferrals 文首表
- [ ] `core/` 子模块变更单独审阅 / 提交（勿与 compat 混为一谈）

---

## 8. 工作区盘点（未提交，2026-08-02）

> **以 `git status` 为准**。协议：[`IPC_RPC_FRAMEWORK.md`](protocols/IPC_RPC_FRAMEWORK.md)。Pattern Log：[`AI_CHECKLIST.md`](../ai/AI_CHECKLIST.md)。  
> **不在此自动提交**；维护者审阅后分批 commit。

### 8.1 本轮 compat 要点

| 主题 | 文件 | 要点 |
|------|------|------|
| **RPC coop 框架** | `linux_layer/ipc/rpc.c`, `include/linux_compat/ipc/rpc.h` | `ipc_rpc_coop_queue` / `job` / `nested_call` / `coop_server_loop`；listen **单 send 槽**；VFS **未切** |
| clean coop 收紧 | `servers/clean_server.c` | poll=`void`；zombie before finished；EXIT_NOTIFY 仍 one-shot |
| 文档 | `IPC_RPC_FRAMEWORK.md`, `EXIT_CLEAN.md`, `DECISIONS.md`, deferrals, 本文 | 成熟度表与进度对齐 |

### 8.2 验证快照

| 项 | 结果 |
|----|------|
| x86_64 busybox `run_all` | ✅ **`pass=52 fail=0`**（`x86_64_run.log` 2026-08-02） |
| `#PF` 0x57f485 / status=139 | ✅ 未见 |
| aarch64 SMP 墙钟 | idle×QEMU；日常 `SMP=1`；非 VFS RPC 本体 |

### 8.3 建议下一刀

1. VFS listen → `ipc_rpc_coop_server_loop`（先单飞 FS 突变 + park 嵌套/reply）  
2. cmdline / `execve("/init")` 正规化  
3. （可选）core idle → WFI（需维护者批准）
