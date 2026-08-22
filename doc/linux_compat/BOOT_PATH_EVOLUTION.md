# Boot 路径演进（incbin → cpio → busybox → 正规 Linux 启动）

> **Purpose**: 把「怎么一路跑到 busybox + `run_all.sh`」写成**单一时间线**，供日后查「为什么现在长这样」。  
> **Not**: 已归档的 busybox 妥协账（见 [`archive/BUSYBOX_BOOT_DEFERRALS.md`](archive/BUSYBOX_BOOT_DEFERRALS.md)）、下一步计划（见 [`NEXT_PLAN.md`](NEXT_PLAN.md)）、rootfs 操作说明（见 [`ROOTFS.md`](ROOTFS.md)）、initramfs 原设计（见 [`INITRAMFS_PLAN.md`](INITRAMFS_PLAN.md)）。  
> **Last updated**: 2026-08-01

---

## 0. 术语（先消歧义）

仓库里「Path A / Path B」出现过**两套**含义，勿混用：

| 语境 | Path A | Path B |
|------|--------|--------|
| **用户首次进用户态**（[`SYSCALL_USER_RETURN_AND_EXECVE.md`](SYSCALL_USER_RETURN_AND_EXECVE.md)） | syscall 返回用户（execve 成功后） | 新线程 `run_elf_program` / PID1 Path B drop → append hook 搭栈 |
| **initramfs 镜像如何进内核**（[`INITRAMFS_PLAN.md`](INITRAMFS_PLAN.md) §5） | `.incbin` 把 cpio 链进内核 | QEMU `-initrd`（未作为主路径） |

下文默认用**第一套**（用户进场）。cpio 进内核目前是 **incbin Path A（initramfs 义）**。

---

## 1. 时间线（做了什么、为何）

### 阶段 1 — 嵌入测例 ELF 证明 syscall（~2026-04 → 05）

**做法**：`make user` 把 `user_payload` 测例链进 `link_app.o`（`.incbin` / `_num_app` / `program_map`），core harness **`gen_thread_from_elf`** 逐个跑（历史阶段；现已删除嵌入测例路径）。

**目的**：在**没有完整 FS** 时，证明 fork/exit/wait/brk/mmap/信号等基本 syscall 与多架构 harness（后到 52/52）。

**证据**：[`CROSS_ARCH_VERIFICATION_LOG.md`](CROSS_ARCH_VERIFICATION_LOG.md) §2026-05-19；早期清单见 `doc/ai` 归档。

**遗留**：测例生命周期绑在内核 for 循环 + 嵌入镜像上，不是 Linux 启动模型。

---

### 阶段 2 — 文件系统 + initramfs cpio（~2026-06 → 07-09）

**做法**：

- `rootfs/` → `build/rootfs.cpio` → 内核 `.incbin`（与测例 ELF **并存**一段时间）
- `vfs_server` + cpio/ramfs backend；用户 open/read/… 走 IPC
- 内核读文件：`vfs_kern_read_file_slice` + `page_slice`（exec/manifest）

**目的**：有真实路径与文件后，才能 exec 真实 ELF、再谈 busybox。

**证据**：verification log §2026-07-09（Phase 4 bootstrap，含 #8 execve）；设计见 `INITRAMFS_PLAN.md`、`VFS_ARCHITECTURE.md`、`FILE_LOADING.md`。

**双轨（已结束）**：曾 `filesystem:true` 进 cpio 同时保留 stub `link_app`；现仅 cpio。

---

### 阶段 3 — busybox 作为「像 Linux 一样」的用户态（~2026-07-12 → 07-30）

**做法**：

- static busybox + applet symlink（`/bin/ls` → busybox）
- **首次进用户**仍用 Path B（glibc 要 auxv/栈）；栈注入放在 append.`init` bootstrap
- 小修：pathname 逐字节读、aarch64 busybox `stat`、wait4 vs SIGCHLD、COW/PTE、`MEM_SIZE=512M` 等
- **VFS 定长 BSS / 栈上大数组** → `vfs_slice_table`（[`VFS_DYNAMIC_STORAGE.md`](VFS_DYNAMIC_STORAGE.md)，2026-07-27），否则 busybox/多测例容易炸栈或撑爆表

