#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "date.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "proc.h"

// void exit(int status);
uint64 sys_exit(void) {
  int n;
  if (argint(0, &n) < 0) return -1;
  exit(n);
  return 0;  // not reached
}

// pid_t getpid(void);
uint64 sys_getpid(void) { return myproc()->pid; }

// pid_t fork(void);
uint64 sys_fork(void) { return fork(); }

/// @brief
/// @param (reg a0)
/// @return
uint64 sys_wait(void) {
  uint64 p;
  if (argaddr(0, &p) < 0) return -1;
  return wait(p);
}

// void *sbrk(intptr_t increment);
uint64 sys_sbrk(void) {
  int n;
  if (argint(0, &n) < 0) return -1;
  uint64 addr = myproc()->sz;
  if (growproc(n) < 0) return -1;
  return addr;
}

// unsigned int sleep(unsigned int seconds);
uint64 sys_sleep(void) {
  int n;
  if (argint(0, &n) < 0) return -1;
  acquire(&tickslock);
  uint ticks0 = ticks;
  while (ticks - ticks0 < n) {
    if (myproc()->killed) {  // check the status of the process
      release(&tickslock);
      return -1;
    }
    sleep(&ticks, &tickslock);
  }
  release(&tickslock);
  return 0;
}

/// @brief
/// @param
/// @return
uint64 sys_kill(void) {
  int pid;
  if (argint(0, &pid) < 0) return -1;
  return kill(pid);
}

// return how many clock tick interrupts have occurred
// since start.
uint64 sys_uptime(void) {
  acquire(&tickslock);
  uint xticks = ticks;
  release(&tickslock);
  return xticks;
}

/// @brief modify the process name
/// @return 0: success, -1: failed
uint64 sys_rename(void) {
  char name[16];
  int len = argstr(0, name, MAXPATH);
  if (len < 0) {
    return -1;
  }
  struct proc *p = myproc();
  kmemmove(p->name, name, len);
  p->name[len] = '\0';
  return 0;
}
