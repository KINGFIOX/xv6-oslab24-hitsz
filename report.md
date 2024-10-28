# cow

## Implement copy-on-write fork

### 内容分析

这个 copy on write, 实际上也是一种 lazy 的策略

依次分为几步:

1. 修改 uvmcopy, 对于原本有 PTE_W 权限的 pte, 那么注销掉他的 PTE_W, 并设置一个新的 flag, PTE_OW(original writable)
2. 发生了 write page fault, 那么检查他是否有 PTE_OW, 如果有 PTE_OW, 那么说明是 cow 的; 如果他没有 PTE_OW, 那么他确实是发生了 write page fault
3. 那么我们要怎么知道发生 copy 的时机呢? 我们可以跟踪每个 physical frame, 对每个 physical frame 进行引用计数. 如果 refcnt = 2, 又发生了 cow's write page fault, 那么 copy, 并减少引用计数; 如果 refcnt = 1, 发生了 cow's write page fault, 那么直接将该 physical frame PTE_OW clear && PTE_W set
4. 同时, 我们内核态下也会发生 copy-on-write, 实际是: copyout, 对于 copytout, 我们也是采用 3 中描述的策略

### 代码实现

kalloc, kfree 引用计数

```c
__attribute__((guarded_by(kmem.lock))) uint8 ref_cnt[(PHYSTOP - KERNBASE) / PGSIZE] = {0};
static inline uint64 pa2idx(void *pa) {
  ASSERT_TRUE(KERNBASE <= (uint64)pa && (uint64)pa < PHYSTOP);
  return ((uint64)pa - KERNBASE) / PGSIZE;
}
void inc_ref(void *pa) { ref_cnt[pa2idx(pa)]++; }
uint8 get_ref(void *pa) { return ref_cnt[pa2idx(pa)]; }

// kernel/kalloc.c
void *kalloc(void) {
  struct run *r;

  acquire(&kmem.lock);
  r = kmem.freelist;
  if (r) {
    kmem.freelist = r->next;
    ASSERT_FALSE(ref_cnt[pa2idx(r)] != 0);
    ref_cnt[pa2idx(r)] = 1;
  }
  release(&kmem.lock);

  // if (r) memset((char *)r, 5, PGSIZE);  // fill with junk
  return (void *)r;
}

void kfree(void *pa) {
  struct run *r;

  ASSERT_FALSE(((uint64)pa % PGSIZE) != 0 || (char *)pa < end || (uint64)pa >= PHYSTOP);

  // // Fill with junk to catch dangling refs.
  // memset(pa, 1, PGSIZE);

  r = (struct run *)pa;

  acquire(&kmem.lock);
  ASSERT_FALSE(ref_cnt[pa2idx(pa)] == 0);
  if (--ref_cnt[pa2idx(pa)] == 0) {
    r->next = kmem.freelist;
    kmem.freelist = r;
  }
  release(&kmem.lock);
}
```

注意一下: 我们的 kinit -> freerange -> kfree, 我们要在 kfree 前初始化 refcnt = 1

```c
void freerange(void *pa_start, void *pa_end) {
  char *p = (char *)PGROUNDUP((uint64)pa_start);
  for (; p + PGSIZE <= (char *)pa_end; p += PGSIZE) {
    ref_cnt[pa2idx(p)] = 1;
    kfree(p);
  }
}
```

uvmcopy 的时候, 我们并不实际分配. 只是增加对 physical frame 的 refcnt

```c
int uvmcopy(pagetable_t old, pagetable_t new, uint64 sz) {
  for (uint i = 0; i < sz; i += PGSIZE) {
    pte_t *pte;
    if ((pte = walk(old, i, 0)) == 0) panic("uvmcopy: pte should exist");
    ASSERT_TRUE((*pte & PTE_V) && "uvmcopy: page not present");
    uint64 pa = PTE2PA(*pte);
    uint flags = PTE_FLAGS(*pte);
    if (flags & PTE_W) {
      flags &= ~PTE_W;
      flags |= PTE_OW;
      *pte &= ~PTE_W;
      *pte |= PTE_OW;
    }
    if (mappages(new, i, PGSIZE, (uint64)pa, flags) != 0) {
      uvmunmap(new, 0, i / PGSIZE, 1);
      return -1;
    }
    inc_ref((void *)pa);
  }
  return 0;
}
```

write page fault 时, 如果有 PTE_OW, 那么就 cow; 如果没有 PTE_OW, 那么确实发生了 segmentation fault

```c
// kernel/trap.c
void usertrap(void) {
  // ...
  } else if (r_scause() == 0x0f) {
    // cow
    uint64 va = r_stval();
    ASSERT_TRUE(0 <= va && va < PLIC);
    uint64 va0 = PGROUNDDOWN(va);
    pte_t *pte = walk(p->pagetable, va0, 0);
    ASSERT_TRUE(pte);
    ASSERT_TRUE((*pte & PTE_V) && "xv6 could not handle page fault");
    if (*pte & PTE_OW) {
      uint64 flags = PTE_FLAGS(*pte);
      uint64 pa = PTE2PA(*pte);
      acquire(&cow_lock);
      if (get_ref((void *)pa) == 1) {
        *pte &= ~PTE_OW;
        *pte |= PTE_W;
      } else {
        flags &= ~PTE_OW;
        flags |= PTE_W;
        void *mem = kalloc();
        if (mem == 0) {
          // printf("usertrap(): no more memory\n");
          p->killed = 1;
        } else {
          memmove(mem, (void *)pa, PGSIZE);
          *pte = PA2PTE(mem) | flags;
          kfree((void *)pa);  // ref_cnt dec
        }
      }
      release(&cow_lock);
    } else {
      printf("usertrap(): write page fault pid=%d name=%s\n", p->pid, p->name);
      printf("            sepc=%p stval=%p\n", r_sepc(), r_stval());
      p->killed = 1;
    }
  } else {
  // ...
}
```

对于 copyout, 我们采用与 usertrap 相同的 schema

```c
int copyout(pagetable_t pagetable, uint64 dstva, char *src, uint64 len) {
  while (len > 0) {
    uint64 va0 = PGROUNDDOWN(dstva);
    if (va0 >= MAXVA) return -1;
    pte_t *pte = walk(pagetable, va0, 0);
    if (pte == 0) return -1;
    uint64 pa0 = PTE2PA(*pte);
    if ((*pte & PTE_V) == 0) return -1;
    if ((*pte & PTE_U) == 0) return -1;
    if (*pte & PTE_OW) {  // cow
      uint64 flags = PTE_FLAGS(*pte);
      acquire(&cow_lock);
      if (get_ref((void *)pa0) == 1) {
        *pte &= ~PTE_OW;
        *pte |= PTE_W;
      } else {
        flags &= ~PTE_OW;
        flags |= PTE_W;
        void *mem = kalloc();
        if (mem == 0) return -1;
        memmove(mem, (void *)pa0, PGSIZE);
        *pte = PA2PTE(mem) | flags;
        kfree((void *)pa0);
        pa0 = (uint64)mem;
      }
      release(&cow_lock);
    }

    if (pa0 == 0) return -1;
    uint64 n = PGSIZE - (dstva - va0);
    if (n > len) n = len;
    memmove((void *)(pa0 + (dstva - va0)), src, n);

    len -= n;
    src += n;
    dstva = va0 + PGSIZE;
  }
  return 0;
}
```
