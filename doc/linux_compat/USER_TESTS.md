# Linux compat user tests: single vs smp

本文件描述 **Linux 兼容层（用户态 ELF 测例）** 的运行模型、同步边界与输出乱序的期望。

## 为什么需要 single / smp 分层

- **single**：优先验证“功能点是否工作”（如 `getpid`/`brk`），减少并发噪声，降低 bring-up 调试成本。
- **smp**：验证“并发下是否正确”（如 `mmap/munmap/exit` 会触发多核 allocator、跨核清理、生命周期竞态），必须在多核参与下加压。

这并非否定多核；而是把验证拆成两个阶段，便于归因。

## 运行方式（对齐 core test 模型）

参考内核侧的 `core/modules/test/single_test.c` 与 `core/modules/test/smp_test.c`：

- **BSP orchestrator**：由 BSP 选择当前 case，打印 case banner，并推进到下一个 case。
- **per-CPU runner 线程**：每个 CPU 上有一个 runner（线程不迁移，因此“在哪个 CPU 上创建就在哪个 CPU 上跑”）。
- **barrier 两次**：每个 case 都有 begin/end 同步点。

Linux user tests 的实现入口在：

- `linux_layer/tests/user_test_runner.c`（`LINUX_COMPAT_TEST` 使能时生效）

## 如何在 config 中开关测试（非侵入式）

- **开启/关闭 Linux compat tests**：编辑 `script/config/root.json`
  - `use: true/false`：总开关（更雅观，不需要删 key）
  - `features: ["LINUX_COMPAT_TEST", ...]`：仅在 `use=true` 时生效
  - `make config ...` 会生成 `Makefile.root.env`，只用于顶层 `linux_layer/servers` 的编译
- **开启/关闭 core tests**：编辑 `core/script/config/config_*.json` 的 `modules.test.use`，并使用 `modules.test.features = ["RENDEZVOS_TEST"]`

注意：`do_init_call()` 会在所有 CPU 上执行，因此 **不能**像早期 `task_test()` 那样在 initcall 里“无条件 spawn 所有 app”，否则会造成跨 CPU 的 case 混跑与输出交错。

## single 阶段语义

- 系统 SMP 仍可启用，但 **只有 BSP 的 runner 会创建/等待用户测例**。
- 其他 CPU 不参与用户测例（可 idle）。

因此在 single 阶段，用户 stdout/stderr 的输出应当呈现“顺序感”（不会被其他 CPU 的用户输出插入）。

## smp 阶段语义（选项一）

你选择的 smp 运行策略是：

- 每个 case（功能点）由 **每个 CPU 的 runner** 各自 spawn 并等待 **该 CPU 上的一个用户测例实例**。
- case 内允许并发，因此 **同一个 case 内的用户 stdout/stderr 允许交错**（用于暴露竞态/时序问题）。
- **不同 case 之间禁止交错**：依靠 runner 的串行调度 + begin/end barrier + case banner 保证。

## 输出乱序的边界（重要）

- **内核日志（`printk/pr_*`）**：调试期允许并发交错，不强求全局有序。
- **用户输出（`write(1|2)`）**：
  - single：应当不被其他 CPU 的用户输出插入（前提：其他 CPU 不跑用户测例）。
  - smp：同 case 内允许交错；不同 case 不应交错。

如果未来需要更强的可读性，可以加“每行前缀 cpu/tid/pid”的 debug 选项，但不作为第一阶段目标。

