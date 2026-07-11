# Per-process fd table（方案 B）

> **Status**: 定稿（2026-07-09）— 取代 bootstrap 阶段「vfs_server 按 pid 维护 fd 编号表」  
> **前置**: [`VFS_ARCHITECTURE.md`](VFS_ARCHITECTURE.md) §1–§2、[`STDIO_SHIM.md`](STDIO_SHIM.md)  
> **Live 接线**: [`VFS_IMPLEMENTATION_STATUS.md`](VFS_IMPLEMENTATION_STATUS.md)

---

## 1. 为什么要改

Bootstrap（Step 3–4）为了快，把 **fd 编号表放在 `vfs_server`**，compat 的 `sys_*` 把 **fd 整数** 直接 RPC 出去；`write(1/2)` 在 compat 硬编码走 UART。

这带来三个结构性问题：

1. **不符合 Linux 模型**：fd 表应在 **每进程**（线程组共享），不在文件系统服务里。
2. **`dup2` 无法重定向 stdout**：若 `write(1)` 永远走 UART，则 `dup2(3,1)` 无效。
3. **pid 耦合**：server 用 `vfs_client_<pid>` 解析 pid 再查 `vfs_fd_tables[pid]`，等于 FS 服务持有进程语义。

**方案 B**（maintainer 确认）：compat **本地 fd 表** + vfs_server **全局 open handle 表**（打开文件描述 / `struct file` 等价物）。

---

## 2. Linux 对照（我们要对齐什么）

| Linux | RendezvOS 方案 B |
|-------|------------------|
| `struct inode` + 页缓存 | `vfs_inode_t` + cpio/ramfs（`vfs_root.c`，不变） |
| `struct file`（offset、flags、→inode） | **`vfs_open_handle_t`**（server 全局表，带 refcnt） |
| `files_struct->fd[]`（fd → struct file*） | **`linux_fs_state_t` in `linux_proc_append_t`** |
| `dup` / `dup2` | 改 compat fd 槽；VFS 条目 **共享 handle** + server `refcnt++` |
| `fork` | 复制 fd 表；每个 VFS handle **retain**（后续 Step；当前 fork 文档已声明未复制 fd） |
| `execve` | 重置 fd 表为 0/1/2 console；关闭旧 VFS handle（release） |

**Per-process vs per-thread**：与 Linux 一致 — **fd 表 per-process**（`Tcb_Base` / 线程组）。同 pid 的线程共享 `linux_proc_append_t.fs`。不用 tid 索引 fd。

**core 不动**：状态放在 `Tcb_Base.append_tcb_info` 的 `linux_proc_append_t` 扩展字段；与 brk、signal disposition 同模式。

---

## 3. 分层与数据流

```text
┌──────────────────────────────────────────────────────────────┐
│ linux_layer — compat front                                    │
│  linux_proc_append_t.fs { cwd, fd[0..31] }                   │
│    fd[i] → { kind: CONSOLE_IN|OUT|ERR|VFS, handle, path }   │
│  sys_openat: resolve path (cwd + dirfd) → IPC OPEN(abs)       │
│  sys_read/write/close/…: 查 fd[i] → CONSOLE* 本地 / VFS IPC │
│  sys_dup2: 改 fd 槽；VFS 则 IPC HANDLE_RETAIN                 │
└────────────────────────────┬─────────────────────────────────┘
                             │ IPC: path 或 handle + user buf
┌────────────────────────────▼─────────────────────────────────┐
│ servers/fs — VFS server thread                                │
│  vfs_handle_t[id]: { refcnt, inode copy, offset, flags }    │
│  vfs_root_*: lookup / read / write / readdir（中端，不变）    │
│  **无** per-pid fd 编号表                                     │
└──────────────────────────────────────────────────────────────┘
```

**IPC 传什么**

