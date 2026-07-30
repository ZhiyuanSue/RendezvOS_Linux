# Busybox 启动 — 临时妥协与待修项

> **Status**: demo 已通；妥协项分批回收中（更新 2026-07-28）  
> **目标**: initramfs 内 static busybox 能跑 `ls` / shell；再迁到脚本编排测例  
> **相关**: [`INITRAMFS_PLAN.md`](INITRAMFS_PLAN.md) · [`VFS_DYNAMIC_STORAGE.md`](VFS_DYNAMIC_STORAGE.md) · [`EXECVE_IMPLEMENTATION_STATUS.md`](EXECVE_IMPLEMENTATION_STATUS.md) · [`USER_TESTS.md`](USER_TESTS.md) · [`SYSCALLS.md`](SYSCALLS.md)

本文记录 **为尽快跑通 busybox demo 而做的妥协**。每一项都应在 demo 稳定后回收或正规化。

---

## 剩余开放项一览（2026-07-28）

对照「快速修通 busybox」路径，**仍未正规化** 的项如下。已回收项见各节「✅」。

| 优先级 | 项 | 现状 | 应改为 / 备注 |
|--------|-----|------|----------------|
| **P0** | demo argv | 🔧 Path B：`sh -c '/bin/ls /bin; echo SHELL_OK'`（须绝对路径；空 envp 无 PATH） | 再 → `PATH=`/`run_all.sh`；fork 前注意物理内存（默认 QEMU `MEM_SIZE=512M`） |
| **P0** | Demo spawn 非 execve | `gen_task_from_elf` + Path B bootstrap | 统一 **execve**（较大，见下「run_all」） |
| **P0** | fake return + append hook 栈 | core + `linux_thread_append_init` | 去掉二次 bootstrap |
| **P1** | 用户 pathname 逐字节读 | `linux_mm_load_cstring_from_user` | `strnlen_user` / 按页探测 bulk |
| **P1** | auxv 长尾 | 缺 `AT_HWCAP` / `AT_EXECFN`；`AT_RANDOM` 占位 | 真随机 + 完整 auxv |
| **P1** | syscall stub 深度 | `getrandom`/`prlimit64`/`ioctl`/`rseq` 等够 demo | 按 applet 需求加深（非阻塞 `ls`） |
| **P2** | busybox 构建妥协 | `CONFIG_STATIC`、applet 子集、`AUTO_FETCH` | 可选 `BUSYBOX_FULL`；工具链文档化 |
| **P3** | harness 仍内核编排 | `user_test_runner` + manifest + `test_cookie` | **`busybox sh /tests/run_all.sh`**（阶段 A→B） |
| **P3** | 无 `/init`、设备节点 | 无 PID1；`/dev/null` 等未做 | 真 initramfs 形态 |
| **P3** | `_num_app` / embedded `program_map` | 与 cpio 路径并存 | 测例/exec **全走 cpio** 后删除 incbin app 表 |
| **P3** | 共享头文件抽象 | server 与 compat 部分头交叉 | 抽公共抽象层（结构整理，非功能阻塞） |
| — | **VFS 定长 BSS / 栈炸弹** | — | ✅ **已回收** → [`VFS_DYNAMIC_STORAGE.md`](VFS_DYNAMIC_STORAGE.md) |

**建议下一步顺序**

1. Demo / harness 改走 **execve**（收 P0 Path B）  
2. 阶段 A：`run_all.sh` + 单次 `busybox sh`（收 P3 编排）  
3. 正规化 pathname 拷贝 + auxv 长尾（P1）  
4. 全量迁 cpio 后清 `_num_app` / `program_map`  
5. VFS 扩展性（readdir O(n²)、tombstone、ramfs body）— 见动态存储文档「建议后续」，**不阻塞** busybox

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

## P0 — Path B 栈与首次进用户（已打通，**仍属妥协**）

### 1. Demo 仍走 `gen_task_from_elf`，非 execve

| 项 | 现状 | 应改为 |
|----|------|--------|
| 测例 spawn | `gen_task_from_elf` → `run_elf_program` | demo 走 **`execve` 路径** 或统一 spawn API |
| 用户栈 | core fake return + compat **二次 bootstrap** | **execve 级**完整栈（或 core 统一 `run_elf_program` 协议） |

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
6. glibc auxv：`AT_PHDR/PHENT/PHNUM/PAGESZ/ENTRY/UID…/RANDOM`（`AT_RANDOM` 仍为占位字节，非 `getrandom`）

**涉及文件**:

- `core/kernel/task/thread_loader.c`（`run_elf_program` 调 append.init）
- `linux_layer/loader/linux_elf_init.c`
- `linux_layer/proc/linux_exec_stack.c`
- `include/linux_compat/proc/linux_exec_stack.h`

**仍缺 / 待正规化**:

