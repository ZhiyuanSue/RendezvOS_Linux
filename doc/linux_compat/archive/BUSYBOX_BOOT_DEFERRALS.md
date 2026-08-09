# Busybox 启动 — 临时妥协与待修项（已归档）

> **Status**: **ARCHIVED**（2026-08-09）— busybox bring-up 妥协账关闭  
> **结论**: 初步启动 busybox 改造视为完成；后续见 [`../NEXT_PLAN.md`](../NEXT_PLAN.md)  
> **演进叙事**: [`../BOOT_PATH_EVOLUTION.md`](../BOOT_PATH_EVOLUTION.md)  
> **相关（历史）**: [`../INITRAMFS_PLAN.md`](../INITRAMFS_PLAN.md) · [`../VFS_DYNAMIC_STORAGE.md`](../VFS_DYNAMIC_STORAGE.md) · [`../EXECVE_IMPLEMENTATION_STATUS.md`](../EXECVE_IMPLEMENTATION_STATUS.md) · [`../USER_TESTS.md`](../USER_TESTS.md) · [`../SYSCALLS.md`](../SYSCALLS.md) · [`../protocols/IPC_RPC_FRAMEWORK.md`](../protocols/IPC_RPC_FRAMEWORK.md) · [`../../ai/DECISIONS.md`](../../ai/DECISIONS.md)

本文是 **为尽快跑通 busybox demo 而做的妥协** 的历史账本，**不再作 live 真源**。  
文中「仍开放」在归档时重分类：交互 UART / `/dev` 等 → [`../NEXT_PLAN.md`](../NEXT_PLAN.md)（额外或主线可选）；勿再当作 busybox P0 阻塞项。

---

## 归档时状态一览（2026-08-09）

### 已收（busybox gate 相关）

| 项 | 落点 |
|----|------|
| `/init` → `bin/busybox` symlink | rootfs pack |
| 默认 boot：`sh /tests/run_all.sh`；可用 core `cmdline_ptr`（QEMU `CMDLINE`）覆盖 | `linux_boot.c` |
| pack 生成 `run_all.sh`（显式 `run_one`，禁 ash `while read`） | `pack_user_rootfs.py` |
| PID1：空 task + `linux_exec_replace_image("/init")` | 与 `sys_execve` 同栈/auxv 路径 |
| IPC reply 会合楔死（ops gate / uninterruptible / 不弃 recv） | core + `IPC_RPC_FRAMEWORK` |
| clone/fork `#PF` @ `0x57f485` | x86 `run_all` 曾 **52/52** |
| pathname 按页 chunk 至 NUL | `linux_mm_load_cstring_from_user` |
| auxv：`AT_HWCAP` / `AT_EXECFN` / 伪随机 `AT_RANDOM` | 硬件熵仍可选（见下） |
| VFS 定长 BSS / 栈炸弹；link_app / `_num_app`；内核 `tests/` 编排器 | 已删或迁 `linux_boot` |
| **VFS listen coop**：全部 listen opcode；leaf nest reply = **transfer**；sync = blocking `ipc_rpc_reply` | `vfs_coop*.c` · DECISIONS 2026-08-02/03/06 |

### 归档时迁出（不再算 busybox 妥协开放项）

| 原优先级 | 项 | 迁往 |
|----------|-----|------|
| P1 | CONSOLE / UART RX / 交互 ash | [`../NEXT_PLAN.md`](../NEXT_PLAN.md) §3（额外：uart_server） |
| P1 | auxv / getrandom 熵 | 可选 polish；不挡 gate |
| P1 | aarch64 SMP 体感慢 | [`../NEXT_PLAN.md`](../NEXT_PLAN.md) §2 D |
| P2 | busybox 构建（static / 关 tc） | [`../NEXT_PLAN.md`](../NEXT_PLAN.md) §2 E |
| P3 | `/dev/*`、shebang、头文件分层 | VFS / exec 主线可选 |
| 扩展 | readdir / tombstone / 256KiB | [`../VFS_DYNAMIC_STORAGE.md`](../VFS_DYNAMIC_STORAGE.md) |
| 演进 | nested request 直发 | 刻意不做，见 NEXT_PLAN §5 |

**归档后下一步** → [`../NEXT_PLAN.md`](../NEXT_PLAN.md)（含 core TODO 索引 + UART server）。

### Exec / 进用户：已收 vs 仍算「特例」的

