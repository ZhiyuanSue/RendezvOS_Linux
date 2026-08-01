# Linux 兼容层 — 进展与追溯索引

> **Purpose**: 单一入口，把 **路线图 → 实现状态 → 验证证据 → 决策** 串起来。  
> **Last updated**: 2026-08-01（含未提交工作区盘点 §8）

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

**busybox / run_all（工作区，未正式 gate）**: 2026-08-01 x86_64 `run_all` **跑完** `pass=41 fail=11`（IPC FS 楔死已消）；开放项见 deferrals。aarch64 busybox 路径相对旧 incbin harness **体感明显变慢**（原因未定性，见 §8.4）。

**core**: 工作区 `core/` 子模块另有 **port ops gate + RR schedule 小修**（需维护者审阅/单独提交）；见 §8.1。

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
| IPC reply 会合楔死（VFS uninterruptible + ops gate + post-send 不弃 recv） | ✅ x86 `run_all` 已跑完；协议见 deferrals / `IPC_RPC_FRAMEWORK` §6–§8 |
| fork/clone `#PF` @ `0x57f485`（status=139 主簇） | ⏳ 另案；见 deferrals 子项 5 |

详见 [`BOOT_PATH_EVOLUTION.md`](BOOT_PATH_EVOLUTION.md)、[`BUSYBOX_BOOT_DEFERRALS.md`](BUSYBOX_BOOT_DEFERRALS.md)。

### P1 — execve 补完（Phase 3）

| 项 | 状态 |
|----|------|
| 内嵌 ELF + argv | ✅ #03/#43/#52 |
| initramfs / VFS execve | ✅ #8 stdout `execve success`（2026-07-09） |
| envp / auxv | 部分（busybox Path B 有；正规化不足） |
| de_thread + 完整 post-exec 清理 | ❌ |
| 缩小 embedded program_map / `_num_app` | ✅ exec 已无 embedded；stub link_app 仍可链 |

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
5. ~~run_all 编排 + IPC 楔死修复~~ — ✅ x86 已跑完；待：审阅/提交工作区（§8）+ aarch64 体感变慢观察
6. fork/clone `#PF` 0x57f485（deferrals 子项 5）— 下一轮 bug
7. cmdline + `execve("/init")`；pathname/auxv；清 stub `link_app.o`
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
| Busybox / boot | [`BOOT_PATH_EVOLUTION.md`](BOOT_PATH_EVOLUTION.md) · [`INITRAMFS_PLAN.md`](INITRAMFS_PLAN.md) | [`BUSYBOX_BOOT_DEFERRALS.md`](BUSYBOX_BOOT_DEFERRALS.md) | x86 `run_all` 41/11；工作区 §8 |
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

## 8. 工作区盘点（未提交，2026-08-01）

> 分支：`busybox-support`。下列按主题归类；**以 `git status` 为准**。  
> 协议细节：[`BUSYBOX_BOOT_DEFERRALS.md`](BUSYBOX_BOOT_DEFERRALS.md) IPC 专节 · [`protocols/IPC_RPC_FRAMEWORK.md`](protocols/IPC_RPC_FRAMEWORK.md) §6–§8。  
> Pattern Log：[`doc/ai/AI_CHECKLIST.md`](../ai/AI_CHECKLIST.md)。  
> **不在此提交**；维护者审阅后再分批 commit。`ASSIST_HISTORY` 仅在批准提交后追加。

### 8.1 core 子模块（需维护者确认）

| 主题 | 文件（core 内） | 要点 |
|------|-----------------|------|
| **Port ops gate** | `include/rendezvos/ipc/port.h`, `kernel/ipc/port.c`, `kernel/ipc/ipc.c` | `ops_life` / `ops_count`；仅 `REGISTERED` 可 `begin`；unregister：`CLOSING`→等 count→`port_clean`→`CLOSED`；阻塞路径 `schedule` 前 `end`；`PORT_CLOSED` + orphan drop |
| **测例** | `modules/test/single_ipc_test.c`, `smp_ipc_test.c`, `single_timer_test.c` | 指针直传路径 `ACTIVE→REGISTERED` 伪造 |
| **调度** | `kernel/task/task_manager.c` | RR：查找前 current `running→ready`（防自扫空转） |
| **配置** | `script/config/config_x86_64.json` | 随子模块 staged |

父仓显示 `core (modified content)`；子模块内变更目前为 **staged**。

### 8.2 兼容层 — IPC / 进程生命周期（本轮主线）

