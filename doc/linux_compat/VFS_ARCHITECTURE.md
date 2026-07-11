# VFS 架构、Linux 对齐与逐步验证

> **Status**: 定稿（2026-07-05，Step 0–5 实施中）  
> **Live 状态**: [`VFS_IMPLEMENTATION_STATUS.md`](VFS_IMPLEMENTATION_STATUS.md) — **改了 VFS 代码先更新此文件**  
> **Index**: [`INITRAMFS_PLAN.md`](INITRAMFS_PLAN.md) · [`VFS_SERVER_IPC.md`](VFS_SERVER_IPC.md) · [`RAMFS_AND_VFS_STORAGE.md`](RAMFS_AND_VFS_STORAGE.md)  
> **代码**: `servers/fs/` · `linux_layer/fs/`

---

## 1. 三层模型（共识）

```text
┌─────────────────────────────────────────────────────────────┐
│ 前端 Front — syscall / IPC 策略                               │
│  • openat 标志组合 (O_CREAT|O_EXCL|O_TRUNC|O_DIRECTORY…)     │
│  • 每 pid fd 表、file offset、O_APPEND 行为                  │
│  • dirfd + cwd 路径展开；copy_from/to_user；Linux errno      │
│  位置: linux_layer/fs/*  +  servers/fs/vfs_open.c, vfs_fd.c │
└───────────────────────────────┬─────────────────────────────┘
                                │ inode 级原语 only
┌───────────────────────────────▼─────────────────────────────┐
│ 中端 Middle — namespace + storage                           │
│  • lookup / mkdir / create / unlink / readdir               │
│  • inode_read / write / truncate（buffer；将来 page 粒度）    │
│  • overlay / 后端路由 / whiteout                             │
│  位置: servers/fs/vfs_root.c（→ 将来 vfs_namespace/storage）│
└───────────────┬─────────────────────┬───────────────────────┘
                │                     │
         cpio_rofs (只读)        ramfs_layer (可写)     disk (将来)
         后端                    后端                   后端
```

**边界纪律**

| 层 | 关心 | 不关心 |
|----|------|--------|
| 前端 | fd、`O_*`、IPC、`AT_FDCWD` | cpio 魔数、kmalloc 缓冲 |
| 中端 | 路径→元数据、字节读写、overlay | fd 编号、open 标志组合 |
| 后端 | 真源在哪（incbin / kmalloc / LBA） | syscall 语义 |