- demo argv 仍硬编码在 bootstrap（跨 `gen_task_from_elf` 传 pending **不可行**，除非挂在 thread/task 上）
- `AT_HWCAP` / `AT_EXECFN`；真随机（`getrandom` 或强 `AT_RANDOM`）
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
| path syscall 读用户 pathname | `linux_mm_load_cstring_from_user`（逐字节至 NUL） | 通用 **`strnlen_user` / `copy_from_user` 字符串** API；或 bulk 读但 **按页探测、遇 guard 停** |
| `LINUX_VFS_PATH_MAX`（256） | 仍作上限；超长无 NUL → `EINVAL` | 与 Linux `PATH_MAX` / `ENAMETOOLONG` 语义对齐 |
| 用户栈大小 | core `thread_ustack_page_num=8` | 若深栈 / 大 `alloca` 测例再评估；**非本次 EFAULT 主因** |

**文件**：`include/linux_compat/linux_mm_radix.h`, `linux_layer/fs/sys_fs_impl.c`（`sys_fs_load_pathname`）

---

## P1 — initramfs / VFS 容量妥协（✅ 已回收 → 动态表）

> **2026-07-27 回收完成**：`vfs_slice_table`（page_slice 可增长）覆盖 S0–S3。权威说明见 [`VFS_DYNAMIC_STORAGE.md`](VFS_DYNAMIC_STORAGE.md)。下列为历史记录。

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

详见 [`VFS_DYNAMIC_STORAGE.md`](VFS_DYNAMIC_STORAGE.md)「建议后续」。

---

## P2 — 构建与 rootfs（妥协仍在）

| 妥协 | 说明 |
|------|------|
| `CONFIG_TC=n` | Linux 6.8+ 无 CBQ UAPI |
| `CONFIG_STATIC=y` | static glibc busybox |
| `BUSYBOX_AUTO_FETCH=1` | 自动拉 busybox 1.36.1 |
| 默认 `BUSYBOX_FULL=0` | 仅 demo applet 子集 |

**文件**: `script/rootfs/build_busybox.sh`

---

## P3 — 测试 harness（现状，待迁脚本）

`linux_layer/tests/user_test_runner.c`（`LINUX_COMPAT_TEST`）当前是 **BSP 内核编排器**，不是用户态 init：

| 步骤 | 机制 |
|------|------|
| 读清单 | 内核 `vfs_kern_read_file_slice("/tests/manifest")`（**不走**用户 `open/read`） |
| 逐个测例 | `vfs_kern_read_file_slice(path)` → **`gen_task_from_elf`**（**不是 execve**） |
| 等待结束 | `test_cookie` + `clean_server` → `linux_user_test_notify_exit` |
| 判定 | harness 只判 spawn/等待是否成功；**不解析** stdout 里 `[PASS]`/`[FAIL]` |

末尾 demo：仍 `linux_spawn_and_wait_test_path("/bin/busybox", …)`，argv 在 bootstrap 写死为 `sh -c '/bin/ls /bin; echo SHELL_OK'`。

- 打印 **`exit_code`**；`RENDEZVOS_ROOT_AUTO_POWEROFF` 后 shutdown  
- **待修**：独立 demo / `execve`；busybox 失败时不 auto poweroff（可配置）

**相关**：[`USER_TESTS.md`](USER_TESTS.md) · `script/config/pack_user_rootfs.py`（生成 `rootfs/tests/manifest`）

---

## P3 — 白话：`run_all.sh` 迁移是什么？

**现状（harness）**：内核 C 代码 `user_test_runner.c` 自己：

1. 读 `/tests/manifest`  
2. **for** 每一行路径 → `gen_task_from_elf` 起一个进程 → 等它退出  
3. 最后再单独起一次 busybox demo  

这是「内核当测试编排器」，不是真 initramfs 用法。

**目标（`run_all.sh`）**：内核只负责启动 **一次** busybox shell，例如：

```text
busybox sh /tests/run_all.sh
```

脚本里再 `while read` manifest、逐个 exec 测例。编排从 **C 循环** 挪到 **用户态 shell**，更接近 Linux：init → shell → 跑测试。

| | 内核 harness（现在） | `run_all.sh`（目标） |
|--|---------------------|----------------------|
| 谁读 manifest | 内核 `vfs_kern_read_file_slice` | shell `read` / 重定向 |
| 谁起测例 | 内核 `gen_task_from_elf` | shell `exec` / 直接跑路径 |
| 等结束 | `test_cookie` + clean_server | shell `wait` / 顺序执行 |
| busybox demo | 另一次内核 spawn + 注入 argv | 脚本里写 `ls /bin` |

**为何还没迁**：shell 链式 `execve`、读脚本、fork/wait 组合尚未当成「唯一编排路径」验证；且 demo 仍依赖 Path B。阶段划分见下节。  
**本次未实现 `run_all.sh`**——只整理 argv 注入；迁移是后续大项。

---

## P3 — 测例编排：迁到 busybox 脚本（待做）

> **方向**：用 **`busybox sh /tests/run_all.sh`** 替代 C 里写死的 manifest 循环。

### 为何可以迁

