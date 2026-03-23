# IPC Message Envelope (Kernel)

This repository uses in-kernel server threads that communicate via the lock-free IPC/port framework. To keep messages extensible and self-describing, all kernel-control messages carried by `Message_t` use a common envelope.

## Envelope

- Carrier: `Message_t` + `Msg_Data_t`
- `Msg_Data_t.msg_type`: `RENDEZ_MSG_TYPE_KERNEL` (currently `1`)
- `Msg_Data_t.data`: points to one allocated `kmsg_t` buffer (`hdr` + `payload`)
- `Msg_Data_t.free_data`: `free_msgdata_ref_default` (frees the `kmsg_t` buffer and the `Msg_Data_t`)

Header (`kmsg_hdr_t`) fields:

- `magic`: `KMSG_MAGIC` (`'KMSG'`)
- `version`: `KMSG_VERSION` (currently `1`)
- `kind`: message kind (`enum kmsg_kind`)
- `payload_len`: bytes after the header
- `src_cpu`: logical CPU index (`cpu_id_t`)
- `src_tid`, `src_pid`: best-effort sender identity (`tid_t`/`pid_t`)

## Versioning

- `version` is bumped only when the header layout changes.
- `kind` identifies the payload schema; a new payload adds a new `kind` value (does not require changing `version`).

## Payload rules

- `payload_len` must match the exact payload struct size for fixed-size payloads.
- For variable-size payloads, define a leading struct with a length field and validate bounds before access.
- The receiver must validate: `msg_type`, `magic`, `version`, and `payload_len` before casting.

## Allocation and lifetime

- `kmsg_create()` allocates a single buffer for `kmsg_t` and copies the payload bytes into it.
- The buffer is owned by `Msg_Data_t` and is freed via `free_msgdata_ref_default` when the message refcount drops to zero.
- Receivers must not store raw pointers into the `kmsg_t` payload beyond the message lifetime.

## Current kinds

- `KMSG_KIND_THREAD_EXIT`: payload `kmsg_thread_exit_t` (`Thread_Base* thread`, `i64 exit_code`), used by `sys_exit` -> `clean_server`.

## Init and layering

- Servers are registered via `DEFINE_INIT` and are started by `do_init_call()`.
- On SMP systems, `do_init_call()` is executed on the BSP and on every secondary CPU.
- A server init function must decide what is BSP-only (e.g., registering one global port) and what is per-CPU (e.g., spawning one worker thread).
