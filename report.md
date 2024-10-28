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

### 代码实现

添加系统调用

```c
// kernel/vm.c
uint64 sys_mmap(void) {
  uint64 addr, len, offset;
  int prot, flags, fd;

  argaddr(0, &addr);
  if (addr != 0) addr = 0;  // 😝
  argaddr(5, &offset);
  if (offset == 0) offset = 0;
  argaddr(1, &len);  // args
  argint(2, &prot);
  argint(3, &flags);
  argint(4, &fd);

  struct file *f;
  if (fd < 0 || fd >= NOFILE || (f = myproc()->ofile[fd]) == 0) return -1;

  return (uint64)mmap(len, prot, flags, f);
}

uint64 sys_munmap(void) {
  uint64 addr, len;
  argaddr(0, &addr);
  argaddr(1, &len);
  return munmap((void *)addr, len);
}
```

添加 mmap / munmap.

mmap 的设计思路: 如果当前进程没有分配 vma, 那么就分配一个页面作为 vma.
流程: 分配一个起始地址 (这个起始地址可以到 MAXVA, 因为我们 pagetable 只是记录的用户态的地址空间).
在 vma 中找到一个空白的 slot, 用于存放在这个 mmap. 然后我们在 pagetable 中写上相应的 mmap 记录.
需要注意资源回收.

```c
// kernel/vm.c
void *mmap(size_t len, int prot, int flags, struct file *f) {
  if (!f->readable && (prot & PROT_READ) && (flags & MAP_SHARED)) {
    printf("%s:%d\n", __FILE__, __LINE__);
    return (void *)-1;
  }

  if (!f->writable && (prot & PROT_WRITE) && (flags & MAP_SHARED)) {
    printf("%s:%d\n", __FILE__, __LINE__);
    return (void *)-1;
  }

  struct proc *p = myproc();
  if (p->vma == 0) {
    p->vma = (vm_area_t *)kalloc();
    if (p->vma == 0) {
      printf("%s:%d\n", __FILE__, __LINE__);
      return (void *)-1;
    }
    memset(p->vma, 0, PGSIZE);
  }

  // select a start address
  uint64 start_addr = VMA_BEGIN;
  for (int i = 0; i < VMA_LENGTH; i++) {
    if (p->vma[i].valid == 1) {
      if (start_addr < p->vma[i].vma_end) {
        start_addr = p->vma[i].vma_end;
      }
    }
  }
  start_addr = PGROUNDUP(start_addr);  // 向上找一个页
  if (!(VMA_BEGIN <= start_addr && start_addr < MAXVA)) {
    printf("%s:%d\n", __FILE__, __LINE__);
    return (void *)-1;
  }
  // find a free slot
  int i, found = 0;
  for (i = 0; i < VMA_LENGTH; i++) {
    if (p->vma[i].valid == 0) {
      p->vma[i].valid = 1;
      p->vma[i].read = !!(prot & PROT_READ);
      p->vma[i].write = !!(prot & PROT_WRITE);
      p->vma[i].execute = 0;
      p->vma[i].private = !!(flags & MAP_PRIVATE);
      p->vma[i].vma_start = start_addr;
      p->vma[i].vma_end = start_addr + len;
      p->vma[i].file = f;
      acquire(&ftable.lock);
      if (f->ref < 1) panic("%s:%d", __FILE__, __LINE__);
      f->ref++;
      release(&ftable.lock);
      found = 1;
      break;
    }
  }

  if (!found) return (void *)-1;

  p->vma[i].vma_origin = start_addr;

  uint64 last = start_addr;
  while (last < start_addr + len) {
    pte_t *pte = walk(p->pagetable, last, 1);  //
    if (pte == 0) {                            // reroll
      p->vma[i].vma_start = 0;
      p->vma[i].vma_end = 0;
      p->vma[i]._mode_value = 0;
      p->vma[i].file = 0;
      acquire(&ftable.lock);
      if (f->ref < 1) panic("%s:%d", __FILE__, __LINE__);
      f->ref--;
      release(&ftable.lock);
      printf("%s:%d\n", __FILE__, __LINE__);
      return (void *)-1;
    }
    uint64 pte_flags = PTE_U;  // 没有 PTE_V, 要引发 page fault
    if (prot & PROT_READ) pte_flags |= PTE_R;
    if (prot & PROT_WRITE) pte_flags |= PTE_W;
    *pte = pte_flags;
    last += PGSIZE;
  }

  return (void *)start_addr;
}
```

munmap 的流程: 找到对应的 vma, munmap 的时候, 需要应付: munmap 中间挖洞的情况.
找到 addr 对应的 vma, 修改其中的字段, 并删除 pagetable 中对应的记录.

```c
// kernel/vm.c
int munmap(void *addr, size_t len) {
  uint64 va = (uint64)addr;
  struct proc *p = myproc();

  int i, found = 0;
  for (i = 0; i < VMA_LENGTH; i++) {
    if (p->vma[i].vma_start <= va && va < p->vma[i].vma_end && p->vma[i].valid) {
      found = 1;
      break;
    }
  }

  if (found == 0) {  // not found
    printf("%s:%d\n", __FILE__, __LINE__);
    return -1;
  }

  if (p->vma[i].vma_end < va + len) {  // end out of range
    printf("%s:%d\n", __FILE__, __LINE__);
    return -1;
  }

  if (p->vma[i].vma_end != va + len && p->vma[i].vma_start != va) {  // 出现了中间挖洞的情况
    printf("%s:%d\n", __FILE__, __LINE__);
    return -1;
  }

  if (p->vma[i].vma_start != va) {  // 去尾
    p->vma[i].vma_end = va;
  } else {                                // 掐头
    if (p->vma[i].vma_end == va + len) {  // delete
      p->vma[i].vma_start = 0;
      p->vma[i].vma_end = 0;
      p->vma[i]._mode_value = 0;

      // free pages
      struct file *f = p->vma[i].file;
      uint64 va0 = PGROUNDDOWN(va);
      uint64 vaend1 = PGROUNDUP(va + len);
      if (!p->vma[i].private) {  // shared mmap
        f->off = va0 - p->vma[i].vma_origin;
        filewrite(f, va0, vaend1 - va0);
      }
      // file
      if (!f) {
        printf("%s:%d\n", __FILE__, __LINE__);
        return -1;
      }
      acquire(&ftable.lock);
      if (f->ref < 1) panic("%s:%d", __FILE__, __LINE__);
      f->ref--;
      release(&ftable.lock);
      p->vma[i].file = 0;
      uvmunmap_f(p->pagetable, va0, (vaend1 - va0) / PGSIZE);
    } else {
      p->vma[i].vma_start = va + len;

      // free pages
      uint64 va1 = PGROUNDUP(va);
      uint64 vaend0 = PGROUNDDOWN(va + len);
      if (!p->vma[i].private) {  // shared mmap
        struct file *f = p->vma[i].file;
        f->off = va1 - p->vma[i].vma_origin;
        filewrite(f, va1, vaend0 - va1);
      }
      uvmunmap_f(p->pagetable, va1, (vaend0 - va1) / PGSIZE);
    }
  }

  return 0;
}
```
