# Initramfs 方案（Phase 4 bootstrap）

> **Status**: 方案定稿（2026-06-13）  
> **Index**: [`PROGRESS.md`](PROGRESS.md) · [`VFS_SERVER_IPC.md`](VFS_SERVER_IPC.md)  
> **Audience**: maintainer + AI 实施 compat/server 前必读

---

## 1. 目标

| 档位 | 含义 |
|------|------|
| **Demo（导师）** | QEMU 串口跑 `/bin/ls`（static busybox） |
| **Regression** | 52/52 harness via **initramfs manifest** (`filesystem:true` in `user.json`); stub `link_app.o` |
| **长期** | initramfs 作为 **只读根文件系统** 长期存在；磁盘镜像 / virtio-blk 为可选 Phase 4.5 |

**不做（第一版）**: virtio-blk 驱动、ext2、动态链接、`mount` 完整语义、vDSO。

---

## 2. 与标准 Linux initramfs 的关系

标准 Linux（含 QEMU `-initrd`）流程：

```text
Bootloader / 内核 early boot
    → 把 cpio 字节流载入内存（-initrd 或 built-in）
    → 解压到 rootfs（内存 tmpfs 或直接解析）
    → 用户态 /init（PID 1）→ 可选 pivot_root 切到真磁盘根
```

**格式**: 几乎总是 **newc cpio**（`070701` 魔数），可用 `cpio -o -H newc`。

**本项目的 RendezvOS 差异**:

| 项 | 标准 Linux | RendezvOS（本方案） |
|----|------------|---------------------|
| cpio 谁解析 | 内核 unpack 或 init | **vfs_server**（或 compat 只读后端）在 **内核态** 解析 |
| `/init` | 用户态 PID 1 | 第一版可由 **内核 init 线程** `execve("/bin/ls")` demo；后续 `/init` 用户态 |
| **servers** | 多在用户态（systemd） | **vfs_server / clean_server 仍是内核线程**（`servers/*.c`），**不进 cpio** |
| 测例 | 通常在磁盘或 initramfs | 过渡期：**link_app.o 嵌入 + initramfs 并存** |

结论：**cpio 打包方式与 Linux 相同**；**server 不在 initramfs 里当文件跑**——那是混合内核架构的 deliberate 选择（见 [`ARCHITECTURE.md`](ARCHITECTURE.md)）。

---

## 3. 什么放进 initramfs，什么不放

### 放进 cpio（用户态文件）

```text
/
├── bin/
│   ├── busybox          # static，可 busybox --install -s bin 做 ls sh cat ...
│   └── ls -> busybox    # 或 applet 链接
├── init                 # 可选：#!/bin/sh  exec /bin/ls
└── tests/               # 可选：从 user_payload 拷出的 ELF（逐步替代 link_app）
    └── test_open
```

### 不放进 cpio（保持内核 .text / servers）

- `core/` 全部
- `linux_layer/` syscall、proc、mm、signal
- `servers/clean_server.c`、`servers/fs/vfs_server.c` — **内核线程 + IPC**
- 现有 `link_app.o`（**过渡期保留**，直到测例迁完）

**内核仍然「干净」的含义**: core 无 Linux/VFS 语义；**文件内容**通过 initramfs 供给 **用户程序**，VFS **机制**在 vfs_server（读 cpio 后端，不读块设备）。

---

## 4. 从「磁盘镜像」到 initramfs

若你已有 **ext2/raw img**（含 busybox + 测例）：

```bash
# 在 Linux 主机上挂载后打 cpio（一次性转换）
mkdir -p /tmp/rootfs-mount
sudo mount -o loop your-rootfs.img /tmp/rootfs-mount
cd /tmp/rootfs-mount && find . | cpio -o -H newc | gzip > /path/to/rootfs.cpio.gz
sudo umount /tmp/rootfs-mount
```

或 **不用 img**，直接从目录打：

```bash
# 推荐：可版本控制的 rootfs/ 树
ROOTFS=rootfs
mkdir -p $ROOTFS/{bin,dev,proc,tests}
cp path/to/busybox $ROOTFS/bin/
$ROOTFS/bin/busybox --install -s $ROOTFS/bin
# 测例 ELF
cp user_payload/user/build/x86_64/test_open $ROOTFS/tests/
cd $ROOTFS && find . -print0 | cpio -o -H newc --null > ../rootfs.cpio
```

**第一版建议**: 维护仓库内 `rootfs/` 目录 + 脚本生成 `build/rootfs.cpio`，不依赖 loop mount。

---

## 5. cpio 如何进内核（两种加载，先 A）

### A. `.incbin` 链进 kernel（**推荐 bootstrap**）

```text
script/rootfs/build_cpio.sh  →  build/rootfs.cpio
script/rootfs/rootfs_cpio.S  →  .incbin "rootfs.cpio"  →  rootfs_cpio.o
Makefile rootfs 目标         →  ROOT_EXTRA_OBJECTS += rootfs_cpio.o
vfs_server 启动时            →  解析 _binary_rootfs_cpio_{start,end}
```

- **优点**: 不改 QEMU 命令、不改 core boot、单 `-kernel kernel.bin` 即可 demo  
- **缺点**: 改 rootfs 需重编 kernel（测例开发可接受）

### B. QEMU `-initrd rootfs.cpio`（后续）

- 需在 **core 或 linux_layer early init** 拿到 initrd 物理地址（x86 multiboot / aarch64 DTB 或固定链接地址）  
- **更多 core 接触面**；Phase 4.5 再做

**决策**: Phase 4 **先做 A**；B 文档预留，不阻塞 busybox ls。

