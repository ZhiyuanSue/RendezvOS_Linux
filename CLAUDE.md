# RendezvOS_Linux - AI Collaboration Guide

## 核心约束（最高优先级）

### ⚠️ core/ 目录限制
**AI不得主动修改core/代码**。如需修改：
1. **停止实施**，提出详细变更方案
2. **等待用户确认**后实施
3. 用户review，迭代改进
4. **由用户手动提交**，AI不负责提交

**原因**：core/代码质量要求高，已过多轮迭代优化。

### ✅ AI自由区域
`linux_layer/`、`servers/`、`tests/`等目录：AI可遵循架构原则自主实现。

---

## 快速导航

### 🚀 项目目标
验证在core/基础框架上，AI能否快速构建Linux兼容层：
- **目标**：200-300+ Linux syscall，多架构支持（x86_64、aarch64、riscv64）
- **策略**：迭代式测试，以测例为导向
- **架构**：保持混合内核模式

### 📋 必读文档（按优先级）

**文档总入口**：
- `doc/README.md` - **全仓库文档地图**（core / compat / ai 三区）

**AI协作核心**：
- `doc/ai/AI_CHECKLIST.md` - **代码审查必读**，防止并发/生命周期/regression
- `doc/ai/INVARIANTS.md` - **运行时不约束式**，SMP/teardown/内存管理关键规则
- `doc/ai/README.md` - 完整AI协作工作流和验证门

**Core（实施 compat 前；勿在 core/docs 写 compat 内容）**：
- `core/docs/USING_CORE.md` - **上层如何使用 core**（唯一入口：调用模式、阅读顺序）
- `core/docs/GUIDE.md` - core 内部分区与 API 索引（§6–§7）

**Linux兼容层设计**：
- `doc/linux_compat/SYSCALLS.md` - **实现顺序和详细清单**（易变，持续更新）
- `doc/linux_compat/ARCHITECTURE.md` - 分层原则、IPC vs 直接调core的决策
- `doc/linux_compat/MM_AND_COW.md` - 内存管理设计（nexus作为真源）
- `doc/linux_compat/DATA_MODEL.md` - 进程/线程数据模型

**参考文档**：
- `doc/ai/TEST_MATRIX.md` - 不同变更类型的测试要求
- `doc/ai/DECISIONS.md` - 重要架构决策记录（避免重复讨论）

---

## 工作流程

### 标准流程
```
实现/提议 → AI_CHECKLIST.md审查 → 验证 → 用户确认 → 更新ASSIST_HISTORY.md
```

### core/变更流程
```
发现需求 → 停止 → 提方案 → 用户确认 → 实施 → 用户review → 迭代 → 用户提交
```

### 验证门
- **记录**：运行了什么（make/test/boot）或**明确未运行**原因
- **不声称正确性**：除非有验证结果或用户审查

### 调试流程（⭐必读！遇到bug先用此法！）
**使用qemu_trace调试法分析崩溃/卡死问题**

**⚠️ 重要：遇到任何崩溃、卡死、异常行为，必须使用此方法！不要盲目加打印！**

**步骤**：
1. **初步定位**：`make ARCH=x86_64 run`，观察问题类型和大概位置
2. **判断场景**：
   - 如果**死循环卡死**（会重复打印）：用timeout截断，如`timeout 20s make ARCH=x86_64 run LOG=true DUMP=true`
   - 如果**正常退出/崩溃**：直接运行`make ARCH=x86_64 run LOG=true DUMP=true`
3. **生成日志**：`make dump`（生成qemu.log和objdump.log）
4. **定位问题**：在qemu.log中搜索关键信息（地址、寄存器、异常类型）
5. **符号定位**：在objdump.log中查找地址对应的函数
6. **源码分析**：结合汇编和源代码找到根因

**详细文档**：`/home/zhiyuansue/.claude/projects/-Users-zhiyuansue-Desktop-RendezvOS-Linux/memory/kernel_debugging_workflow.md`

---

## 关键架构原则

