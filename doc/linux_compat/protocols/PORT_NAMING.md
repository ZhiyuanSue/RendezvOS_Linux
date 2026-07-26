# Port 命名约定（canonical）

**地位**：`linux_layer/` / `servers/` 里凡注册进 **全局 `global_port_table`** 的端口名，必须遵守本文。  
撞名 → `register_port` 失败 / 消息丢到错误端点 / 退出路径 `via_table=NULL` 一类故障，已反复出现。

**相关**：[`IPC_RPC_FRAMEWORK.md`](IPC_RPC_FRAMEWORK.md)（RPC）、[`../../ai/IPC_MESSAGE.md`](../../ai/IPC_MESSAGE.md)（`t` = reply 端口名）、core 表实现见 `core/docs/USING_CORE.md`（IPC）。

---

## 1. 为什么必须约定

全局 port 表是 **按名字唯一** 的字符串索引（跨 CPU 共享）。下列对象都会占一条名字：

| 角色 | 典型生命周期 | 数量 |
|------|----------------|------|
| **Listen / dispatcher** | 与 server 同寿 | 每 `(service, cpu)` 至多 1（或全局 1） |
| **Worker work-port** | 随 pool worker 创建/回收 | 每 `(service, cpu)` 多个 |
| **Client reply** | 随进程 / RPC 调用方 | 每 caller id 1 |

没有结构化名字时，最常见翻车是：

- 每 CPU 一个 pool，却都注册 `ipc_wk_0` → 后注册失败 → dispatch 丢消息；
- listen 名写死成全局一词，却与「每核一线程」心智不一致，排查时搞不清该找谁。

---

## 2. 身份模型（先定「是谁」，再拼名字）

每个会挂 port 的内核线程（或等价端点）用三元组描述：

```text
(service, cpu, local_id)
```

| 字段 | 含义 | 取值 |
|------|------|------|
| **service** | 逻辑服务名 | 短、稳定、小写蛇形；如 `clean`、`vfs`、`kernel` |
| **cpu** | 该端点所属核 | `percpu(cpu_number)`；**全局单例**用约定哨兵（见 §4） |
| **local_id** | 该核上该服务内的角色编号 | `0` = listen/dispatcher；`1..N` = worker；client 不用此字段，改用 caller id |

**规则**：port 名是该三元组（或 client 的 `(service, caller_id)`）的 **唯一可逆编码**。  
禁止两个不同三元组映射到同一字符串；禁止「凭感觉」再发明第三套前缀。

---

## 3. 名字语法（现行约定）

字符集：`[a-z0-9_]`，总长 `< PORT_NAME_LEN_MAX`（今日 64）。字段用 `_` 分隔。

### 3.1 Server 侧（listen + worker）

```text
{service}_c{cpu}              # listen / dispatcher（local_id == 0）
{service}_c{cpu}_w{wid}       # worker work-port（wid == local_id >= 1）
```

示例：

| 端点 | 名字 |
|------|------|
| CPU0 上 clean 的 listen | `clean_c0` |
| CPU0 上 clean 的 worker 3 | `clean_c0_w3` |
| CPU2 上 vfs 的 listen | `vfs_c2` |

**拼装顺序固定：service → cpu → worker。**  
后两段永远是十进制数字（无前导零要求，但禁止空字段）。不要用 `ipc_wk_<cpu>_<id>` 这类与 service 脱钩的名字。

### 3.2 Client 侧（reply port）

```text
{service}_cli_{caller_id}
```

- `caller_id`：通常为 **Linux `pid`**（或项目内等价进程 id）；特殊调用方用文档写死的字面量（如 init 收尸用 `0` / `init`，须在该服务协议里写明）。
- 示例：`vfs_cli_12`、`clean_cli_0`。

Client **不**编码 cpu：reply 跟进程走，不跟某核的 server 实例绑死。

### 3.3 一眼分辨

| 形态 | 角色 |
|------|------|
| `{svc}_c{N}` | 该核 listen |
| `{svc}_c{N}_w{M}` | 该核 worker |
| `{svc}_cli_{ID}` | 某调用方 reply |

---

## 4. 全局单例 listen（例外）

少数服务 **故意** 全机一个 listen（所有核的 server 线程 `recv` 同一 port），例如历史上的 `kernel_port`、早期 `clean_server_port`。

仅当协议文档 **明确写「global listen」** 时可用：

```text
{service}_listen
```

此时：

- **不得**再为同 service 注册 `{service}_c{cpu}` listen（两套并存禁止）；
- worker 若仍 per-CPU pool，work-port **仍必须**带 cpu：`{service}_c{cpu}_w{wid}`（pool 跨核共享全局表，不带 cpu 必撞）。

新服务默认走 **§3.1 per-CPU listen**；选 global 要在 [`DECISIONS.md`](../ai/DECISIONS.md) 或该服务协议里记一笔理由。

---

## 5. 生命周期与注册纪律

1. **Create → register → 持有 creator pin（或等价长期 ref）**，直到主动 `unregister_port`。不要「注册完就 `ref_put` 光」却假设表项永在（lookup 的 `hold` 在 ref=0 时会失败，表现为「名字不在」）。
2. Worker 退出：先 `unregister_port(work_name)`，再释放结构。
3. Client reply：进程 teardown 时按 `{service}_cli_{pid}` unregister（已有 `ipc_rpc_unregister_port_by_pid` 一类辅助）。
4. **Lookup 只用拼出来的全名**；禁止靠「猜前缀」或 per-thread 私货绕过全局表约定（诊断用 `port_table_lookup` 对照可以，发送路径仍走正式 API）。

---

## 6. 与现状的关系（迁移）

| 角色 | 现行名字 | 说明 |
|------|----------|------|
| clean listen | `clean_listen` | 全局单例（仅 BSP 一线程） |
| clean client | `clean_cli_{pid}` | 含 init 的 `clean_cli_0` |
| vfs listen | `vfs_listen` | 绑 VFS service CPU |
| vfs client | `vfs_cli_{pid}` | 用户进程 → VFS |
| vfs kernel client | `vfs_cli_k_srv` | VFS → backend（listen 串行） |
| vfs kernel client | `vfs_cli_k_reg_{fstype}` | backend register → VFS（每后端一端口） |
| kernel listen | `kernel_port` | core 字面量；登记为 singleton，暂不改名 |

辅助拼装：`include/linux_compat/ipc/port_naming.h` + `ipc_port_name_*`（`linux_layer/ipc/rpc.c`）。

**未完成**：`kernel_port` → `kernel_listen`；若某服务改为 per-CPU listen，用 `ipc_port_name_listen_cpu`。

改名须服务端 + 所有 client 查找点 + 协议头宏 **同一变更** 落地。

---

## 7. 检查清单（加 port / 改 server 时）

- [ ] 写出三元组或 `(service, caller_id)`，再写字符串。
- [ ] Listen / worker / client 三种形态有且仅有一种匹配 §3 / §4。
- [ ] SMP 下 worker 名含 **cpu**；多服务时含 **service**。
- [ ] 头文件里的 `*_PORT_NAME` / `*_PREFIX` 与本文一致，并在协议文档点名。
- [ ] 注册失败有日志；撞名当作硬错误修，不要静默改名。

---

## 8. 一句话

**Port 名 = 服务身份的字符串编码：先定 `(service, cpu, local_id)`（或 client 的 caller id），再按 `service_c{cpu}[_w{wid}]` / `service_cli_{id}` 拼出来。**
