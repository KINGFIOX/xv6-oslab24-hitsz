# mmap

## 内容分析

mit's xv6 实验指导书中有提示

1. 添加系统调用
2. mmap 在页表中, 对应的 page 采用 lazy allocate
3. pcb 中添加 vma(virtual memory area) 数据结构
4. 实现 mmap, munmap
5. 注意 fork, exit 的处理, 资源回收等

### 设计

主要是添加一个数据结构, 这个数据结构是参考的 csapp

```c
typedef struct VIRTUAL_MEMORY_AREA_STRUCT {
  uint64 vma_start;
  uint64 vma_end;
  uint64 vma_origin;  // 用于 munmap 后计算 offset 的
  union {
    uint64 _mode_value;
    struct {
      uint64 read : 1;
      uint64 write : 1;
      uint64 execute : 1;  // 这个对于 mmap 来说是无效的
      uint64 private : 1;
      uint64 valid : 1;  // 是否有效
    };
  };
  struct file *file;
} vm_area_t;
```

其实按道理来说, 对于 .stack, .text, .data 来说, 实际上应该都加入到 vma 中.
但是对于这个实验来说, 实际上那个只用记录 mmap 的 vma 就行了.