| 操作 | IPC 参数 | 不传 |
|------|----------|------|
| `openat` | 绝对路径、`flags`、`mode` | dirfd（compat 已展开） |
| `read/write/close/fstat/lseek/getdents64` | **handle**、user 指针/计数 | compat fd 编号 |
| `mkdirat/unlinkat/newfstatat` | 绝对路径（compat 展开） | dirfd |
| `chdir` | 绝对路径（server 只 **校验** 是目录） | — |
| `getcwd` | **无 RPC** | — |
| `HANDLE_RETAIN` | handle | dup/fork 增加 refcnt |

**pid 的用途**：仅 `find_task_by_pid` + `linux_mm_*_user` 访问 **发起 syscall 的进程** 用户内存；**不**用于查 fd 表。

---

## 4. 数据结构（定稿）

### 4.1 Compat — `include/linux_compat/fs/linux_fd_table.h`

```c
#define LINUX_FD_MAX 32

typedef enum linux_fd_kind {
    LINUX_FD_NONE = 0,
    LINUX_FD_CONSOLE_IN,
    LINUX_FD_CONSOLE_OUT,
    LINUX_FD_CONSOLE_ERR,
    LINUX_FD_VFS,
} linux_fd_kind_t;

typedef struct linux_fd_entry {
    linux_fd_kind_t kind;
    u32 vfs_handle;           /* valid if kind == LINUX_FD_VFS */
    char path[VFS_PATH_MAX];  /* abs path at open; for dirfd join + debug */
    bool is_dir;
} linux_fd_entry_t;

typedef struct linux_fs_state {
    char cwd[VFS_PATH_MAX];   /* default "/" */
    linux_fd_entry_t fds[LINUX_FD_MAX];
} linux_fs_state_t;
```

嵌入 `linux_proc_append_t`（见 `proc_compat.h`）**仅保存指针** `linux_fs_state_t *fs`；表体 kmalloc，生命周期与进程一致（`linux_fs_proc_attach` / `linux_fs_proc_destroy`）。

**初始状态**（`linux_fs_init_proc`）：

| fd | kind |
|----|------|
| 0 | `CONSOLE_IN` |
| 1 | `CONSOLE_OUT` |
| 2 | `CONSOLE_ERR` |
| 3–31 | `NONE` |

### 4.2 Server — `servers/fs/vfs_handle.h`

```c
#define VFS_HANDLE_INVALID 0u
#define VFS_HANDLE_MAX     128u

typedef struct vfs_open_handle {
    bool in_use;
    u32 refcnt;
    vfs_inode_t ino;
    u64 offset;
    i32 open_flags;
} vfs_open_handle_t;
```

- `vfs_handle_open(ino, flags)` → 新 id，`refcnt = 1`
- `vfs_handle_retain(id)` → `refcnt++`（dup / fork）
- `vfs_handle_close(id)` → `refcnt--`；为 0 时释放槽位

---

## 5. Syscall 分发（compat）

```text
write(fd):
  CONSOLE_OUT / CONSOLE_ERR → sys_write_impl（UART，无 IPC）
  VFS → IPC WRITE(handle, buf, count)
  CONSOLE_IN / NONE → -EBADF

read(fd):
  CONSOLE_IN → 0（EOF bootstrap）或日后 UART
  VFS → IPC READ(handle, …)
  CONSOLE_OUT/ERR → -EBADF

close(fd):
  清空 fd 槽
  VFS 且 compat 内无其它 fd 共享同一 handle → IPC CLOSE(handle)

dup2(old, new):
  若 new 曾持有 VFS handle 且将被覆盖 → 先对旧 handle 尝试 release
  复制 old 槽到 new
  VFS → IPC HANDLE_RETAIN(handle)

openat:
  linux_vfs_resolve_path(dirfd, path) → abs
  IPC OPEN(abs, flags, mode) → handle
  alloc 最小 fd ≥ 0 的空槽，写入 VFS 条目
```

**`write(1)` 在 dup2 之后**：查表若为 `LINUX_FD_VFS`，走 VFS IPC — **不再**永久截获 fd 1。

---

## 6. 路径展开（compat 负责）

从 server 迁出 `vfs_resolve_path` 逻辑到 `linux_layer/fs/linux_vfs_path.c`：

