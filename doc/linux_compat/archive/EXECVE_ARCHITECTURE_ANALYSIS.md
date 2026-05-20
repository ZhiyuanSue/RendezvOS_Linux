# execve系统调用实现架构分析

## 问题分析

execve是Linux兼容层最复杂的系统调用之一，主要涉及：

1. **地址空间替换**：完全替换当前进程的内存映射
2. **栈帧构造**：在用户栈上设置argc/argv/envp参数
3. **寄存器状态重置**：设置新的用户态执行上下文
4. **文件路径解析**：支持ELF文件定位和加载
5. **进程状态保持**：PID保持不变，但进程镜像完全替换

## Core现有机制分析

### 已有基础设施

#### 1. ELF加载器 (`core/kernel/task/thread_loader.c`)

**关键函数**：
```c
// 运行ELF程序（在当前线程上下文）
error_t run_elf_program(vaddr elf_start, vaddr elf_end, VSpace *vs,
                        elf_init_handler_t elf_init)

// 从ELF创建新任务
error_t gen_task_from_elf(Thread_Base **elf_thread_ptr,
                          size_t append_tcb_info_len,
                          size_t append_thread_info_len,
                          vaddr elf_start, vaddr elf_end,
                          elf_init_handler_t elf_init)
```

**功能**：
- ✅ 解析ELF头部和程序头
- ✅ 加载PT_LOAD段到虚拟内存
- ✅ 设置页面权限（R/W/X）
- ✅ 处理BSS段清零
- ✅ 生成用户栈
- ✅ 设置初始寄存器状态

#### 2. 地址空间管理

**已有功能**：
- ✅ VSpace创建和注册
- ✅ 页表映射和权限管理
- ✅ radix tree虚拟内存管理
- ✅ 地址空间引用计数

#### 3. 栈帧操作 (`arch_return_to_user`)

**已有功能**：
- ✅ trap_frame构造
- ✅ 用户态寄存器设置
- ✅ 系统调用返回路径

### Core缺少的功能

#### 1. 地址空间替换机制
```c
// 需要添加的core API
error_t replace_vspace(Thread_Base *thread, VSpace *new_vs);
```

**功能需求**：
- 安全地替换线程的地址空间
- 保持线程结构不变，只替换vs指针
- 处理引用计数和旧地址空间清理

#### 2. 用户栈参数构造
```c
// 需要添加的core API
error_t setup_user_stack_params(VSpace *vs, vaddr *user_sp,
                                int argc, char **argv,
                                int envc, char **envp);
```

**功能需求**：
- 在用户栈上构造Linux标准的参数布局
- 处理字符串指针数组
- 确保栈对齐和边界检查

#### 3. 当前进程ELF替换
```c
// 需要添加的core API
error_t exec_current_process(vaddr elf_start, vaddr elf_end,
                             const char *filename,
                             int argc, char **argv,
                             int envc, char **envp);
```

## 架构分层设计

### Core层提供：进程替换原语

#### 新增Core API

```c
/* core/kernel/task/exec.h */

/**
 * @brief 替换当前进程的地址空间和执行镜像
 *
 * 这是execve的核心实现，提供安全的进程替换机制。
 * linux层通过这个API实现Linux execve语义。
 *
 * @param elf_start ELF文件在内存中的起始地址
 * @param elf_end ELF文件在内存中的结束地址
 * @param filename 可执行文件名（用于错误报告）
 * @param argc 参数个数
 * @param argv 参数数组（用户空间虚拟地址）
 * @param envc 环境变量个数
 * @param envp 环境变量数组（用户空间虚拟地址）
 *
 * @return 成功时不会返回（进程已替换），失败时返回错误码
 */
error_t exec_replace_current_process(vaddr elf_start, vaddr elf_end,
                                    const char *filename,
                                    int argc, char **argv,
                                    int envc, char **envp);
```

#### 实现要点

1. **地址空间替换**：
   ```c
   // 1. 创建新地址空间
   VSpace *new_vs = create_vspace(root_vspace.pmm);

   // 2. 在新地址空间中加载ELF
   // （复用run_elf_program的加载逻辑）

   // 3. 构造用户栈参数
   vaddr user_sp = setup_exec_stack(new_vs, argc, argv, envc, envp);

   // 4. 原子性替换当前线程的地址空间
   lock_cas(&current_thread->vs_lock);
   VSpace *old_vs = current_thread->vs;
   current_thread->vs = new_vs;
   unlock_cas(&current_thread->vs_lock);

   // 5. 清理旧地址空间
   ref_put(&old_vs->refcount, free_vspace_ref);
   ```

2. **用户栈参数构造**：
   ```c
   static vaddr setup_exec_stack(VSpace *vs,
                                int argc, char **argv,
                                int envc, char **envp)
   {
       vaddr stack_top = USER_SPACE_TOP;
       vaddr current_sp = stack_top;

       // 1. 保留栈空间
       current_sp -= USER_STACK_RESERVE;

       // 2. 对齐到16字节边界
       current_sp = ALIGN_DOWN(current_sp, 16);

       // 3. 构造Linux标准的栈布局
       // [envp strings][argv strings][NULL][envp pointers][NULL][argv pointers][argc][auxv]

       // ... 详细构造逻辑
   }
   ```

