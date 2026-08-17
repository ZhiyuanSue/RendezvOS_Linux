# 下一步计划（busybox bring-up 收尾之后）

> **Status**: live  
> **Date**: 2026-08-09  
> **前提**: busybox `/init` + 用户态 `run_all` 已作为日常 gate；妥协清单已归档。  
> **相关**: [`PROGRESS.md`](PROGRESS.md) · [`BOOT_PATH_EVOLUTION.md`](BOOT_PATH_EVOLUTION.md) · [`archive/BUSYBOX_BOOT_DEFERRALS.md`](archive/BUSYBOX_BOOT_DEFERRALS.md) · [`core/docs/TODO.md`](../../core/docs/TODO.md)

---

## 0. 阶段结论

**初步的 busybox 启动改造可以认为做完了。**

- 路径：`linux_boot` → `/init`（busybox）→ `sh /tests/run_all.sh` → 用户态 `execve` 测例  
- x86_64 上曾对齐全 harness（`run_all` **52/52**）  
- VFS RPC coop + nested **reply** transfer 已落地；request 仍走 port rendezvous（刻意）  
- **CMDLINE 策略在上层 Makefile 注入**；core 只提供 `CMDLINE=` 机制（空则不 `-append`）  
- 交互 console / UART server **不挡** 上述 gate，列为额外工作  

`busybox-support` 分支上的「为跑通 busybox 而开的妥协账」关闭；后续在兼容层主线上按本文件推进。

---

## 1. 本波未提交改动盘点（整理用）

以下对应当前工作区相对 `busybox-support` tip 的主要增量（以 `git status` 为准，提交前再核）。

| 区域 | 内容 | 分层含义 |
|------|------|----------|
| **根 `Makefile`** | `LINUX_BOOT_CMDLINE_DEFAULT` / `CONFIG_CMDLINE` / `RUN_CMDLINE`；`config`/`build`/`run` 向 core **注入** cmdline | **compat 策略**；不写进 core Makefile |
| **`core` 子模块指针** | 指向含 cmdline **机制**的 commit（`CMDLINE=` → env / QEMU `-append`；默认空） | **机制 only**；禁止再塞 busybox 默认串 |
| **`linux_boot.c`** | 空 cmdline 时 compat 默认 `sh /tests/run_all.sh`；有 cmdline 则按 token 拆 argv | 上层语义默认，合法 |
| **`include/.../rpc.h` + `linux_layer/ipc/rpc.c`** | nest reply `@n` + `ipc_transfer_message`；删死代码 `ipc_rpc_server_loop`；sync/coop 分路 | compat IPC |
| **`servers/fs/vfs_backend_ipc.*` / `vfs_server.c` / `vfs.h`** | leaf nest-resp drain；direct deliver 仅 nested reply | servers |
| **文档** | `IPC_RPC_FRAMEWORK` / `DECISIONS` / `AI_CHECKLIST` / 本文 + deferrals **归档** | — |

**纪律（分体式内核）**

- core：bootargs / IRQ→IPC / 日志 sink 等**原语**；不承载「跑 busybox 测例」的策略串。  
- 上层：默认 argv、rootfs pack、VFS/uart server 策略。  
- 改 core 必须先提案、用户确认（见 `CLAUDE.md`）。

---

## 2. 推荐下一波（兼容层主线）

按需取用，不绑定必须一次做完。

| 序 | 项 | 说明 |
|----|-----|------|
| A | **Phase 5 / 更多 syscall** | 见 [`SYSCALLS.md`](SYSCALLS.md)；以测例为导向 |
| B | **execve 正规化收尾** | de_thread、post-exec 清理；Path B 仅保留「首次进用户」出口（非第二套 loader） |
| C | **VFS 演进** | shebang、`/dev/*`、readdir/tombstone 等见 [`VFS_EVOLUTION.md`](VFS_EVOLUTION.md) |
| D | **aarch64 日常** | 多核慢偏 idle×QEMU；日常 `SMP=1`；core WFI 另案 |
| E | **构建妥协（可选）** | static busybox / 关 `tc`：需要动态链接或真 UAPI 时再开 |

---

## 3. 额外项：UART server（不挡 busybox gate）

> 目标：串口 MMIO / IRQ **单一所有者**；TX/RX 经 IPC；与 core 早期 `uart_putc` / `pr_*` 的权衡另议。

### 现状缺口（compat + platform）

- 用户态 console：无真 UART RX；`read(0)` / CONSOLE_IN 偏 EOF/`POLLHUP`  
- 驱动侧常见：16550 `IER=0`、PL011 RX 未接 handler、x86 APIC 路径 IOAPIC 空等  
- core **已有** `register_irq_handler` + IRQ→IPC（timer 模式可对齐）

### 建议方向（待设计拍板后再动 core）

1. **uart_server**（servers）：独占 MMIO；RX IRQ → 消息；TX 只收「写串口」请求。  
2. **handoff**：boot 早期仍允许 core 直写；handoff 后常规日志走 server；panic/emergency 可保留直写。  
3. **权衡未决**：是否让所有 `pr_*` 走 IPC（延迟/死锁面）vs 仅 `/dev/console` 走 server。  
4. 与 core TODO **#46**（Log/output via dedicated IPC server）重叠——改 core 前走提案流程。

实现前：读 `core/docs/USING_CORE.md`（IRQ/IPC）、[`STDIO_SHIM.md`](STDIO_SHIM.md)、[`protocols/`](protocols/README.md)。

---

## 4. Core 待办（提案用，非本分支必做）

权威列表（中文条列）：[`core/docs/TODO.md`](../../core/docs/TODO.md)。

和上层交叉多的几条：

- 平台：UART getc；TLB IPI（软 IPI 已有）；x86 IOAPIC / 外设 IRQ 为**远期**（非冻结）  
- 37、38、46：日志前后端与 IPC 输出；交接说明见 `core/docs/log.md`  
- 42：Linux argv 归兼容层，别塞回 core  
- 52：系统化 call→IPC wrapper 偏上层；core 留 port 原语  
- 内存：boot 栈已结（见 core TODO_DONE #58）；多 zone **骨架**已结（#60，`configure_pmm_zones_hook`）；第二 DMA 池等有硬件约束再加。fork/COW / mmap 后续仍可能动 MM  

规则：compat 需要动 core 时先提案再改；别把上层 cmdline 策略写进 core Makefile。

---

## 5. 刻意不做 / 已关闭

| 项 | 说明 |
|----|------|
| busybox 妥协 live 清单 | → [`archive/BUSYBOX_BOOT_DEFERRALS.md`](archive/BUSYBOX_BOOT_DEFERRALS.md) |
| 在 core Makefile 写死 `sh /tests/run_all.sh` | ❌ 禁止；上层注入 |
| nested **request** 纯直投 leaf（无 wake） | ❌ 除非 core 提供「直投 + 解除 port wait」 |
| 把 UART server 当成 busybox P0 | ❌ 额外项 |

---

## 6. 验证习惯（不变）

- 改 boot/IPC/VFS：优先 x86_64 `run_all`（或 `boot_smoke`）；记录跑了什么。  
- 崩溃/卡死：`LOG=true DUMP=true` + `make dump`（见 `CLAUDE.md`）。  
- 无证据不声称正确性。
