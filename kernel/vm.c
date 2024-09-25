#include "param.h"
#include "types.h"
#include "memlayout.h"
#include "elf.h"
#include "riscv.h"
#include "defs.h"
#include "fs.h"

/// @brief the kernel's page table.
static pagetable_t kernel_pagetable;

extern char etext[];  // kernel.ld sets this to end of kernel code.

extern char trampoline[];  // trampoline.S

/*
 * create a direct-map page table for the kernel.
 */
void kvminit() {
  kernel_pagetable = (pagetable_t)kalloc();
  kmemset(kernel_pagetable, 0, PGSIZE);

  /* ---------- 外设 ---------- */

  // uart registers
  kvmmap(UART0, UART0, PGSIZE, PTE_R | PTE_W);
  // virtio mmio disk interface
  kvmmap(VIRTIO0, VIRTIO0, PGSIZE, PTE_R | PTE_W);
  // CLINT
  kvmmap(CLINT, CLINT, 0x10000, PTE_R | PTE_W);
  // PLIC
  kvmmap(PLIC, PLIC, 0x400000, PTE_R | PTE_W);

  /* ---------- kernel ---------- */

  // map kernel text executable and read-only.
  kvmmap(KERNBASE, KERNBASE, (uint64)etext - KERNBASE, PTE_R | PTE_X);

  // map kernel data and the physical RAM we'll make use of.
  kvmmap((uint64)etext, (uint64)etext, PHYSTOP - (uint64)etext, PTE_R | PTE_W);

  // map the trampoline for trap entry/exit to
  // the highest virtual address in the kernel.
  // os 中, 确实需要一个 用户态, 内核态的相同的空间.
  // 因为: 保存上下文是在用户态的, 恢复上下文是在内核态的, 两个地址空间不同
  // 只能映射成相同
  kvmmap(TRAMPOLINE, (uint64)trampoline, PGSIZE, PTE_R | PTE_X);
}

// Switch h/w page table register to the kernel's page table,
// and enable paging.

