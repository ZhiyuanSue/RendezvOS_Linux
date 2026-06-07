# Linux 兼容层 — 进展与追溯索引

> **Purpose**: 单一入口，把 **路线图 → 实现状态 → 验证证据 → 决策** 串起来。  
> **Last updated**: 2026-05-19

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
| **2E** | 🔧 进行中 | sigaltstack / SA_ONSTACK (#21) | 测例 my_write(rdx)+内核安装时不 probe 映射 |
| **3** | 🔧 进行中 | execve（3a 内嵌 ELF 部分可用） | [`EXECVE_IMPLEMENTATION_STATUS.md`](EXECVE_IMPLEMENTATION_STATUS.md) |
| **3.5** | 📋 下一步 | **time 子系统**（gettimeofday/nanosleep/times） | [`TIME_SUBSYSTEM_PLAN.md`](TIME_SUBSYSTEM_PLAN.md) |
| **4** | 📋 待做 | VFS + initramfs | [`VFS_SERVER_IPC.md`](VFS_SERVER_IPC.md) |
| **5** | 📋 待做 | IPC/socket/rlimit 等 | [`SYSCALLS.md`](SYSCALLS.md) |

**Cross-arch gate (最新)**: 2026-05-19 — x86_64 + aarch64 **52/52 harness PASS** → [`CROSS_ARCH_VERIFICATION_LOG.md`](CROSS_ARCH_VERIFICATION_LOG.md)

---

## 3. 当前缺口（按优先级）

### P0 — 测例 stdout 失败（非 VFS）

| 测例 # | 领域 | 缺口 | 负责阶段 |
|--------|------|------|----------|
| **21** | sigaltstack | 用户 `stack_t` 跨页 EFAULT；alt 区未校验 | **2E**（修复中） |
| **16–17, 20** | time | 无 gettimeofday / nanosleep / times | **3.5** |
| **19** | uname | 未实现 | 3.5 或 4 前 stub |

### P1 — execve 补完（Phase 3）

| 项 | 状态 |
|----|------|
| 内嵌 ELF + argv | ✅ #03/#43/#52 |
| envp / auxv | ❌ |
| de_thread + 完整 post-exec 清理 | ❌ |
| FS 加载 ELF | ❌（#18 被 open 挡住） |

详见 [`EXECVE_IMPLEMENTATION_STATUS.md`](EXECVE_IMPLEMENTATION_STATUS.md)。

### P2 — VFS（Phase 4，time + sigaltstack 之后）

19 个 stdout FAIL 中 **14 个纯 FS**（open/pipe/dup/mkdir/mount…）；#13/#40 文件 mmap/munmap 亦依赖 open。

---

## 4. 推荐实施顺序（maintainer）

```text
1. sigaltstack #21 修复 + 双架构验证
2. time 子系统（见 TIME_SUBSYSTEM_PLAN.md）
   2a. compat-only 粗粒度（jeffies）— 可选 bootstrap
   2b. core P2 提案：单调 ns + 定时唤醒 — 正确 nanosleep
3. execve 3b–3c（env/auxv、de_thread、状态重置）
4. VFS Phase 4
5. execve 3d（FS + PT_INTERP）
```

---

## 5. 各子系统文档索引

| 子系统 | 设计 | 状态 | 验证 |
|--------|------|------|------|
| Signal | [`SIGNAL_DELIVERY_TRAP_PATHS.md`](SIGNAL_DELIVERY_TRAP_PATHS.md) | [`SIGNAL_IMPLEMENTATION_STATUS.md`](SIGNAL_IMPLEMENTATION_STATUS.md) | log §2026-05-19 |
| Wait | [`DATA_MODEL.md`](DATA_MODEL.md) | [`WAIT4_IMPLEMENTATION_STATUS.md`](WAIT4_IMPLEMENTATION_STATUS.md) | log §#49 |
| Exec | [`SYSCALL_USER_RETURN_AND_EXECVE.md`](SYSCALL_USER_RETURN_AND_EXECVE.md) | [`EXECVE_IMPLEMENTATION_STATUS.md`](EXECVE_IMPLEMENTATION_STATUS.md) | 待补 gate |
| Time | [`TIME_SUBSYSTEM_PLAN.md`](TIME_SUBSYSTEM_PLAN.md) | （随实现更新 plan 内 checklist） | #16/#17/#20 |
| VFS | [`VFS_SERVER_IPC.md`](VFS_SERVER_IPC.md) | 待 Phase 4 新建 status | #10–#51 FS 集 |
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
