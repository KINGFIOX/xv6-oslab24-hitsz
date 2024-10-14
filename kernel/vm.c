#include "param.h"
#include "types.h"
#include "memlayout.h"
#include "elf.h"
#include "riscv.h"
#include "defs.h"
#include "fs.h"

/*
 * the kernel's page table.
 */
pagetable_t g_space;

extern char etext[];  // kernel.ld sets this to end of kernel code.

extern char trampoline[];  // trampoline.S

/*
 * create a direct-map page table for the kernel.
 */
void g_space_init() {
  g_space = (pagetable_t)kalloc();
  memset(g_space, 0, PGSIZE);

  if (space_map(g_space, UART0, PGSIZE, UART0, PTE_R | PTE_W) != 0) panic("g_space_init");
  if (space_map(g_space, VIRTIO0, PGSIZE, VIRTIO0, PTE_R | PTE_W) != 0) panic("g_space_init");
  if (space_map(g_space, CLINT, 0x10000, CLINT, PTE_R | PTE_W) != 0) panic("g_space_init");
  if (space_map(g_space, PLIC, 0x400000, PLIC, PTE_R | PTE_W) != 0) panic("g_space_init");

  if (space_map(g_space, KERNBASE, (uint64)etext - KERNBASE, KERNBASE, PTE_R | PTE_X) != 0) panic("g_space_init");
  if (space_map(g_space, (uint64)etext, PHYSTOP - (uint64)etext, (uint64)etext, PTE_R | PTE_W) != 0) panic("g_space_init");  // 剩余的一些物理地址
  if (space_map(g_space, TRAMPOLINE, PGSIZE, (uint64)trampoline, PTE_R | PTE_X) != 0) panic("g_space_init");
}

// Switch h/w page table register to the kernel's page table,
// and enable paging.
void kvminithart() {
  w_satp(MAKE_SATP(g_space));
  sfence_vma();
}

// Return the address of the PTE in page table pagetable
// that corresponds to virtual address va.  If alloc!=0,
// create any required page-table pages.
//
// The risc-v Sv39 scheme has three levels of page-table
// pages. A page-table page contains 512 64-bit PTEs.
// A 64-bit virtual address is split into five fields:
//   39..63 -- must be zero.
//   30..38 -- 9 bits of level-2 index.
//   21..29 -- 9 bits of level-1 index.
//   12..20 -- 9 bits of level-0 index.
//    0..11 -- 12 bits of byte offset within the page.
pte_t *walk(pagetable_t pagetable, uint64 va, int alloc) {
  if (va >= MAXVA) panic("walk");

  for (int level = 2; level > 0; level--) {
    pte_t *pte = &pagetable[PX(level, va)];
    if (*pte & PTE_V) {
      pagetable = (pagetable_t)PTE2PA(*pte);
    } else {
      if (!alloc || (pagetable = (pde_t *)kalloc()) == 0) return 0;
      memset(pagetable, 0, PGSIZE);
      *pte = PA2PTE(pagetable) | PTE_V;
    }
  }
  return &pagetable[PX(0, va)];
}

// Look up a virtual address, return the physical address,
// or 0 if not mapped.
// Can only be used to look up user pages.
uint64 walkaddr(pagetable_t pagetable, uint64 va) {
  pte_t *pte;
  uint64 pa;

  if (va >= MAXVA) return 0;

  pte = walk(pagetable, va, 0);
  if (pte == 0) return 0;
  if ((*pte & PTE_V) == 0) return 0;
  if ((*pte & PTE_U) == 0) return 0;
  pa = PTE2PA(*pte);
  return pa;
}

// translate a kernel virtual address to
// a physical address. only needed for
// addresses on the stack.
// assumes va is page aligned.
uint64 kvmpa(uint64 va) {
  uint64 off = va % PGSIZE;
  pte_t *pte;
  uint64 pa;

  pte = walk(g_space, va, 0);
  if (pte == 0) panic("kvmpa");
  if ((*pte & PTE_V) == 0) panic("kvmpa");
  pa = PTE2PA(*pte);
  return pa + off;
}

// Create PTEs for virtual addresses starting at va that refer to
// physical addresses starting at pa.

/// @brief
/// @warning va and size might not be page-aligned.
/// @return Returns 0 on success, -1 if walk() couldn't allocate a needed page-table page.
int space_map(pagetable_t pagetable, uint64 va, uint64 size, uint64 pa, int perm) {
  uint64 a = PGROUNDDOWN(va);
  uint64 last = PGROUNDDOWN(va + size - 1);
  for (;;) {
    pte_t *pte;
    // 因为这个是写记录, 所以 walk 的时候, 会进行 alloc
    if ((pte = walk(pagetable, a, 1)) == 0) {
      // clean up, unmap, 被创建出来的目录的话, 之前会调用 uvmfree 来清理, 范围是 PGROUNDDOWN(va) ~ a
      space_unmap(pagetable, PGROUNDDOWN(va), (a - PGROUNDDOWN(va)) / PGSIZE, 0);
      return -1;
    };
    if (*pte & PTE_V) panic("remap");
    *pte = PA2PTE(pa) | perm | PTE_V;
    if (a == last) break;
    a += PGSIZE;
    pa += PGSIZE;
  }
  return 0;
}