3. **安全性考虑**：
   - 原子性地址空间切换
   - 失败时完全回滚（不影响原进程）
   - 正确的引用计数管理
   - 栈溢出检查

### Linux层提供：Linux语义和接口

#### Linux层职责

1. **系统调用接口**：
   ```c
   i64 sys_execve(const char *filename, char **argv, char **envp);
   ```

2. **路径解析**：
   - 支持相对路径和绝对路径
   - PATH环境变量解析
   - shebang支持（后续）

3. **参数验证和复制**：
   - 从用户空间复制参数
   - 验证指针有效性
   - 构造内核参数缓冲区

4. **错误处理**：
   - Linux标准错误码转换
   - 部分执行失败处理

#### 实现框架

```c
/* linux_layer/proc/sys_exec.c */

i64 sys_execve(const char *user_filename, char **user_argv, char **user_envp)
{
    // 1. 验证和复制文件名
    char filename[MAX_PATH];
    error_t e = linux_mm_copy_string(filename, user_filename);
    if (e != REND_SUCCESS) {
        return -LINUX_EFAULT;
    }

    // 2. 验证和复制参数
    int argc;
    char **argv_kernel;
    e = copy_and_validate_args(user_argv, &argc, &argv_kernel);
    if (e != REND_SUCCESS) {
        return -LINUX_EFAULT;
    }

    // 3. 验证和复制环境变量
    int envc;
    char **envp_kernel;
    e = copy_and_validate_env(user_envp, &envc, &envp_kernel);
    if (e != REND_SUCCESS) {
        // 清理argv_kernel
        return -LINUX_EFAULT;
    }

    // 4. 定位并加载ELF文件
    vaddr elf_start, elf_end;
    e = locate_and_load_elf(filename, &elf_start, &elf_end);
    if (e != REND_SUCCESS) {
        // 清理参数
        return -LINUX_ENOEXEC;
    }

    // 5. 调用core的exec原语
    e = exec_replace_current_process(elf_start, elf_end, filename,
                                    argc, argv_kernel,
                                    envc, envp_kernel);

    // 如果成功，不会到达这里
    // 如果失败，清理并返回错误码
    return (e == REND_SUCCESS) ? 0 : -LINUX_EIO;
}
```

## 分层总结

### Core层新增功能

| 功能 | API | 复杂度 | 风险 |
|------|-----|--------|------|
| 地址空间替换 | `exec_replace_current_process` | ⭐⭐⭐⭐ | 高 - 直接操作进程状态 |
| 栈参数构造 | `setup_exec_stack` | ⭐⭐⭐ | 中 - 栈布局复杂性 |
| ELF加载复用 | 现有`run_elf_program` | ⭐⭐ | 低 - 已有基础 |

### Linux层新增功能

| 功能 | API | 复杂度 | 风险 |
|------|-----|--------|------|
| 系统调用接口 | `sys_execve` | ⭐⭐ | 低 - 标准包装 |
| 参数验证复制 | 本地实现 | ⭐⭐ | 低 - 标准操作 |
| 路径解析 | 本地实现 | ⭐⭐⭐ | 中 - 字符串处理 |

## 实现策略建议

### 阶段1：Core基础设施（优先）
1. 实现`exec_replace_current_process`基础框架
2. 实现简化的栈参数构造
3. 地址空间替换机制
4. 测试：简单的ELF替换

### 阶段2：Linux语义实现
1. 实现`sys_execve`系统调用
2. 参数验证和复制
3. 错误处理
4. 测试：基本execve功能

### 阶段3：高级特性
1. shebang支持
2. PATH环境变量
3. 更复杂的栈布局
4. 性能优化

## 风险评估

### 高风险操作（必须在Core实现）

1. **地址空间替换**：
   - 涉及页表切换
   - 需要原子性保证
   - 错误时必须完全回滚

2. **用户栈构造**：
   - 直接操作用户栈指针
   - 错误可能导致用户态崩溃
   - 需要严格的对齐和边界检查

3. **寄存器状态重置**：
   - 涉及架构特定的上下文切换
   - 错误可能导致内核态崩溃

### 中等风险操作（Linux层可以实现）

1. **参数验证**：
   - 用户空间指针验证
   - 标准的内存访问检查

2. **路径解析**：
   - 字符串处理
   - 文件系统访问

## 结论

### 核心发现

1. **Core必须提供关键原语**：
   - 地址空间替换机制
   - 用户栈参数构造
   - ELF加载流程的execve适配

2. **Linux层可以实现接口**：
   - 系统调用封装
   - 参数验证和复制
   - Linux特定语义

3. **安全性考虑**：
   - 高风险操作必须在Core实现
   - 提供原子性保证
   - 失败时完全回滚

### 建议实现顺序

1. **立即开始**：Core的`exec_replace_current_process`实现
2. **并行进行**：Linux层的参数验证和路径解析
3. **集成测试**：完整的execve系统调用

这个架构设计既保证了安全性，又保持了分层清晰的原则。