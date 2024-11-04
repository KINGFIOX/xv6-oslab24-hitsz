# lazy

这个实验虽然拆分成了三个部分, 但是实际上只做了一件事情: lazy 的 sbrk, 并且保证其正确性

## Eliminate allocation from sbrk()

从 user/ulib.c 的 malloc 可以知道: malloc 内部实际上是调用了系统调用 sbrk,
sbrk 内部可以看到: 修改了 `myproc()->sz` 同时调用 `uvmalloc(p->pagetable, sz, sz + n)` 分配空间.

直接注释掉 uvmalloc 的部分就行了

```c
int growproc(int n) {
  uint sz;
  struct proc *p = myproc();

  sz = p->sz;
  if (n > 0) {
    sz = sz + n;  // 默认就让他成功吧
    // if ((sz = uvmalloc(p->pagetable, sz, sz + n)) == 0) {
    //   return -1;
    // }
  } else if (n < 0) {
    sz = uvmdealloc(p->pagetable, sz, sz + n);
  }
  p->sz = sz;
  return 0;
}
```

## Lazy allocation && Lazytests and Usertests

这个时候, 我们并没有分配, 因此: 只要访问了 sbrk 分配出来的内存, 就会发生 page fault,
load page fault -> scause=0xd, store page fault -> scause=0xf

那么我们就要到 usertrap 中, 判断: 是真的发生了 segmentation fault, 还是因为 lazy 导致的 page fault,
如果是 lazy 导致的 page fault, 那么我们这个时候按需分配即可

```c
void usertrap(void) {
  // ...
  } else if (r_scause() == 13 || r_scause() == 15) {
    uint64 va = r_stval();
    uint64 va0 = PGROUNDDOWN(va);
    if (va >= p->sz) {
      printf("%s:%d out of memory: pid=%d pname=%s\n", __FILE__, __LINE__, p->pid, p->name);
      printf("            sepc=%p stval=%p\n", r_sepc(), r_stval());
      p->killed = 1;
    } else {
      uint64 mem = (uint64)kalloc();
      if (!mem) {
        printf("%s:%d kalloc failed\n", p->name, p->pid);
        p->killed = 1;
      } else {
        memset((void *)mem, 0, PGSIZE);
        if (mappages(p->pagetable, va0, PGSIZE, mem, PTE_R | PTE_W | PTE_U) != 0) {
          kfree((void *)mem);
          p->killed = 1;
        }
      }
    }
  } else {
  // ...
}
```

这样做了以后, lazy 就基本上完成了, 我们需要修复一些 panic, 并要让资源正确的被回收, 以免发生资源泄露

### uvmunmap

```c
void uvmunmap(pagetable_t pagetable, uint64 va, uint64 npages, int do_free) {
  if ((va % PGSIZE) != 0) panic("uvmunmap: not aligned");

  for (uint64 a = va; a < va + npages * PGSIZE; a += PGSIZE) {
    pte_t *pte = walk(pagetable, a, 0);
    if (pte) {
      // if ((*pte & PTE_V) == 0) panic("uvmunmap: not mapped");
      // if (!(PTE_FLAGS(*pte) & PTE_R) && !(PTE_FLAGS(*pte) & PTE_W) && !(PTE_FLAGS(*pte) & PTE_X)) panic("uvmunmap: not a leaf");
      // 因为我这里是: 直接就不分配 pte 了
      if (do_free) {
        if (*pte & PTE_V) {
          uint64 pa = PTE2PA(*pte);
          kfree((void *)pa);
        }
      }
      *pte = 0;
    }
  }
}
```

去掉一些断言, `(*pte & PTE_V) == 0` 可能是因为 lazy 导致的: 在进程结束前, 一块内存依然没有用到

### uvmcopy

因为 lazy, 可能会触发一些断言, 导致 panic, 我们去掉这些断言. 并且: 只有 `*pte & PTE_V` 才会进行复制

```c
int uvmcopy(pagetable_t old, pagetable_t new, uint64 sz) {
  for (uint64 i = 0; i < sz; i += PGSIZE) {
    pte_t *pte = walk(old, i, 0);
    if (pte) {             // 只有 pte 存在
      if (*pte & PTE_V) {  // 并且 pte 有效
        uint64 pa = PTE2PA(*pte);
        uint flags = PTE_FLAGS(*pte);
        char *mem = kalloc();
        if (!mem) {
          uvmunmap(new, 0, i / PGSIZE, 1);
          return -1;
        }
        memmove(mem, (char *)pa, PGSIZE);  // 才会进行 copy
        if (mappages(new, i, PGSIZE, (uint64)mem, flags) != 0) {
          kfree(mem);
          uvmunmap(new, 0, i / PGSIZE, 1);
          return -1;
        }
      }
    }
  }
  return 0;
}
```

### copyin/copyinstr/copyout

这三个函数, 也会操作 一个 proc 的虚拟内存. 比方说 copyout 访问的 va 并没有映射, 那么这个时候需要建立映射, 并分配 page.
当然, 前提是: 访问的地址合法.

然后我观察到, 这三个函数都会访问一个 walkaddr 的函数. 这个 walkaddr 是: va -> pa 的,
所以, 我只需要修改 walkaddr 这个函数就行了: 如果访问的内存合法, 并且并没有实际分配 page, 那么就分配 page

```c
uint64 walkaddr(pagetable_t pagetable, uint64 va) {
  if (va >= MAXVA) return 0;

  pte_t *pte = walk(pagetable, va, 1);
  if (!pte) return 0;
  uint64 mem = 0;
  struct proc *p = myproc();
  if (!(*pte & PTE_V)) {
    if (va >= p->sz) {
      return 0;
    }
    mem = (uint64)kalloc();
    if (!mem) {
      return 0;
    } else {
      uint64 va0 = PGROUNDDOWN(va);
      if (mappages(pagetable, va0, PGSIZE, mem, PTE_R | PTE_W | PTE_U) != 0) {
        kfree((void *)mem);
        return 0;
      }
    }
  }
  if ((*pte & PTE_U) == 0) {
    if (mem) {
      kfree((void *)mem);
    }
    return 0;
  }
  uint64 pa = PTE2PA(*pte);
  return pa;
}
```

## make grade

![](image/grade.png)

BUGS: 只能在 qemu 5.x 版本上运行, 原因出在: mret 并没有正确被跳转.
