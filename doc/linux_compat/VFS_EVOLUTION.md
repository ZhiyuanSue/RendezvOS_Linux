# VFS 演进路线图（待办）

> **基线（2026-07-12）**：树形 `vfs_namespace` + cpio page cache + mount 登记/覆盖 + bootstrap 权限 + renameat/linkat + **fd 表 page_slice**。  
> **验证**：x86_64 **52/52**（fd 栈修复后待 re-run）；aarch64 **52/52**（前次基线）。

**文档层级**：本文 = 演进与待办真源；[`VFS_ARCHITECTURE.md`](VFS_ARCHITECTURE.md) = 分层原则；[`VFS_IMPLEMENTATION_STATUS.md`](VFS_IMPLEMENTATION_STATUS.md) = live 文件索引。

---

## 状态图例

| 标记 | 含义 |
|------|------|
| ✅ | 已实现 |
| 🚧 | 骨架 / 部分 |
| ⬜ | 待做 |

---

## 架构分层（当前）

```text
linux_layer/fs/          compat：fd 表(page_slice)、cwd、path、syscall → IPC
        ↕ IPC
servers/fs/              namespace 树、page cache、mount、perm、ramfs/cpio 后端
        ↕ vfs_backend_dispatch (sync / async IPC)
  cpio / ramfs / blkdev   各后端线程或 in-process handler
```

| 模块 | 文件 | 职责 |
|------|------|------|
| 命名空间 | `vfs_namespace.c` | 树形 dentry；**启动只 materialize 元数据** |
| 后端框架 | `vfs_backend.c`, `vfs_backend_ipc.c`, `vfs_backend_*.c` | VFS→后端 **仅 IPC**；每后端 `DEFINE_INIT_LEVEL(…,4)` @ **CPU1** |
| 后端 cpio | `cpio_rofs.c`, `vfs_backend_cpio.c` | `vfs_cpio_backend_port` 线程 |
| 后端 ramfs | `ramfs_layer.c`, `vfs_backend_ramfs.c` | `vfs_ramfs_backend_port` 线程 |
| 后端 blkdev | `vfs_backend_blkdev.c` | `vfs_blkdev_port` 线程（伪 64KiB 盘） |
| 缓存 | `vfs_page_cache.c` | 按需 **page_slice**；文件/页双 LRU |
| 内核 ingest | `vfs_kern_load.c` | **cache clone** 或直填 owned slice |
| 挂载 / 权限 | `vfs_mount.c`, `vfs_perm.c` | mount 登记 + owner mode |
| 路径 | `linux_layer/fs/vfs_path.c` | 唯一 normalize/join/equal |
| fd 表 | `linux_fd_table.c` | 每进程 **page_slice**（cwd + fd 条目）；**非**文件内容 |

---

## page_slice 三种用途（勿混）

| 用途 | 谁持有 | 内容 | 生命周期 |
|------|--------|------|----------|
| **A. fd 表 slice** | `linux_proc_append_t.fs` | hdr(cwd,capacity) + `linux_fd_entry_t[]` | 与进程同寿；fork **clone** |
| **B. 文件字节 slice**（目标态） | server **page cache** | cpio/ramfs 文件内容 pgoff→kva | LRU/引用；按需填充 |
| **C. ingest 临时 slice** | exec/manifest/test harness | 单次读入后 map/load | **load → destroy** |

**当前缺口**：B 未用 page_slice；page cache 用 `u8*` kmalloc。C 与 B **未统一**（kern load 直拷 cpio 指针）。

**目标（下一批实现）**：**B 与 C 合并** — 所有「把文件读进内核可寻址连续字节流」走 **同一 page cache 层**，底层存储用 **core page_slice**；启动 **只建 namespace/cpio 元数据**，**不**预读文件体。

---

## 启动卡顿：现状 vs 目标

