# Linux 兼容层代码结构

> **Last updated**: 2026-08-01  
> **文件加载**: [`FILE_LOADING.md`](FILE_LOADING.md)  
> **分层原则**: [`ARCHITECTURE.md`](ARCHITECTURE.md)  
> **工作区盘点**: [`PROGRESS.md`](PROGRESS.md) §8

---

## 目录树（现行）

```text
include/linux_compat/          # 对外头文件（syscall、IPC、mm、fs）
├── debug_trace.h              # 可选 bring-up 跟踪宏（默认关）
├── fs/
│   ├── linux_fcntl.h          # fcntl/open 标志常量
│   ├── linux_poll.h           # pollfd + 事件位
│   └── linux_fd_table.h
├── ipc/rpc.h
└── …

linux_layer/
├── proc/                      # fork/clone/execve/wait/kill/…
│   ├── sys_execve.c
│   ├── proc_wait_ipc.c        # EXIT_NOTIFY 投递 / pending
│   ├── clean_ipc.c            # → clean_server one-way
│   └── linux_exec_proc.c
├── fs/
│   ├── sys_fs_impl.c          # open/read/write/dup/… → VFS RPC
│   ├── sys_fcntl.c            # fcntl(2)
│   ├── linux_fd_table.c / linux_pipe.c
│   ├── vfs_exec_load.c        # IPC VFS → page_slice
│   ├── linux_exec_image.c     # exec 镜像解析链
│   └── …
├── misc/
│   └── sys_poll.c             # poll / ppoll（ash）
├── mm/
│   ├── linux_page_slice_file.c
│   └── sys_{mmap,munmap,brk,…}.c
├── loader/
│   └── linux_elf_init.c
├── signal/                    # Phase 2B（含 arch/）
├── time/                      # 含 time/arch/
├── ipc/
│   └── rpc.c                  # request–reply + uninterruptible 变体
├── syscall/
│   ├── syscall_entry.c
│   └── thread_syscall.c
├── init/
└── tests/
    ├── user_test_runner.c     # Path B: /init → run_all.sh
    └── …

servers/
├── fs/                        # VFS listen + cpio/ramfs backends
└── clean_server.c             # THREAD_REAP / EXIT_NOTIFY worker

rootfs/                        # initramfs 源树（见 ROOTFS.md）
script/rootfs/                 # build_cpio / build_busybox / boot_smoke.sh
script/config/pack_user_rootfs.py  # 生成 tests/run_all.sh + manifest
```

---

## 模块职责（简表）

| 区域 | 职责 |
|------|------|
| `syscall/` | 分发表、线程 syscall |
| `proc/` | 进程语义、execve、wait/EXIT_NOTIFY |
| `mm/` | Linux MM syscall、radix、page_slice 灌入 |
| `fs/` | VFS IPC 客户端、fd 表、fcntl、exec 读文件 |
| `misc/` | 杂项 syscall（poll/ppoll） |
| `ipc/` | RPC 框架（commit 后不弃 recv；reply 重试） |
| `loader/` | ELF 线程启动后的 compat 初始化 |
| `signal/` / `time/` | 投递、mask、时间 syscall（arch 子目录） |
| `servers/fs/` | VFS 服务与 middle-layer CPIO 读 |
| `tests/` | boot harness（busybox Path B） |

---

## 关键头文件

| 头文件 | 用途 |
|--------|------|
| `include/linux_compat/fs/linux_fcntl.h` | `F_*` / `O_*` / `FD_CLOEXEC` |
| `include/linux_compat/fs/linux_poll.h` | `POLLIN`… / `linux_pollfd_t` |
| `include/linux_compat/debug_trace.h` | `LINUX_COMPAT_TRACE_{POLL,VFS_IO}` |
| `include/linux_compat/fs/vfs_kern_load.h` | `vfs_kern_read_file_slice` |
| `include/linux_compat/fs/vfs_exec_load.h` | IPC slice 读 |
| `include/linux_compat/fs/linux_exec_image.h` | `linux_exec_load_elf_slice` |
| `include/linux_compat/mm/linux_page_slice_file.h` | `copy_from_kva`、`file_base` |
| `include/linux_compat/ipc/rpc.h` | RPC API（含 uninterruptible） |
| `include/linux_compat/proc_compat.h` | append 模型 |

Core 侧：`rendezvos/mm/page_slice.h`、`page_slice_copy.h`、`task/thread_loader.h`（**不由 compat 文档重复 API 说明**）。

---

## 架构相关代码

信号 arch、`time/arch/` 等已在 `linux_layer/signal/arch/`、`linux_layer/time/arch/`。  
未来若 syscall 寄存器差异增大，可再抽 `linux_layer/arch/<arch>/`（见 [`ARCHITECTURE.md`](ARCHITECTURE.md)）。