| 项 | 状态 |
|----|------|
| 内核 `gen_task_from_elf` 起测例 / busybox demo | ✅ 已删；测例经 `/init`→`run_all`→**用户态 `execve`** |
| append.init 二次 bootstrap / PT_NOTE 分流搭栈 | ✅ 已删；栈只由 `linux_exec_replace_image` 建 |
| PID1 镜像+argv/auxv | ✅ `linux_exec_replace_image("/init")`（与 `sys_execve` 同路） |
| boot argv | ✅ make 默认注入 `CMDLINE=sh /tests/run_all.sh`；`cmdline_ptr` 覆盖 |
| PID1 **首次落入用户** | 仍用 Path B：`arch_return_to_user`（无 syscall 帧，与 fork 子线程首次进用户同类；**不是**再走一套 loader） |

**不是**「再搞一套内核 spawn ELF」；用户态内容已经统一。剩下的 Path B drop 是「内核线程第一次进 EL0」的出口，不是旧 demo 特例。

---

## P0 — IPC request–reply 偶发卡死（busybox `run_all`，2026-08-01）

> 与 Path B 共存的 **协议/生命周期缺口**；调度器空转是表象。权威协议：[`protocols/IPC_RPC_FRAMEWORK.md`](../protocols/IPC_RPC_FRAMEWORK.md) §6–§8。  
> **推进方式**：下面子项 **一个一个** 对齐再改，不打包一次改完。  
> **状态（2026-08-06）**：子项 1–5 ✅；nested reply 已改为 `ipc_transfer_message`（`@n` token）；request 仍走 backend listen port。

### 现象（已用三件套确认）

| 观察 | 含义 |
|------|------|
| 串口停在 `=== /tests/oscomp_munmap ===` → INFO 后无 open 结果 | 卡在该测例的 FS 路径（常为 `open`→VFS RPC） |
| DUMP 后期几乎只有 `schedule` / `round_robin` / `idle` / `ebr_*` | 干活线程在 IPC `block_on_*`；CPU 上 idle 合法反复 `schedule` |
| 偶发、前面 fork/clone/exit 139 多时更易出现 | 与「进程 teardown ↔ reply `send_msg`」时序竞态一致 |
| VFS 已 `ipc_rpc_call_va_uninterruptible` | 堵住「pending SIGCHLD → `-EINTR` 弃 recv」；**仍不够** |

调度侧曾修「current 查找时仍是 `running` → RR 空转」；修好后 DUMP **仍可**满是 schedule——那是 **全员堵在协议上** 的正常表象，不是又回到 RR while 死循环。

### 协议楔形（一句话）

单线程 VFS listen：`handler` 后 **`send_msg(reply)` 阻塞会合**。Client 侧 reply port 若在会合完成前被 **unregister / 进程退出**，而 server 仍握着 lookup ref 卡在 `block_on_send`，则 listen **永不再 `recv`** → 后续所有 FS RPC 全堵。

### 待推进子项（按序）

| # | 子项 | 状态 | 说明 |
|---|------|------|------|
| **1** | VFS / TASK_REAP 等走 uninterruptible | ✅ | 防 commit 前误用 interruptible 路径 |
| **2** | **unregister 时唤醒 port 等待者** | ✅ 2026-08-01 | per-port **ops gate**（见下） |
| **3** | interruptible RPC：`send` 成功后禁止弃 recv | ✅ 2026-08-01 | `ipc_rpc_call_va_flags`：EINTR 仅 commit 前；之后 drain interrupt / 重试 recv |
| **4** | reply / EXIT_NOTIFY 勿静默丢消息 | ✅ 2026-08-01 | `ipc_rpc_send_reply` 分配失败重试；EXIT_NOTIFY OOM 重试 + 失败走 pending+poke；`PORT_CLOSED` 视为已处理 |
| **5** | clone/fork `#PF` 0x57f485 | ✅ 2026-08-02 | `linux_signal_proc_reset`；x86 `run_all` **pass=52 fail=0**（`x86_64_run.log`） |

### 子项 2 说明：为什么说「unregister 时就要 clean」，会不会「每次减 ref 都 clean」？

**不是**「无论何时、每次 `ref_put` 都调用 `port_clean_thread_queue`」。  
**是**：「**unregister（从全局名表摘掉 = 协议上关闭该 port）时** 就必须 drain 等待队列」，**不要等到** refcount 归零才 clean。

**修复前**的楔形（为何不能绑 `ref_put→0`）：

```text
unregister_port  →  摘表 + ref_put(表档)     ← 旧：这里不 clean
ref_put → 0      →  delete → port_clean       ← 旧：只在最后 free
```

`ipc_rpc_send_reply` 在整个 `send_msg` 期间 **lookup 持 ref**。若此时 client `ipc_rpc_unregister_port_by_pid`：表 ref 放掉但 server 仍握 lookup → **refcount > 0** → **不 free** → **不 clean** → server `block_on_send` 永醒不来 → listen 楔死。