| 阶段 | 现在做什么 | 是否读文件字节 |
|------|------------|----------------|
| `cpio_rofs_init` | 线性解析 cpio 头，建平坦 entry 表（`data` = 镜像内指针） | ❌ 不拷贝 payload |
| `vfs_namespace_init` | `cpio_rofs_visit` → 每路径 **materialize 树节点**（mode/is_dir） | ❌ 不读文件体 |
| 用户 `read` / page cache miss | 按需 fill **page_slice** cache 槽 | ✅ 按需 |
| `vfs_kern_read_file_slice` | **cache clone**（小文件）或直填 owned slice | ✅ 复用 cache |

**你观察到的卡顿**主要来自：(1) cpio 解析 + 全镜像 visit 建树；(2) harness **每个测例** kern load 全量拷贝 ELF（未复用 cache/slice）。  
**目标**：boot 仅 (1) 元数据；(2) 首次读文件时才 fill page_slice cache，exec/harness **复用同一路径**。

---

## Page cache 统一演进（§PageCache — 合并原 M3/M4/F）

原「F 文件内容↔slice」与「P2 缓存」「M3 kern load」「M4 backend direct」**是同一主线**，不是四条线：

| ID | 项 | 状态 | 说明 |
|----|-----|------|------|
| PC-1 | 启动仅 metadata | 🚧 | namespace populate **已**只建节点；cpio 解析仍一次扫全镜像（必要） |
| PC-2 | page cache 存 **page_slice** | ✅ | LRU 槽内 slice；>256KiB 直填 owned slice |
| PC-3 | cpio miss → slice 按需 insert | ✅ | `vfs_pcache_fill_from_inode` |
| PC-4 | **`vfs_kern_read_file_slice` → cache clone** | ✅ | `vfs_page_cache_clone_inode` |
| PC-5 | 统一 read 路径 | ✅ | `vfs_page_cache_read_inode` + backend_ops |
| PC-6 | ramfs 写 mirror + dirty + flush-on-drop | ✅ | `sync_write` / `flush_backing` |
| PC-7 | dirty 页 / 写合并 / O_DIRECT | 🚧 | per-page `VFS_PCACHE_PAGE_DIRTY`；无 O_DIRECT |
| PC-8 | LRU / 热页策略 | ✅ | 文件级 active/inactive + `page_list_node` 页 touch |
| **BE-1** | **后端消息框架** | ✅ | 全 IPC：`vfs_backend_ipc_call` → 各 backend 线程 |
| **BE-2** | **cpio flush=drop** | ✅ | `VFS_BACKEND_CAP_FLUSH_DROP`；不写回镜像 |
| **BE-3** | **blkdev 线程 stub** | 🚧 | `vfs_blkdev_port` + `vfs_cpio/ramfs_backend_port` |

**fd 表（用途 A）不参与 PC 线**：open/read 仍 **fd → server handle → inode → page cache**；不在 fd slice 里塞文件 bytes（除非未来 mmap 专门设计）。

---

## fd 表 SMP 锁（§SMP）

| 事实 | 说明 |
|------|------|
| 共享对象 | 同进程线程共享 `linux_fs_state_t.table` |
| 保护 | `cas_lock_t lock` — load/store/grow/fork-clone 读父表 |
| scratch | per-CPU；持锁仅覆盖 slice 读写，返回 scratch 后释放锁 |
| 残余 | 同 CPU 嵌套 scratch API 仍应避免；多线程 scratch 已按 CPU 隔离 |

---

## 重构合并清单（§Merge）

| ID | 重复项 | 状态 | 目标 |
|----|--------|------|------|
| M1 | path 双实现 | ✅ | `linux_layer/fs/vfs_path.c` |
| M2 | 路径常量 + equal | ✅ | `VFS_PATH_MAX` 统一；`vfs_path_equal()` |
| M3 | kern load ↔ exec load | ✅ | `vfs_page_cache_clone_inode` |
| M4 | cache vs backend direct | ✅ | `vfs_page_cache_read_inode` |
| M5 | perm cred vs setuid | ⬜ | cred 真源 `linux_proc_append_t` |
| M6 | 文档三角 | 🚧 | 本文 + ARCHITECTURE + IMPLEMENTATION_STATUS |