| 主题 | 文件 | 要点 |
|------|------|------|
| RPC 框架 | `linux_layer/ipc/rpc.c`, `include/linux_compat/ipc/rpc.h` | `-EINTR` 仅 commit 前；commit 后等 reply；reply 分配失败重试；`PORT_CLOSED` 已处理；请求 `msg_data` `ref_put` |
| VFS 客户端 | `linux_layer/fs/fs_ipc.c` | `ipc_rpc_call_va_uninterruptible` |
| Backend | `servers/fs/vfs_backend_ipc.c` | 嵌套 / register 走 uninterruptible |
| EXIT_NOTIFY | `linux_layer/proc/proc_wait_ipc.c`, `servers/clean_server.c`, `linux_layer/proc/clean_ipc.c` | notify OOM 重试；失败 fallback pending+poke；one-way `PORT_CLOSED` 已处理 |
| 文档 | `BUSYBOX_BOOT_DEFERRALS.md`, `protocols/IPC_RPC_FRAMEWORK.md`, `AI_CHECKLIST.md` | 子项 2–4 ✅；x86 `run_all` 41/11 |

### 8.3 兼容层 — busybox boot / 编排 / FS stub（同工作区，非仅 IPC）

| 主题 | 文件 | 要点 |
|------|------|------|
| **Boot 叙事** | `doc/linux_compat/BOOT_PATH_EVOLUTION.md`（**untracked**） | incbin→cpio→busybox→`/init`+`run_all` |
| **pack / run_all** | `script/config/pack_user_rootfs.py`, `script/rootfs/*`, `rootfs/init`（untracked） | pack 时展开显式 `run_one`；禁 ash `while read` manifest |
| **poll / fcntl** | `misc/sys_poll.c` + `fs/linux_poll.h`；`fs/sys_fcntl.c` + `fs/linux_fcntl.h`；`syscall_entry.c` | 已从 `sys_fs_impl` 拆出；CONSOLE_IN=`POLLHUP` 见 deferrals |
| **fd / pipe / open** | `linux_fd_table.*`, `linux_pipe.c`, `vfs_open.c`, `vfs_path.c`, `vfs_backend_cpio.c` | busybox 路径加深 |
| **exec** | `linux_exec_image.c`, `linux_exec_stack.c`, `user_test_runner.c` | 去掉 exec embedded fallback；Path B argv/`run_all` |
| **其它文档** | `ROOTFS.md`, `FILE_LOADING.md`, `USER_TESTS.md`, `EXECVE_*`, `FD_TABLE.md`, … | 与 boot/FS 对齐 |
| **debug** | `include/linux_compat/debug_trace.h`（untracked） | 可选跟踪宏 |

### 8.4 验证快照与开放观察

| 项 | 结果 |
|----|------|
| x86_64 busybox `run_all` | ✅ 跑完；`pass=41 fail=11`；**不再卡 FS/IPC** |
| fail 主簇 | `#PF pc=far=0x57f485` status=139（fork/clone/wait/…）；另 mount `-19`、`ch2b_exit` 255 |
| aarch64 | 相对 **旧 incbin harness 全绿时代**，busybox/`run_all` 路径 **体感慢一大截**（未定性：路径更重 vs 模拟器 vs 其它；**先不修**） |
| 成功路径开销（预期） | ops gate / RPC 成功路径仅原子与分支；重试环仅失败/OOM |

### 8.5 代码布局整理（本轮，相对 §8.2–§8.3 语义不变）

| 动作 | 说明 |
|------|------|
| 拆出 `sys_fcntl.c` | 不再塞在 `sys_fs_impl.c` |
| `linux_fcntl.h` / `linux_poll.h` | open/fcntl/poll 常量集中 |
| `misc/sys_poll.c` | 用 `linux_poll.h`；跟踪走 `debug_trace.h` |
| 删除误导性 `script/rootfs/run_all.sh` | `run_all` **仅**由 `pack_user_rootfs.py` 生成到 `rootfs/tests/` |
| `.gitignore` | 增加 `rootfs/init`（busybox symlink） |
| `CODE_STRUCTURE.md` | 同步现行树 |

### 8.6 建议提交切分（供审阅，未执行）

1. **core**：ops gate + RR（维护者单独审 / 提交）  
2. **compat IPC 生命周期**：`rpc.c` / `fs_ipc` / wait notify / clean / 协议文档 + checklist  
3. **busybox boot 编排**：pack `run_all`、`boot_smoke.sh`、ROOTFS/BOOT 文档  
4. **FS/syscall stub**：`sys_fcntl` / `sys_poll` / fd/pipe/open 加深  

提交前再 `git status` 核对；**勿**把 core 与 compat 糊成一个 commit。