| 问题 | 答案 |
|------|------|
| clean 绑在什么事件上？ | **unregister / close（语义关闭）一次**，不是每个 `ref_put` |
| 为什么不能等 refcount==0？ | 归零只表示「没有指针还握着结构体」；**关闭名表项**时等待者就该醒，哪怕还有 in-flight lookup |
| 多次 `ref_put` 会多次 clean 吗？ | **不会**——clean 在 `unregister_port`（每注册生命周期至多一次）；`delete` 再 drain 一次只是幂等空转 |

**已落地（ops gate，命名以源码为准）**：

```text
unregister_port
  → name_index_unregister（on_unregister: REGISTERED→CLOSING）
  → 等 ops_count==0（阻塞路径在 schedule 前 port_ops_end）
  → port_clean_thread_queue（醒等待者 + PORT_CLOSED / kmsg）
  → CLOSING→CLOSED
  → ref_put(表档)
```

send/recv/try 包在 `port_ops_begin/end`；醒来认 `THREAD_FLAG_IPC_PORT_CLOSED`（send 另 drop orphan）。`ipc_rpc_send_reply` 认 `-E_REND_PORT_CLOSED`。`delete` 仍防御性 drain。

### 诊断（卡在 FS 测例时）

1. `LOG=true DUMP=true`；忽略 Ctrl-A+X 过晚的时钟尾噪声  
2. 看卡死前是否有 `#PF` / 进程退出与 FS RPC 交错  
3. 卡死后采样：无 `vfs_*` / `send_msg` 进度、只有 schedule → 优先查 reply 会合 / teardown，而不是再改 RR

---

## 当前现象（2026-07-13，x86_64；后续 aarch64 亦通）

52/52 harness 通过后，`user_test_runner` spawn `/bin/busybox`（Path B，`argv = ["ls", "/bin"]`）：

```text
[ Linux compat ] Trying initramfs /bin/busybox demo
start gen task from elf slice ... vs ...
…（newfstatat / openat / getdents64 / write）…
ash
busybox
cat
…（/bin 下列表）…
[ LINUX USER ] demo '/bin/busybox' exit_code=0
```

### user payload 与 `ls` 的关系

| 路径 | `make user` 放入内容 |
|------|----------------------|
| `rootfs/tests/*` | musl 测例 ELF + `manifest`（`pack_user_rootfs.py`） |
| `rootfs/bin/busybox` | 仅 static busybox（`user.json` `"busybox": true` → `build_busybox.sh`） |
| `rootfs/bin/ls` 等 | **symlink → busybox**（demo applet 子集），**不是**独立 `ls` ELF |

demo 的 **`argv[0]="ls"`** 是 busybox **多调用名**；进程镜像是 `/bin/busybox`。argv/auxv 在新线程 **`run_elf_program` → append.init → `linux_exec_bootstrap_elf_spawn_stack`** 里注入（与 `sys_execve` 共用 `linux_exec_build_initial_stack`）。  
**注意**：bootstrap **不在** `gen_task_from_elf` 返回前执行；曾尝试的「spawn 前 set_argv / 返回后 clear」会在 bootstrap 前清掉 pending → 退化成 Usage 帮助页。  
**当前 demo argv**：`sh` `-c` `/bin/ls /bin; echo SHELL_OK`（验证 ash；**须绝对路径**——Path B 空 envp，裸 `ls` → `not found`）。勿用裸 `sh`。  
QEMU 默认 **`MEM_SIZE=512M`**（仅 `run` 的 `-m`，**不必**为改内存而 `config`）。ash `fork` 忙盒子时若出现大量 `pmm_alloc failed`，先加大内存再查泄漏。

| 项 | 状态 |
|----|------|
| Spawn / ELF 加载 | ✅ `gen_task_from_elf` 成功 |
| glibc `_start` / 进用户 | ✅ 不再 exit **139** |
| 用户态有输出 | ✅ |
| **`ls /bin` 列目录** | ✅ **exit_code=0**，stdout 打出 applet 列表 |
| busybox 所需 syscall stub | ✅ 见 §P1 syscall（够用，非完整） |
| VFS 表容量（曾抬到 2048 BSS） | ✅ 已迁 `vfs_slice_table` |

典型 syscall 序列（成功一次）：`newfstatat(/bin)` → `openat` → `fstat` → 多次 `getdents64` → `close` → `write(1, …)`。

---

## P0 — Path B 栈与首次进用户（PID1 ✅；**loader 痕迹仍属妥协**）

> **2026-08-03**：PID1 已 `linux_exec_replace_image("/init")`（与 sys_execve 同路径）。下文「Path B / append.init / PT_NOTE」仍描述 **loader 侧残留**，见文首开放项。

### 1. Demo 仍走 `gen_task_from_elf`，非 execve

