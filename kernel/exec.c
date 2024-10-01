#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"
#include "elf.h"

static int loadseg(pde_t *pgdir, uint64 addr, struct inode *ip, uint offset, uint sz);

#define HANDLE_BAD                                    \
  {                                                   \
    if (pagetable) proc_freepagetable(pagetable, sz); \
    if (ip) {                                         \
      iunlockput(ip);                                 \
      end_op();                                       \
    }                                                 \
    return -1;                                        \
  }

int kexec(char *path, char **argv) {
  uint64 argc, sz = 0, sp, ustack[MAXARG + 1], stackbase;

  pagetable_t oldpagetable;

  struct proc *p = myproc();

  begin_op();

  struct inode *ip = namei(path);
  if (ip == 0) {  // 文件不存在
    end_op();
    return -1;
  }
  ilock(ip);

  // Check ELF header
  struct elfhdr elf;
  if (readi(ip, 0, (uint64)&elf, 0, sizeof(elf)) != sizeof(elf)) {
    if (ip) {
      iunlockput(ip);
      end_op();
    }
    return -1;
  }
  if (elf.magic != ELF_MAGIC) {
    if (ip) {
      iunlockput(ip);
      end_op();
    }
    return -1;
  }

  pagetable_t pagetable = proc_pagetable(p);
  if (pagetable == 0) {  // 创建地址空间失败
    if (ip) {
      iunlockput(ip);
      end_op();
    }
    return -1;
  }

  // Load program into memory.
  for (int i = 0, off = elf.phoff; i < elf.phnum; i++, off += sizeof(struct proghdr)) {
    struct proghdr ph;
    if (readi(ip, 0, (uint64)&ph, off, sizeof(struct proghdr)) != sizeof(struct proghdr)) HANDLE_BAD;
    if (ph.type != ELF_PROG_LOAD) continue;
    if (ph.memsz < ph.filesz) HANDLE_BAD;
    if (ph.vaddr + ph.memsz < ph.vaddr) HANDLE_BAD;
    uint64 sz1;
    if ((sz1 = uvmalloc(pagetable, sz, ph.vaddr + ph.memsz)) == 0) HANDLE_BAD;
    sz = sz1;
    if (ph.vaddr % PGSIZE != 0) HANDLE_BAD;
    if (loadseg(pagetable, ph.vaddr, ip, ph.off, ph.filesz) < 0) HANDLE_BAD;
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
  if ((sz1 = uvmalloc(pagetable, sz, sz + 2 * PGSIZE)) == 0) HANDLE_BAD;
  sz = sz1;
  uvmclear(pagetable, sz - 2 * PGSIZE);
  sp = sz;
  stackbase = sp - PGSIZE;

  // Push argument strings, prepare rest of stack in ustack.
  for (argc = 0; argv[argc]; argc++) {
    if (argc >= MAXARG) HANDLE_BAD;
    sp -= kstrlen(argv[argc]) + 1;
    sp -= sp % 16;  // riscv sp must be 16-byte aligned
    if (sp < stackbase) HANDLE_BAD;
    if (copyout(pagetable, sp, argv[argc], kstrlen(argv[argc]) + 1) < 0) HANDLE_BAD;
    ustack[argc] = sp;
  }
  ustack[argc] = 0;

  // push the array of argv[] pointers.
  sp -= (argc + 1) * sizeof(uint64);
  sp -= sp % 16;
  if (sp < stackbase) HANDLE_BAD;
  if (copyout(pagetable, sp, (char *)ustack, (argc + 1) * sizeof(uint64)) < 0) HANDLE_BAD;

  // arguments to user main(argc, argv)
  // argc is returned via the system call return
  // value, which goes in a0.
  p->trapframe->a1 = sp;

  // Save program name for debugging.
  char *s, *last;
  for (last = s = path; *s; s++)
    if (*s == '/') last = s + 1;
  ksafestrcpy(p->name, last, sizeof(p->name));

  // Commit to the user image.
  oldpagetable = p->pagetable;
  p->pagetable = pagetable;
  p->sz = sz;
  p->trapframe->epc = elf.entry;  // initial program counter = main
  p->trapframe->sp = sp;          // initial stack pointer
  proc_freepagetable(oldpagetable, oldsz);

  return argc;  // this ends up in a0, the first argument to main(argc, argv)
}

/// @brief Load a program segment into pagetable at virtual address va.
/// @warning va must be page-aligned and the pages from va to va+sz must already be mapped.
/// @param pagetable
/// @param va
/// @param ip
/// @param offset
/// @param sz
/// @return 0 on success, -1 on failure.
static int loadseg(pagetable_t pagetable, uint64 va, struct inode *ip, uint offset, uint sz) {
  if ((va % PGSIZE) != 0) panic("loadseg: va must be page aligned");
  for (int i = 0; i < sz; i += PGSIZE) {
    uint64 pa = walkaddr(pagetable, va + i);
    if (pa == 0) panic("loadseg: address should exist");
    uint n;
    if (sz - i < PGSIZE)
      n = sz - i;
    else
      n = PGSIZE;
    if (readi(ip, 0, (uint64)pa, offset + i, n) != n) return -1;
  }
  return 0;
}