---

## 6. VFS 与 mount

第一版 **不需要** `mount` syscall 也能用 initramfs：

```text
vfs_server 启动
    → cpio_init(_binary_rootfs_cpio_start, size)
    → 内部 path 树 + file offset
    → open("/bin/ls") 读 cpio 内 ELF
```

`/`` 即 cpio 根；`getcwd` 返回 `"/"`。  
**Phase 4.1+**: 实现 `mount` 时，可把 cpio 视为已 mount 的 root；或 mount tmpfs 覆盖 `/tmp` 等——与 demo 无关可后做。

---

## 7. 构建 / 兼容层改造清单（几乎不动 core）

### 7.1 新增（repo 根）

| 路径 | 职责 |
|------|------|
| `rootfs/` | 源目录（bin/busybox、可选 tests/） |
| `script/rootfs/build_cpio.sh` | 调 busybox 交叉编译 + `cpio -o -H newc` |
| `script/rootfs/rootfs_cpio.S` | `.incbin` 导出 `_binary_rootfs_cpio_{start,end,size}` |
| `doc/linux_compat/INITRAMFS_PLAN.md` | 本文 |

### 7.2 Makefile

| 目标 | 行为 |
|------|------|
| `make rootfs ARCH=x86_64` | 生成 `build/rootfs.cpio` + `build/rootfs_cpio.o` |
| `make build` | 依赖 `rootfs`（或 `HAVE_ROOTFS=1` 开关） |
| `make user` | **`filesystem:true`** → build ELFs, `pack_user_rootfs.py`, stub `link_app.o` |

`user.json` 的 `"filesystem": true` **暂不实现 img 挂载**；将来表示「测例从 initramfs 跑而非 link_app」。

### 7.3 servers / linux_layer（实施顺序）

1. **`servers/fs/cpio_rofs.c`** — newc 解析、path lookup、read at offset  
2. **`servers/fs/ramfs_layer.c` + `vfs_root.c`** — 可写 overlay + 统一 lookup  
3. **`vfs_server.c`** — OPEN/READ/CLOSE/LSEEK/GETDENTS 接 vfs_root  
3. **`linux_layer/fs/fd_table.c`** — 每进程 fd；0/1/2 特殊处理（[`STDIO_SHIM.md`](STDIO_SHIM.md)）  
4. **`sys_fs_impl.c`** — openat/read/close/getdents64 → RPC  
5. **`sys_execve.c`** — 3d：从 VFS read ELF 加载（不仅 embedded map）  
6. **Demo init** — `DEFINE_INIT` 里 spawn 用户线程 `execve("/bin/ls", ...)` 或跑 `linux_user_test` 旁路  

### 7.4 core 变更

**预期为零或极少**（例如若用 `.incbin` 仅链接器段，无 core 代码改动）。  
禁止在 core 写 cpio 解析 / path 语义。

---

## 8. 测例程序 vs 测例数据（必读）

integrated harness 里有两层，**不要混为一谈**：

| 是什么 | 在哪 | 怎么跑 |
|--------|------|--------|
| **测例程序** | `rootfs/tests/*` + `manifest` → cpio | `user_test_runner` reads manifest via `vfs_kern_read_file_slice` + spawn |
| **测例数据**（`./text.txt`、`./mnt/` 等） | `rootfs/` → `build/rootfs.cpio` | 测例里 `open("./text.txt")` 时由 **VFS 从 cpio 读** |

因此实施顺序是：

```text
① rootfs fixtures + build_cpio.sh     ← 已可做（script/rootfs/）
② vfs_server 解析 cpio + open/read    ← 测例 open/read 才能 PASS
③ incbin 链进 kernel                  ← cpio 进镜像
④ build_busybox.sh + bin/ls           ← 导师 demo，与 ② 可并行
⑤ mkdir/openat O_CREATE 等            ← ramfs overlay（见 RAMFS_AND_VFS_STORAGE.md）
```

**第一刀让测例过**：`text.txt` + open/read/close/fstat，**不必等 busybox**。

仓库命令：

```bash
make rootfs                    # → build/rootfs.cpio
ARCH=x86_64 script/rootfs/build_busybox.sh   # 可选
```

说明：`script/rootfs/README.txt` · Git 策略：[`ROOTFS.md`](ROOTFS.md)

---

## 9. 验收标准

| # | 检查 |
|---|------|
| 1 | `make ARCH=x86_64 rootfs build run` — 串口见 `[VFS] root ready: cpio N entries…` |
| 2 | 现有 `52/52` harness 仍 PASS（link_app 未动） |
| 3 | #50 `open` stdout 开始 PASS（打开 cpio 内文件） |
| 4 | aarch64 同路径（可选第二里程碑） |

---

## 10. FAQ

**Q: server 能放进 initramfs 当文件跑吗？**  
A: 架构上 **将来可以**（真微内核），但 **当前不是**。vfs/clean 是内核线程；initramfs 只供 **用户 ELF**。这样 core 保持干净，VFS 语义在 server 线程。

**Q: initramfs 要一直留着吗？**  
A: 可以。很多嵌入式/Linux 安装器长期用 initramfs；后面加磁盘只是多一个 vfs 后端 + `mount`。

**Q: 要先改 user.py / filesystem:true 吗？**  
A: **不必须**。先 `make rootfs` + incbin；`filesystem`  flag 留给「测例来源切换」时再接。

---

## Changelog

| 日期 | 变更 |
|------|------|
| 2026-06-13 | 初版：cpio/incbin、server 边界、构建清单、与标准 Linux 对照 |