| 项 | 现状 | 应改为 |
|----|------|--------|
| 测例 spawn | 用户态经 `/init`→`run_all`→exec；内核侧偶发仍 `gen_task_from_elf` | 统一 **execve 级** API |
| 用户栈 | core fake return + compat **二次 bootstrap** 痕迹可能仍在 | **execve 级**完整栈（或 core 统一 `run_elf_program` 协议） |

内置 musl 测例 ELF 自带简单 `_start`；**static glibc busybox** 需要标准 Linux 栈 + auxv。

### 2. 现行临时方案（2026-07-13 定稿）

**core**（`run_elf_program`，两架构）：

1. fake return 后调用 `thread.append.init`
2. **重新读取** `arch_get_thread_user_sp(ctx)`（hook 可能已改 SP）
3. `arch_empty_drop_trap_frame` + **`arch_syscall_set_user_return`**（`syscall_ret=0`）→ `arch_return_to_user`  
   — 与 Path A 共用 live SP commit（x86 `user_rsp_scratch` / aarch64 `SP_EL0`）

**compat**（`linux_thread_append_init`，**唯一**栈注入点）：

1. `linux_exec_bootstrap_elf_spawn_stack(thread, vs, info)`
2. **不在** `linux_spawn_and_wait_test_path` 写 argv（已删除 `spawn_user_argv*` 耦合）
3. 检测：无 `PT_INTERP` 且 **`PT_NOTE` 段数 > 1**（musl harness = 1，static glibc busybox = 3）— 不用扫 `"GLIBC"` 字符串（busybox 里约 2.1MB 处，易漏检）
4. 默认 demo argv：bootstrap 内硬编码 `sh -c '/bin/ls /bin; echo SHELL_OK'`（临时）
5. 栈布局（低→高）：`argc, argv[], NULL, envp[], NULL, auxv(…), random16, argv strings`
6. glibc auxv：`AT_PHDR/PHENT/PHNUM/PAGESZ/ENTRY/UID…/HWCAP/RANDOM/EXECFN`（`AT_RANDOM` 为伪随机混入，非硬件熵）

**涉及文件**:

- `core/kernel/task/thread_loader.c`（`run_elf_program` 调 append.init）
- `linux_layer/loader/linux_elf_init.c`
- `linux_layer/proc/linux_exec_stack.c`
- `include/linux_compat/proc/linux_exec_stack.h`

**仍缺 / 待正规化**:

- demo argv 仍硬编码在 bootstrap（跨 `gen_task_from_elf` 传 pending **不可行**，除非挂在 thread/task 上）
- 真随机熵源（`getrandom` / 硬件）
- 去掉 fake return + 二次 bootstrap，改 execve 或 core 统一协议

### 3. 「spawn」应是什么？（说明）

| 名称 | 含义 |
|------|------|
| **现状 spawn** | 内核 `gen_task_from_elf`：新建进程、加载 ELF、Path B 进用户。**不是** Linux `execve` |
| **Linux execve** | **已有进程** 调用 syscall，换掉自己的地址空间与栈，返回用户入口 |
| **目标 demo** | 最终应由 init/shell **`execve("/bin/busybox", ["ls","/bin"], …)`**；或 harness 先建最小进程再走同一套 execve 代码 |

**时序陷阱（2026-07-28）**：`linux_thread_append_init` / 栈注入发生在新线程的 `run_elf_program` 里，**晚于** `gen_task_from_elf` 返回。因此「调用方 set_argv → gen_task → clear_argv」会在 bootstrap 前清掉 argv（BusyBox 只打印 Usage）。在完整 init 路径落地前，保持 bootstrap 内硬编码 demo argv。

### 4. musl 测例 vs busybox glibc static

| 组件 | 工具链 | Path B 栈 |
|------|--------|-----------|
| `tests/*` | musl / 简单 `_start` | 仅 fake return（`PT_NOTE` = 1） |
| `rootfs/bin/busybox` | `x86_64-linux-gnu-gcc` + `CONFIG_STATIC=y` | 完整栈 + auxv（`PT_NOTE` > 1） |

---

## P1 — busybox demo syscall（已实现 stub，**深度仍属妥协**）

日志中曾出现的 **`unimplemented`** 与实现位置：

| NR (x86_64) | 名称 | 实现文件 | 行为摘要 |
|-------------|------|----------|----------|
| 318 | `getrandom` | `misc/sys_random.c` | 伪随机填充用户缓冲（≤256B） |
| 302 | `prlimit64` | `misc/sys_resource.c` | 查询返回默认 rlimit；set 基本 no-op |
| 16 | `ioctl` | `misc/sys_ioctl.c` | `TIOCGWINSZ` → 80×24；`TCGETS` → `ENOTTY` |
| 102 | `getuid` | `proc/sys_id.c` | `linux_proc_append.uid` |
| 104 | `getgid` | `proc/sys_id.c` | `linux_proc_append.gid` |
| 105 | `setuid` | `proc/sys_id.c` | root/no-op；非 root 改 uid → `EPERM` |
| 106 | `setgid` | `proc/sys_id.c` | 同上 |
| 201 | `time` | `time/sys_legacy_time.c` | x86_64 only |
| 334 | `rseq` | `misc/sys_rseq.c` | `ENOSYS`（glibc 回退） |