---

## fd 表 page_slice（§FdSlice — 用途 A）

| 能力 | 状态 |
|------|------|
| 表体在 page_slice | ✅ |
| fork `page_slice_clone` | ✅ |
| exit 不 rebuild | ✅ |
| hdr 不上 kstack | ✅ |
| dir 路径单源 `vfs_abs_path` | ✅ |
| SMP 锁 | ✅ |

详见 [`FD_TABLE.md`](FD_TABLE.md)。

---

## P0 — 测例阻塞

全部 ✅（fd 修复待 re-run 确认）。

---

## P1 — 命名空间

全部 ✅。

---

## P2 — 缓存与 I/O

| ID | 项 | 状态 |
|----|-----|------|
| P2-1 | cpio 按需 page_slice cache | ✅ |
| P2-2 | ramfs 写 mirror + flush-on-drop | ✅ |
| P2-3 | dirty / 写合并 | 🚧 → **PC-7** |
| P2-4 | O_DIRECT | ⬜ → **PC-7** |
| **P2-5** | **page cache → page_slice** | ✅ |
| **P2-6** | **kern/user 读统一** | ✅ |

---

## P3 — 挂载

| P3-4 bind | ⬜ |
| P3-5 后端切换 | ⬜ |
| 其余 | ✅ |

---

## P4 — 权限

| P4-3 sticky/group | ⬜ |
| P4-4 symlink | ⬜ |
| 其余 | ✅ |

---

## P5 — syscall / RPC

| P5-2 statx | ⬜ |
| P5-4 readlink/symlink | ⬜ |
| P5-5 path kmalloc scratch | ⬜ |
| 其余 | ✅ |

---

## P6 — 文档

| ID | 项 | 状态 |
|----|-----|------|
| P6-1 | `VFS_ARCHITECTURE.md` | ✅ 2026-07-12 刷新 |
| P6-2 | `RAMFS_AND_VFS_STORAGE.md` | ✅ 2026-07-12 刷新 |
| P6-3 | `ROOTFS.md` | ✅ |
| P6-4 | `VFS_IMPLEMENTATION_STATUS.md` | ✅ |
| P6-5 | `FD_TABLE.md` | ✅ |
| P6-6 | `DIRECTORY_PHASE.md` 归档标记 | ✅ |
| P6-7 | `PROGRESS.md` 与 52/52 对齐 | ⬜ |
| P6-8 | `VFS_SERVER_IPC.md` MOUNT/rename opcodes | ⬜ |

---

## 完成度概览

| 阶段 | 进度 |
|------|------|
| P0 / P1 / M1 / M2 | **100%** |
| fd 表 slice（用途 A） | **100%** |
| P2 缓存（page_slice 统一） | **90%** |
| P3 挂载 | **60%** |
| P4 权限 | **50%** |
| P5 syscall | **60%** |
| P6 文档 | **85%** |

---

## 下一批实现顺序（建议）

1. **P3-5**：mount 真切换后端。
2. **PC-7 / O_DIRECT**；块设备 writeback。
3. **P5-2** statx；**P6-7/8** 文档收尾。

---

## 验证门

```bash
make ARCH=x86_64 user && make ARCH=x86_64 build && make ARCH=x86_64 run
make ARCH=aarch64 user && make ARCH=aarch64 build && make ARCH=aarch64 run
```

---

## 参考

- [`FD_TABLE.md`](FD_TABLE.md) · [`FILE_LOADING.md`](FILE_LOADING.md)
- [`VFS_ARCHITECTURE.md`](VFS_ARCHITECTURE.md)
- [`VFS_IMPLEMENTATION_STATUS.md`](VFS_IMPLEMENTATION_STATUS.md)
