# Busybox 启动 — 临时妥协与待修项

> **Status**: 进行中（2026-07-13）  
> **目标**: initramfs 内 static busybox 能跑 `ls` / shell demo  
> **相关**: [`INITRAMFS_PLAN.md`](INITRAMFS_PLAN.md) · [`EXECVE_IMPLEMENTATION_STATUS.md`](EXECVE_IMPLEMENTATION_STATUS.md) · [`USER_TESTS.md`](USER_TESTS.md) · [`SYSCALLS.md`](SYSCALLS.md)

本文记录 **为尽快跑通 busybox demo 而做的妥协**。每一项都应在 demo 稳定后回收或正规化。

---

## 当前现象（2026-07-13，x86_64）

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

demo 的 **`argv[0]="ls"`** 是 busybox **多调用名**；进程镜像是 `/bin/busybox`，由 `linux_exec_spawn_default_argv()` 在 **`linux_thread_append_init`** 注入。  
**`rootfs/bin/ls` symlink 非必须**（demo 直接 exec `/bin/busybox` + 注入 argv），但保留 symlink 便于以后 shell 里敲 `ls`。

| 项 | 状态 |
|----|------|
| Spawn / ELF 加载 | ✅ `gen_task_from_elf` 成功 |
| glibc `_start` / 进用户 | ✅ 不再 exit **139** |
| 用户态有输出 | ✅ |
| **`ls /bin` 列目录** | ✅ **exit_code=0**，stdout 打出 applet 列表 |
| busybox 所需 syscall stub | ✅ 见 §4 |

典型 syscall 序列（成功一次）：`newfstatat(/bin)` → `openat` → `fstat` → 多次 `getdents64` → `close` → `write(1, …)`。

---

## P0 — Path B 栈与首次进用户（已打通，仍属妥协）

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
4. 默认 demo argv：`ls` + `/bin`（写死在 `linux_exec_spawn_default_argv`）
5. 栈布局（低→高）：`argc, argv[], NULL, envp[], NULL, auxv(…), random16, argv strings`
6. glibc auxv：`AT_PHDR/PHENT/PHNUM/PAGESZ/ENTRY/UID…/RANDOM`（`AT_RANDOM` 仍为占位字节，非 `getrandom`）

**涉及文件**:

- `core/kernel/task/thread_loader.c`
- `linux_layer/loader/linux_elf_init.c`
- `linux_layer/proc/linux_exec_stack.c`
- `include/linux_compat/proc/linux_exec_stack.h`

**仍缺 / 待正规化**:

- demo argv 写死在 compat（应按 exec 路径或 `elf_load_info` 传参）
- `AT_HWCAP` / `AT_EXECFN`；真随机（`getrandom` 或强 `AT_RANDOM`）
- 去掉 fake return + 二次 bootstrap，改 execve 或 core 统一协议

### 3. musl 测例 vs busybox glibc static

| 组件 | 工具链 | Path B 栈 |
|------|--------|-----------|
| `tests/*` | musl / 简单 `_start` | 仅 fake return（`PT_NOTE` = 1） |
| `rootfs/bin/busybox` | `x86_64-linux-gnu-gcc` + `CONFIG_STATIC=y` | 完整栈 + auxv（`PT_NOTE` > 1） |

---

## P1 — busybox demo syscall（2026-07-13 已实现 stub）

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

### `ls /bin` EFAULT 根因与修复（2026-07-13）

**实测**（`[VFS-STAT]` 日志）：失败在 **syscall 侧读 pathname**，未到 VFS server。

| 现象 | 说明 |
|------|------|
| `path_va≈0x7fffffffeff3` | glibc/busybox 把 `"/bin"` 放在**用户栈顶附近** |
| 旧代码 `linux_mm_load_from_user(..., 256)` | 一次读满 `LINUX_VFS_PATH_MAX`，越过 `USER_SPACE_TOP` 下方 **未映射 guard 页** → `EFAULT`（内核 `e=-1024`） |
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

## P1 — initramfs / VFS 容量妥协（已做，需正规化）

### cpio / namespace 上限