**目的**：验证「能跑真实用户态程序」，而不只是 musl 小测例。

**里程碑**：2026-07-13 `ls /bin` exit 0；历史妥协账 → [`archive/BUSYBOX_BOOT_DEFERRALS.md`](archive/BUSYBOX_BOOT_DEFERRALS.md)（已归档）。

---

### 阶段 4 — 编排从内核循环迁到用户 shell（~2026-07-28 → 07-31）

**做法**：

- 默认 spawn **`/init`**（symlink → `bin/busybox`）
- Boot argv：make 默认 `CMDLINE=sh /tests/run_all.sh`（可覆盖）；经 `cmdline_ptr` → `/init`
- `pack_user_rootfs.py` **生成**显式 `run_one` 列表的 `run_all.sh`（**禁止** ash `while read` 读 manifest——会 `poll`+逐字节 `read`，在缺 UART / 嵌套 VFS 时易卡死）
- exec / 构建：`link_app` / `_num_app` / 测例 ELF `.incbin` **已删除**；仅 `rootfs.cpio` `.incbin`
- poll：CONSOLE_IN 按 EOF/`POLLHUP`；等待走 `sleep_port`（与 nanosleep 同族）

**目的**：测例由 **PID1 shell** 拉起，接近「init → 脚本 → 子进程」的 Linux 形态。

---

### 阶段 5 — 当前（2026-08）：收干净残留 + 稳住 suite

**已做 / 进行中**：

| 项 | 状态 |
|----|------|
| 用户态 `run_all.sh` 编排 | ✅ 默认路径 |
| 嵌入测例 exec fallback | ✅ 已删 |
| stub `link_app.o` / `_num_app` | ✅ **已删除**（构建与源码） |
| cmdline → argv | ✅ make 默认 `CMDLINE=sh /tests/run_all.sh`；core `cmdline_ptr`；`linux_boot` 分词 |
| PID1 改 `execve("/init")` | ✅ `linux_exec_replace_image`；首次落入用户仍 Path B drop（无 syscall 帧） |
| VFS 客户端 RPC 不可中断 | 🔧 2026-08-01 已改代码，**待复跑验证**（见 §3） |

**目标态（「基本标准 Linux 启动」）**：

```text
内核解析 cmdline / 默认
  → execve("/init") 或等价
  → busybox init/ash
  → 用户脚本编排测例 / 真 init
不再依赖：嵌入测例 ELF、内核 manifest for 循环、硬编码 Path B argv
```

busybox 妥协账已关闭；后续见 [`NEXT_PLAN.md`](NEXT_PLAN.md)。历史细节见 [`archive/BUSYBOX_BOOT_DEFERRALS.md`](archive/BUSYBOX_BOOT_DEFERRALS.md)。

---

## 2. 当前默认执行路径（对照）

```text
make user          → rootfs/tests/* + manifest + 生成的 run_all.sh
make rootfs/build  → rootfs.cpio .incbin → 内核
make run

linux_boot:
  空 user task + thread
  → linux_exec_replace_image("/init", ["sh","/tests/run_all.sh"])
       （与 sys_execve 同一套 load/栈/auxv）
  → Path B arch_return_to_user 落入 /init
  envp = 空（脚本用绝对路径）

busybox ash:
  按 run_all.sh 的 run_one 逐个 sys_execve 测例 ELF
```

操作说明：[`script/rootfs/README.txt`](../../script/rootfs/README.txt)、[`ROOTFS.md`](ROOTFS.md)、[`USER_TESTS.md`](USER_TESTS.md)。

---

## 3. 2026-08-01：间歇卡在测例 `sys_exit` 之后

