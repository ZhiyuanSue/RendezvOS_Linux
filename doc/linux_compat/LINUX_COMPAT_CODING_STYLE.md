# Linux兼容层编码规范

> **文档目的**: 确保Linux兼容层代码风格一致性和可维护性  
> **适用范围**: `linux_layer/` 目录下所有代码  
> **强制级别**: 必须遵守 (MUST) / 建议遵守 (SHOULD)

---

## 📁 文件命名规范

### 规则
所有文件名必须使用小写字母和下划线，使用功能前缀。

### 命名模式
```
linux_layer/
├── proc/           → sys_*.c (syscall实现)
├── signal/         → signal_*.c (信号功能)  
├── mm/             → linux_mm_*.c (内存管理)
├── syscall/        → syscall_*.c (syscall框架)
├── loader/         → linux_loader_*.c (加载器)
├── io/             → linux_io_*.c (IO操作)
└── init/           → linux_init_*.c (初始化)
```

### 示例
```c
✅ 正确：
linux_layer/proc/sys_kill.c
linux_layer/signal/signal_queue.c
linux_layer/mm/linux_mm_radix.c

❌ 错误：
linux_layer/proc/kill.c
linux_layer/signal/queue.c
linux_layer/mm/radix.c
```

---

## 🔧 函数命名规范

### 规则
函数名必须使用小写字母和下划线，使用前缀表明功能和作用域。

### 命名模式
```c
// Syscall实现函数
sys_<syscall_name>(参数...)

// 内部辅助函数 (static)
<module>_<action>_helper(参数...)
<module>_<action>_internal(参数...)

// 信号相关函数
signal_<action>(参数...)

// 内存管理函数  
linux_mm_<action>(参数...)

// 导出的API函数
linux_<module>_<action>(参数...)
```

### 示例
```c
✅ 正确：
i64 sys_kill(i64 pid, i64 sig)
static void signal_queue_helper(...)
static bool signal_validate_internal(...)
void linux_mm_radix_insert(...)

❌ 错误：
i64 kill(i64 pid, i64 sig)           // 缺少sys_前缀
void queue_signal(...)               // 缺少signal_前缀  
void insert(...)                     // 过于通用
void linux_do_kill_stuff(...)       // 冗长且不清晰
```

---

## 📊 日志规范

### 规则
使用统一的日志标签格式，标签必须全大写，使用方括号。

### 标签体系
```c
[SYSCALL]  - syscall相关 (sys_*.c文件)
[SIGNAL]   - 信号相关 (signal_*.c文件)
[MM]       - 内存管理 (linux_mm_*.c文件)
[PROC]     - 进程管理 (proc/目录)
[IO]       - IO操作 (io/目录)
[LOADER]   - 加载器 (loader/目录)
[TEST]     - 测试相关 (tests/目录)
```

### 日志级别使用
```c
pr_debug("[MODULE] 详细调试信息\n");    // 调试信息
pr_info("[MODULE] 正常操作信息\n");     // 重要操作
pr_warn("[MODULE] 警告信息\n");         // 警告但不严重
pr_error("[MODULE] 错误信息\n");        // 错误信息
```

### 示例
```c
✅ 正确：
pr_debug("[SIGNAL] Queueing signal %d\n", sig);
pr_info("[PROC] Process %d created\n", pid);
pr_error("[MM] Page fault at addr %p\n", addr);

❌ 错误：
pr_debug("[Signal] Queueing signal %d\n", sig);  // 小写标签
pr_info("Process %d created\n", pid);              // 缺少标签
pr_debug("signal %d queued\n", sig);                // 格式不统一
```

---

## 💬 注释规范

### 文件头注释
每个文件必须包含文件头注释，说明文件用途和所属Phase。

```c
/*
 * 文件功能简述
 *
 * 详细描述：文件的主要功能、实现方式、相关设计文档。
 *
 * Phase: Phase X - 功能名称
 * 
 * 相关文档:
 * - doc/linux_compat/相关文档.md
 *
 * 实现注意事项:
 * - 关键设计决策
 * - 已知限制
 * - 依赖关系
 */
```

### 函数注释
重要函数必须包含注释，说明功能、参数、返回值。

```c
/*
 * 函数功能简述
 *
 * @param param1: 参数1说明
 * @param param2: 参数2说明  
 * @return: 返回值说明
 *
 * 实现说明：简要实现逻辑
 */
```

### 行内注释
```c
/* 简短说明 */  或  // 简短说明

✅ 正确：
/* Check if signal is pending */
// Validate user pointer

❌ 错误：
/*检查信号是否pending*/  // 中文混用
// check signal          // 缺少大写
```

---

## 📦 结构体规范

### 命名
结构体使用`_<module>_t`后缀，typedef名称使用`_<module>_append_t`。

### 字段排列
字段按功能分组，相关字段放在一起，添加注释说明。