### 分层纪律
- **core/**：内核原语，不含Linux特定逻辑
- **linux_layer/**：Linux语义，能直接调core就不调IPC
- **servers/**：IPC串行化，仅在需要全局序列化时使用

### Core 能力复用（实施 linux_layer/servers 前）
- 查 `core/docs/USING_CORE.md` 与 `GUIDE.md` §6；**已有 core 能力必须复用**。
- 缺口记在 `doc/linux_compat/` 或 `doc/ai/`；**勿在 compat 目录重复 core 使用说明**。
- Linux 语义见 `doc/linux_compat/README.md`。

### 内存管理
- **nexus作为虚存真源**：不建独立VMA子系统
- **COW**：fork后共享只读物理页，写故障时分裂
- **地址空间角色**：区分KERNEL_HEAP_REF vs 用户VS_Common

### 并发安全
- **per-CPU数据**：Task_Manager、调度队列、kmem路由
- **MCS锁**：`me`参数必须是**当前CPU**的percpu slot
- **teardown**：修改其他CPU调度列表前必须同步

---

## 多架构支持

**策略**：从设计阶段就同步考虑多架构，架构相关代码隔离。

**支持目标**：x86_64、aarch64、riscv64，可能loongarch64

---

## 代码风格和文档

### 风格原则
- **实用主义**：正确性 > 风格一致性
- **Linux兼容**：必要时使用Linux内核头文件，接受一定风格侵入
- **与core/协调**：在Linux兼容性和core/一致性之间平衡

### 文档要求
- **代码注释**：关键逻辑必须充分注释
- **模块文档**：主要模块说明职责、接口、使用方式
- **决策记录**：重要设计记录在`doc/ai/DECISIONS.md`

---

## 常见命令

```bash
# 构建和运行
make ARCH=x86_64 config
make ARCH=x86_64 user    # 生成用户payload
make ARCH=x86_64 build   # 构建完整集成镜像
make ARCH=x86_64 run     # 运行

# 多架构（示例）
make ARCH=aarch64 config && make ARCH=aarch64 user && make ARCH=aarch64 build
```

---

## IPC RPC框架使用指南

### 📡 何时使用RPC框架

**使用场景**：当系统调用需要与服务器进程进行request-reply IPC通信时。

**典型用例**：
- 文件系统操作（VFS server）
- 进程管理操作（proc server）
- 其他需要全局序列化的资源操作

### 🏗️ RPC框架架构

**核心组件**：
- `include/linux_compat/ipc/rpc.h` - RPC接口定义
- `linux_layer/ipc/rpc.c` - RPC实现
- `doc/linux_compat/IPC_RPC_FRAMEWORK.md` - 详细文档

**两种server模式**：
1. **Request-Reply**：客户端发送请求，等待服务器响应（如VFS）
2. **One-Way**：客户端发送消息，不等待响应（如clean_server）

### 🔧 客户端使用模板

**基本用法**：
```c
// 1. 创建或查找reply端口
Message_Port_t* reply_port = ipc_rpc_port_lookup_or_create("vfs_client_<pid>");

// 2. 查找服务器端口
Message_Port_t* server_port = thread_lookup_port("vfs_server_port");

// 3. 发送RPC请求并等待响应
i64 result = ipc_rpc_call(server_port, reply_port,
                         MY_OPCODE, "pu", user_ptr, size);

// 4. 清理引用
ref_put(&server_port->refcount, free_message_port_ref);
ref_put(&reply_port->refcount, free_message_port_ref);
```

**VFS专用封装**：
```c
// VFS系统调用使用vfs_ipc_request_response封装
i64 result = vfs_ipc_request_response(KMSG_OP_VFS_GETCWD,
                                     VFS_KMSG_FMT_GETCWD,
                                     user_buf, size);
```

### 🖥️ 服务端使用模板

**Request-Reply服务器**（如VFS）：
```c
// 1. 定义handler处理不同opcode
static i64 my_rpc_handler(u16 opcode, const kmsg_t* km, char** reply_port_out)
{
        u64 param1, param2;
        i64 result = 0;

        switch (opcode) {
        case MY_OP_GETCWD:
                // 解码参数：业务参数 + reply port ('t')
                ipc_serial_decode(km->payload, km->hdr.payload_len,
                                "put", &param1, &param2, reply_port_out);
                // 处理请求...
                result = 0;  // 返回Linux errno或0
                break;
        default:
                return -LINUX_ENOSYS;
        }

        return result;
}

// 2. 服务器线程入口
static void my_server_thread(void)
{
        ipc_rpc_server_loop(MY_SERVER_PORT_NAME,  // 监听端口名
                           my_service_id,          // 服务ID
                           MY_RESP_OPCODE,          // 响应opcode
                           "q",                    // 响应格式
                           my_rpc_handler);        // handler函数
}

// 3. 初始化服务器
static void my_server_init(void)
{
        // 创建并注册服务器端口
        Message_Port_t* port = create_message_port(MY_SERVER_PORT_NAME);
        my_service_id = port->service_id;
        register_port(global_port_table, port);

        // 创建服务器线程
        gen_thread_from_func(&server_thread_ptr,
                            (kthread_func)my_server_thread,
                            "my_server_thread",
                            percpu(core_tm), NULL);
}
DEFINE_INIT(my_server_init);
```

**One-Way服务器**（如clean_server）：
```c
static void on_message(Message_t* msg, u16 service_id)
{
        // 处理消息，不发送响应
        const kmsg_t* km = kmsg_from_msg(msg);
        // ... 处理逻辑 ...
}

void clean_server_thread(void)
{
        ipc_server_recv_loop(CLEAN_SERVER_PORT_NAME, on_message);
}
```

### ⚠️ 关键注意事项

1. **端口命名约定**：
   - 客户端reply端口：`<service>_client_<pid>`
   - 服务器端口：`<service>_server_port`

2. **消息格式**：
   - 请求：业务参数格式 + `'t'`（reply port）
   - 响应：默认`"q"`（单个i64），可自定义

3. **错误处理**：
   - 服务端必须使用`ipc_rpc_reply_best_effort`发送错误响应
   - 避免客户端卡在`recv_msg`等待

4. **引用计数**：
   - 使用`ref_put`释放端口和消息引用
   - 遵循core的引用计数规则

5. **服务ID**：
   - 使用端口的`service_id`而非硬编码常量
   - 确保消息路由正确

### 📚 相关文档

- `doc/linux_compat/IPC_RPC_FRAMEWORK.md` - RPC框架详细文档
- `doc/ai/IPC_MESSAGE.md` - IPC消息机制
- `include/linux_compat/ipc/rpc.h` - API接口

---

## 总结

**AI角色**：在linux_layer/验证快速构建Linux兼容层的可行性

**最高约束**：⚠️ **core/修改必须得到用户确认**

**工作方式**：遵循架构原则，迭代式测试，保持与Linux测例兼容

**质量标准**：遵循AI_CHECKLIST.md和INVARIANTS.md，保持与core/相同质量

开始工作前，请先阅读本文档，然后深入相关的`doc/ai/`和`doc/linux_compat/`文档。