# Linux Layer Core依赖重新分析（基于name_index机制）

## 发现：core已有name_index通用注册机制

**位置**：`core/kernel/registry/name_index.c`
**功能**：基于字符串名称的通用索引，支持注册、查找、注销
**特性**：
- 哈希表实现（开放寻址法）
- MCS锁保护
- 支持refcount回调（hold/drop）
- 支持注册/注销回调
- Token缓存机制避免重复查找

**现有用途**：IPC port注册（`global_port_table`）

## 重新分析：linux_layer对core的依赖

### 1. 进程查找（wait/waitpid需要）

#### ❌ 之前错误分析
```
错误：需要core添加find_task_by_pid() API
错误：需要core添加proc_registry
```

#### ✅ 正确方案

**方案A：使用现有的name_index机制（推荐）**
```c
// linux_layer/proc/proc_registry.c - 使用core的name_index
static name_index_t pid_index;

static const char* task_get_name(void* value) {
    Tcb_Base* task = (Tcb_Base*)value;
    // 将PID转换为字符串名称
    static char name_buf[16];
    snprintf(name_buf, sizeof(name_buf), "%d", task->pid);
    return name_buf;
}

void proc_registry_init(void) {
    name_index_init(&pid_index, NULL, 16, NULL,
                    task_get_name, NULL, NULL, NULL, NULL);
}

error_t register_process(Tcb_Base* task) {
    u64 row_idx;
    return name_index_register(&pid_index, task, &row_idx);
}

Tcb_Base* find_task_by_pid(pid_t pid) {
    char name_buf[16];
    snprintf(name_buf, sizeof(name_buf), "%d", pid);
    return (Tcb_Base*)name_index_lookup(&pid_index, name_buf, NULL);
}
```

**方案B：遍历Task_Manager->sched_task_list（简单但O(n)）**
```c
// core/kernel/task/task_manager.c - Task_Manager有sched_task_list
struct list_entry sched_task_list;  // 所有task的链表

// linux_layer/proc/proc_find.c
Tcb_Base* find_task_by_pid_slow(pid_t pid) {
    Task_Manager* tm = percpu(core_tm);
    struct list_entry* pos;

    lock_cas(&tm->sched_lock);
    list_for_each(pos, &tm->sched_task_list) {
        Tcb_Base* task = container_of(pos, Tcb_Base, sched_task_list);
        if (task->pid == pid) {
            unlock_cas(&tm->sched_lock);
            return task;
        }
    }
    unlock_cas(&tm->sched_lock);
    return NULL;
}
```

**结论**：
- **推荐方案A**：使用name_index，O(1)查找，适合频繁调用
- **方案B备用**：简单实现，O(n)性能，适合原型验证
- 两种方案都不需要core修改

---

### 2. 线程遍历（exit_group需要）

#### ❌ 之前错误分析
```
错误：需要core添加iterate_task_threads() API
```

#### ✅ 正确方案
直接遍历Tcb_Base的thread_head_node链表：

```c
// linux_layer/signal/signal.c - exit_group实现
void sys_exit_group(i64 exit_code) {
    Tcb_Base* task = get_cpu_current_task();

    // 直接遍历task的所有线程
    struct list_entry* pos;
    list_for_each(pos, &task->thread_head_node) {
        Thread_Base* thread = container_of(pos, Thread_Base, thread_list_node);
        // 杀死每个线程
        thread_set_flags(thread, THREAD_FLAG_EXIT_REQUESTED);
    }

    // 最后杀死当前线程
    sys_exit(exit_code);
}
```

**结论**：不需要core API，直接访问Tcb_Base->thread_head_node

---

### 3. Fork真正需要的core支撑

### ✅ 高优先级（fork必需）

#### 3.1 线程复制API
**问题**：创建子线程，复制父线程的寄存器状态
**需要core支撑**：

```c
// core/kernel/task/tcb.c - 需要添加
Thread_Base* copy_thread_for_fork(
    Thread_Base* parent_thread,
    Tcb_Base* child_task,
    u64 child_return_value  // 0 for child
);
```

