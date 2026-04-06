# IPC Message Envelope (Kernel)

This repository uses in-kernel server threads that communicate via the lock-free IPC/port framework (`Message_t`, `Message_Port_t`, global port table). Kernel-control messages use **one** payload discipline: **TLV inside `kmsg_t`** (see below). Do not introduce parallel ad-hoc binary layouts for new opcodes.

## Envelope

- Carrier: `Message_t` + `Msg_Data_t`
- `Msg_Data_t.msg_type`: when `data` points at a `kmsg_t`, this is **`MSG_DATA_TAG_KMSG`** (`1`). That value only means “buffer layout is `kmsg_t`”; **operation identity** is `kmsg_hdr_t.module` + `kmsg_hdr_t.opcode`, not `msg_type`. Other uses of `Msg_Data_t` (tests, raw payloads) use their own numeric tags.
- `Msg_Data_t.data`: points to one allocated `kmsg_t` buffer (`hdr` + `payload`)
- `Msg_Data_t.free_data`: `free_msgdata_ref_default` (frees the `kmsg_t` buffer and the `Msg_Data_t`)

Header (`kmsg_hdr_t`) — slim, no in-band version field (layout changes bump `KMSG_MAGIC` and all producers/consumers together):

- `magic`: `KMSG_MAGIC` (`0x47534d4c`, ASCII `LMSG` in little-endian byte order) — distinguishes this wire shape from older experiments.
- `module`: subsystem id (e.g. `KMSG_MOD_CORE`)
- `opcode`: operation within that module (e.g. `KMSG_OP_CORE_THREAD_REAP`)
- `payload_len`: bytes of TLV payload after the fixed header prefix (`offsetof(kmsg_t, payload)` through end of buffer)

Sender identity / reply routing is **not** duplicated in the header: use TLV (e.g. `t` = reply port name) when an opcode needs a reply endpoint.

## Versioning

- There is **no** `version` field in the header. Changing `kmsg_hdr_t` requires a new **`KMSG_MAGIC`** (or coordinated roll) and updating `kmsg_from_msg` + all encoders.
- `(module, opcode)` identifies which **TLV schema** (format string) receivers apply.

## TLV payload (single encapsulation for `kmsg` data)

The `kmsg` **payload** is always a serialized TLV blob (`core/include/rendezvos/ipc/ipc_serial.h`). The public encoder is **`kmsg_create(module, opcode, fmt, ...)`** in `core/include/rendezvos/ipc/kmsg.h` (implementation builds the serialized payload internally; no `tlv` in the name).

- `u32 param_count`, then for each parameter: `u8 type_tag` (ASCII format character), `u32 value_len`, `value_len` bytes.
- Helpers: `ipc_serial_measure_va`, `ipc_serial_encode_into_va` (in-place encode), `ipc_serial_encode_alloc`, `ipc_serial_decode`.

Format characters (whitespace ignored; one char per parameter):

| Char | Meaning |
|------|---------|
| `p` | pointer / machine word (`void*`) |
| `q` | `i64` |
| `i` | `i32` |
| `u` | `u32` |
| `s` | C string (`char*`; length = `strlen`, no trailing NUL in wire) |
| `t` | Same wire as `s`; **by convention**, a **registered port name** (global port table) for reply routing (see Request–reply). |

Unpack: for `s`/`t`, the returned `char*` points into the message buffer; copy if needed after the message is freed.

### What “format checking” means (and what we do not have)

For `printf`, GCC/Clang can use `__attribute__((format(printf, ...)))` so **mismatches between the format string and the arguments are compile-time warnings**. Our TLV format letters (`"p q"`, etc.) are **custom**; the compiler does **not** verify that they match the `va_list` types. Wrong pairs are **undefined behavior** (like calling `printf` with the wrong types).

**Mitigation:** treat the format string as part of the protocol: **one shared macro or identical literal per `(module, opcode)`** on client and server (never hand-edit two different strings).

## Request–reply on top of ports (design space)

Today, many paths are **one-way** (e.g. clean server reaps a thread; no reply `Msg_Data_t`). For RPC-style calls, the natural transport is still `Message_t` on a `Message_Port_t`.

**Intended pattern (name-based reply port, aligns with your idea):**

1. The **caller** creates or owns a port, **registers** it in the global table under a stable name (see `register_port`, `PORT_NAME_LEN_MAX`).
2. The **request TLV** includes a leading `t` (or a dedicated position agreed per opcode) carrying that **port name string**.
3. The **callee** unpacks the TLV, resolves the name with `thread_lookup_port` / `port_table_lookup` (same mechanism as today’s string ports), builds a reply `Msg_Data_t` (also TLV + `kmsg` if the reply is kernel-control), enqueues on the current thread, and `send_msg(reply_port)`.

**Alternatives to negotiate later:**

- **Slot token** (`port_table_slot_token_t`): compact, but `port_table_resolve_token` still needs the **name** for validation today — so a token-only wire format would require API or table changes.
- **Opaque `Message_Port_t*` in TLV**: forbidden as a stable wire type across address spaces / reboots; names or tokens are safer.

**Complexity note:** reply correlation (request id), back-pressure, cancellation, and “which thread holds the reply port” need explicit rules; this doc only fixes the **payload envelope** and the **`t`** hook. Implement reply flows incrementally when you add a concrete opcode that needs them.

## Payload rules

- Receivers validate `msg_type == MSG_DATA_TAG_KMSG` (for kmsg carriers), `magic`, `module`, `opcode`, then **`ipc_serial_decode`** on `payload` / `payload_len`.
- `payload_len` must equal the packed TLV size; `ipc_serial_decode` consumes the buffer tightly.

## Allocation and lifetime

- `kmsg_create` measures the encoded size (`ipc_serial_measure_va`), allocates **one** buffer for `kmsg_hdr_t` + payload, then encodes **directly into** `km->payload` (`ipc_serial_encode_into_va`) — no separate heap block and no `memcpy` of the full payload.
- `ipc_serial_encode_alloc` / `ipc_serial_encode_va` still allocate a standalone encoded buffer for callers that only need bytes (e.g. tests).
- The `kmsg` buffer is owned by `Msg_Data_t` and freed via `free_msgdata_ref_default` when the message refcount drops to zero.

## Call-site façade (clean server)

- Current call site is in `linux_layer/syscall/thread_syscall.c` (`sys_exit`): it looks up the clean server port by name and sends a `kmsg_create(KMSG_MOD_CORE, KMSG_OP_CORE_THREAD_REAP, "p q", thread, exit_code)`.\n+- Server: `kmsg_from_msg` + `ipc_serial_decode(..., "p q", ...)`.

## Init and layering

- Servers are registered via `DEFINE_INIT` and are started by `do_init_call()`.
- On SMP systems, `do_init_call()` is executed on the BSP and on every secondary CPU.
- A server init function must decide what is BSP-only (e.g., registering one global port) and what is per-CPU (e.g., spawning one worker thread).
