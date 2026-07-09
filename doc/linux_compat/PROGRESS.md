# Linux 兼容层 — 进展与追溯索引

> **Purpose**: 单一入口，把 **路线图 → 实现状态 → 验证证据 → 决策** 串起来。  
> **Last updated**: 2026-07-09

---

## 1. 文档怎么读（追溯链）

```text
SYSCALLS.md              阶段路线图（只看未来 + 顶表）
    ↓
PROGRESS.md (本文)       当前阶段、缺口汇总、下一步
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

---

## 2. 阶段总览（当前共识）

| 阶段 | 状态 | 目标 | 状态文档 |
|------|------|------|----------|
| **0** | ✅ | syscall 框架 | — |
| **1** | ✅ | fork/exit/wait/brk/mmap | [`archive/PHASE1_SUMMARY.md`](archive/PHASE1_SUMMARY.md) |
| **2A–2D** | ✅ | clone + 信号 + 缺页 SIGSEGV | [`SIGNAL_IMPLEMENTATION_STATUS.md`](SIGNAL_IMPLEMENTATION_STATUS.md), [`WAIT4_IMPLEMENTATION_STATUS.md`](WAIT4_IMPLEMENTATION_STATUS.md) |
| **2E** | ✅ | sigaltstack / SA_ONSTACK (#21) | 2026-06 gate #21 PASS |
| **3** | 🔧 进行中 | execve（3a 内嵌 ELF 部分可用） | [`EXECVE_IMPLEMENTATION_STATUS.md`](EXECVE_IMPLEMENTATION_STATUS.md) |
| **3.5** | ✅ | time 子系统（gettimeofday/nanosleep/times/uname） | [`TIME_SUBSYSTEM_PLAN.md`](TIME_SUBSYSTEM_PLAN.md) |
| **4** | 📋 **下一步** | VFS + initramfs | [`VFS_SERVER_IPC.md`](VFS_SERVER_IPC.md) |
| **5** | 📋 待做 | IPC/socket/rlimit 等 | [`SYSCALLS.md`](SYSCALLS.md) |

**Cross-arch gate (最新)**: 2026-06-13 — x86_64 + aarch64 **52/52 harness PASS**（含 DAIF/sleep 修复后）→ [`CROSS_ARCH_VERIFICATION_LOG.md`](CROSS_ARCH_VERIFICATION_LOG.md) §2026-06-13

**core 前置（FS 前）**: 调度、trap/syscall、IPC、VSpace/COW、SMP boot、timer/sleep — **无大块 core 缺口**；Phase 4 在 `linux_layer` + `servers/fs`，不扩 core（见 [`GOALS_AND_CORE_CONTRACT.md`](GOALS_AND_CORE_CONTRACT.md) §4–§7）。

---

## 3. 当前缺口（按优先级）

### P0 — #49 wait stdout（compat，已修）

| 现象 | 根因 | 状态 |
|------|------|------|
| #49 内三子测例 stdout FAIL（harness 仍 PASS） | `SIGCHLD` 默认动作误触发 wait4 EINTR，zombie 未 reap | **已修**；x86_64 + aarch64 post-fix **3/3 PASS** |

详见 [`WAIT4_IMPLEMENTATION_STATUS.md`](WAIT4_IMPLEMENTATION_STATUS.md)、verification log §2026-06-13。

### P1 — execve 补完（Phase 3，可与 FS 并行）

| 项 | 状态 |
|----|------|
| 内嵌 ELF + argv | ✅ #03/#43/#52 |
| envp / auxv | ❌ |
| de_thread + 完整 post-exec 清理 | ❌ |
| FS 加载 ELF | ❌（#18 被 open 挡住，依赖 Phase 4） |

详见 [`EXECVE_IMPLEMENTATION_STATUS.md`](EXECVE_IMPLEMENTATION_STATUS.md)。

### P2 — VFS（Phase 4，**当前主战场**）

约 **14 个纯 FS** stdout FAIL（open/pipe/dup/mount…）；#13/#40 文件 mmap 亦依赖 open。

**Live 进度**：[`VFS_IMPLEMENTATION_STATUS.md`](VFS_IMPLEMENTATION_STATUS.md)（Step 0–5 代码已落地；V4–V7 待 maintainer 验证）。

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
1. ~~#49 wait4 SIGCHLD/EINTR 修复~~ — x86_64 + aarch64 已验证 ✅
2. VFS Phase 4（initramfs → open/read/close → fd 表 → 消减 FS stdout FAIL）
3. execve 3b–3c（env/auxv、de_thread）与 FS 并行
4. execve 3d（FS + PT_INTERP，依赖 Phase 4）
5. time polish：settimeofday、times CPU 统计（按需）
```

---

## 5. 各子系统文档索引

| 子系统 | 设计 | 状态 | 验证 |
|--------|------|------|------|
| Signal | [`SIGNAL_DELIVERY_TRAP_PATHS.md`](SIGNAL_DELIVERY_TRAP_PATHS.md) | [`SIGNAL_IMPLEMENTATION_STATUS.md`](SIGNAL_IMPLEMENTATION_STATUS.md) | log §2026-06-13 |
| Wait | [`DATA_MODEL.md`](DATA_MODEL.md) | [`WAIT4_IMPLEMENTATION_STATUS.md`](WAIT4_IMPLEMENTATION_STATUS.md) | log §#49 |
| Exec | [`SYSCALL_USER_RETURN_AND_EXECVE.md`](SYSCALL_USER_RETURN_AND_EXECVE.md) | [`EXECVE_IMPLEMENTATION_STATUS.md`](EXECVE_IMPLEMENTATION_STATUS.md) | #52 execve PASS |
| Time | [`TIME_SUBSYSTEM_PLAN.md`](TIME_SUBSYSTEM_PLAN.md) | §0 已落地 checklist | #16–#20 |
| VFS | [`VFS_SERVER_IPC.md`](VFS_SERVER_IPC.md) | [`INITRAMFS_PLAN.md`](INITRAMFS_PLAN.md) | 待 Phase 4 新建 status | #10–#51 FS 集 |
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
- [ ] 本文 §2–§3 日期与 P0 表
