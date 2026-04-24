# vspace复制API设计方案

## 需求分析

Linux的fork()需要复制父进程的地址空间到子进程。当前core/缺少vspace复制API，linux_layer无法高效实现这一功能。

## API设计

### 函数签名
```c
error_t copy_vspace(VS_Common* src_vs, 
                    VS_Common* dst_vs,
                    struct map_handler* src_handler,
                    struct map_handler* dst_handler,
                    u64 flags);
```

### 参数说明
- `src_vs`: 源vspace (父进程)
- `dst_vs`: 目标vspace (子进程，已创建但为空)
- `src_handler`: 源map_handler
- `dst_handler`: 目标map_handler  
- `flags`: 复制标志
  - `COPY_VSPACE_USER_ONLY`: 只复制用户空间映射
  - `COPY_VSPACE_KERNEL_SHARED`: 内核空间共享引用
  - `COPY_VSPACE_COW_MARK`: 标记为COW (为future COW做准备)

### 返回值
- `REND_SUCCESS`: 复制成功
- `-REND_ERR_INVALID`: 参数无效
- `-REND_ERR_NOMEM`: 内存不足

## 实现策略

### 阶段1: 简化版本 (当前实现)
**目标**: 直接复制所有物理页和映射
**步骤**:
1. 遍历源vspace的页表
2. 对每个有效映射：
   - 分配新的物理页
   - 复制内容
   - 在目标vspace中创建映射
3. 复制nexus树结构

### 阶段2: 优化版本 (future)
**目标**: COW支持
**优化**:
- 页表共享
- COW标记
- 按需分裂

## 实现位置

**文件**: `core/kernel/mm/vmm.c`
**函数**: `copy_vspace()`

## 依赖关系

### 需要的现有API
- `map()` - 创建页表映射
- `unmap()` - 删除映射  
- `VS_Common` 结构访问
- `nexus` 遍历和复制

### 新增API (如果需要)
- `遍历vspace映射` - 可能需要新增页表遍历函数

## 测试计划

### 单元测试
1. **基本复制** - 简单vspace复制验证
2. **权限保持** - 确保权限正确复制
3. **大vspace** - 测试大地址空间的复制

### 集成测试
1. **fork功能** - 使用copy_vspace实现fork
2. **内存隔离** - 父子进程内存正确隔离
3. **性能测试** - 复制性能验证

## 风险评估

### 技术风险
- **页表遍历复杂度** - 页表结构可能复杂
- **原子性要求** - 复制过程中的一致性
- **性能影响** - 复制开销可能较大

### 缓解措施
- **分步实现** - 先实现简单版本
- **充分测试** - 确保各种场景正确
- **性能监控** - 添加性能统计

## 修改范围

### 修改文件
1. `core/include/rendezvos/mm/vmm.h` - 添加API声明
2. `core/kernel/mm/vmm.c` - 实现copy_vspace函数

### 不修改
- 现有API保持不变
- 不影响现有功能
- 向后兼容

## 实施时间估算

- **设计**: 2小时
- **实现**: 4-6小时  
- **测试**: 2-3小时
- **总计**: 8-11小时

## 审查要点

请审查以下方面：
1. **API设计合理性** - 接口是否满足需求
2. **实现方案可行性** - 技术路径是否正确
3. **风险评估** - 风险识别是否充分
4. **修改范围** - 是否影响现有功能