| 保留 | 替换 |
|------|------|
| `make user` → `rootfs/tests/*` + `manifest` | 不再在 `user_test_runner.c` 里 `for` 每个路径 |
| `pack_user_rootfs.py` / `manifest.order` | 额外生成 **`/tests/run_all.sh`** 打进 cpio |
| cpio / VFS 布局 | 内核 bootstrap **只启动一次** shell 脚本 |

### 为何不能立刻删掉 `user_test_runner.c`

当前 busybox 成功路径 ≠ shell 跑脚本：

| 能力 | 脚本是否需要 | 现状 |
|------|-------------|------|
| 启动 busybox | 必须 | 仍 **`gen_task_from_elf`** + Path B 栈，非用户态 `execve` |
| `fork` + `wait4` | shell 子进程 | Phase 1 已有 |
| `execve` 各测例 ELF | 必须 | FS execve 有；**经 shell 链式 exec 未验证** |
| `open/read` 脚本与 manifest | 必须 | 依赖文件 syscall 成熟度 |
| shebang `#!/bin/sh` | 可选 | execve **未做** shebang（可显式 `busybox sh script`） |
| glibc auxv / 栈 | busybox + 部分测例 | Path B 仅在 kernel spawn hook |

### 目标脚本形态（示例）

```sh
#!/bin/busybox sh
set -e
while read -r t; do
  case "$t" in ''|\#*) continue ;; esac
  echo "=== $t ==="
  "$t" || exit 1
done < /tests/manifest
```

成败以 **进程 exit code**（及 stdout 文案）为准；不再需要 `test_cookie` / `linux_user_test_notify_exit` 做 harness 同步。

### 分阶段迁移

| 阶段 | 内容 | 涉及 |
|------|------|------|
| **A** | 保留 manifest 构建；runner **只启动一次** `busybox sh /tests/run_all.sh`（`gen_task_from_elf` 或 `execve`）；脚本内顺序跑 manifest | `rootfs/tests/run_all.sh`、`pack_user_rootfs.py`、`user_test_runner.c` |
| **B** | 内核仅做 VFS/early init，然后 **`execve("/bin/busybox", ["sh", "/tests/run_all.sh"], …)`** 或 `/init`；缩掉 `linux_user_test_thread` 大循环 | `sys_execve.c`、`linux_exec_stack.c` |
| **C** | 去掉 harness 专用 `test_cookie` / notify_exit；SMP 压测若仍需 per-CPU case，另保留内核 smp 模式或脚本并行策略 | `append_hooks`、`clean_server` |

### 迁移前需验证

1. **musl 测例**经 **用户态 `execve`**（非 `gen_task_from_elf`）栈/auxv 是否正常（`PT_NOTE`=1 路径）。
2. **busybox ash** 读脚本、`fork`、`wait`、对 manifest 每项 `exec` 测例 ELF。
3. **`elf_read_test`**：保留为内核自检，或改为脚本内用户态读文件。
4. **SMP 语义**：[`USER_TESTS.md`](USER_TESTS.md) 的 per-CPU barrier 与纯 shell 单进程顺序默认不一致，需单独决策。

### 建议落地顺序（相对 P0 execve）

1. 在 `rootfs/tests/` 增加 `run_all.sh`；构建时由 `pack_user_rootfs.py` 生成或拷贝模板。  
2. runner 末尾（或替换 manifest 循环）改为 spawn/exec **`/bin/busybox` + `sh` + `/tests/run_all.sh`**。  
3. demo 与测例编排统一为 execve / 脚本路径后，busybox demo 改由脚本调用 `ls`（不再单独内核 spawn）。  
4. 阶段 B：eval 是否以 `/init` 替代 `linux_user_test_init`。

---

## P3 — 尚未实现、busybox 后续可能需要

| 项 | 状态 |
|----|------|
| `/dev/null`、`/dev/console` | 未做 |
| PID 1 + `/init` | 未做 |
| `read(0)` UART RX | EOF stub |
| 完整 `mount` / umount | 部分 |

---

## P3 — embedded `_num_app` / `program_map`（待清）

为早期无 cpio 时的测例/ELF 加载保留了 **`_num_app` + `.incbin` app 表**（`script/config/user_payload_link_app.py`、`linux_layer/fs/linux_exec_image.c`）。

| 现状 | 应改为 |
|------|--------|
| harness / 部分 exec 仍可能走 embedded map | **一律**经 initramfs cpio + VFS |
| `_num_app` 与 rootfs.cpio 双轨 | 删除 app 表与 link 脚本路径（依赖「测例已在 cpio」） |

与 busybox「只从 `/bin/busybox` 启动」同一清理方向；**阻塞依赖**：编排迁到 cpio + execve（上节阶段 A/B）。

详见 [`FILE_LOADING.md`](FILE_LOADING.md)、[`EXECVE_IMPLEMENTATION_STATUS.md`](EXECVE_IMPLEMENTATION_STATUS.md)。

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