参考：[Linux VFS overview](https://docs.kernel.org/filesystems/vfs.html)、[ramfs/rootfs/initramfs](https://www.kernel.org/doc/html/latest/filesystems/ramfs-rootfs-initramfs.html)。

---

## 2. 与 Linux 对齐：必须 vs 可简

### 2.1 必须对齐（测例 / ABI / 可见行为）

| 项 | Linux 约定 | 本项目 |
|----|------------|--------|
| **Syscall 号** | x86_64: openat=257, read=0, write=1, close=3, fstat=5, newfstatat=262, mkdirat=… | `syscall_entry.c` 已有；保持与 [syscall_64.tbl](https://github.com/torvalds/linux/blob/master/arch/x86/entry/syscalls/syscall_64.tbl) 一致 |
| **errno 值** | 正数 errno 取负返回（如 ENOENT=2 → -2） | [`include/linux_compat/errno.h`](../../include/linux_compat/errno.h) |
| **openat(2) 标志** | `O_CREAT` 无则不存在→`-ENOENT`；`O_EXCL\|O_CREAT` 已存在→`-EEXIST`；`O_TRUNC` 截断普通文件；目录+写→`-EISDIR` 等 | 前端 `vfs_open.c` 必须按 [openat(2)](https://man7.org/linux/man-pages/man2/openat.2.html) 组合 |
| **initramfs 内容** | newc `070701` | `script/rootfs/build_cpio.sh` |
| **相对路径** | 相对 cwd；`AT_FDCWD` | Phase 4b：**per-process cwd** + dirfd（见 [`DIRECTORY_PHASE.md`](DIRECTORY_PHASE.md)） |
| **stat / fstat 输出** | 测例用 `struct kstat`：`st_mode`, `st_size`, `st_nlink`… | `vfs_kstat_t` ← `vfs_inode_t`（见 `vfs_kstat.c`）；**非** ucore 小 `Stat` |
| **mode 位** | `S_IFREG`/`S_IFDIR` + 权限八进制 | cpio 自带 `0100644` 等；ramfs create 时 `\| S_IFREG` |
| **read/write 语义** | 返回字节数；EOF 返回 0；普通文件可 partial read | 中端 `vfs_root_read/write` |
| **只读根文件** | 写只读→`-EROFS` | cpio 条目 `writable=false` |
| **initramfs 内容** | newc `070701` | `script/rootfs/build_cpio.sh` |

### 2.2 可简化（不阻塞 Phase 4 测例）

| Linux | 本项目 bootstrap | 何时补 |
|-------|------------------|--------|
| `struct inode` + icache | `vfs_object_t` / entry 表 | 硬链接、设备节点 |
| dentry 哈希树 | 路径规范化 + 线性 scan | 条目 >>128 |
| page cache + address_space | ramfs `kmalloc` buffer；cpio 零拷贝指针 | 文件 mmap (#13)、大文件 |
| writeback 线程 | 无（ramfs 无盘） | virtio-blk |
| `mount` / `pivot_root` | 硬编码 `/` = overlay | Phase 4.1+ |
| 权限 uid/gid 检查 | 可全当 root | 安全测例 |
| symlink / hardlink | 未实现 | 测例要求时 |
| `getdents64` 顺序 | 任意稳定顺序即可 | ls demo |
| atime/mtime 精度 | 可填 0 或 cpio mtime | fstat 测例若检查时间再补 |

### 2.3 概念对齐（设计思想，非结构体同名）

Linux 文档说明：ramfs 把 **page cache + dentry** 当存储，**无 backing store**（[initramfs 文档](https://www.kernel.org/doc/html/latest/filesystems/ramfs-rootfs-initramfs.html)）。  
我们 **功能等价、实现更薄**：

| Linux 概念 | RendezvOS Phase 4 对应 |
|------------|------------------------|
| dentry + path walk | `vfs_path_*` + `vfs_root_lookup` |
| inode 元数据 | `vfs_inode_t`（`vfs_backend_ops.h`） |
| page cache 数据 | cpio: 指向 incbin；ramfs: kmalloc |
| `struct file` | 前端 fd 表（offset、flags） |
| file_system_type / mount | `vfs_root_init` + 隐含根 |

---

## 3. 当前代码地图

> 详细文件职责与 RPC 接线表见 [`VFS_IMPLEMENTATION_STATUS.md`](VFS_IMPLEMENTATION_STATUS.md) §2–§3。

| 层 | 文件 | 状态 |
|----|------|------|
| 后端 cpio | `cpio_rofs.c` | ✅ lookup/read/`visit`（`visit` 仅 `vfs_root` readdir） |
| 后端 ramfs | `ramfs_layer.c` | ✅ mkdir/create/write/whiteout |
| 中端 | `vfs_root.c`, `vfs_path.c`, `vfs_backend_ops.c`, `vfs_kstat.c` | ✅ overlay + stat/readdir |
| 前端 server | `vfs_open.c`, `vfs_fd.c`, `vfs_rpc.c`, `vfs_server.c` | ✅ open/read/write/close/fstat/lseek/statat/mkdir/unlink RPC |
| 前端 compat | `sys_fs_impl.c`, `fs_ipc.c` | ✅ 上表 syscall 已 IPC；write fd≥3 分流 |
| IPC | `vfs_protocol.h` | ✅ |

**SMP**：`NR_CPU>1` 时 vfs_server **CPU 1**；单核 BSP。

---

## 4. cpio 向上接口（已收束）

**原则**：`cpio_rofs.h` **仅**给 `vfs_root.c` include；linux_layer / RPC 不得直接依赖 cpio。

公开 API：

```c
error_t cpio_rofs_init(const void *image, u64 image_len);
u32     cpio_rofs_parsed_count(void);
bool    cpio_rofs_lookup(const char *path, cpio_rofs_stat_t *out);
i64     cpio_rofs_read(const cpio_rofs_stat_t *st, u64 offset, void *buf, u64 len);
void    cpio_rofs_visit(cpio_rofs_visit_fn fn, void *ctx);  /* vfs_root readdir only */
```

- 无 boot dump；`cpio_rofs_stat_t` 含 `nlink`（stat 用）
- 启动日志：**一行** `[VFS] root ready: cpio N entries, ramfs M entries`（在 `vfs_root_init`）
- 解析失败仍 `pr_error` 带 offset（排错必需）

---

## 5. 代码演进路线（微调版）

```text
Step 0  ✅ 文档 + cpio 接口/日志收束
Step 1  ✅ 中端: vfs_backend_ops + vfs_inode 句柄
Step 2  ✅ 中端: readdir + vfs_kstat（syscall 未接 getdents）
Step 3  ✅ 前端 server: vfs_open + vfs_fd + RPC
Step 4  ✅ 前端 compat: sys_fs_impl IPC（fd 表在 server 侧）
Step 5  ✅ write + unlinkat RPC
Step 6  ✅ execve 从 initramfs/VFS 读 ELF（2026-07-09 验证）；busybox demo ⬜
Step 7  ⬜ 目录：chdir + cwd + openat(dirfd) + getdents64 — [`DIRECTORY_PHASE.md`](DIRECTORY_PHASE.md)
Step 8  ⬜ dup/dup2/pipe + fd 生命周期
Step 9  ⬜ 磁盘后端 + page cache
```

目标目录（终点，可渐进）：

```text
servers/fs/
  backends/cpio_rofs.*   ramfs_layer.*
  vfs_core/              vfs_root → 拆 namespace + storage + backend_ops
  vfs_front/             vfs_open.c, vfs_fd.c, vfs_rpc.c
  vfs_server.c           init + thread
linux_layer/fs/
  fd_table.c, vfs_path_user.c, sys_fs_impl.c
```

---

## 6. 逐步验证方案

每一档：**怎么证**、**通过标准**、**无法单独验时怎么办**。

| 档 | 验证内容 | 命令 / 方法 | 通过标准 | 备注 |
|----|----------|-------------|----------|------|
| **V0** | cpio 镜像 | 主机 `cpio -t < build/rootfs.cpio` | 列 `text.txt`, `mnt/`… | 不启动内核 |
| **V1** | cpio 解析 | `make run`，看串口 | 一行 `[VFS] root ready: cpio 4 entries, ramfs 0 entries`；无 magic error | ✅ 已过；**无 entry dump** |
| **V2** | 中端 lookup/read | server 内自检或临时 `pr_info`（**不**长期 dump）；或单元式 host 脚本解析同一 cpio 对比 path/size | `/text.txt` size=81；读前 16 字节一致 | 可用 Python 离线对照 incbin 不必 QEMU |
| **V3** | overlay | 启动后 ramfs 自检：`mkdir /tmp` + write + read（**开发期**可选；接 syscall 后删） | ramfs count≥1；读回字节正确 | 无 syscall 时只能 server 内测 |
| **V4** | OPEN/READ RPC | `make run` + oscomp open/read stdout | `[PASS]` | ✅ 2026-07-09 双架构 |
| **V5** | fstat | oscomp fstat | mode/size 匹配 | ✅ 2026-07-09 |
| **V6** | mkdir/O_CREATE | mkdir / openat 测例 stdout | PASS | mkdir ✅；openat ⬜ |
| **V7** | write | write 测例（fd≥3） | PASS | ✅ 2026-07-09 |
| **V8** | 双架构 | `ARCH=aarch64` 同序 | 同 V4–V7 | ✅ 2026-07-09 log §2026-07-09 |
| **V9** | execve initramfs | #8 stdout | `execve success` | ✅ 2026-07-09 |
| **V10** | 目录 | chdir/getdents/openat stdout | PASS | ⬜ Step 7 |
| **V11** | execve `/bin/ls` | 串口见 ls 输出 | demo | 依赖 busybox + V10 |

**无法逐步验的情况（承认即可）**

- **纯前端 open 标志组合**：无 syscall 前可在 server 写 `#ifdef VFS_TEST` 小 harness，或一次性接 open 后用测例验。
- **page cache / mmap 一致性**：等有 #13 mmap 测例再验；bootstrap 不声称已验。
- **磁盘 writeback**：无块设备时不验。

**回归门**：每档合并前 `make ARCH=x86_64 build run`；FS 档额外看相关 oscomp stdout；文档更新本表 ✅/⬜。

---

## 7. 启动日志约定

| 级别 | 内容 |
|------|------|
| 正常 | `[VFS] root ready: cpio N entries, ramfs M entries` |
| 正常 | `[VFS] registered vfs_server_port …` |
| 错误 | cpio magic / truncated / table full → `pr_error` 保留 |
| **禁止** | 启动时逐条 entry dump、preview（bootstrap 已结束） |

调试 entry 列表：主机 `cpio -tv < build/rootfs.cpio`，不在内核刷屏。

---

## 8. 相关文档

- 存储与 ramfs 细节：[`RAMFS_AND_VFS_STORAGE.md`](RAMFS_AND_VFS_STORAGE.md)  
- initramfs 打包：[`INITRAMFS_PLAN.md`](INITRAMFS_PLAN.md)  
- IPC：[`VFS_SERVER_IPC.md`](VFS_SERVER_IPC.md)  
- **实现快照（改代码必更）**：[`VFS_IMPLEMENTATION_STATUS.md`](VFS_IMPLEMENTATION_STATUS.md)  
- fd 0/1/2：[`STDIO_SHIM.md`](STDIO_SHIM.md)