接线：`linux_layer/syscall/syscall_entry.c`。

### `ls /bin` EFAULT 根因与修复（2026-07-13）✅

**实测**（`[VFS-STAT]` 日志）：失败在 **syscall 侧读 pathname**，未到 VFS server。

| 现象 | 说明 |
|------|------|
| `path_va≈0x7fffffffeff3` | glibc/busybox 把 `"/bin"` 放在**用户栈顶附近** |
| 旧代码 `linux_mm_load_from_user(..., 256)` | 一次读满 `LINUX_VFS_PATH_MAX`，越过 `USER_SPACE_TOP` 下方 **未映射 guard 页** → `EFAULT` |
| 修复 | `linux_mm_load_cstring_from_user()`：按字节读到 `'\0'`，不 bulk 读 256B |

**这不是「栈只有 256 字节」**；`thread_ustack_page_num=8`（32KiB）对 demo 够用。问题是 **路径指针贴栈顶 + 固定长度 bulk 拷贝** 的交互。

**并行修复（仍保留，VFS 路径正确性）**：

1. `NEWFSTATAT` RPC：`p1=statbuf`、`p2=flags`（`vfs_server.c`）
2. `vfs_kstat_t` → `linux_user_stat_t` 再 `linux_mm_store_to_user`（`vfs_open.c`）

**涉及**：`linux_layer/mm/linux_mm_radix.c`, `linux_layer/fs/sys_fs_impl.c`, `servers/fs/vfs_server.c`, `servers/fs/vfs_open.c`, `include/linux_compat/fs/linux_user_stat.h`

### 妥协：用户路径字符串加载（P1，待正规化）

| 项 | 现状 | 应改为 |
|----|------|--------|
| path syscall 读用户 pathname | ✅ `linux_mm_load_cstring_from_user`（**按页 chunk** 至 NUL） | — |
| `LINUX_VFS_PATH_MAX`（256） | 仍作上限；超长无 NUL → `EINVAL` | 与 Linux `PATH_MAX` / `ENAMETOOLONG` 语义对齐 |
| 用户栈大小 | core `thread_ustack_page_num=8` | 若深栈 / 大 `alloca` 测例再评估；**非本次 EFAULT 主因** |

**文件**：`include/linux_compat/linux_mm_radix.h`, `linux_layer/fs/sys_fs_impl.c`（`sys_fs_load_pathname`）

---

## P1 — initramfs / VFS 容量妥协（✅ 已回收 → 动态表）

> **2026-07-27 回收完成**：`vfs_slice_table`（page_slice 可增长）覆盖 S0–S3。权威说明见 [`VFS_DYNAMIC_STORAGE.md`](../VFS_DYNAMIC_STORAGE.md)。下列为历史记录。

### cpio / namespace 上限（历史）

| 原值 | 曾调至 | 原因 |
|------|--------|------|
| `CPIO_ROFS_MAX_ENTRIES` **64** | **2048** | busybox applet symlink + tests |
| `VFS_NS_MAX_NODES` **256** | **2048** | 与 cpio 条目同量级 |

**现况**：无固定 BSS 容量；软上限 `VFS_SLICE_TABLE_SOFT_MAX`（小表另有 per-table soft_max）。

### `cpio_rofs_readdir` static BSS（历史）

曾用 `static char names[2048][64]`（~128KiB）。**现况**：临时 `vfs_slice_table` 存名字，用完销毁。

### 动态表后仍属「扩展性」而非 busybox 阻塞

| 项 | 说明 |
|----|------|
| 大目录 readdir O(n²) | 每次 index 重建名字表 |
| tombstone 不压缩 | ramfs / ns deleted 槽位 |
| ramfs / pcache 256KiB 文件上限 | 策略上限 |
| cpio/ramfs 扁平 `path[]` | 主键，有意保留 |

详见 [`VFS_DYNAMIC_STORAGE.md`](../VFS_DYNAMIC_STORAGE.md)「建议后续」。

---

## P2 — 构建与 rootfs（妥协仍在）

| 妥协 | 说明 |
|------|------|
| `CONFIG_TC=n` | Linux 6.8+ 无 CBQ UAPI |
| `CONFIG_STATIC=y` | static glibc busybox |
| `BUSYBOX_AUTO_FETCH=1` | 自动拉 busybox 1.36.1 |
| 默认 `BUSYBOX_FULL=1` | `--list` 全 symlink；`user.json` `busybox_full:false` 可缩回 demo 子集 |