/// @brief 这里是: 在 space 中删除一个范围的连续的记录(npages)
/// @warning va must be page-aligned
/// @param do_free Optionally free the physical memory.
void space_unmap(pagetable_t space, uint64 va, uint64 npages, int do_free) {
  if ((va % PGSIZE) != 0) panic("space_unmap: not aligned");

  for (uint64 a = va; a < va + npages * PGSIZE; a += PGSIZE) {
    pte_t *pte;
    if ((pte = walk(space, a, 0)) == 0) panic("space_unmap: walk");
    if ((*pte & PTE_V) == 0) panic("space_unmap: not mapped");
    if (PTE_FLAGS(*pte) == PTE_V) panic("space_unmap: not a leaf");
    if (do_free) {
      uint64 pa = PTE2PA(*pte);
      kfree((void *)pa);
    }
    *pte = 0;
  }
}

// create an empty user page table.
// returns 0 if out of memory.
pagetable_t uvmcreate() {
  pagetable_t pagetable;
  pagetable = (pagetable_t)kalloc();
  if (pagetable == 0) return 0;
  memset(pagetable, 0, PGSIZE);
  return pagetable;
}

// Load the user initcode into address 0 of pagetable,
// for the very first process.
// sz must be less than a page.
void uvminit(pagetable_t pagetable, uchar *src, uint sz) {
  char *mem;

  if (sz >= PGSIZE) panic("inituvm: more than a page");
  mem = kalloc();
  memset(mem, 0, PGSIZE);
  space_map(pagetable, 0, PGSIZE, (uint64)mem, PTE_W | PTE_R | PTE_X | PTE_U);
  memmove(mem, src, sz);
}

// Allocate PTEs and physical memory to grow process from oldsz to
// newsz, which need not be page aligned.  Returns new size or 0 on error.
uint64 uvmalloc(pagetable_t pagetable, uint64 oldsz, uint64 newsz) {
  if (newsz < oldsz) return oldsz;
  if (newsz >= PLIC) {  // 分配地址要注意边界
    return 0;
  }
  oldsz = PGROUNDUP(oldsz);
  for (uint64 a = oldsz; a < newsz; a += PGSIZE) {
    char *mem = kalloc();
    if (mem == 0) {
      uvmdealloc(pagetable, a, oldsz);
      return 0;
    }
    memset(mem, 0, PGSIZE);
    if (space_map(pagetable, a, PGSIZE, (uint64)mem, PTE_W | PTE_X | PTE_R | PTE_U) != 0) {
      kfree(mem);
      uvmdealloc(pagetable, a, oldsz);
      return 0;
    }
  }
  return newsz;
}

// Deallocate user pages to bring the process size from oldsz to
// newsz.  oldsz and newsz need not be page-aligned, nor does newsz
// need to be less than oldsz.  oldsz can be larger than the actual
// process size.  Returns the new process size.
uint64 uvmdealloc(pagetable_t pagetable, uint64 oldsz, uint64 newsz) {
  if (newsz >= oldsz) return oldsz;

  if (PGROUNDUP(newsz) < PGROUNDUP(oldsz)) {
    int npages = (PGROUNDUP(oldsz) - PGROUNDUP(newsz)) / PGSIZE;
    space_unmap(pagetable, PGROUNDUP(newsz), npages, 1);
  }

  return newsz;
}

/// @brief Recursively free page-table pages. 就是说: 这个值会删除目录
/// @warning All leaf mappings must already have been removed.
void freewalk(pagetable_t pagetable) {
  // there are 2^9 = 512 PTEs in a page table.
  for (int i = 0; i < 512; i++) {
    pte_t pte = pagetable[i];
    if ((pte & PTE_V) && (pte & (PTE_R | PTE_W | PTE_X)) == 0) {
      // this PTE points to a lower-level page table.
      uint64 child = PTE2PA(pte);
      freewalk((pagetable_t)child);
      pagetable[i] = 0;
    } else if (pte & PTE_V) {
      panic("freewalk: leaf");
    }
  }
  kfree((void *)pagetable);
}

//

/// @brief Free user memory pages, then free page-table pages.
/// 就是: 这个只会 unmap 用户的内容(从 0 开始), 在进入到这一步之前, 我们会先 unmap TRAMPOLINE, TRAPFRAME 之类的
void uvmfree(pagetable_t pagetable, uint64 sz) {
  if (sz > 0) space_unmap(pagetable, 0, PGROUNDUP(sz) / PGSIZE, 1);  // 会先删除用户空间(从0开始)
  freewalk(pagetable);
}

