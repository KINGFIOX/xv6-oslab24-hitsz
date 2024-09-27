// Mutual exclusion spin locks.

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "riscv.h"
#include "proc.h"
#include "defs.h"

void initlock(struct spinlock *lk, char *name) {
  lk->name = name;
  lk->locked = 0;
  lk->cpu = 0;
}

/// @brief Acquire the lock. Loops (spins) until the lock is acquired.
/// @param lk
/// @warning close the interrupt to avoid deadlock.
void acquire(struct spinlock *lk) {
  push_off();  // disable interrupts to avoid deadlock.
  if (holding(lk)) panic("acquire");

  // On RISC-V, sync_lock_test_and_set turns into an atomic swap:
  //   a5 = 1
  //   s1 = &lk->locked
  //   amoswap.w.aq a5, a5, (s1)
  while (__sync_lock_test_and_set(&lk->locked, 1) != 0);

  // Tell the C compiler and the processor to not move loads or stores
  // past this point, to ensure that the critical section's memory
  // references happen strictly after the lock is acquired.
  // On RISC-V, this emits a fence instruction.
  __sync_synchronize();

  // Record info about lock acquisition for holding() and debugging.
  lk->cpu = mycpu();
}

// Release the lock.
void release(struct spinlock *lk) {
  if (!holding(lk)) panic("release");

  lk->cpu = 0;

  // Tell the C compiler and the CPU to not move loads or stores
  // past this point, to ensure that all the stores in the critical
  // section are visible to other CPUs before the lock is released,
  // and that loads in the critical section occur strictly before
  // the lock is released.
  // On RISC-V, this emits a fence instruction.
  __sync_synchronize();

  // Release the lock, equivalent to lk->locked = 0.
  // This code doesn't use a C assignment, since the C standard
  // implies that an assignment might be implemented with
  // multiple store instructions.
  // On RISC-V, sync_lock_release turns into an atomic swap:
  //   s1 = &lk->locked
  //   amoswap.w zero, zero, (s1)
  __sync_lock_release(&lk->locked);

  // 这里是 pop_off, 而不能是直接的开中断.
  // 我们要做的是: 中断前后的状态机, 除了临界资源修改外, 其他的状态不做任何的改变
  // 就是有一种可能: 之前是关中断的, 又关了一遍, 但是这里不小心打开了
  pop_off();
}

/// @brief Check whether this cpu is holding the lock.
/// @param lk
/// @return (bool)
/// @warning Interrupts must be off, when entering this function.
int holding(struct spinlock *lk) {
  int r;
  r = (lk->locked && lk->cpu == mycpu());
  return r;
}

// push_off/pop_off are like intr_off()/intr_on() except that they are matched:
// it takes two pop_off()s to undo two push_off()s.  Also, if interrupts
// are initially off, then push_off, pop_off leaves them off.

void push_off(void) {
  int old = intr_get();

  intr_off();
  // 只会记录: 第一层的 push_off() 时的中断状态, 因为后面不管怎么样都是 disable
  if (mycpu()->noff == 0) mycpu()->intena = old;
  mycpu()->noff += 1;
}

void pop_off(void) {
  struct cpu *c = mycpu();
  if (intr_get()) panic("pop_off - interruptible");
  if (c->noff < 1) panic("pop_off");  // 出现了 pop 和 push 不匹配的情况
  c->noff -= 1;
  if (c->noff == 0 && c->intena) intr_on();
}