**文件**: `script/rootfs/build_busybox.sh`

---

## P3 — 测试 harness（✅ 已迁：内核侧改名/删除）

> **2026-08-02**：入口为 `linux_layer/init/linux_boot.c` + `boot_wait_cookie`；内核 manifest 循环已删。下文保留演进叙事。

原 `user_test_runner.c` 曾是 **BSP 内核编排器**，不是用户态 init：

| 步骤 | 机制 |
|------|------|
| 读清单 | 内核 `vfs_kern_read_file_slice("/tests/manifest")`（**不走**用户 `open/read`） |
| 逐个测例 | `vfs_kern_read_file_slice(path)` → **`gen_task_from_elf`**（**不是 execve**） |
| 等待结束 | `test_cookie` + `clean_server` → `linux_user_test_notify_exit` |
| 判定 | harness 只判 spawn/等待是否成功；**不解析** stdout 里 `[PASS]`/`[FAIL]` |

末尾 demo：仍 `linux_spawn_and_wait_test_path("/bin/busybox", …)`，argv 在 bootstrap 写死为 `sh -c '/bin/ls /bin; echo SHELL_OK'`。

- 打印 **`exit_code`**；`RENDEZVOS_ROOT_AUTO_POWEROFF` 后 shutdown  
- **待修**：独立 demo / `execve`；busybox 失败时不 auto poweroff（可配置）

**相关**：[`USER_TESTS.md`](../USER_TESTS.md) · `script/config/pack_user_rootfs.py`（生成 `rootfs/tests/manifest`）

---

## P3 — 白话：`run_all.sh` 迁移是什么？

> **2026-08-03**：阶段 A（内核只起一次 `/init`→`sh /tests/run_all.sh`，pack 生成显式 `run_one`）**已完成**。下文保留「为何迁 / 曾为何不能立刻删 harness」叙事。

**曾用 harness**：内核 C 自己读 manifest、`gen_task_from_elf` 循环。

**现用**：内核只 `exec` 一次 busybox；脚本编排测例（更接近 Linux initramfs）。

| | 内核 harness（旧） | `run_all.sh`（现） |
|--|---------------------|----------------------|
| 谁读清单 / 起测例 | 内核 | pack 展开的 shell + 用户态 exec |
| 等结束 | `test_cookie` | shell 顺序 / wait |
| busybox demo | 另一次内核 spawn | 已并入 `/init` 路径 |

**仍属妥协**：进用户 Path B 痕迹；无 shebang（显式 `sh script`）。

---

## P3 — 测例编排：迁到 busybox 脚本（阶段 A ✅）

> **方向**：用 **`busybox sh /tests/run_all.sh`** 替代 C 里写死的 manifest 循环 — **已落地**（pack 显式 `run_one`，非 `while read`）。

### 分阶段（更新）

| 阶段 | 内容 | 状态 |
|------|------|------|
| **A** | 内核只启 `/init`→`sh /tests/run_all.sh`；pack 生成脚本 | ✅ |
| **B** | 去掉 Path B 特例；缩 loader 特例 | 开放（见文首） |
| **C** | `/dev`、交互 console、可选 shebang；harness 残留全清 | 开放 |

### 迁移前曾需验证（多数已过）

1. musl 测例经用户态 `execve`（非纯 `gen_task_from_elf`）— 随 `run_all` 路径验证  
2. ash 跑脚本 / fork / wait — ✅（编排用显式 `run_one`）  
3. SMP 与纯 shell 单进程顺序 — 仍按 [`USER_TESTS.md`](../USER_TESTS.md) 单独决策  

---

## P3 — 尚未实现、busybox 后续可能需要

| 项 | 状态 |
|----|------|
| `/dev/null`、`/dev/console` | 未做 |
| 内核 cmdline → argv | ✅ `cmdline_ptr` 空白分词覆盖 `/init` argv；空则默认 `sh /tests/run_all.sh` |
| `read(0)` UART RX | EOF / POLLHUP stub |
| 完整 `mount` / umount | 部分；listen 上仍多为同步路径 |
| shebang `#!` | 未做 |

---

## P3 — embedded `_num_app` / `program_map`（✅ 已删除）

| 已删除 | 说明 |
|--------|------|
| `script/config/user_payload_link_app.py`、`stub_link_app.S` | 不再生成 ELF `.incbin` / `_num_app` 表 |
| `linux_layer/tests/task_test.c`、`misc/num_app_stub.c` | 无调用方 |
| `user.py` 非 filesystem 分支 | 仅 cpio + busybox；`filesystem:false` 直接报错 |

