// Physical memory allocator, for user processes,
// kernel stacks, page-table pages,
// and pipe buffers. Allocates whole 4096-byte pages.

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "riscv.h"
#include "defs.h"

extern char end[];  // first address after kernel.
                    // defined by kernel.ld.

// 这个就是将: page 🔗 起来的
struct run {
  struct run *next;
};

struct kmem {
  struct spinlock lock;
  struct run *freelist;
};

struct kmem kmems[NCPU];

void freerange(void *pa_start, void *pa_end) {
  char *pa = (char *)PGROUNDUP((uint64)pa_start);
  uint64 i = 0;
  for (; pa + PGSIZE <= (char *)pa_end; pa += PGSIZE, i++) {
    // Fill with junk to catch dangling refs.
    memset(pa, 1, PGSIZE);

    uint64 cpu = i % NCPU;

    struct run *r = (struct run *)pa;

    acquire(&kmems[cpu].lock);
    r->next = kmems[cpu].freelist;
    kmems[cpu].freelist = r;
    release(&kmems[cpu].lock);
  };
}

void kinit() {
  for (int i = 0; i < NCPU; i++) {
    initlock(&kmems[i].lock, "kmem");
  }
  freerange(end, (void *)PHYSTOP);
}

// Free the page of physical memory pointed at by v,
// which normally should have been returned by a
// call to kalloc().  (The exception is when
// initializing the allocator; see kinit above.)
// 简单来说, 就是将 pa push 到 对应的 kmem.freelist 中
void kfree(void *pa) {
  if (((uint64)pa % PGSIZE) != 0 || (char *)pa < end || (uint64)pa >= PHYSTOP) panic("kfree");

  push_off();
  int cpu = cpuid();
  pop_off();

  // Fill with junk to catch dangling refs.
  memset(pa, 1, PGSIZE);

  struct run *r = (struct run *)pa;

  acquire(&kmems[cpu].lock);
  r->next = kmems[cpu].freelist;
  kmems[cpu].freelist = r;
  release(&kmems[cpu].lock);
}

// Allocate one 4096-byte page of physical memory.
// Returns a pointer that the kernel can use.
// Returns 0 if the memory cannot be allocated.
void *kalloc() {
  push_off();
  int cpu = cpuid();
  pop_off();

  acquire(&kmems[cpu].lock);
  struct run *r = kmems[cpu].freelist;
  if (r) {
    kmems[cpu].freelist = r->next;
  } else {  // 如果当前 cpu 的 freelist 空了, 去其他 cpu 中的 freelist 中找
    for (int i = 0; i < NCPU; i++) {
      if (i == cpu) continue;
      acquire(&kmems[i].lock);
      r = kmems[i].freelist;
      if (r) kmems[i].freelist = r->next;
      release(&kmems[i].lock);
      if (r) break;
    }
  }
  release(&kmems[cpu].lock);

  if (r) memset((char *)r, 5, PGSIZE);  // fill with junk
  return (void *)r;
}
