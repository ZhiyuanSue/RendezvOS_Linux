# Linux Layer 代码结构

## 当前结构 (2026-04-17)

```
linux_layer/
├── proc/           # 进程管理相关
├── init/           # Linux兼容层初始化
├── io/             # I/O操作 (write等)
├── tests/          # 测试和测试运行器
├── loader/         # ELF加载和进程创建
├── mm/             # 内存管理syscall
└── syscall/        # syscall分发和线程管理
```

## 分层原则

### 架构无关层
**位置**: `linux_layer/` 大部分代码
**职责**: Linux语义实现，直接调用core接口

### 架构相关层
**位置**: `linux_layer/arch/` (未来需要建立)
**职责**: 架构特定的syscall处理、trap处理、寄存器访问

## 当前实现状态

### 内存管理 (mm/)
- ✅ `sys_mmap.c` - mmap匿名映射
- ✅ `sys_munmap.c` - 内存释放
- ✅ `sys_mprotect.c` - 权限管理
- ✅ `sys_mremap.c` - 内存重映射
- ✅ `sys_brk.c` - 堆管理
- ✅ `linux_mm_flags.h` - 内存标志转换
- ✅ `linux_page_fault_irq.c` - 页故障处理 (基础设施)

### 进程管理 (proc/)
- ✅ `sys_proc.c` - getpid等进程信息syscall

### syscall分发 (syscall/)
- ✅ `syscall_entry.c` - 主要syscall分发表
- ✅ `thread_syscall.c` - exit/exit_group等线程管理

### I/O (io/)
- ✅ `sys_write.c` - write shim (fd 1/2)

### 测试 (tests/)
- ✅ `user_test_runner.c` - 用户态测试运行器
- ✅ `linux_compat_tests.c` - 内核态测试
- ✅ `elf_read_test.c` - ELF读取测试

### 加载器 (loader/)
- ✅ `linux_elf_init.c` - ELF初始化和brk设置

## 架构改进建议

### 1. 架构相关代码分离
```
linux_layer/
├── arch/
│   ├── x86_64/
│   │   ├── syscall_arch.h        # x86_64特定syscall定义
│   │   └── trap_arch.c           # x86_64特定trap处理
│   ├── aarch64/
│   └── riscv64/
└── (现有架构无关代码)
```

### 2. Syscall实现规范化
```
linux_layer/
├── syscall/
│   ├── syscall_entry.c           # 主分发器 (架构无关)
│   ├── syscall_table.c           # syscall表生成
│   └── arch/
│       └── (架构特定的syscall处理)
└── syscalls/
    ├── mm/
    │   ├── mmap.c
    │   ├── munmap.c
    │   └── mprotect.c
    ├── process/
    │   ├── exit.c
    │   ├── fork.c (未来)
    │   └── exec.c (未来)
    └── fs/
        ├── open.c (未来)
        └── read.c (未来)
```

### 3. 头文件组织
```
include/linux_compat/
├── syscall.h              # 当前syscall声明
├── proc_compat.h          # 进程append结构
├── elf_init.h             # ELF初始化
├── test_runner.h          # 测试运行器接口
└── errno.h                # Linux错误码
```

## 命名约定

### 文件命名
- **syscall实现**: `sys_<syscall_name>.c`
- **架构相关**: `<arch>_<feature>.c`
- **头文件**: 功能描述性命名

### 函数命名
- **syscall入口**: `sys_<syscall_name>`
- **内部辅助**: `<module>_<operation>`
- **架构相关**: `arch_<arch>_<operation>`

## 实现指导原则

1. **架构无关优先**: 默认在`linux_layer/`实现架构无关代码
2. **最小化架构代码**: 只有必须的架构差异放在`arch/`下
3. **统一接口**: 架构相关代码通过统一接口暴露给架构无关层
4. **测试驱动**: 每个功能都应该有对应的测试

## 下一阶段规划

### Phase 3: COW和进程复制
- [ ] 完善页故障处理
- [ ] 实现fork syscall
- [ ] COW机制

### Phase 4: 程序执行
- [ ] execve syscall
- [ ] 程序加载器增强

### 架构支持扩展
- [ ] aarch64支持
- [ ] riscv64支持
