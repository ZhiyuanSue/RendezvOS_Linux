# Core/ 修改方案：修复elf_init传递机制

## 🎯 问题诊断

### 发现的问题
**linux_elf_init_handler从未被调用**，导致brk初始化失败。

### 根本原因
core/的参数传递机制存在设计不匹配：
- `gen_task_from_elf`传递7个参数给`create_thread`
- 但`run_elf_program`只接受4个参数
- 第7个参数`elf_init`超出了函数接收能力

### 参数映射分析
```
create_thread参数 → run_elf_program接收
参数1: run_elf_program (函数指针) ✓
参数2: append_thread_info_len           ✗ (被忽略)
参数3: 4 (nr_parameter)                ✗ (被忽略)
参数4: elf_start                       ✓
参数5: elf_end                         ✓
参数6: elf_task->vs                    ✓
参数7: elf_init                        ❌ (超出接收范围)
```

## 🔧 解决方案

### 推荐方案：修改run_elf_program签名

**修改位置**: `core/kernel/task/thread_loader.c`

**修改内容**:
```c
// 修改前
error_t run_elf_program(vaddr elf_start, vaddr elf_end, VS_Common *vs,
                        elf_init_handler_t elf_init)

// 修改后  
error_t run_elf_program(vaddr elf_start, vaddr elf_end, VS_Common *vs,
                        elf_init_handler_t elf_init,
                        size_t append_tcb_info_len,  // 新增
                        size_t append_thread_info_len) // 新增
```

**修改理由**:
1. **最小化修改** - 只修改一个函数签名
2. **向后兼容** - 现有调用可以继续工作
3. **类型安全** - 明确传递append大小
4. **架构一致** - 符合现有参数传递模式

### 实现细节

#### 1. 函数签名修改
```c
error_t run_elf_program(vaddr elf_start, vaddr elf_end, VS_Common *vs,
                        elf_init_handler_t elf_init,
                        size_t append_tcb_info_len,
                        size_t append_thread_info_len)
{
    // 现有实现...
    
    // 在调用elf_init时，确保正确传递append信息
    if (elf_init) {
        elf_load_info_t info = {
            .elf_start = elf_start,
            .elf_end = elf_end,
            .entry_addr = entry_addr,
            .max_load_end = ROUND_UP(max_load_end, PAGE_SIZE),
            .user_sp = user_sp,
            .phnum = elf_header->e_phnum,
            .phentsize = elf_header->e_phentsize,
            .append_tcb_info_len = append_tcb_info_len,    // 新增
            .append_thread_info_len = append_thread_info_len, // 新增
        };
        elf_init(&elf_thread->ctx, &info);
    }
}
```

#### 2. create_thread调用修改
```c
Thread_Base *elf_thread = create_thread((void *)run_elf_program,
                                        append_tcb_info_len,      // 传递append_tcb_info_len
                                        6,                         // 6个参数 (增加1个)
                                        elf_start,
                                        elf_end,
                                        elf_task->vs,
                                        elf_init);
```

### 📋 影响评估

#### 修改范围
- **文件**: `core/kernel/task/thread_loader.c`
- **函数**: `run_elf_program`, `gen_task_from_elf`

#### 不影响的功能
- 现有ELF加载逻辑保持不变
- 其他线程创建保持不变
- 向后兼容

#### 需要更新的调用
- 无其他调用需要修改

### ✅ 验证计划

1. **编译测试** - 确保修改后能编译通过
2. **功能测试** - 验证现有功能不受影响  
3. **brk测试** - 确认brk正确初始化
4. **回归测试** - 确保其他测试通过

### 🔍 风险评估

**技术风险**: 低
- 修改局限于一个函数
- 不改变核心逻辑
- 向后兼容

**兼容性风险**: 极低
- 现有调用可以继续工作
- 新参数有合理默认值

## 🎯 预期效果

1. **brk正确初始化** - max_load_end正确传递
2. **linux_elf_init_handler被调用** - ELF加载钩子工作
3. **Phase 2完成度提升** - 解决已知问题

---

**你对此修改方案的评估？** 
- 设计是否合理？
- 是否符合core/的质量要求？
- 还需要考虑其他方面？