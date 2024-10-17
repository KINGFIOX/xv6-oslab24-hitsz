// Physical memory allocator, for user processes,
// kernel stacks, page-table pages,
// and pipe buffers. Allocates whole 4096-byte pages.

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "riscv.h"
#include "defs.h"

void freerange(void *pa_start, void *pa_end);

extern char end[];  // first address after kernel.
                    // defined by kernel.ld.

struct run {
  struct run *next;
};

struct {
  struct spinlock lock;
  struct run *freelist;  // 第一个元素的位置
} kmem;

void kinit() {
  initlock(&kmem.lock, "kmem");
  freerange(end, (void *)PHYSTOP);
}

__attribute__((guarded_by(kmem.lock))) uint8 ref_cnt[(PHYSTOP - KERNBASE) / PGSIZE] = {0};
static inline uint64 pa2idx(void *pa) {
  ASSERT_TRUE(KERNBASE <= (uint64)pa && (uint64)pa < PHYSTOP);
  return ((uint64)pa - KERNBASE) / PGSIZE;
}
void inc_ref(void *pa) { ref_cnt[pa2idx(pa)]++; }
uint8 get_ref(void *pa) { return ref_cnt[pa2idx(pa)]; }

void freerange(void *pa_start, void *pa_end) {
  char *p = (char *)PGROUNDUP((uint64)pa_start);
  for (; p + PGSIZE <= (char *)pa_end; p += PGSIZE) {
    ref_cnt[pa2idx(p)] = 1;
    kfree(p);
  }
}

// Free the page of physical memory pointed at by v,
// which normally should have been returned by a
// call to kalloc().  (The exception is when
// initializing the allocator; see kinit above.)
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

// Allocate one 4096-byte page of physical memory.
// Returns a pointer that the kernel can use.
// Returns 0 if the memory cannot be allocated.
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
