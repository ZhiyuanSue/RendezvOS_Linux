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

**AI协作核心**：
- `doc/ai/AI_CHECKLIST.md` - **代码审查必读**，防止并发/生命周期/regression
- `doc/ai/INVARIANTS.md` - **运行时不约束式**，SMP/teardown/内存管理关键规则
- `doc/ai/README.md` - 完整AI协作工作流和验证门

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

---

## 关键架构原则

### 分层纪律
- **core/**：内核原语，不含Linux特定逻辑
- **linux_layer/**：Linux语义，能直接调core就不调IPC
- **servers/**：IPC串行化，仅在需要全局序列化时使用

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

## 总结

**AI角色**：在linux_layer/验证快速构建Linux兼容层的可行性

**最高约束**：⚠️ **core/修改必须得到用户确认**

**工作方式**：遵循架构原则，迭代式测试，保持与Linux测例兼容

**质量标准**：遵循AI_CHECKLIST.md和INVARIANTS.md，保持与core/相同质量

开始工作前，请先阅读本文档，然后深入相关的`doc/ai/`和`doc/linux_compat/`文档。