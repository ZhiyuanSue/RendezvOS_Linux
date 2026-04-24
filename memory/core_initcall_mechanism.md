---
name: core_initcall_mechanism
description: RendezvOS core/ initcall机制和x86_64参数传递bug
type: reference
---

# RendezvOS Core Initcall机制和x86_64汇编Bug

## Initcall机制

**位置**: `core/include/rendezvos/task/initcall.h`

**重要**: RendezvOS**不使用**C标准的`__attribute__((constructor))`，而是使用自己的initcall机制。

**正确用法**:
```c
#include <rendezvos/task/initcall.h>

static void my_init_function(void) {
    // 初始化代码
}

// 使用DEFINE_INIT宏注册
DEFINE_INIT(my_init_function);
```

**机制原理**:
- `DEFINE_INIT`将函数指针放入特殊的`.init.call.3`段
- 内核启动时通过`do_init_call()`遍历这个段并调用所有注册的函数
- `_s_init`和`_e_init`标记段的开始和结束

## x86_64参数传递Bug

**位置**: `core/arch/x86_64/task/arch_run_thread.S`

**问题**: 原始代码用`%rdi`加载第一个参数，但`%rdi`原本保存para指针，导致后续参数访问错误。

**错误代码**:
```asm
mov (%rdi), %rax      # 加载函数指针
mov 8(%rdi), %rdi     # ❌ 覆盖了para指针！
...
call *%rax
```

**正确代码**:
```asm
mov %rdi, %r10        # ✓ 先保存para指针
mov (%r10), %rax      # 从r10加载
mov 8(%r10), %rdi
...
call *%rax
```

**症状**: 第四个及之后的参数全部变成0或垃圾值。

**为什么aarch64没问题**: aarch64使用`x0`作为para指针，只从`[x0+offset]`读取数据，不会覆盖原指针。

## 链接顺序和符号解析

**发现**: 即使符号存在于kernel.elf中，运行时仍可能为NULL，这是链接器符号解析问题。

**症状**:
- `nm`显示符号存在且地址正确
- initcall中打印地址正确
- 但在其他编译单元中该符号是NULL或0

**可能原因**:
1. 链接顺序问题
2. 符号可见性问题
3. 编译单元之间的符号解析错误

