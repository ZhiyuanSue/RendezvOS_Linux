# 信号栈实现方案：复用core现有机制

## 🎯 发现的core现有机制

### 1. percpu(user_rsp_scratch) - 核心机制
**位置**: `core/arch/x86_64/task/arch_thread.c:11`
```c
DEFINE_PER_CPU(vaddr, user_rsp_scratch);  // 已有perCPU用户栈指针
```

**我已在用**: 在信号投递中使用它来设置用户SP
```c
// signal_deliver.c:111
percpu(user_rsp_scratch) = user_sp;  // 我已经在用！
```

### 2. 线程切换的栈管理模式
**位置**: `core/arch/x86_64/task/arch_thread.c:21-22`
```c
// core已经有成熟的栈指针保存/恢复模式
old_context->user_rsp = percpu(user_rsp_scratch);  // 保存
percpu(user_rsp_scratch) = new_context->user_rsp;   // 恢复
```

### 3. 数据结构已预留
**位置**: `include/linux_compat/proc_compat.h:47`
```c
typedef struct {
    sigset_t blocked_signals;
    sigset_t pending_signals;
    stack_t *alt_stack;         // 🔥 已预留！
} linux_thread_append_t;
```

## 🔧 复用策略：模仿线程切换模式

### 核心思路
信号栈切换 = **临时的线程切换**（只切换栈，不切换任务）

```c
// 模仿core的线程切换逻辑
void switch_to_signal_stack() {
    // 1. 保存主栈指针（模仿线程切换）
    saved_main_sp = percpu(user_rsp_scratch);
    
    // 2. 切换到信号栈（模仿设置新线程的栈）
    percpu(user_rsp_scratch) = alt_stack->ss_sp + alt_stack->ss_size;
    
    // 3. 标记在信号栈上
    alt_stack->ss_flags |= SS_ONSTACK;
}

void restore_from_signal_stack() {
    // 1. 恢复主栈指针（模仿线程切换恢复）
    percpu(user_rsp_scratch) = saved_main_sp;
    
    // 2. 清除信号栈标记
    alt_stack->ss_flags &= ~SS_ONSTACK;
}
```

## 📋 具体实现方案

### 阶段1：sigaltstack syscall实现
**复用**: 用户空间内存管理
```c
// linux_layer/syscall/sys_sigaltstack.c
i64 sys_sigaltstack(stack_t *ss, stack_t *old_ss)
{
    Thread_Base *thread = get_cpu_current_thread();
    linux_thread_append_t *thread_append = linux_thread_append(thread);
    
    if (old_ss) {
        // 返回当前信号栈设置
        if (thread_append->alt_stack) {
            memcpy(old_ss, thread_append->alt_stack, sizeof(stack_t));
        } else {
            old_ss->ss_sp = NULL;
            old_ss->ss_flags = SS_DISABLE;
            old_ss->ss_size = 0;
        }
    }
    
    if (ss) {
        // 设置新的信号栈
        if (ss->ss_flags == SS_DISABLE) {
            // 禁用信号栈
            thread_append->alt_stack = NULL;
        } else {
            // 启用信号栈
            // 验证用户提供的栈地址
            if (!user_ptr_ok((u64)ss->ss_sp, ss->ss_size)) {
                return -LINUX_EFAULT;
            }
            
            // 分配内核管理结构
            stack_t *new_stack = kalloc(sizeof(stack_t));
            new_stack->ss_sp = ss->ss_sp;
            new_stack->ss_size = ss->ss_size;
            new_stack->ss_flags = ss->ss_flags;
            
            thread_append->alt_stack = new_stack;
        }
    }
    
    return 0;
}
```

