# wait4 实现状态总结

## ✅ 完整实现状态

**实现时间**: 2026-04-25  
**cross-arch gate**: 2026-05-19 (stdout #49 parity) · **2026-06-13** (52/52 harness; #49 stdout regression → SIGCHLD/EINTR fix)  
**架构**: 无需 core/ 修改，纯 linux_layer 扩展  
**测试状态**: integrated harness PASS both arches — see [`CROSS_ARCH_VERIFICATION_LOG.md`](CROSS_ARCH_VERIFICATION_LOG.md)

## 核心实现

### 1. proc_registry - 进程注册表

**功能**: 基于core的name_index机制实现O(1) PID查找

**API接口**（现行：堆 `linux_proc_resource_t`，EXIT_CLEAN v2）:
```c
linux_proc_resource_t *find_proc_by_pid(pid_t pid);
bool proc_parent_has_unreaped_child(pid_t ppid, pid_t pgid, bool filter_by_pgid);
bool proc_has_wait_reaper(linux_proc_resource_t *proc);
```

**实现文件**: `linux_layer/proc/sys_proc_registry.c`

### 2. sys_wait4 - 完整Linux标准实现

**支持的所有pid选项**:

| pid选项 | 语义 | 实现方式 |
|---------|------|----------|
| `pid > 0` | 等待特定PID的子进程 | `find_proc_by_pid()` + wait_port 阻塞 |
| `pid == -1` | 等待任意子进程 | `proc_parent_has_unreaped_child` + EXIT_NOTIFY |
| `pid == 0` | 等待同进程组的子进程 | 同上 + pgid 过滤 |
| `pid < -1` | 等待特定进程组的子进程 | 同上 + 指定 pgid |

收尸：**EXIT_NOTIFY 携带 `proc*`** → `linux_proc_reap`（无 registry zombie 扫描）。

**支持的options**:
- ✅ `WNOHANG` (0x00000001) - 非阻塞模式，子进程运行中立即返回0
- ❌ `WUNTRACED` (0x00000002) - 需要信号机制支持
- ❌ `WCONTINUED` (0x00000008) - 需要信号机制支持

**实现文件**: `linux_layer/proc/sys_wait.c`

### 3. IPC 阻塞机制

**架构特点**: wait4 阻塞在 parent `wait_port`；**EXIT_NOTIFY** 由 clean listen 在 Link A 投递（非 exitor 直发）。

**Link A 流程**:
```
sys_exit:
  ZOMBIE + exit_last_thread hint → THREAD_REAP → zombie; schedule

clean (thread_number==0):
  delete_thread + sync detach → EXIT_NOTIFY(proc*) → parent wait_port

parent wait4:
  decode proc* → wstatus → linux_proc_reap (ZOMBIE or NOTIFIED → CLAIMED)
```

**Link B**（orphan / 无活父）：clean inline `linux_proc_reap`，无 EXIT_NOTIFY。

**消息格式**: EXIT_NOTIFY `"q p i"`（pid + proc* + exit_code）；`WAIT_INTERRUPT` 仅 SIGCHLD EINTR 路径。

### 4. 进程组支持

**数据结构扩展**:
```c
typedef struct linux_proc_resource {
    pid_t ppid;  // 父进程PID
    pid_t pgid;  // 进程组ID (新增)
    // ... 其他字段 ...
} linux_proc_resource_t;
```

**继承机制**: fork()时子进程继承父进程的pgid

## 竞态条件修复

### 问题场景（旧模型；已移除 core `delete_task`）
```
T1: child 发送 wait 通知
T2: child 请求 clean_server
T3: 末线程 teardown：delete_thread → fini detach → thread->vs put（vs 可能仍被 CPU extra 钉住）
T4: parent 若过早假设 vs/proc 已销毁会竞态
```

### 解决方案（现行）
```c
// sys_exit：设置 exit_state；wait 侧在 reap 前 proc 仍可查
// clean_server / linux_proc_reap：personality 回收；core 只做 delete_thread
// 末线程 fini detach 后 thread_number==0；wait 不依赖 del_vspace 完成
```

## 测试结果

**Integrated harness (2026-05-19)**: x86_64 and aarch64 both **52/52 PASS**, Failed 0/52.

| Test # | Focus | x86_64 | aarch64 |
|--------|-------|--------|---------|
| 07 | basic wait | PASS | PASS |
| 38 | fork + wait4 + WNOHANG | PASS | PASS |
| 41 | waitpid | PASS | PASS |
| 49 | fork/wait4 (basic + WNOHANG + 3 children) | PASS | PASS |

**#49 multiple children** (both arches): reaped exit_code **10 / 20 / 30**; `[TEST 49/52] PASS` after `Test Summary`.

### 2026-06-13 regression + fix

**现象**: #49 harness PASS 但 stdout 0/3 或 1/3（见 verification log §2026-06-13）。

**链式失败**:

1. 子进程 `exit` → `linux_queue_signal(parent, SIGCHLD)` + `EXIT_NOTIFY` on `wait_port`
2. `wait4_block_on_port` 在 blocking wait 时把 pending `SIGCHLD`（默认 DFL）当作 EINTR
3. 父进程 `wait4` 返回 `-EINTR`（libc 见 `-1`），**未 reap**（`exit_state` 仍为 1）
4. `test_multiple_children` 的 `wait4(-1)` 先收到遗留 zombie（exit 42 或 0），非 10/20/30

**修复**（对齐 [`protocols/EXIT_CLEAN.md`](protocols/EXIT_CLEAN.md) SIGCHLD 边界）:

- `linux_signal_wait4_should_return_eintr()` — **SIGCHLD 一律不中断 wait4**
- `WAIT_INTERRUPT`：仅非 SIGCHLD；处理时先 `wait4_try_pending`
- 层 B：handler 返回路径（`SA_RESTORER` / trampoline）— 投递完整性，不改变收尸握手

**x86_64 + aarch64 post-fix (2026-06-13)**: #49 stdout 3/3 PASS；reaped exit_code 10/20/30（PID 68/69/70）。

**ash smoke (2026-07-30)**: Channel R（EXIT_NOTIFY）正确；层 B 用 `SA_RESTORER` 或 RX stub（禁止 RW 栈 EXEC）；spawn 失败走 pending+poke。`AFTER_LS` hang 根因是 core COW 子 PTE 曾可写（见 `protocols/WAIT_AND_SIGCHLD.md` §4）。

Full paired log checklist: [`CROSS_ARCH_VERIFICATION_LOG.md`](CROSS_ARCH_VERIFICATION_LOG.md).

**Earlier unit-style run (2026-04-25, aarch64 only)**: 10/10 PASS (standalone wait suite).

```
[TEST 09/10] PASS - fork + wait4完整流程 ✅
[TEST 10/10] PASS - WNOHANG选项 ✅
```

## 架构优势

### 1. 无需core/修改
**完全基于linux_layer扩展**:
- 利用core的name_index机制
- 利用core的IPC基础设施
- 扩展proc_compat append区
- 不需要修改core/任何代码

### 2. 保持架构一致性
**IPC作为统一同步机制**:
- ❌ 旧实现: while(child->exit_state != 1) { schedule(); } // 轮询
- ✅ 新实现: recv_msg(wait_port) // IPC阻塞

### 3. 高效查询
**O(1)复杂度**:
- PID查找: name_index提供O(1)精确查找
- 反向查询: 扩展API支持ppid/pgid查找
- 无大锁: 使用core的索引机制，避免全局锁

### 4. 可扩展性
**为未来功能预留空间**:
- rusage: 可在proc_append中添加统计字段
- WUNTRACED: 可扩展proc_state支持stopped状态
- 信号机制: 可基于现有IPC架构实现

## 未实现功能

### 需要额外系统调用支持:
- **WUNTRACED/WCONTINUED**: 需要信号机制 (kill, sigaction等)
- **rusage参数**: 需要资源统计收集机制

### 优先级评估:
- **低优先级**: rusage (资源统计)
- **中优先级**: WUNTRACED (调试支持)
- **高优先级**: 信号机制 (多个功能依赖)

## 总结

通过proc_registry + IPC阻塞机制，我们实现了**完整的Linux标准wait4**，包括所有pid选项和WNOHANG支持。这个实现展示了linux_layer扩展模型的强大之处：

1. ✅ **无需core/修改** - 纯linux_layer实现
2. ✅ **架构一致性** - IPC作为统一同步机制
3. ✅ **高效查询** - O(1)复杂度
4. ✅ **竞态安全** - 正确处理并发场景
5. ✅ **可扩展性** - 为未来功能预留空间

这为后续实现更复杂的进程管理功能奠定了坚实的基础。