**仍保留（不同机制）**：`build/rootfs.cpio` 经 Makefile 生成的 `rootfs_cpio.S` **`.incbin` 进内核**——这是 initramfs 镜像嵌入，不是测例 `link_app`。

详见 [`FILE_LOADING.md`](../FILE_LOADING.md)、[`BOOT_PATH_EVOLUTION.md`](../BOOT_PATH_EVOLUTION.md)。

---

## P3 — 头文件结构（待整理）

部分头文件同时被 **fs server**（`servers/fs/`）与 **compat fs**（`linux_layer/fs/`、`include/linux_compat/fs/`）使用，缺少清晰的「公共协议 vs server 私有 vs compat 私有」分层。

| 方向 | 说明 |
|------|------|
| 公共 | opcodes、path 常量、`linux_user_stat`、port 命名 |
| server 私有 | `vfs_slice_table`、namespace、backend 内部 |
| compat 私有 | fd 表、syscall 包装 |

**非功能阻塞**；在下一轮 fs 目录整理时做即可。

---

## 诊断 checklist

| 现象 | 查什么 |
|------|--------|
| 无 `start gen task from elf` | VFS / cpio 无 `/bin/busybox` |
| **exit 139** | Path B SP 未 commit；或栈未 bootstrap（查 `PT_NOTE` 检测、hook 是否跑） |
| 有 syscall 无输出 | auxv / `getrandom` |
| **`ls: … Bad address`** | pathname **bulk 256B** 越过栈顶 guard；查 `linux_mm_load_cstring_from_user` |
| **aarch64：`ls /bin` 只打印 `/bin` 不列目录** | `struct stat` 布局：a64 为 `mode` 在 `nlink` 前；曾误用 x86 布局 → `S_ISDIR` 失败。查 `linux_user_stat.h` |
| **exit 0** + `/bin` 列表 | ✅ demo 目标达成 |
| **exit 1** 且有 stderr 文案 | 用户态已跑通，属功能/compat 缺口非 spawn 失败 |
| 旧 `.o` 导致端口/表行为怪异 | 全量 `make ARCH=… build`（`-MMD` 依赖；勿混用过期对象） |

---

## Changelog

