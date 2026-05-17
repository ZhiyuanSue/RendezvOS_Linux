# Linux信号机制可行性分析

## 信号功能分类分析

### 1. 默认信号动作 (SIG_DFL) - 可行性分析

#### 1.1 进程终止类信号 (Term)
**信号**: SIGTERM(15), SIGINT(2), SIGHUP(1), SIGPIPE(13), etc.

**实现难度**: 🟢 **容易** - 可复用已有功能
```c
// 当前已有功能
sys_exit(exit_code)  // ✅ 已实现
```

**实现方案**:
```c
if (sig == SIGTERM || sig == SIGINT || ...) {
    // 直接复用已有的exit机制
    sys_exit(128 + sig);  // Linux标准: 信号终止 = 128 + 信号号
    return;
}
```

**可行性**: ✅ **立即可实现**

#### 1.2 Core Dump类信号 (Core)
**信号**: SIGQUIT(3), SIGABRT(6), SIGSEGV(11), SIGBUS(7), SIGFPE(8), SIGILL(4)

**实现难度**: 🔴 **困难** - 需要新功能
```c
// 需要实现的功能
1. ELF Core文件生成      // ❌ 未实现
2. 进程内存快照         // ❌ 未实现  
3. 寄存器状态保存       // ⚠️  部分可用 (trap_frame)
```

**临时方案**:
```c
// 先实现为普通终止，等core dump功能完成后再添加
if (sig == SIGSEGV || sig == SIGBUS || ...) {
    pr_warn("[SIGNAL] Core dump not implemented, terminating process\n");
    sys_exit(128 + sig);  // 暂时当作普通终止
    return;
}
```

**可行性**: ⚠️ **暂时简化实现，等待core dump功能**

#### 1.3 忽略类信号 (Ign)
**信号**: SIGCHLD(17), SIGCONT(18), SIGWINCH(28), etc.

**实现难度**: 🟢 **容易**
```c
// 这些信号的默认动作就是忽略
if (sig == SIGCHLD || sig == SIGCONT || ...) {
    sigdelset(&pending_signals, sig);
    return;  // 直接丢弃，什么都不做
}
```

**可行性**: ✅ **立即可实现**

#### 1.4 进程停止/继续类 (Stop/Cont)
**信号**: SIGSTOP(19), SIGTSTP(20), SIGTTIN(21), SIGTTOU(22), SIGCONT(18)

**实现难度**: 🟡 **中等** - 需要调度器接口
```c
// 可能的已有功能
task->state = TASK_STOPPED;   // ❓ 需要确认调度器是否支持
task->state = TASK_RUNNING;   // ❓ 需要确认调度器是否支持
```

**需要调查**:
- core/调度器是否支持TASK_STOPPED状态？
- 是否有API可以设置进程状态？

**临时方案**:
```c
// 如果调度器不支持，先记录日志
if (sig == SIGSTOP) {
    pr_warn("[SIGNAL] Process stop not fully implemented\n");
    // 暂时不做处理，或设置一个标志位
}
```

**可行性**: ⚠️ **需要调查调度器接口，可能需要简化实现**

---

### 2. 信号掩码语义 - 可行性分析

#### 2.1 基本掩码操作 (rt_sigprocmask)
**实现难度**: 🟢 **容易** - 已有部分实现

**当前状态**: 
```c
// 已有基本的位图操作
sigset_t blocked_signals;  // ✅ 已存在
```

**需要完善**:
```c
// 1. 信号处理期间自动屏蔽当前信号
void linux_deliver_pending_signals() {
    // 保存原mask
    sigset_t old_mask = thread_append->blocked_signals;
    
    // 添加当前信号到blocked
    sigaddset(&thread_append->blocked_signals, sig);
    
    // 投递信号...
    
    // 信号处理完成后恢复mask ❓ 需要机制来恢复
}
```

**可行性**: ✅ **可以实现，但需要设计恢复机制**

#### 2.2 信号掩码的保存和恢复
**实现难度**: 🟡 **中等**

**需要实现**:
```c
// 在signal frame中保存mask
struct signal_frame {
    sigset_t old_mask;     // 信号处理前的mask
    // ... 其他内容
};

// 在sigreturn时恢复
void sigreturn() {
    current_thread->blocked_signals = frame->old_mask;
}
```

**可行性**: ✅ **可以实现，需要完整的signal frame支持**

---

### 3. 实时信号队列 - 可行性分析

#### 3.1 队列数据结构
**实现难度**: 🟡 **中等**

**需要实现**:
```c
// 实时信号队列
typedef struct sigqueue {
    struct sigqueue *next;
    siginfo_t info;        // 信号详细信息
} sigqueue_t;

// 每个线程一个队列
linux_thread_append_t {
    sigqueue_t *sigqueue_list[64];  // 每个信号一个队列
    // ...
}
```

**可行性**: ✅ **可以实现，但需要内存管理**

#### 3.2 siginfo_t 结构
**实现难度**: 🟢 **容易**

**需要定义**:
```c
typedef struct {
    int      si_signo;     // 信号号
    int      si_errno;     // errno值
    int      si_code;      // 信号代码
    pid_t    si_pid;       // 发送进程PID
    uid_t    si_uid;       // 发送进程UID
    union {
        void *si_ptr;      // 通用指针
        int  si_int;       // 通用整数
    } si_value;
} siginfo_t;
```

**可行性**: ✅ **立即可实现**

---

### 4. 系统调用中断处理 - 可行性分析

#### 4.1 EINTR 返回
**实现难度**: 🟢 **容易**