**功能需求**：
- 分配子线程的内核栈
- 复制父线程的Arch_Task_Context
- **关键**：修改子线程的返回值寄存器（rax=0）
- 设置子线程为ready状态
- **关键**：设置子线程从fork恢复点执行，不是程序入口

**为什么在core**：
- 通用OS功能，任何OS都需要进程复制
- 需要架构特定的寄存器操作
- RendezvOS原生服务也可能需要

---

#### 3.2 架构上下文复制API
**问题**：复制寄存器状态，设置不同返回值
**需要core支撑**：

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

---

#### 3.3 Fork执行恢复机制
**问题**：子进程从fork点恢复执行，不是程序入口
**需要core支撑**：

```c
// core/kernel/task/thread.c - 需要添加
void resume_after_fork(Thread_Base* child_thread);
```

**技术难点**：
- ELF加载：`arch_return_via_trap_frame` + 零化 `trap_frame`（填 rcx/ELR）- 从程序入口开始
- Fork恢复：需要从`sys_fork`返回点继续执行
- 可能需要特殊的上下文设置技巧

**为什么在core**：
- 涉及底层上下文切换和用户空间恢复
- 需要架构特定的实现

---

### 🟡 中优先级（完善功能）

#### 3.4 Nexus树复制（munmap正确性）
**问题**：当前fork不复制nexus树，munmap可能影响父子进程
**需要core支撑**：

```c
// core/kernel/mm/nexus.c - 可能需要添加
error_t copy_nexus_tree(
    const struct nexus_node* src_root,
    struct nexus_node* dst_root,
    VS_Common* dst_vs
);
```

**临时方案**：
- 子进程创建空nexus树
- 通过页故障逐步复制nexus节点
- 或者第一次访问时复制整个nexus树

---

#### 3.5 页故障COW处理
**问题**：当前page fault handler直接崩溃
**需要core支撑**：

```c
// core/kernel/mm/vmm.c - 需要添加
error_t handle_cow_fault(VS_Common* vs, vaddr fault_addr, struct trap_frame* tf);

// core/include/rendezvos/mm/vmm.h
typedef error_t (*cow_fault_handler_t)(VS_Common* vs, vaddr addr, struct trap_frame* tf);
void register_cow_fault_handler(cow_fault_handler_t handler);
```

---

### 🟢 低优先级（高级功能）

#### 3.6 信号机制
**问题**：完全缺失
**需要core支撑**：
- 通用的信号发送和传递机制
- 集成到调度器
- 架构特定的信号处理

---

## 总结：修正后的core依赖

### 🔴 必须core支撑（fork无法工作）
1. **`copy_thread_for_fork()`** - 创建子线程并复制上下文
2. **`arch_copy_task_context()`** - 复制寄存器状态并设置返回值
3. **执行恢复机制** - 让子进程从fork点恢复执行

### 🟡 可以在linux_layer实现（使用现有core API）
1. **PID注册表** - 使用name_index机制
2. **进程查找** - 使用name_index_lookup
3. **线程遍历** - 直接访问Tcb_Base->thread_head_node
4. **exit_group** - 遍历线程并逐个杀死

### 🟢 未来增强
1. **Nexus树复制** - 完善munmap语义
2. **COW处理** - 减少内存使用
3. **信号机制** - 完整的Linux信号支持

---

## 设计原则修正

### 之前的错误
- 过度设计core API
- 忽略了现有的name_index机制
- 错误地将Linux特定概念放到core

### 修正后的原则
1. **最大化利用现有core机制**：name_index、list_head、refcount
2. **最小化core变更**：只添加真正通用的、架构特定的功能
3. **Linux特定逻辑留在linux_layer**：进程组、信号、文件描述符等
4. **清晰分层**：core提供原语，linux_layer实现Linux语义

---

## 下一步工作

1. **短期**：实现copy_thread_for_fork()和arch_copy_task_context()
2. **中期**：使用name_index实现proc_registry
3. **长期**：完善nexus树复制和COW
