# Fork实现所需的Core层支撑分析

## 当前fork实现状态

### 已完成部分
✅ **任务结构创建** - `new_task_structure()` (core API)
✅ **VSpace结构复制** - `linux_copy_vspace()` (调用core API)
✅ **PID分配** - `get_new_id()` (core API)
✅ **任务管理器添加** - `add_task_to_manager()` (core API)

### 缺失部分（需要core支撑）

## 1. 线程复制基础API

### 缺失：`copy_thread()`
```c
// core/kernel/task/tcb.c - 需要添加
Thread_Base* copy_thread(
    Thread_Base* parent_thread,
    Tcb_Base* child_task,
    u64 child_return_value  // 子进程返回0
);
```

**功能需求**：
- 复制父线程的内核栈配置
- 分配子线程的内核栈
- 复制父线程的架构上下文（Arch_Task_Context）
- **关键**：修改子线程的返回值寄存器（rax=0）
- 设置子线程为ready状态

**为什么在core**：
- 通用OS功能，任何OS都需要进程复制
- 需要架构特定的寄存器操作
- RendezvOS原生服务也可能需要

### 缺失：架构上下文复制API
```c
// core/arch/x86_64/task/tcb_arch.h - 需要添加
void arch_copy_task_context(
    const Arch_Task_Context* src,
    Arch_Task_Context* dst,
    u64 new_return_value  // 设置子进程返回值
);
```

**功能需求**：
- 复制通用寄存器状态
- 复制控制寄存器（rsp, rip等）
- **关键**：修改返回值寄存器（rax/x0=0）
- 复制浮点/SIMD状态（如果需要）

**为什么在core**：
- 架构特定，每个架构需要实现
- 通用的上下文复制机制

## 2. VSpace复制完善

### 当前状态
- ✅ `new_vs_root(old_root)` - 复制页表
- ✅ `nexus_create_vspace_root_node()` - 创建nexus根节点
- ❌ **Nexus树复制** - 缺失

### 缺失：Nexus树复制API
```c
// core/kernel/mm/nexus.c - 可能需要添加
error_t copy_nexus_tree(
    const struct nexus_node* src_root,
    struct nexus_node* dst_root,
    VS_Common* dst_vs
);
```

**功能需求**：
- 遍历父进程的nexus树
- 复制每个nexus节点到子进程
- **关键**：增加物理页引用计数（COW基础）
- 更新nexus节点的vspace指针

**为什么在core**：
- Nexus是core的内存管理核心
- Linux层不应该直接操作nexus内部结构
- 任何OS都需要物理页引用计数

### 临时方案
如果nexus树复制太复杂，可以先用简化版：
```c
// 临时方案：子进程创建空nexus树
// 后续通过页故障逐步复制nexus节点
// 或者第一次访问时复制整个nexus树
```

## 3. Fork恢复执行机制

### 缺失：从fork点恢复执行
```c
// core/kernel/task/thread.c - 需要添加
void resume_after_fork(Thread_Base* child_thread);
```

**功能需求**：
- 设置子线程从用户空间恢复执行
- **关键**：从fork系统调用返回点恢复，不是程序入口点
- 处理父子进程的不同返回值

**技术难点**：
- ELF加载：`arch_return_via_trap_frame` + 零化 `trap_frame`（填 rcx/ELR）- 从程序入口开始
- Fork恢复：需要从`sys_fork`返回点继续执行
- 可能需要特殊的上下文设置技巧

## 4. 当前实现差距

### Syscall路径
```
用户空间: fork() syscall
    ↓
linux_layer: sys_fork()
    ↓ 创建child task + vspace
    ↓ ❌ copy_thread() - 缺失
    ↓ ❌ 设置child执行上下文 - 缺失
    ↓
core: 调度器运行child
    ↓ ❌ child恢复执行 - 缺失
```

### 现状
- **父进程**：返回child PID ✅
- **子进程**：不会运行 ❌

## 5. 实现优先级建议

### 高优先级（fork能工作）
1. **`copy_thread()`** - 创建子线程
2. **架构上下文复制** - 复制寄存器状态
3. **执行恢复机制** - 让子进程从fork点恢复

### 中优先级（完善功能）
4. **Nexus树复制** - 支持munmap等操作
5. **物理页引用计数** - COW基础

### 低优先级（优化）
6. **性能优化** - 减少fork开销
7. **完整COW** - 减少内存使用

## 6. Core变更总结

### 需要添加的core API

```c
// core/kernel/task/tcb.c
Thread_Base* copy_thread(Thread_Base* parent, Tcb_Base* child_task, u64 ret_val);

// core/arch/*/task/tcb_arch.h  
void arch_copy_task_context(const Arch_Task_Context* src, Arch_Task_Context* dst, u64 ret_val);

// core/kernel/mm/nexus.c (可选，或用简化版)
error_t copy_nexus_tree(const struct nexus_node* src, struct nexus_node* dst, VS_Common* dst_vs);

// core/kernel/mm/vmm.c
VS_Common* copy_vspace(VS_Common* parent_vs);  // 替代linux_copy_vspace
```

### 设计原则
1. **通用性**：API不依赖Linux语义
2. **分层清晰**：core提供通用机制，linux_layer处理Linux特定逻辑
3. **SMP安全**：考虑多核场景
4. **向后兼容**：不破坏现有core功能