**需要修改**:
```c
// 在syscall_entry.c中
void syscall(struct trap_frame *syscall_ctx) {
    // ... 执行syscall ...
    
    // 检查是否有信号pending
    if (signal_pending && syscall_needs_restart(syscall_id)) {
        ret = -LINUX_EINTR;  // 返回中断错误
    }
    
    linux_deliver_pending_signals(syscall_ctx);
}
```

**可行性**: ✅ **立即可实现**

#### 4.2 SA_RESTART 自动重启
**实现难度**: 🟡 **中等**

**需要实现**:
```c
// 在sigaction中设置标志
struct sigaction_t {
    void (*handler)(int);
    unsigned long flags;    // 包含SA_RESTART等
    sigset_t mask;
};

// 在syscall中检查
if (signal_pending && (action->flags & SA_RESTART)) {
    // 重启syscall而不是返回EINTR
    restart_syscall(syscall_ctx);
}
```

**可行性**: ⚠️ **需要设计syscall重启机制**

---

### 5. 特定信号的复杂处理

#### 5.1 SIGCHLD (子进程状态变化)
**实现难度**: 🟡 **中等** - 可复用wait4机制

**已有功能**:
```c
sys_wait4()       // ✅ 已实现
进程退出通知      // ✅ clean_server已实现
```

**实现方案**:
```c
// 在进程退出时，向父进程发送SIGCHLD
process_exit() {
    // ... 已有的清理逻辑 ...
    
    // 向父进程发送SIGCHLD
    linux_queue_signal(parent_pid, SIGCHLD, &siginfo);
}
```

**可行性**: ✅ **可以实现，复用已有wait4机制**

#### 5.2 SIGSEGV (段错误) - 真实场景
**实现难度**: 🟡 **中等**

**已有功能**:
```c
page_fault_handler()     // ✅ 已实现
访问权限检查            // ✅ 已实现
```

**实现方案**:
```c
// 在page_fault处理中
page_fault_handler(vaddr, error_code) {
    if (is_user_address && !is_accessible) {
        // 检查是否有SIGSEGV handler
        if (has_user_handler(SIGSEGV)) {
            // 构建siginfo，投递信号
            siginfo_t info = {
                .si_signo = SIGSEGV,
                .si_code = SEGV_MAPERR,
                .si_addr = (void *)vaddr
            };
            linux_queue_signal_with_info(current_pid, SIGSEGV, &info);
        } else {
            // 默认动作：终止进程
            sys_exit(128 + SIGSEGV);
        }
    }
}
```

**可行性**: ✅ **可以实现，增强已有page fault处理**

---

## 优先级排序

### 🔴 高优先级 (立即可实现)
1. **完善SIG_DFL处理** - 复用sys_exit
2. **基本信号掩码语义** - 完善blocked操作
3. **SIGCHLD支持** - 复用wait4机制
4. **EINTR处理** - 修改syscall返回

### 🟡 中优先级 (需要设计)
1. **实时信号队列** - 需要数据结构
2. **完整signal frame** - 需要内存管理
3. **sigreturn实现** - 需要上下文恢复
4. **SA_RESTART** - 需要syscall重启机制

### ⚠️ 低优先级 (需要其他功能)
1. **Core dump** - 需要ELF core功能
2. **进程停止/继续** - 需要调度器支持
3. **信号栈** - 需要内存管理支持

---

## 可复用的已有功能清单

### ✅ 进程管理
- `sys_exit()` - 进程终止
- `sys_kill()` - 信号发送
- `sys_wait4()` - 子进程等待
- 进程注册表 (proc_registry)
- 线程管理 (thread_append)

### ✅ 内存管理  
- `user_sp` - 用户栈指针
- `percpu(user_rsp_scratch)` - 用户栈scratch空间
- 页错误处理 (page_fault_handler)
- 地址空间检查

### ✅ 架构特定
- `trap_frame` - 陷阱帧
- `sysretq/eret` - 系统调用返回
- 寄存器操作接口

### ❌ 缺失功能
- ELF core dump
- 进程停止/继续状态
- 信号栈 (altstack)
- 系统调用重启机制

---

## 实现路线图

### Phase 1: 完善基础 (立即开始)
1. ✅ **SIG_DFL处理** - Term类信号复用exit
2. ✅ **忽略类信号** - SIGCHLD等默认忽略
3. ✅ **基本掩码** - 完善blocked操作
4. ✅ **EINTR** - syscall中断返回

### Phase 2: 增强功能 (中期)
1. ⚠️ **SIGCHLD真实场景** - 集成wait4
2. ⚠️ **SIGSEGV真实场景** - 集成page fault
3. ⚠️ **实时信号队列** - 基础数据结构
4. ⚠️ **完整signal frame** - 包含siginfo

### Phase 3: 高级功能 (长期)
1. ❌ **Core dump** - 等待ELF core功能
2. ❌ **进程控制** - 等待调度器支持
3. ❌ **信号栈** - 等待内存管理增强

---

## 总结

### 可以立即实现的 (不需要其他功能)
- ✅ SIG_DFL的Term类处理 (复用exit)
- ✅ 忽略类信号处理
- ✅ 基本信号掩码操作
- ✅ EINTR系统调用中断
- ✅ SIGCHLD基础支持 (复用wait4)

### 需要设计的 (中等难度)
- ⚠️ 实时信号队列机制
- ⚠️ 完整signal frame结构
- ⚠️ sigreturn系统调用
- ⚠️ SA_RESTART自动重启

### 需要其他功能的 (暂时无法实现)
- ❌ Core dump (需要ELF core)
- ❌ 进程停止/继续 (需要调度器支持)
- ❌ 信号栈 (需要内存管理增强)

**关键发现**: 很多信号功能可以复用已有的进程管理、内存管理、错误处理机制，不需要从零实现！