/// @brief enable the kernel's virtual memory -> set the satp register to the kernel's page table
/// @warning should be called only once on entry to the kernel. and after kvminit()
void kvminithart() {
  w_satp(MAKE_SATP(kernel_pagetable));
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

/// @brief page walk by hand, not using hardware page table walker.
/// if pte not valid -> create !
/// @param pagetable
/// @param va
/// @param alloc (bool) allocatable or not
/// @return pte
///
/// reserved [63:54] | ppn [53:10] | RSW(reserved for supervisor software) [9:8] | D(dirty) [7] | A(accessed) [6]
/// | G(global) [5] | U [4] | X [3] | W [2] | R [1] | V [0]
static pte_t *__walk(pagetable_t pagetable, uint64 va, int alloc) {
  if (va >= MAXVA) panic("__walk");

  for (int level = 2; level > 0; level--) {
    pte_t *pte = &pagetable[PX(level, va)];
    if (*pte & PTE_V) {
      pagetable = (pagetable_t)PTE2PA(*pte);
    } else {  // no valid -> create !
      if (!alloc || (pagetable = (pde_t *)kalloc()) == 0) return 0;
      kmemset(pagetable, 0, PGSIZE);
      *pte = PA2PTE(pagetable) | PTE_V;
    }
  }
  // get the pte from pagetable0
  return &pagetable[PX(0, va)];
}

/// @brief Look up a virtual address, return the physical address, or 0 if not mapped.
/// Can only be used to look up user pages.
/// @param pagetable
/// @param va
/// @return base address of the ppn given by the va
uint64 walkaddr(pagetable_t pagetable, uint64 va) {
  if (va >= MAXVA) return 0;
  pte_t *pte = __walk(pagetable, va, 0);
  if (pte == 0) return 0;
  if ((*pte & PTE_V) == 0) return 0;
  if ((*pte & PTE_U) == 0) return 0;
  return PTE2PA(*pte);
}

/// @brief add a mapping to the kernel page table.
/// only used when booting. does not flush TLB or enable paging.
/// @param va
/// @param pa
/// @param sz
/// @param perm
void kvmmap(uint64 va, uint64 pa, uint64 sz, int perm) {
  if (mappages(kernel_pagetable, va, sz, pa, perm) != 0) panic("kvmmap");
}

/// @brief translate a kernel virtual address to a physical address.
/// only needed for addresses on the stack. assumes va is page aligned.
/// @param va
/// @return
uint64 kvmpa(uint64 va) {
  pte_t *pte = __walk(kernel_pagetable, va, 0);
  if (pte == 0) panic("kvmpa");
  if ((*pte & PTE_V) == 0) panic("kvmpa");
  return PTE2PA(*pte) | PGOFFSET(va);
}

/// @brief Create PTEs for virtual addresses starting at va that refer to
/// physical addresses starting at pa. va and size might not
/// be page-aligned. Returns 0 on success, -1 if __walk() couldn't
/// allocate a needed page-table page. 省流: 建立相应的 pte 在 kernel 的 pagetable 中.
/// 或者这么说: 就是在 pagetable 上写一个记录, pa 是自己提供的, 有几方面的来源
/// 1. 真的是映射(外设) 2. kalloc 分配的内存 3. 如果是 kernel, 还会有: etext 之类的(qemu 装载的时候, 已经有了的)
/// @param pagetable
/// @param va
/// @param size
/// @param pa
/// @param perm permission of pte
/// @return success: 0, failed: -1
int mappages(pagetable_t pagetable, uint64 va, uint64 size, uint64 pa, int perm) {
  // [a, last]
  uint64 a = PGROUNDDOWN(va);                // base addr of the page of va
  uint64 last = PGROUNDDOWN(va + size - 1);  // base addr of the page of (va + size - 1)
  for (;;) {
    pte_t *pte;
    if ((pte = __walk(pagetable, a, 1)) == 0) return -1;
    if (*pte & PTE_V) panic("remap");  // failed: 重复映射(覆盖)
    *pte = PA2PTE(pa) | perm | PTE_V;
    // 就是注意一点: map 是不会 alloc 页面的
    if (a == last) break;  // 可能出现: 跨页面(比方说: 显存映射)
    a += PGSIZE;
    pa += PGSIZE;
  }
  return 0;
}

// Remove npages of mappings starting from va. va must be
// page-aligned. The mappings must exist.
// Optionally free the physical memory.

/// @brief
/// @param pagetable
/// @param va should be page-aligned
/// @param npages number of pages to unmap
/// @param do_free 一般来说, map 并不会 alloc. 但是如果真的 alloc 了, 就需要释放
void uvmunmap(pagetable_t pagetable, uint64 va, uint64 npages, int do_free) {
  if ((va % PGSIZE) != 0) panic("uvmunmap: not aligned");

  for (uint64 a = va; a < va + npages * PGSIZE; a += PGSIZE) {
    pte_t *pte;
    if ((pte = __walk(pagetable, a, 0)) == 0) panic("uvmunmap: __walk");  // 既然是 unmap, 肯定不能分配
    if ((*pte & PTE_V) == 0) panic("uvmunmap: not mapped");
    // riscv 中: 如果PTE中的 R（Read）、W（Write）、X（Execute） 任意一个位被设置（即为1），则该PTE被视为叶子PTE。
    if (PTE_FLAGS(*pte) == PTE_V) panic("uvmunmap: not a leaf");
    if (do_free) {
      uint64 pa = PTE2PA(*pte);
      kfree((void *)pa);
    }
    *pte = 0;  // 有个疑问, 这个会不会被回收 ? 可能会出现严重的资源泄露
  }
}

/// @brief create an empty user page table.
/// @return 0 if out of physical memory.
pagetable_t uvmcreate() {
  pagetable_t pagetable = (pagetable_t)kalloc();  // 分配一个页面, 作为 page table
  if (pagetable == 0) return 0;
  kmemset(pagetable, 0, PGSIZE);
  return pagetable;
}

/// @brief Load the user initcode into address 0 of pagetable, for the very first process.
/// @param pagetable
/// @param src
/// @param sz sz must be less than a page.
void uvminit(pagetable_t pagetable, uchar *src, uint sz) {
  if (sz >= PGSIZE) panic("inituvm: more than a page");
  char *mem = kalloc();
  kmemset(mem, 0, PGSIZE);
  mappages(pagetable, 0, PGSIZE, (uint64)mem, PTE_W | PTE_R | PTE_X | PTE_U);
  kmemmove(mem, src, sz);
}

/// @brief  Allocate PTEs and physical memory to grow process from oldsz to newsz,
/// which need not be page aligned. Returns new size or 0 on error.
/// @param pagetable
/// @param oldsz
/// @param newsz
/// @return
uint64 uvmalloc(pagetable_t pagetable, uint64 oldsz, uint64 newsz) {
  if (newsz < oldsz) return oldsz;

  oldsz = PGROUNDUP(oldsz);
  for (uint64 a = oldsz; a < newsz; a += PGSIZE) {
    char *mem = kalloc();
    if (mem == 0) {
      uvmdealloc(pagetable, a, oldsz);
      return 0;
    }
    kmemset(mem, 0, PGSIZE);
    if (mappages(pagetable, a, PGSIZE, (uint64)mem, PTE_W | PTE_X | PTE_R | PTE_U) != 0) {
      // failed, roll back
      kfree(mem);
      uvmdealloc(pagetable, a, oldsz);
      return 0;
    }
  }
  return newsz;
}

/// @brief Deallocate user pages to bring the process size from oldsz to newsz.
/// oldsz and newsz need not be page-aligned, nor does newsz need to be less than oldsz.  oldsz can be larger than the
/// actual/ process size.
/// @param pagetable
/// @param oldsz
/// @param newsz
/// @return the new process size.
uint64 uvmdealloc(pagetable_t pagetable, uint64 oldsz, uint64 newsz) {
  if (newsz >= oldsz) return oldsz;

  if (PGROUNDUP(newsz) < PGROUNDUP(oldsz)) {
    int npages = (PGROUNDUP(oldsz) - PGROUNDUP(newsz)) / PGSIZE;
    uvmunmap(pagetable, PGROUNDUP(newsz), npages, 1);
  }

  return newsz;
}

/// @brief Recursively free page-table pages. All leaf mappings must already have been removed.
/// @param pagetable
void freewalk(pagetable_t pagetable) {
  // there are 2^9 = 512 PTEs in a page table.
  for (int i = 0; i < 512; i++) {
    pte_t pte = pagetable[i];
    if ((pte & PTE_V) && (pte & (PTE_R | PTE_W | PTE_X)) == 0) {  // 说明是 dir
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

/// @brief free user memory pages, then free page-table pages.
/// @param pagetable
/// @param sz
void uvmfree(pagetable_t pagetable, uint64 sz) {
  if (sz > 0) uvmunmap(pagetable, 0, PGROUNDUP(sz) / PGSIZE, 1);
  freewalk(pagetable);
}

/// @brief Given a parent process's page table, copy its memory into a child's page table.
/// Copies both the page table and the physical memory.
/// @param old
/// @param
/// @param sz
/// @return 0 on success, -1 on failure.
///
/// @failure: frees any allocated pages on failure.
int uvmcopy(pagetable_t old, pagetable_t new, uint64 sz) {
  for (int i = 0; i < sz; i += PGSIZE) {
    pte_t *pte;
    if ((pte = __walk(old, i, 0)) == 0) panic("uvmcopy: pte should exist");
    if ((*pte & PTE_V) == 0) panic("uvmcopy: page not present");  // 从这里就可以看出: 没有处理 缺页异常
    uint64 pa = PTE2PA(*pte);
    uint flags = PTE_FLAGS(*pte);
    char *mem;
    if ((mem = kalloc()) == 0) {
      uvmunmap(new, 0, i / PGSIZE, 1);
      return -1;
    }
    kmemmove(mem, (char *)pa, PGSIZE);
    if (mappages(new, i, PGSIZE, (uint64)mem, flags) != 0) {  // failed to add pte to new pagetable
      kfree(mem);
      uvmunmap(new, 0, i / PGSIZE, 1);
      return -1;
    }
  }
  return 0;
}

/// @brief mark a PTE invalid for user access. used by exec for the user stack guard page.
/// @param pagetable
/// @param va
void uvmclear(pagetable_t pagetable, uint64 va) {
  pte_t *pte = __walk(pagetable, va, 0);
  if (pte == 0) panic("uvmclear");
  *pte &= ~PTE_U;
}

/// @brief Copy from kernel to user
/// @param pagetable
/// @param dstva to
/// @param src from
/// @param len
/// @return 0 on success, -1 on error.
int copyout(pagetable_t pagetable, uint64 dstva, const char *src, uint64 len) {
  while (len > 0) {
    uint64 va0 = PGROUNDDOWN(dstva);
    uint64 pa0 = walkaddr(pagetable, va0);
    if (pa0 == 0) return -1;
    uint64 n = PGSIZE - (dstva - va0);
    if (n > len) n = len;
    kmemmove((void *)(pa0 + (dstva - va0)), src, n);

    len -= n;
    src += n;
    dstva = va0 + PGSIZE;
  }
  return 0;
}

/// @brief Copy from user to kernel.
/// @param pagetable
/// @param dst
/// @param srcva
/// @param len
/// @return 0 on success, -1 on error.
int copyin(pagetable_t pagetable, char *dst, uint64 srcva, uint64 len) {
  uint64 n, va0, pa0;

  while (len > 0) {
    uint64 va0 = PGROUNDDOWN(srcva);
    uint64 pa0 = walkaddr(pagetable, va0);
    if (pa0 == 0) return -1;
    n = PGSIZE - (srcva - va0);
    if (n > len) n = len;
    kmemmove(dst, (void *)(pa0 + (srcva - va0)), n);

    len -= n;
    dst += n;
    srcva = va0 + PGSIZE;
  }
  return 0;
}

/// @brief Copy a null-terminated string from user to kernel.
/// Copy bytes to dst from virtual address srcva in a given page table, until a '\0', or max.
/// @param pagetable
/// @param dst
/// @param srcva
/// @param max
/// @return 0 on success, -1 on error.
int copyinstr(pagetable_t pagetable, char *dst, uint64 srcva, uint64 max) {
  int got_null = 0;

  while (got_null == 0 && max > 0) {
    uint64 va0 = PGROUNDDOWN(srcva);
    uint64 pa0 = walkaddr(pagetable, va0);
    if (pa0 == 0) return -1;
    uint64 n = PGSIZE - (srcva - va0);
    if (n > max) n = max;

    const char *p = (char *)(pa0 + (srcva - va0));
    while (n > 0) {
      if (*p == '\0') {
        *dst = '\0';
        got_null = 1;
        break;
      } else {
        *dst = *p;
      }
      --n;
      --max;
      p++;
      dst++;
    }

    srcva = va0 + PGSIZE;
  }
  if (got_null) {
    return 0;
  } else {
    return -1;
  }
}