```c
✅ 正确：
typedef struct linux_thread_append {
    /* Thread management */
    u64 clear_tid;
    u64 test_cookie;
    
    /* Signal state (per-thread) */
    sigset_t blocked_signals;
    sigset_t pending_signals;
    stack_t alt_stack;
    vaddr saved_main_sp;
} linux_thread_append_t;

❌ 错误：
typedef struct linux_thread_append {
    u64 clear_tid;
    sigset_t blocked_signals;
    u64 test_cookie;
    stack_t alt_stack;
    // ... 混乱排列
} linux_thread_append_t;
```

---

## 🏗️ 架构相关代码规范

### 原则
架构特定代码必须与主逻辑分离，使用内联函数或独立函数。

### 规范
```c
✅ 正确：
/* 架构特定的内联函数 */
static inline void arch_restore_stack(vaddr sp)
{
#if defined(_X86_64_)
    percpu(user_rsp_scratch) = sp;
#elif defined(_AARCH64_)
    /* TODO: AArch64 implementation */
#endif
}

/* 主逻辑函数 */
void restore_stack(void)
{
    // ... 逻辑代码 ...
    arch_restore_stack(sp);
    // ... 更多逻辑 ...
}

❌ 错误：
void restore_stack(void)
{
    // ... 逻辑代码 ...
#if defined(_X86_64_)
    percpu(user_rsp_scratch) = sp;
#elif defined(_AARCH64_)
    /* TODO */
#endif
    // ... 更多逻辑 ...
}
```

---

## 🔐 安全规范

### 用户空间访问
**必须**通过 `linux_mm_store_to_user` / `linux_mm_load_from_user`（内部用 `have_mapped` + `map_handler_copy_data_range`）访问用户 VA；小结构体也可仿照 `sys_sigaltstack.c` 单页 `map_handler_map_slot`。**禁止**再写一套 slot 循环，也**禁止**把用户指针强转为内核指针。

```c
✅ 正确（syscall 写用户缓冲区）：
error_t e = linux_mm_store_to_user(vs, user_va, &kernel_buf, sizeof(kernel_buf));
if (e != REND_SUCCESS) {
    return -LINUX_EFAULT;
}

✅ 正确（map_handler 读/写一页内字段）：
ppn_t user_ppn = have_mapped(vs, user_vpn, NULL, NULL, handler);
if (user_ppn <= 0) {
    return -LINUX_EFAULT;
}
vaddr kva = map_handler_map_slot(handler, 0, user_ppn);
// ... 仅访问该页映射窗口 ...
map_handler_unmap_slot(handler, 0);

❌ 错误：
memcpy(user_ptr, &act, sizeof(act));           /* user_ptr 未映射 */
stack_t *u = (stack_t *)user_vaddr;
u->ss_flags = flags;                           /* 可能 #PF / 越界 */
```

**信号 / `rt_sig*` 约定**：
- `rt_sigaction`、`rt_sigprocmask` 的 `oldact`/`set`/`oldset` 一律走 `linux_mm_*_user`。
- 用户 handler 的 `sigframe`/`ucontext`：Phase 2B 用内核侧 `linux_signal_restore_t` + `rt_sigreturn`；**未**在用户栈写完整 Linux `rt_sigframe`（见 `SIGNAL_IMPLEMENTATION_STATUS.md`）。

### 信号模块命名（与 `CODE_STYLE_ISSUES.md` 对齐）
| 层级 | 前缀 | 示例 |
|------|------|------|
| syscall | `sys_` | `sys_kill` |
| 导出 API | `linux_` | `linux_queue_signal` |
| 文件内 static | `<module>_<action>_helper` | `signal_select_thread_helper` |

---

## 🎯 代码组织原则

### 单一职责
每个文件/函数只负责一个明确的功能。

### 模块化
相关功能组织在一起，跨模块依赖最小化。

### 可测试性
代码结构便于单元测试和集成测试。

---

## 📏 代码格式

### 缩进
使用TAB缩进，或4个空格（与core保持一致）。

### 行长度
建议不超过100字符，最长不超过120字符。

### 大括号
```c
✅ 推荐：
if (condition) {
    do_something();
}

❌ 不推荐：
if (condition)
{
    do_something();
}
```

---

## ✅ 代码审查清单

提交代码前检查：
- [ ] 文件名符合命名规范
- [ ] 函数名符合命名规范  
- [ ] 日志使用统一标签格式
- [ ] 包含必要的注释
- [ ] 用户空间访问使用安全机制
- [ ] 架构相关代码已分离
- [ ] 两个架构都能编译通过
- [ ] 相关文档已更新

---

*版本: 1.0*  
*创建: 2025-01-17*  
*维护者: Claude Sonnet*  
*相关文档: ARCHITECTURE.md · 历史 style 问题见 archive/CODE_STYLE_ISSUES.md*