### 阶段2：信号投递时的栈切换
**复用**: percpu(user_rsp_scratch) + 线程切换模式
```c
// linux_layer/signal/signal_deliver.c
void linux_deliver_pending_signals(struct trap_frame *tf)
{
    // ... 选择信号 ...
    
    sigaction_t *disp = &proc_append->signal_dispositions[sig - 1];
    
    // 🔥 关键：检查是否需要信号栈
    bool use_signal_stack = false;
    vaddr saved_main_sp = 0;
    
    if ((disp->flags & SA_ONSTACK) && thread_append->alt_stack) {
        stack_t *alt_stack = thread_append->alt_stack;
        
        // 检查是否已经在信号栈上（防止递归）
        if (!(alt_stack->ss_flags & SS_ONSTACK)) {
            pr_debug("[SIGNAL] Switching to signal stack: %p\n", alt_stack->ss_sp);
            
            // 🔥 复用core的栈指针保存模式
            saved_main_sp = percpu(user_rsp_scratch);
            
            // 切换到信号栈（模仿线程切换）
            vaddr signal_sp = (vaddr)alt_stack->ss_sp + alt_stack->ss_size - 8;
            signal_sp = signal_sp & ~0xF;  // 对齐
            percpu(user_rsp_scratch) = signal_sp;
            
            // 标记在信号栈上
            alt_stack->ss_flags |= SS_ONSTACK;
            use_signal_stack = true;
        }
    }
    
    // ... 正常的信号投递 ...
    tf->rcx = (u64)disp->handler;
    tf->rax = (u64)sig;
    
    // 如果使用了信号栈，需要在sigreturn时恢复
    if (use_signal_stack) {
        // TODO: 在sigreturn中恢复saved_main_sp
        // 或者让信号处理器自动返回时恢复
    }
}
```

### 阶段3：sigreturn实现（自动恢复）
**复用**: 函数返回时的栈恢复机制
```c
// 用户空间的sigreturn包装器（自动生成）
void sigreturn_wrapper(void) {
    // 内核会在这个包装器中调用sigreturn syscall
    // 返回时会自动恢复主栈指针
}

// 或者更简单的方法：在信号处理完成后自动恢复
void linux_deliver_pending_signals(struct trap_frame *tf)
{
    // ... 信号投递 ...
    
    // 🔥 关键：信号处理器返回后自动恢复主栈
    // 因为sysretq会恢复到之前的栈指针
    // 我们只需要确保信号处理器不破坏saved_main_sp
    
    // 将saved_main_sp保存在thread_append中
    if (use_signal_stack) {
        thread_append->saved_signal_sp = saved_main_sp;
    }
}

// 在某个合适的时机恢复（比如下一个syscall时）
void restore_main_stack_if_needed(void)
{
    Thread_Base *thread = get_cpu_current_thread();
    linux_thread_append_t *thread_append = linux_thread_append(thread);
    
    if (thread_append->alt_stack && 
        (thread_append->alt_stack->ss_flags & SS_ONSTACK)) {
        
        // 恢复主栈指针
        percpu(user_rsp_scratch) = thread_append->saved_signal_sp;
        thread_append->alt_stack->ss_flags &= ~SS_ONSTACK;
        
        pr_debug("[SIGNAL] Restored main stack pointer\n");
    }
}
```

## 🎯 实现的复用模式总结

| core机制 | 复用方式 | 信号栈中的应用 |
|---------|---------|----------------|
| **percpu(user_rsp_scratch)** | 直接使用 | 保存主栈指针，设置信号栈指针 |
| **线程切换栈保存模式** | 模仿 | 保存/恢复逻辑模式相同 |
| **linux_thread_append_t** | 扩展 | 添加管理字段 |
| **用户空间内存管理** | 直接使用 | 用户mmap/malloc分配栈 |

## 📋 实现步骤

### Step 1: 实现sys_sigaltstack（容易）
- 验证用户提供的栈地址
- 在thread_append中保存信号栈信息
- 复用用户空间内存管理

### Step 2: 实现信号投递时的栈切换（中等）
- 检查SA_ONSTACK标志
- 保存主栈指针到percpu(user_rsp_scratch)
- 切换到信号栈
- 设置SS_ONSTACK标志

### Step 3: 实现恢复机制（需要设计）
- 选择恢复时机（sigreturn或下一个syscall）
- 恢复主栈指针
- 清除SS_ONSTACK标志

## 💡 关键优势

1. **不需要新的硬件机制**：完全复用现有的percpu和栈管理
2. **模式成熟**：模仿线程切换，core已经验证过
3. **集成简单**：与现有信号投递机制无缝集成
4. **多架构友好**：aarch64也有类似的机制

## 🚀 实现优先级

这个实现相对容易，可以优先级较高：
- **难度**: 🟡 中等（比SIG_DFL稍难，比实时信号队列简单）
- **复用度**: 🟢 高（大量复用core机制）
- **用户价值**: 🟡 中等（提高鲁棒性，但非核心功能）

## 总结

信号栈实现可以**完美复用core现有机制**：
1. **percpu(user_rsp_scratch)** - 直接用于栈指针管理
2. **线程切换模式** - 模仿栈指针保存/恢复
3. **linux_thread_append_t** - 已预留alt_stack字段

这是一个**架构友好**的实现，不需要改变core，完全在linux_layer中完成！