| 原值 | 现值 | 原因 |
|------|------|------|
| `CPIO_ROFS_MAX_ENTRIES` **64** | **2048** | busybox applet symlink + tests |
| `VFS_NS_MAX_NODES` **256** | **2048** | 与 cpio 条目同量级 |

**文件**: `servers/fs/cpio_rofs.h`, `servers/fs/vfs_namespace.h`

### `cpio_rofs_readdir` static BSS

`static char names[CPIO_ROFS_MAX_ENTRIES][64]`（约 128KiB BSS），避免 2048×64 爆内核栈。

---

## P2 — 构建与 rootfs

| 妥协 | 说明 |
|------|------|
| `CONFIG_TC=n` | Linux 6.8+ 无 CBQ UAPI |
| `CONFIG_STATIC=y` | static glibc busybox |
| `BUSYBOX_AUTO_FETCH=1` | 自动拉 busybox 1.36.1 |
| 默认 `BUSYBOX_FULL=0` | 仅 demo applet 子集 |

**文件**: `script/rootfs/build_busybox.sh`

---

## P3 — 测试 harness

- Demo 仍在 `user_test_runner` 末尾调用 `linux_spawn_and_wait_test_path("/bin/busybox", 9998u)`  
- **不再**在 runner 注入 argv；栈仅在 **`linux_thread_append_init`**  
- 打印 **`exit_code`**；`RENDEZVOS_ROOT_AUTO_POWEROFF` 后 shutdown  

**待修**: 独立 demo / `execve`；busybox 失败时不 auto poweroff（可配置）。

---

## P3 — 尚未实现、busybox 后续可能需要

| 项 | 状态 |
|----|------|
| `/dev/null`、`/dev/console` | 未做 |
| PID 1 + `/init` | 未做 |
| `read(0)` UART RX | EOF stub |
| 完整 `mount` / umount | 部分 |

---

## 诊断 checklist

| 现象 | 查什么 |
|------|--------|
| 无 `start gen task from elf` | VFS / cpio 无 `/bin/busybox` |
| **exit 139** | Path B SP 未 commit；或栈未 bootstrap（查 `PT_NOTE` 检测、hook 是否跑） |
| 有 syscall 无输出 | auxv / `getrandom` |
| **`ls: … Bad address`** | pathname **bulk 256B** 越过栈顶 guard；查 `linux_mm_load_cstring_from_user` |
| **exit 0** + `/bin` 列表 | ✅ demo 目标达成 |
| **exit 1** 且有 stderr 文案 | 用户态已跑通，属功能/compat 缺口非 spawn 失败 |

---

## 建议修复顺序（更新）

1. ~~调试 **`ls /bin` EFAULT**~~ ✅（pathname cstring load + VFS stat）  
2. **Demo 改用 execve**（或 core 统一 Path B，去掉 fake return）  
3. 正规化 **用户字符串拷贝**（替代逐字节 bring-up）  
4. auxv 长尾（`AT_HWCAP`、`AT_EXECFN`、真随机 / `getrandom` 增强）  
5. 收紧 cpio/namespace 或 lazy 策略  

---

## Changelog

| 日期 | 变更 |
|------|------|
| 2026-07-13 | **里程碑**：`ls /bin` **exit_code=0**，列出 `/bin` applet；根因 pathname bulk 读越过栈顶 guard → `linux_mm_load_cstring_from_user` |
| 2026-07-13 | 修复 `NEWFSTATAT` RPC p1/p2 + `linux_user_stat_t`（VFS stat 写回） |
| 2026-07-13 | busybox 进用户；曾 exit 1 + `Bad address`（已由 cstring path load 修复） |
| 2026-07-13 | Path B 定稿：core `arch_syscall_set_user_return`；init hook `linux_exec_bootstrap_elf_spawn_stack`；`PT_NOTE>1` 检测；移除 runner `spawn_user_argv*` |
| 2026-07-13 | 同步 user_rsp_scratch / re-read `user_sp` → 修复 exit 139 |
| 2026-07-13 | glibc auxv：修正 AT_*；补 PHDR/RANDOM |
| 2026-07-12 | 初版：cpio/ns 上限、static readdir、busybox 构建 |
