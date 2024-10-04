#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"
#include "elf.h"

static inline char *setflags(pte_t pte) {
  static char flags[] = "----";
  flags[0] = (pte & PTE_R) ? 'r' : '-';
  flags[1] = (pte & PTE_W) ? 'w' : '-';
  flags[2] = (pte & PTE_X) ? 'x' : '-';
  flags[3] = (pte & PTE_U) ? 'u' : '-';
  return flags;
}

static void _vmprint0(pagetable_t pgtbl, uint64 vpn2, uint64 vpn1) {
  for (int i = 0; i < 512; i++) {
    pte_t pte = pgtbl[i];
    if (pte & PTE_V) {
      char *flags = setflags(pte);
      uint64 va = (vpn2 << 30) | (vpn1 << 21) | (i << 12);
#if 1
      if ((KERNBASE <= va && va < PHYSTOP) || (PLIC <= va && va < PLIC + 0x400000)) {
        continue;
      }
#endif
      printf("||   ||   ||idx: %d: va: %p -> pa: %p, flags: %s\n", i, va, PTE2PA(pte), flags);
    }
  }
  return;
}

static void _vmprint1(pagetable_t pgtbl, uint64 vpn2) {
  for (int i = 0; i < 512; i++) {
    pte_t pte = pgtbl[i];
    if (pte & PTE_V) {
      uint64 child = PTE2PA(pte);
      char *flags = setflags(pte);
#if 1
      uint64 va = (vpn2 << 30) | (i << 21) | (0 << 12);
      if ((KERNBASE <= va && va < PHYSTOP) || (PLIC <= va && va < PLIC + 0x400000)) {
        continue;
      }
#endif
      printf("||   ||idx: %d: pa: %p, flags: %s\n", i, child, flags);
      _vmprint0((pagetable_t)child, vpn2, i);
    }
  }
  return;
}

void vmprint(pagetable_t pgtbl) {
  printf("page table %p\n", pgtbl);
  for (int i = 0; i < 512; i++) {
    pte_t pte = pgtbl[i];
    if (pte & PTE_V) {
      uint64 child = PTE2PA(pte);
      char *flags = setflags(pte);
      printf("||idx: %d: pa: %p, flags: %s\n", i, child, flags);
      _vmprint1((pagetable_t)child, i);
    }
  }
  return;
}

static int loadseg(pde_t *pgdir, uint64 addr, struct inode *ip, uint offset, uint sz);

int exec(char *path, char **argv) {
  char *s, *last;
  int i, off;
  uint64 argc, sz = 0, sp, ustack[MAXARG + 1], stackbase;
  struct elfhdr elf;
  struct inode *ip;
  struct proghdr ph;
  pagetable_t new_space = 0, old_space;
  struct proc *p = myproc();

  begin_op();

  if ((ip = namei(path)) == 0) {
    end_op();
    return -1;
  }
  ilock(ip);

  // Check ELF header
  if (readi(ip, 0, (uint64)&elf, 0, sizeof(elf)) != sizeof(elf)) goto bad;
  if (elf.magic != ELF_MAGIC) goto bad;

  if ((new_space = proc_pagetable(p)) == 0) goto bad;

  // Load program into memory.
  for (i = 0, off = elf.phoff; i < elf.phnum; i++, off += sizeof(ph)) {
    if (readi(ip, 0, (uint64)&ph, off, sizeof(ph)) != sizeof(ph)) goto bad;
    if (ph.type != ELF_PROG_LOAD) continue;
    if (ph.memsz < ph.filesz) goto bad;
    if (ph.vaddr + ph.memsz < ph.vaddr) goto bad;
    uint64 sz1;
    if ((sz1 = uvmalloc(new_space, sz, ph.vaddr + ph.memsz)) == 0) goto bad;
    sz = sz1;
    if (ph.vaddr % PGSIZE != 0) goto bad;
    if (loadseg(new_space, ph.vaddr, ip, ph.off, ph.filesz) < 0) goto bad;
  }
  iunlockput(ip);
  end_op();
  ip = 0;

  p = myproc();
  uint64 oldsz = p->sz;

  // Allocate two pages at the next page boundary.
  // Use the second as the user stack.
  sz = PGROUNDUP(sz);
  uint64 sz1;
  if ((sz1 = uvmalloc(new_space, sz, sz + 2 * PGSIZE)) == 0) goto bad;
  sz = sz1;
  uvmclear(new_space, sz - 2 * PGSIZE);
  sp = sz;
  stackbase = sp - PGSIZE;

  // Push argument strings, prepare rest of stack in ustack.
  for (argc = 0; argv[argc]; argc++) {
    if (argc >= MAXARG) goto bad;
    sp -= strlen(argv[argc]) + 1;
    sp -= sp % 16;  // riscv sp must be 16-byte aligned
    if (sp < stackbase) goto bad;
    if (copyout(new_space, sp, argv[argc], strlen(argv[argc]) + 1) < 0) goto bad;
    ustack[argc] = sp;
  }
  ustack[argc] = 0;

  // push the array of argv[] pointers.
  sp -= (argc + 1) * sizeof(uint64);
  sp -= sp % 16;
  if (sp < stackbase) goto bad;
  if (copyout(new_space, sp, (char *)ustack, (argc + 1) * sizeof(uint64)) < 0) goto bad;

  // arguments to user main(argc, argv)
  // argc is returned via the system call return
  // value, which goes in a0.
  p->trapframe->a1 = sp;

  // Save program name for debugging.
  for (last = s = path; *s; s++)
    if (*s == '/') last = s + 1;
  safestrcpy(p->name, last, sizeof(p->name));

  w_satp(MAKE_SATP(new_space));
  sfence_vma();

  // Commit to the user image.
  old_space = p->su_space;
  p->su_space = new_space;
  p->sz = sz;
  p->trapframe->epc = elf.entry;  // initial program counter = main
  p->trapframe->sp = sp;          // initial stack pointer
  proc_freepagetable(old_space, oldsz);

  if (p->pid == 1) {  // 这个应该是 shell
    vmprint(p->su_space);
  }
  return argc;  // this ends up in a0, the first argument to main(argc, argv)

bad:
  if (new_space) proc_freepagetable(new_space, sz);
  if (ip) {
    iunlockput(ip);
    end_op();
  }
  return -1;
}

// Load a program segment into pagetable at virtual address va.
// va must be page-aligned
// and the pages from va to va+sz must already be mapped.
// Returns 0 on success, -1 on failure.
static int loadseg(pagetable_t pagetable, uint64 va, struct inode *ip, uint offset, uint sz) {
  uint i, n;
  uint64 pa;

  if ((va % PGSIZE) != 0) panic("loadseg: va must be page aligned");

  for (i = 0; i < sz; i += PGSIZE) {
    pa = walkaddr(pagetable, va + i);
    if (pa == 0) panic("loadseg: address should exist");
    if (sz - i < PGSIZE)
      n = sz - i;
    else
      n = PGSIZE;
    if (readi(ip, 0, (uint64)pa, offset + i, n) != n) return -1;
  }

  return 0;
}