// Given a parent process's page table, copy
// its memory into a child's page table.
// Copies both the page table and the
// physical memory.
// returns 0 on success, -1 on failure.
// frees any allocated pages on failure.
int uvmcopy(pagetable_t old, pagetable_t new, uint64 sz) {
  pte_t *pte;
  uint64 pa, i;
  uint flags;
  char *mem;

  for (i = 0; i < sz; i += PGSIZE) {
    if ((pte = walk(old, i, 0)) == 0) panic("uvmcopy: pte should exist");
    if ((*pte & PTE_V) == 0) panic("uvmcopy: page not present");
    pa = PTE2PA(*pte);
    flags = PTE_FLAGS(*pte);
    if ((mem = kalloc()) == 0) goto err;
    memmove(mem, (char *)pa, PGSIZE);
    if (space_map(new, i, PGSIZE, (uint64)mem, flags) != 0) {
      kfree(mem);
      goto err;
    }
  }
  return 0;

err:
  space_unmap(new, 0, i / PGSIZE, 1);
  return -1;
}

// mark a PTE invalid for user access.
// used by exec for the user stack guard page.
void uvmclear(pagetable_t pagetable, uint64 va) {
  pte_t *pte;

  pte = walk(pagetable, va, 0);
  if (pte == 0) panic("uvmclear");
  *pte &= ~PTE_U;
}

// Copy from kernel to user.
// Copy len bytes from src to virtual address dstva in a given page table.
// Return 0 on success, -1 on error.
int copyout(pagetable_t pagetable, uint64 dstva, char *src, uint64 len) {
  uint64 n, va0, pa0;
  while (len > 0) {
    va0 = PGROUNDDOWN(dstva);
    pa0 = walkaddr(pagetable, va0);
    if (pa0 == 0) return -1;
    n = PGSIZE - (dstva - va0);
    if (n > len) n = len;
    memmove((void *)(pa0 + (dstva - va0)), src, n);

    len -= n;
    src += n;
    dstva = va0 + PGSIZE;
  }
  return 0;
}

// Copy from user to kernel.
// Copy len bytes to dst from virtual address srcva in a given page table.
// Return 0 on success, -1 on error.
int copyin(pagetable_t pagetable, char *dst, uint64 srcva, uint64 len) {
  // extern void vmprint(pagetable_t pagetable);
  // vmprint(pagetable);

  if (srcva >= PLIC) return -1;
  w_sstatus(r_sstatus() | SSTATUS_SUM);
  // memmove(dst, (void *)srcva, len);
  extern int copyin_new(pagetable_t pagetable, char *dst, uint64 srcva, uint64 len);
  int ret = copyin_new(pagetable, dst, srcva, len);
  w_sstatus(r_sstatus() & ~SSTATUS_SUM);
  return ret;

  // uint64 n, va0, pa0;
  // while (len > 0) {
  //   va0 = PGROUNDDOWN(srcva);
  //   pa0 = walkaddr(pagetable, va0);
  //   if (pa0 == 0) return -1;
  //   n = PGSIZE - (srcva - va0);
  //   if (n > len) n = len;
  //   memmove(dst, (void *)(pa0 + (srcva - va0)), n);

  //   len -= n;
  //   dst += n;
  //   srcva = va0 + PGSIZE;
  // }
  // return 0;
}

// Copy a null-terminated string from user to kernel.
// Copy bytes to dst from virtual address srcva in a given page table,
// until a '\0', or max.
// Return 0 on success, -1 on error.
int copyinstr(pagetable_t pagetable, char *dst, uint64 srcva, uint64 max) {
  // for (int i = 0; i < max; i++) {
  //   if (copyin(pagetable, dst, srcva, 1) < 0) return -1;
  //   if (*dst == '\0') return 0;
  //   srcva++;
  //   dst++;
  // }
  // return 0;

  w_sstatus(r_sstatus() | SSTATUS_SUM);
  extern int copyinstr_new(pagetable_t pagetable, char *dst, uint64 srcva, uint64 max);
  int ret = copyinstr_new(pagetable, dst, srcva, max);
  w_sstatus(r_sstatus() & ~SSTATUS_SUM);
  return ret;

  // uint64 n, va0, pa0;
  // bool got_null = false;
  // while (got_null == 0 && max > 0) {
  //   va0 = PGROUNDDOWN(srcva);
  //   pa0 = walkaddr(pagetable, va0);
  //   if (pa0 == 0) return -1;
  //   n = PGSIZE - (srcva - va0);
  //   if (n > max) n = max;

  //   char *p = (char *)(pa0 + (srcva - va0));
  //   while (n > 0) {
  //     if (*p == '\0') {
  //       *dst = '\0';
  //       got_null = 1;
  //       break;
  //     } else {
  //       *dst = *p;
  //     }
  //     --n;
  //     --max;
  //     p++;
  //     dst++;
  //   }

  //   srcva = va0 + PGSIZE;
  // }
  // if (got_null) {
  //   return 0;
  // } else {
  //   return -1;
  // }
}

// check if use global kpgtbl or not
int test_pagetable() {
  uint64 satp = r_satp();
  uint64 gsatp = MAKE_SATP(g_space);
  printf("test_pagetable: %d\n", satp != gsatp);
  return satp != gsatp;
}