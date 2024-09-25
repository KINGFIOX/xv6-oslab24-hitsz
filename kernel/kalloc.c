// Physical memory allocator, for user processes,
// kernel stacks, page-table pages,
// and pipe buffers. Allocates whole 4096-byte pages.

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "riscv.h"
#include "defs.h"

static void __freerange(void *pa_start, void *pa_end);

/// @brief first address after kernel. defined by kernel.ld.
extern char end[];

struct run {
  struct run *next;
};

static struct {
  struct spinlock lock;
  struct run *freelist;
} kmem;

/// @brief init memory list
///
/// @globals
/// - PHYSTOP
/// - kmem
/// - end
void kinit() {
  initlock(&kmem.lock, "kmem");
  __freerange(end, (void *)PHYSTOP);
}

/// @brief
/// @param pa_start
/// @param pa_end
static void __freerange(void *pa_start, void *pa_end) {
  for (char *p = (char *)PGROUNDUP((uint64)pa_start); p + PGSIZE <= (char *)pa_end; p += PGSIZE) kfree(p);
}

/// @brief Free the page of physical memory pointed at by v,
/// which normally should have been returned by a call to  kalloc().
/// (The exception is when initializing the allocator; see kinit above.)
/// @param pa
///
/// @globals
/// - (mut) kmem
void kfree(void *pa) {
  // failed:
  // - 1. pa is not aligned to PGSIZE
  // - 2. pa is in the range of kernel memory
  // - 3. pa is not in the range of physical memory
  if (((uint64)pa % PGSIZE) != 0 || (char *)pa < end || (uint64)pa >= PHYSTOP) panic("kfree");

  // Fill with junk to catch dangling refs.
  kmemset(pa, 1, PGSIZE);

  acquire(&kmem.lock);
  // physical frame's head is a (struct run), 然后 物理页帧用链表穿起来
  struct run *r = (struct run *)pa;
  r->next = kmem.freelist;
  kmem.freelist = r;
  release(&kmem.lock);
}

/// @brief Allocate one 4096-byte page of physical memory.
/// @param
/// @return
/// a pointer that the kernel can use.
/// 0 if the memory cannot be allocated.
///
/// @globals
/// - (mut) kmem
void *kalloc(void) {
  acquire(&kmem.lock);
  struct run *r = kmem.freelist;
  if (r) kmem.freelist = r->next;  // pop a free page
  release(&kmem.lock);

  if (r) kmemset((char *)r, 5, PGSIZE);  // fill with junk
  return (void *)r;
}
