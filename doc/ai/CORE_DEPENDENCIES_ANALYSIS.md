# Linux Layer中需要Core支撑的功能分析

## 1. 进程生命周期管理

### ✅ 当前正确位置
**`sys_getpid()`** in `linux_layer/proc/sys_proc.c`
- 只是简单访问`core->task->pid`
- 正确：这是Linux系统调用wrapper

**`sys_exit()`** in `linux_layer/syscall/thread_syscall.c`  
- 使用core的`thread_set_status()` + `schedule()`
- 正确：使用core的调度API

### ❌ 缺失功能

#### **`exit_group` - 杀死整个进程组**
```c
// 当前实现：linux_layer/syscall/thread_syscall.c
void sys_exit_group(i64 exit_code) {
    sys_exit(exit_code);  // ❌ 只杀当前线程
}
```

**需要core支撑**：
```c
// core/kernel/task/tcb.c - 需要添加
error_t kill_task_group(Tcb_Base* task_group_leader, i64 exit_code);
void kill_all_threads_in_task(Tcb_Base* task);
```

**为什么需要core**：
- 需要遍历task的所有线程
- 需要SMP安全的线程终止机制
- 这是通用的进程组管理，不是Linux特定

#### **进程注册表（proc_registry）**
```c
// SYSCALLS.md提到：wait4/waitpid依赖proc_registry
// 当前：❌ 完全没有实现
```

**需要core支撑**：
```c
// core/kernel/task/proc_registry.c - 需要添加
struct proc_registry {
    pid_t parent_pid;
    struct list_entry children;  // 子进程列表
    struct list_entry siblings;  // 兄弟进程列表
    exit_status_t exit_status;
};

error_t register_process(Tcb_Base* parent, Tcb_Base* child);
error_t unregister_process(Tcb_Base* task);
Tcb_Base* find_task_by_pid(pid_t pid);
```

**为什么需要core**：
- 进程树管理是通用OS功能
- wait/waitpid需要查询进程状态
- 僵尸进程回收需要全局进程注册

## 2. 线程管理

### ✅ 当前正确位置  
**`sys_exit()`** - 使用core的线程调度API ✅
**`thread_syscall.c`** - Linux线程系统调用 ✅

### ❌ 缺失功能（已在fork分析中）

## 3. 内存管理

### ✅ 当前正确位置
所有内存管理syscall都正确使用core的vspace/nexus API：
- `sys_mmap()` → `get_free_page()`, `map()` ✅
- `sys_munmap()` → `free_pages()`, `unmap()` ✅  
- `sys_mprotect()` → 修改页表权限 ✅
- `sys_mremap()` → `get_free_page()`, `free_pages()` ✅

### ❌ 缺失功能

#### **页故障COW处理**
```c
// linux_layer/mm/linux_page_fault_irq.c
static void linux_trap_pf_handler(struct trap_frame *tf)
{
    // TODO: resolve lazy-anon / COW; for now fall back to default handler.
    arch_unknown_trap_handler(tf);  // ❌ 崩溃！
}
```

**需要core支撑**：
```c
// core/kernel/mm/vmm.c - 需要添加
error_t handle_cow_fault(VS_Common* vs, vaddr fault_addr, struct trap_frame* tf);

// core/include/rendezvos/mm/vmm.h  
// 注册COW处理回调
typedef error_t (*cow_fault_handler_t)(VS_Common* vs, vaddr addr, struct trap_frame* tf);
void register_cow_fault_handler(cow_fault_handler_t handler);
```

**为什么需要core**：
- 页故障是底层异常处理
- 需要直接操作页表
- 任何OS都需要COW或类似的机制

## 4. 信号机制

### ❌ 完全缺失

**需要core支撑**：
```c
// core/kernel/signal/signal.c - 需要添加
struct signal_state {
    sigset_t pending;
    sigset_t blocked;
    struct sigaction actions[_NSIG];
};

error_t send_signal(Tcb_Base* target, int signo);
error_t deliver_signal(Tcb_Base* task, int signo);
void handle_signal_return(void);
```

**为什么需要core**：
- 信号是通用的进程间通信机制
- 需要集成到调度器
- 需要架构特定的信号处理

## 5. 进程间同步

### ❌ 缺失wait/waitpid

**需要core支撑**：
- proc_registry（见上面）
- 僵尸进程状态管理
- 进程状态变更通知机制

## 6. 文件系统（潜在需要）

### 当前状态：未实现
Phase 6需要文件系统，可能需要：
- 文件描述符管理
- inode缓存
- 路径解析

**可能需要core支撑**：
- 通用的文件描述符管理
- VFS层抽象

## 7. 总结：按优先级分类

### 🔴 高优先级（阻塞基础功能）
1. **exit_group实现** - 杀进程组需要
2. **proc_registry** - wait/waitpid需要  
3. **页故障COW处理** - mprotect+fork需要
4. **copy_thread()** - fork需要

### 🟡 中优先级（完善功能）
5. **信号机制** - 很多程序需要
6. **Nexus树复制** - munmap正确性

### 🟢 低优先级（高级功能）
7. **完整信号支持**
8. **文件系统**

## 8. 架构建议

### 短期策略
- 继续实现fork框架，记录缺失的core功能
- 优先实现高优先级项的core API
- 保持linux_layer只做Linux特定逻辑

### 长期策略  
- 在core建立完整的进程/线程管理
- linux_layer只做Linux系统调用wrapper
- 通过清晰的API边界分离关注点