串口常见最后一行：`[PROC] sys_exit: clear_tid write failed`（已降噪）。  
真实卡点在其后的 `THREAD_REAP`：listen 死等 `zombie`，而 exitor 可能已是 `ready` 尚未跑到 `set_status(zombie)`。  
修复：`servers/clean_server.c` 在 IPC 结束后对 `ready` 提升为 zombie。

---

## 3b. 2026-08-01：`run_all` 卡在 `oscomp_munmap`（DUMP 结论）

**串口**：suite 已能跑过大量测例；停在：

```text
=== /tests/oscomp_munmap ===
[INFO] This test requires filesystem support
[vfs-be] ipc_call … ramfs … LOOKUP … leave ret=0
（之后无 write/mmap 日志）
```

**`qemu.log` + `objdump.log`（修复前镜像）**：

1. 卡死后 PC 几乎全在 `round_robin_schedule` / `switch_to` → **阻塞空转**，不是 poll 忙等。
2. 全程几乎采不到用户态 RIP；IPC/VFS 采样稀疏，但可见 `sys_openat`、`vfs_backend_lookup`、`ipc_rpc_call_va_flags`（**可中断**客户端）、后期仍有 `vfs_open_path`。
3. 中段出现 `ipc_rpc_unregister_port_by_pid`（进程 teardown 拆 `vfs_cli_*`），与「单线程 VFS + 阻塞 `send_msg(reply)`」叠在一起时，若客户端曾 `-EINTR` 放弃 `recv`，listen 线程会楔死——与 [`IPC_RPC_FRAMEWORK.md`](protocols/IPC_RPC_FRAMEWORK.md) §8 同类。
4. 更早的 `#PF`：`pc=CR2=0x57f485`、`e=0014`（用户取指）对应 `clone`/`fork`/`exit` status=139，**与本次 FS 卡死无关**。

**代码侧修复（待你复跑）**：`vfs_ipc_request_response` → `ipc_rpc_call_va_uninterruptible`（`linux_layer/fs/fs_ipc.c`）。Pattern Log：[`AI_CHECKLIST.md`](../ai/AI_CHECKLIST.md) 2026-08-01 条。

---

## 4. 文档角色（谁写什么）

| 文档 | 职责 |
|------|------|
| **本文** | 演进叙事 + 当前路径图 + 关键事故索引 |
| [`NEXT_PLAN.md`](NEXT_PLAN.md) | busybox 收尾后的下一步（live） |
| [`archive/BUSYBOX_BOOT_DEFERRALS.md`](archive/BUSYBOX_BOOT_DEFERRALS.md) | 历史妥协账（已归档） |
| [`INITRAMFS_PLAN.md`](INITRAMFS_PLAN.md) | 当初 Phase 4 设计意图 |
| [`PROGRESS.md`](PROGRESS.md) | 阶段索引（链到本文） |
| [`CROSS_ARCH_VERIFICATION_LOG.md`](CROSS_ARCH_VERIFICATION_LOG.md) | 配对跑通证据（应继续 append busybox/`run_all` gate） |
| [`doc/ai/ASSIST_HISTORY.md`](../ai/ASSIST_HISTORY.md) | 合入后的短日志（boot 长叙事不放这里） |

---

## 5. 里程碑速查

| 日期 | 里程碑 |
|------|--------|
| 2026-05-19 | 多架构 harness 52/52（嵌入测例时代） |
| 2026-06-13 | INITRAMFS 方案定稿；wait4/SIGCHLD 等 |
| 2026-07-09 | Phase 4 FS bootstrap + initramfs execve |
| 2026-07-13 | busybox `ls /bin` exit 0 |
| 2026-07-27 | VFS 动态表（去栈/BSS 炸弹） |
| 2026-07-31 | `/init` + `sh /tests/run_all.sh`；pack 生成编排；去 embedded exec |
| 2026-08-01 | `run_all` 中段卡死定位（VFS 可中断 RPC）；本文归档 |
| 2026-08-09 | busybox 妥协账归档；[`NEXT_PLAN.md`](NEXT_PLAN.md)；CMDLINE 策略仅上层注入 |
