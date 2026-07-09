# Linux 兼容层代码结构

> **Last updated**: 2026-07-09  
> **文件加载**: [`FILE_LOADING.md`](FILE_LOADING.md)  
> **分层原则**: [`ARCHITECTURE.md`](ARCHITECTURE.md)

---

## 目录树（现行）

```text
include/linux_compat/          # 对外头文件（syscall、IPC、mm、fs）
linux_layer/
├── proc/                      # fork/clone/execve/wait/kill/…
│   ├── sys_execve.c
│   └── linux_exec_proc.c      # brk/mmap_hint、exec 后 proc 状态
├── fs/
│   ├── vfs_exec_load.c        # IPC VFS → page_slice
│   ├── linux_exec_image.c     # exec 镜像解析链（CPIO/IPC/embedded）
│   ├── vfs_exec_path.c        # /tests/<basename> 路径映射
│   ├── vfs_root_bootstrap.c
│   └── sys_fs_impl.c          # open/read/… → VFS RPC
├── mm/
│   ├── linux_page_slice_file.c   # copy_from_kva、file_base
│   ├── linux_mm_radix.c / linux_vspace.c
│   └── sys_{mmap,munmap,brk,…}.c
├── loader/
│   └── linux_elf_init.c       # spawn 后 brk、slice destroy
├── signal/                    # Phase 2B 信号
├── time/
├── ipc/
│   └── rpc.c
├── syscall/
│   ├── syscall_entry.c
│   └── thread_syscall.c
├── init/
└── tests/
    ├── user_test_runner.c
    ├── user_test_manifest.c   # manifest via vfs_kern_read_file_slice
    ├── elf_read_test.c
    └── task_test.c

servers/
├── fs/
│   ├── vfs_kern_load.c        # CPIO middle layer → page_slice
│   ├── vfs_server.c
│   └── …                      # cpio/ramfs/vfs_root
└── clean_server.c

rootfs/                        # initramfs 源树（见 INITRAMFS_PLAN §8）
script/rootfs/                   # build_cpio.sh、README.txt
```

---

## 模块职责（简表）

| 区域 | 职责 |
|------|------|
| `syscall/` | 分发表、线程 syscall |
| `proc/` | 进程语义、execve 编排 |
| `mm/` | Linux MM syscall、radix 封装、**page_slice 灌入** |
| `fs/` | VFS IPC 客户端、**exec 读文件** |
| `loader/` | ELF 线程启动后的 compat 初始化 |
| `signal/` | 投递、mask、sigreturn、altstack |
| `servers/fs/` | VFS 服务与 **middle-layer CPIO 读** |
| `tests/` | harness、manifest、elf 解析测例 |

---

## 关键头文件

| 头文件 | 用途 |
|--------|------|
| `include/linux_compat/fs/vfs_kern_load.h` | `vfs_kern_read_file_slice` |
| `include/linux_compat/fs/vfs_exec_load.h` | IPC slice 读 |
| `include/linux_compat/fs/linux_exec_image.h` | `linux_exec_load_elf_slice` |
| `include/linux_compat/mm/linux_page_slice_file.h` | `copy_from_kva`、`file_base` |
| `include/linux_compat/proc/linux_exec_proc.h` | brk、exec reset、slice 校验 |
| `include/linux_compat/proc_compat.h` | append 模型 |

Core 侧：`rendezvos/mm/page_slice.h`、`page_slice_copy.h`、`task/thread_loader.h`（**不由 compat 文档重复 API 说明**）。

---

## 架构相关代码

信号 arch、`time/arch/` 等已在 `linux_layer/signal/arch/`、`linux_layer/time/arch/`。  
未来若 syscall 寄存器差异增大，可再抽 `linux_layer/arch/<arch>/`（见 [`ARCHITECTURE.md`](ARCHITECTURE.md)）。