| 日期 | 变更 |
|------|------|
| 2026-08-03 | **VFS rename/link/getdents → coop** nested park；开放项去掉这三项 |
| 2026-08-03 | **开放项刷新**：文首改为「已收 / 仍开放」；VFS path coop + leaf REPLIED 记入已收；`run_all` 阶段 A ✅；修正 `BUSYBOX_FULL` 默认；建议下一步重排 |
| 2026-08-02 | **aarch64 MIDR undef**：`AT_HWCAP` 勿设 `HWCAP_CPUID`（无 EL0 MRS 模拟时 glibc `mrs midr_el1` → unknown trap） |
| 2026-08-02 | **PID1 exec 合并**：`linux_exec_replace_image` 供 sys_execve + boot；空 task 起 `/init`；删除 bootstrap 栈特例 |
| 2026-08-02 | **boot 命名重构**：删 `linux_layer/tests/*` 与 `test_*.h`；`linux_boot.c` + `boot_wait_cookie`；默认 `BUSYBOX_FULL=1` |
| 2026-08-02 | **清除 link_app 整套**：删脚本/`task_test`/`_num_app`；`user.py` 仅 cpio/busybox；保留 rootfs.cpio `.incbin` |
| 2026-08-02 | **不动 core / 不迁 VFS coop**：pathname 按页 chunk；auxv `AT_HWCAP`/`AT_EXECFN`+加强 `AT_RANDOM`；`init_thread`→`boot_thread` |
| 2026-08-01 | **工作区盘点**：[`PROGRESS.md`](../PROGRESS.md) §8（core ops gate / compat IPC / boot·FS stub 切分）；aarch64 相对 incbin 体感变慢记入开放项 |
| 2026-08-01 | **验证**：busybox `run_all` 跑完 `pass=41 fail=11`，IPC 楔死不再出现；失败簇为 `#PF 0x57f485` + mount -19 + ch2b_exit 255 |
| 2026-08-01 | **子项 3–4 + EXIT_NOTIFY**：RPC post-send 不弃 recv；reply/EXIT_NOTIFY 分配重试；notify 失败 fallback pending+poke；`PORT_CLOSED` 不当事故丢消息 |
| 2026-08-01 | **子项 2 审阅对齐**：life 仅 REGISTERED 可 begin；`ops_count` 命名；doc 去掉「今天不 clean」过时叙述；手写并发注释不动 |
| 2026-08-01 | **子项 2（ops gate）**：`port_ops_begin/end` + unregister `CLOSING`/等 `ops_count`/`port_clean`/`CLOSED`；阻塞 `end` 在 `schedule` 前；醒来 `PORT_CLOSED`+drop orphan；取代「到处查 registered / 自 drain」丑方案 |
| 2026-08-01 | **IPC reply 会合楔死**：记入 P0 开放项与专节；VFS uninterruptible 不足；澄清 unregister-time `port_clean` ≠ 每次 `ref_put` |
| 2026-07-31 | **run_all 编排模型**：pack 时展开 `run_all.sh`（显式 `run_one`），禁止 ash `while read` manifest；CONSOLE_IN=EOF 设备（poll→POLLHUP）；cpio READ 直读 blob（绕开嵌套 IPC 下 page_slice 填充挂死） |
| 2026-07-31 | **poll IPC 模型**：禁止 `timeout<0` 直接 return 0（ash `poll(…,-1)` 空转；DUMP 见 RAX=7/RDX=-1）。有限等 → `linux_time_sleep_until_count`；CONSOLE_IN 无限等 → `POLLHUP`（无 UART RX 生产者前）；`ppoll(NULL tsp)`=无限 |
| 2026-07-31 | **poll 空转**：CONSOLE_IN 上 always-POLLIN + `read`→0 → ash spin（`run_all start` 后挂，RIP 在 syscall/schedule）；CONSOLE_IN 不再报 POLLIN |
| 2026-07-31 | **poll/ppoll stub**：`run_all.sh` `while read` 曾 ENOSYS id=7（x86 `__NR_poll`）→ 空跑完；乐观 always-ready |
| 2026-07-31 | **正规 boot**：`/init`→`bin/busybox`；argv=`sh /tests/run_all.sh`；去掉 freestanding init 与 exec embedded `program_map` fallback |
| 2026-07-31 | **阶段 B**（已回收）：曾用 freestanding `/init` + execve busybox；现改回 symlink 模型 |
| 2026-07-31 | **阶段 A**：`linux_boot` Path B `/bin/busybox` + `boot_smoke.sh`；跳过内核 manifest 套件 |
| 2026-07-30 | ash `AFTER_LS` hang：根因 core COW 子 PTE 可写（父子共享页应两侧 RO）；兼容层 Channel R/S + RX stub 保留 |
| 2026-07-29 | ash `SHELL_OK` 前：SIGCHLD 曾 EINTR wait4；现 SIGCHLD 不打断 wait；禁止 RW 栈 EXEC trampoline |
| 2026-07-28 | ash smoke：裸 `ls`→not found（空 envp）；改 `/bin/ls`；默认 `MEM_SIZE` 256→512M（仅 QEMU `-m`，无需 reconfig） |
| 2026-07-28 | **回归**：pending argv 跨 `gen_task_from_elf` 有竞态 → BusyBox Usage；恢复 bootstrap 硬编码，并文档化时序 |
| 2026-07-28 | 白话说明 `run_all.sh`；spawn API 封装 argv（后回滚） |
| 2026-07-27 | **VFS 容量妥协回收**：S0–S3 → `vfs_slice_table`；文首增加「剩余开放项一览」；去掉「收紧 cpio/ns」旧建议；补 `_num_app`/头文件分层节 |
| 2026-07-24 | **aarch64 `ls /bin` 只打印路径**：`linux_user_stat` 按 arch 分布局（a64: mode 在 nlink 前） |
| 2026-07-24 | **aarch64 busybox**：入口 SP 按 arch 对齐；padding 只能在 auxv 之上（曾误插 envp↔auxv → 假 AT_NULL → 用户态访问 0x0） |
| 2026-07-13 | §P3：补充 **测例编排迁到 busybox 脚本**（`run_all.sh`、阶段 A/B/C、前置条件）；更新建议修复顺序 |
| 2026-07-13 | **里程碑**：`ls /bin` **exit_code=0**，列出 `/bin` applet；根因 pathname bulk 读越过栈顶 guard → `linux_mm_load_cstring_from_user` |
| 2026-07-13 | 修复 `NEWFSTATAT` RPC p1/p2 + `linux_user_stat_t`（VFS stat 写回） |
| 2026-07-13 | busybox 进用户；曾 exit 1 + `Bad address`（已由 cstring path load 修复） |
| 2026-07-13 | Path B 定稿：core `arch_syscall_set_user_return`；init hook `linux_exec_bootstrap_elf_spawn_stack`；`PT_NOTE>1` 检测；移除 runner `spawn_user_argv*` |
| 2026-07-13 | 同步 user_rsp_scratch / re-read `user_sp` → 修复 exit 139 |
| 2026-07-13 | glibc auxv：修正 AT_*；补 PHDR/RANDOM |
| 2026-07-12 | 初版：cpio/ns 上限、static readdir、busybox 构建 |
