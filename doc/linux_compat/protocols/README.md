# Communication protocols

Authoritative wire protocols for servers, `linux_layer`, and how they use core IPC.
Design essays and status docs stay under `doc/linux_compat/`; **message order, ports, opcodes, and rendezvous rules live here.**

| Document | Scope |
|----------|--------|
| [`PORT_NAMING.md`](PORT_NAMING.md) | Global port-table name grammar (`service` / `cpu` / worker / `cli`) |
| [`IPC_RPC_FRAMEWORK.md`](IPC_RPC_FRAMEWORK.md) | Request-reply / one-way server templates |
| [`EXIT_CLEAN.md`](EXIT_CLEAN.md) | exit ↔ clean_server ↔ wait4（链路 A/B、`exit_state`） |
| [`WAIT_AND_SIGCHLD.md`](WAIT_AND_SIGCHLD.md) | wait4 ↔ SIGCHLD ↔ Layer B（Channel R/S、禁止项） |
| [`VFS_SERVER_IPC.md`](VFS_SERVER_IPC.md) | vfs_server opcodes and client RPC |
| [`IPC_CAPABILITIES.md`](IPC_CAPABILITIES.md) | Capability / port-access notes |

Related (envelope / TLV, not a server protocol):

- [`doc/ai/IPC_MESSAGE.md`](../../ai/IPC_MESSAGE.md) — kmsg layout, reply-port `t`
- Headers: `include/linux_compat/ipc/`, `include/linux_compat/fs/vfs_protocol.h`

**Rule:** fix hangs by correcting **legal message order** in these docs, not by short `try_send` timeouts on protocol paths.