- 绝对路径 → normalize
- `AT_FDCWD` → `fs.cwd`
- 其它 dirfd → 本地 fd 表项须为 **目录** VFS 条目，用 `entry.path` 作 base
- `vfs_path_join` 可复用 server 侧 `servers/fs/vfs_path.c` 的算法；compat 侧实现一份或共享头文件（**不** RPC）

**chdir**：compat 展开路径 → RPC 校验目录存在 → 写 `fs.cwd`  
**getcwd**：直接读 `fs.cwd` 写入 user（**删除** GETCWD RPC）

---

## 7. 生命周期

| 事件 | 行为 |
|------|------|
| 首进程 / ELF init | `linux_fs_proc_attach(task)` |
| `fork` | `linux_fs_proc_fork(child, parent)`；对每个 **唯一** VFS handle `HANDLE_RETAIN` |
| `execve` | `linux_fs_proc_reset(task)`：release 所有 VFS handle；重置 0/1/2 console |
| 进程 exit / `delete_task` | `linux_fs_proc_destroy(task)`：release handles + kfree |

---

## 8. 与 bootstrap 的差异（迁移清单）

| 项 | Bootstrap | 方案 B |
|----|-----------|--------|
| fd 表位置 | `vfs_fd_tables[pid]` | `linux_proc_append_t.fs` → heap `linux_fs_state_t` |
| OPEN RPC fmt | `isiu`（dirfd, path, …） | **`siu`**（abs path, …） |
| OPEN 返回值 | fd 编号 (≥3) | **handle** id |
| READ/CLOSE/… | fd 编号 | **handle** |
| GETCWD RPC | 有（曾 stub） | **删除** |
| cwd 存储 | server `vfs_fd_table.cwd` | **`fs.cwd`** |
| `VFS_FD_MIN` | 3 | **删除**；server 无 fd 概念 |

**内部调用**（`vfs_exec_load.c`）：改用 OPEN `siu` + handle RPC，不经过 fd 表。

---

## 9. 非目标（本文档阶段）

- stdin UART 中断 / 阻塞 read
- `pipe` / `pipe2`（单独 Phase 4c）
- `fcntl` / `ioctl` / tty 行规程
- 完整 fork fd 继承（可先 retain API 就绪，fork 复制后续接）
- 把 console 注册进 vfs_server（stdout 未重定向时仍 **本地** UART）

---

## 10. 验证门

- [ ] `#6 dup` / `#7 dup2` stdout PASS（至少 stdio + VFS 组合）
- [ ] `#22 openat` 仍 PASS（路径在 compat 展开）
- [ ] `#3 chdir` / `#12 getcwd` PASS（cwd 在 compat）
- [ ] `#51 write`：fd≥3 与 dup2 重定向后的 fd 1 写文件
- [ ] harness 52/52 不退化
- [ ] 双架构 run.log 更新 [`CROSS_ARCH_VERIFICATION_LOG.md`](CROSS_ARCH_VERIFICATION_LOG.md)

---

## 11. 追溯

| 日期 | 事件 |
|------|------|
| 2026-07-09 | Maintainer 确认方案 B；本文档定稿；**代码已落地**（待 run 验证） |

---

## 12. 设计复核（第二遍 — 与实现一致）

| 检查项 | 结论 |
|--------|------|
| dup2 重定向 stdout | ✅ `write` 查表，非硬编码 fd 1 |
| console 是否全 RPC | ❌ 仅 `LINUX_FD_VFS` 走 IPC；未重定向的 1/2 仍 UART |
| cwd 在哪 | ✅ `linux_fs_state_t.cwd`；GETCWD 无 RPC |
| server 是否还存 fd 编号 | ❌ 已删 `vfs_fd.c`；仅 handle 表 |
| fork/exec | ✅ `linux_fs_fork_copy` / `linux_fs_reset_proc` |
| open `is_dir` 元数据 | bootstrap：由 `O_DIRECTORY` 标志推断；足够 getdents 测例 |

改 fd 表 / handle / OPEN RPC 时：**先更新本文 + VFS_IMPLEMENTATION_STATUS + DECISIONS**。
