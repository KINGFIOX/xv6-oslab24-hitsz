#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"

struct cpu cpus[NCPU];

/// @brief the vector of the process
static struct proc proc[NPROC];

static struct proc *initproc;  // openrc or systemd

static int nextpid = 1;
static struct spinlock pid_lock;  // the lock protect the pid allocator(nextpid)

extern void forkret(void);
static void wakeup1(struct proc *chan);
static void freeproc(struct proc *p);

extern char trampoline[];  // trampoline.S

/// @brief only called once by master cpu.
/// initialize the proc table at boot time.
/// @param
void procinit(void) {
  initlock(&pid_lock, "nextpid");
  // 这里并不 lazy
  for (struct proc *p = proc; p < &proc[NPROC]; p++) {
    initlock(&p->lock, "proc");

    // Allocate a page for the process's kernel stack.
    // Map it high in memory, followed by an invalid guard page.
    // 这里就不是 lazy 的
    char *pa = kalloc();
    if (pa == 0) panic("kalloc");
    uint64 va = KSTACK((int)(p - proc));  // 中间会差一个 guard

    // 这个时候, 可能会有两个 pte 对应一个 pa
    // 首先, 所有的物理内存 kvminit 中被映射到了 KERNBASE 后面
    // 然后这里从: 最高虚拟地址开始, 映射到了 pa
    kvmmap(va, (uint64)pa, PGSIZE, PTE_R | PTE_W);
    // 这里的 pa 实际上也不是 pa. 这里的 pa 是内核的地址空间的. 但是(内核的地址空间的子集)与物理地址是 恒等映射
    // 这里的 va 也是在内存地址空间的, 只不过是位于高地址
    // 每个进程私有一个 kernel stack

    p->kstack = va;
  }
  kvminithart();
}

/// @brief
/// @return
/// @warning Must be called with interrupts disabled, to prevent race with process being moved to a different CPU.
int cpuid() { return r_tp(); }

/// @brief
/// @return this CPU's cpu struct.
/// @warning Interrupts must be disabled, because of calling cpuid().
struct cpu *mycpu(void) {
  int id = cpuid();
  struct cpu *c = &cpus[id];
  return c;
}

/// @brief
/// @return the current struct proc *, or zero if none.
struct proc *myproc(void) {
  push_off();
  struct cpu *c = mycpu();
  struct proc *p = c->proc;  // the proc running on cpu
  pop_off();
  return p;
}

int allocpid() {
  acquire(&pid_lock);
  int pid = nextpid;
  nextpid = nextpid + 1;
  release(&pid_lock);

  return pid;
}

/// @brief Look in the process table for an UNUSED proc.
/// @return 0 if not found, otherwise a locked and referenced proc.
/// - If found, initialize state required to run in the kernel, and return with p->lock held.
/// - If there are no free procs, or a memory allocation fails, return 0.
static struct proc *allocproc(void) {
  for (struct proc *p = proc; p < &proc[NPROC]; p++) {
    acquire(&p->lock);  // 防止被其他核心偷偷占用了
    if (p->state == UNUSED) {
      p->pid = allocpid();

      // Allocate a trapframe page. 每个进程私有一个 trapframe
      if ((p->trapframe = (struct trapframe *)kalloc()) == 0) {
        release(&p->lock);
        return 0;
      }

      // An empty user page table.
      // proc_pagetable 不会 kalloc, 只是建立映射
      p->pagetable = proc_pagetable(p);
      if (p->pagetable == 0) {
        freeproc(p);
        release(&p->lock);
        return 0;
      }

      // Set up new context to start executing at forkret,
      // which returns to user space.
      kmemset(&p->context, 0, sizeof(p->context));
      p->context.ra = (uint64)forkret;  // xv6 中, 用 fork 创建进程
      p->context.sp = p->kstack + PGSIZE;

      return p;  // exit the func
    } else {
      release(&p->lock);  // 没用到的锁, 一定要记得释放
    }
  }
  return 0;  // not found
}

/// @brief free a proc structure and the data hanging from it, including user pages.
/// @param p
/// @warning p->lock must be held.
static void freeproc(struct proc *p) {
  if (p->trapframe) kfree((void *)p->trapframe);
  p->trapframe = 0;
  if (p->pagetable) proc_freepagetable(p->pagetable, p->sz);
  p->pagetable = 0;
  p->sz = 0;
  p->pid = 0;
  p->parent = 0;
  p->name[0] = 0;
  p->chan = 0;
  p->killed = 0;
  p->xstate = 0;
  p->state = UNUSED;
}

/// @brief Create a user page table for a given process, with no user memory, but with trampoline pages.
/// @param p
/// @return 0 on allocation failure.
pagetable_t proc_pagetable(struct proc *p) {
  // An empty page table.
  pagetable_t pagetable = uvmcreate();
  if (pagetable == 0) return 0;

  // map the trampoline code (for system call return)
  // at the highest user virtual address.
  // only the supervisor uses it, on the way
  // to/from user space, so not PTE_U.
  if (mappages(pagetable, TRAMPOLINE, PGSIZE, (uint64)trampoline, PTE_R | PTE_X) < 0) {
    uvmfree(pagetable, 0);
    return 0;
  }

  // map the trapframe just below TRAMPOLINE, for trampoline.S.
  if (mappages(pagetable, TRAPFRAME, PGSIZE, (uint64)(p->trapframe), PTE_R | PTE_W) < 0) {
    uvmunmap(pagetable, TRAMPOLINE, 1, 0);
    uvmfree(pagetable, 0);
    return 0;
  }

  return pagetable;
}

/// @brief Free a process's page table, and free the physical memory it refers to.
/// @param pagetable
/// @param sz
void proc_freepagetable(pagetable_t pagetable, uint64 sz) {
  // unmap
  uvmunmap(pagetable, TRAMPOLINE, 1, 0);
  uvmunmap(pagetable, TRAPFRAME, 1, 0);
  // free
  uvmfree(pagetable, sz);
}

// a user program that calls exec("/init")
// od -t xC initcode
uchar initcode[] = {0x17, 0x05, 0x00, 0x00, 0x13, 0x05, 0x45, 0x02, 0x97, 0x05, 0x00, 0x00, 0x93,
                    0x85, 0x35, 0x02, 0x93, 0x08, 0x70, 0x00, 0x73, 0x00, 0x00, 0x00, 0x93, 0x08,
                    0x20, 0x00, 0x73, 0x00, 0x00, 0x00, 0xef, 0xf0, 0x9f, 0xff, 0x2f, 0x69, 0x6e,
                    0x69, 0x74, 0x00, 0x00, 0x24, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};

/// @brief Set up first user process.
/// called once in main() by the master CPU.
/// @globals
/// - (mut)initproc
void userinit(void) {
  struct proc *p = allocproc();
  initproc = p;

  // allocate one user page and copy init's instructions
  // and data into it.
  uvminit(p->pagetable, initcode, sizeof(initcode));
  p->sz = PGSIZE;

  // prepare for the very first "return" from kernel to user.
  p->trapframe->epc = 0;      // user program counter
  p->trapframe->sp = PGSIZE;  // user stack pointer

  ksafestrcpy(p->name, "initcode", sizeof(p->name));
  p->cwd = namei("/");

  p->state = READY;

  release(&p->lock);
}

/// @brief Grow or shrink user memory by n bytes.
/// @param n
/// @return 0 on success, -1 on failure.
int growproc(int n) {
  struct proc *p = myproc();
  uint sz = p->sz;
  if (n > 0) {
    if ((sz = uvmalloc(p->pagetable, sz, sz + n)) == 0) {
      return -1;
    }
  } else if (n < 0) {
    sz = uvmdealloc(p->pagetable, sz, sz + n);
  }
  p->sz = sz;
  return 0;
}

/// @brief Create a new process, copying the parent.
/// Sets up child kernel stack to return as if from fork() system call.
/// @param
/// @return
int fork(void) {
  struct proc *p = myproc();

  // Allocate process.
  struct proc *np;
  if ((np = allocproc()) == 0) {
    return -1;
  }

  // Copy user memory from parent to child.
  if (uvmcopy(p->pagetable, np->pagetable, p->sz) < 0) {
    freeproc(np);
    release(&np->lock);
    return -1;
  }
  np->sz = p->sz;

  np->parent = p;

  // copy saved user registers.
  *(np->trapframe) = *(p->trapframe);

  // Cause fork to return 0 in the child.
  np->trapframe->a0 = 0;

  // increment reference counts on open file descriptors.
  for (int i = 0; i < NOFILE; i++)
    if (p->ofile[i]) np->ofile[i] = filedup(p->ofile[i]);
  np->cwd = idup(p->cwd);

  ksafestrcpy(np->name, p->name, sizeof(p->name));

  int pid = np->pid;

  np->state = READY;

  release(&np->lock);

  return pid;
}

/// @brief Pass p's abandoned children to init. (认贼作父)(指的是: 将自己的 child 全部给 init)
/// @warning Caller must hold p->lock.
/// @param p
void reparent(struct proc *p) {
  for (struct proc *pp = proc; pp < &proc[NPROC]; pp++) {
    // this code uses pp->parent without holding pp->lock.
    // acquiring the lock first could cause a deadlock
    // if pp or a child of pp were also in exit()
    // and about to try to lock p.
    if (pp->parent == p) {
      // pp->parent can't change between the check and the acquire()
      // because only the parent changes it, and we're the parent.
      acquire(&pp->lock);
      pp->parent = initproc;
      // we should wake up init here, but that would require
      // initproc->lock, which would be a deadlock, since we hold
      // the lock on one of init's children (pp). this is why
      // exit() always wakes init (before acquiring any locks).
      release(&pp->lock);
    }
  }
}

/// @brief Exit the current process.  Does not return.
/// An exited process remains in the zombie state until its parent calls wait().
/// @param status
void exit(int status) {
  struct proc *p = myproc();

  if (p == initproc) panic("init exiting");

  // Close all open files.
  for (int fd = 0; fd < NOFILE; fd++) {
    if (p->ofile[fd]) {
      struct file *f = p->ofile[fd];
      fileclose(f);
      p->ofile[fd] = 0;
    }
  }

  begin_op();
  iput(p->cwd);
  end_op();
  p->cwd = 0;

  // we might re-parent a child to init. we can't be precise about
  // waking up init, since we can't acquire its lock once we've
  // acquired any other proc lock. so wake up init whether that's
  // necessary or not. init may miss this wakeup, but that seems
  // harmless.
  acquire(&initproc->lock);
  wakeup1(initproc);
  release(&initproc->lock);

  // grab a copy of p->parent, to ensure that we unlock the same
  // parent we locked. in case our parent gives us away to init while
  // we're waiting for the parent lock. we may then race with an
  // exiting parent, but the result will be a harmless spurious wakeup
  // to a dead or wrong process; proc structs are never re-allocated
  // as anything else.
  acquire(&p->lock);
  struct proc *original_parent = p->parent;
  release(&p->lock);

  // we need the parent's lock in order to wake it up from wait().
  // the parent-then-child rule says we have to lock it first.
  acquire(&original_parent->lock);

  acquire(&p->lock);  // it should be locked before entering sched() and wouldn't be scheduled in again

  // Give any children to init.
  reparent(p);

  // Parent might be sleeping in wait().
  wakeup1(original_parent);

  p->xstate = status;  // record the exit status for wait()
  p->state = ZOMBIE;   // step into the zombie state

  release(&original_parent->lock);

  // Jump into the scheduler, never to return.
  sched();
  // sched() into the scheduler. 但是状态是 zombie, 不是 ready, 因此不会再被调度

  // unreachable!()
  panic("zombie exit");
}

/// @brief Wait for a child process to exit and return its pid.
/// @param addr to save the child's exit status.
/// @return -1 if this process has no children.
int wait(uint64 addr) {
  struct proc *p = myproc();  // caller process

  // hold p->lock for the whole time to avoid lost
  // wakeups from a child's exit().
  acquire(&p->lock);

  for (;;) {
    // Scan through table looking for exited children.
    int havekids = 0;
    for (struct proc *np = proc; np < &proc[NPROC]; np++) {  // np 是迭代器
      // this code uses np->parent without holding np->lock.
      // acquiring the lock first would cause a deadlock,
      // since np might be an ancestor, and we already hold p->lock.
      if (np->parent == p) {
        // np->parent can't change between the check and the acquire()
        // because only the parent changes it, and we're the parent.
        acquire(&np->lock);
        havekids = 1;
        if (np->state == ZOMBIE) {
          // Found one.
          int pid = np->pid;
          if (addr != 0 && copyout(p->pagetable, addr, (char *)&np->xstate, sizeof(np->xstate)) < 0) {
            // failed: copyout
            release(&np->lock);
            release(&p->lock);
            return -1;
          }
          // addr == 0 or copyout succeeded
          freeproc(np);
          release(&np->lock);
          release(&p->lock);
          return pid;
        }
        release(&np->lock);
      }
    }

    // No point waiting if we don't have any children.
    if (!havekids || p->killed) {
      release(&p->lock);
      return -1;
    }

    // Wait for a child to exit.
    sleep(p, &p->lock);  // DOC: wait-sleep
  }
}

/// @brief Per-CPU process scheduler.
/// Each CPU calls scheduler() after setting itself up.
/// Scheduler never returns.  It loops, doing:
///  - choose a process to run.
///  - swtch to start running that process.
///  - eventually that process transfers control
///    via swtch back to the scheduler.
/// @warning only called once by in main() by each CPU.
void scheduler(void) {
  struct cpu *c = mycpu();

  c->proc = 0;
  for (;;) {
    // Avoid deadlock by ensuring that devices can interrupt.
    intr_on();

    int found = 0;
    for (struct proc *p = proc; p < &proc[NPROC]; p++) {  // 循环遍历
      acquire(&p->lock);
      if (p->state == READY) {
        // Switch to chosen process.  It is the process's job
        // to release its lock and then reacquire it
        // before jumping back to us.
        p->state = RUNNING;
        c->proc = p;
        // 1. mycpu()->context := current's context (current 也就是 schedular 的 context)
        // 2. cpu regs := p->context , for p in proc_vec
        swtch(&c->context, &p->context);  // 这是切入一个 process -> 两个入口: 新生进程, 被 sched 的进程
        // 能从这里返回, 只能说明: 进程被 sched

        // Process is done running for now.
        // It should have changed its p->state before coming back.
        c->proc = 0;

        found = 1;
      }
      release(&p->lock);
    }
    if (found == 0) {
      intr_on();
      asm volatile("wfi");
    }
  }
}

/// @brief Switch to scheduler.
/// @warning Must hold only p->lock and have changed proc->state.
/// the lock would be released in scheduler after swtch()
/// @warning It should be proc->intena and proc->noff
/// but that would break in the few places where a lock is held but there's no process
void sched(void) {
  struct proc *p = myproc();

  if (!holding(&p->lock)) panic("sched p->lock");
  if (mycpu()->noff != 1) panic("sched locks");
  if (p->state == RUNNING) panic("sched running");
  if (intr_get()) panic("sched interruptible");  // 切换的时候, 不能有中断

  // Saves and restores intena because intena is a property of this kernel thread, not this CPU.
  int intena = mycpu()->intena;
  // 1. myproc()->context := current's context (current 也就是 sched 的 context)
  // 2. cpu regs := mycpu()->context (mycpu()->context 是 scheduler 的 context)
  // mycpu()-> context was
  swtch(&p->context, &mycpu()->context);  // 这是切出一个 process -> into schedular
  // switch to scheduler
  // 能从这里返回: 说明, 又被调度器看上了, 又进来了

  mycpu()->intena = intena;
}

/// @brief Give up the CPU for one scheduling round.
void yield(void) {
  struct proc *p = myproc();
  acquire(&p->lock);
  p->state = READY;
  sched();  // 被切出, 被调度切入, 这个过程中, 依然持有锁
  release(&p->lock);
}

// A fork child's very first scheduling by scheduler()
// will swtch to forkret.
void forkret(void) {
  static int first = 1;

  // Still holding p->lock from scheduler.
  release(&myproc()->lock);

  if (first) {
    // File system initialization must be run in the context of a
    // regular process (e.g., because it calls sleep), and thus cannot
    // be run from main().
    first = 0;
    fsinit(ROOTDEV);
  }

  usertrapret();
}

// Atomically release lock and sleep on chan.
// Reacquires lock when awakened.
void sleep(void *chan, struct spinlock *lk) {
  struct proc *p = myproc();

  // Must acquire p->lock in order to change p->state and then call sched.
  // Once we hold p->lock, we can be guaranteed that we won't miss any wakeup (wakeup locks p->lock),
  // so it's okay to release lk.
  if (lk != &p->lock) {  // DOC: sleeplock0
    acquire(&p->lock);   // DOC: sleeplock1
    release(lk);
  }

  // Go to sleep.
  p->chan = chan;
  p->state = SLEEPING;

  sched();

  // Tidy up.
  p->chan = 0;

  // Reacquire original lock.
  if (lk != &p->lock) {
    release(&p->lock);
    acquire(lk);
  }
}

/// @brief Wake up all processes sleeping on chan. Must be called without any p->lock.
/// @param chan
void wakeup(void *chan) {
  for (struct proc *p = proc; p < &proc[NPROC]; p++) {
    acquire(&p->lock);
    if (p->state == SLEEPING && p->chan == chan) {
      p->state = READY;
    }
    release(&p->lock);
  }
}

/// @brief Wake up p if it is sleeping in wait(); used by exit().
/// @param p
/// @warning Caller must hold p->lock.
static void wakeup1(struct proc *p) {
  if (!holding(&p->lock)) panic("wakeup1");
  if (p->chan == p && p->state == SLEEPING) {
    p->state = READY;
  }
}

/// @brief Kill the process with the given pid.
/// The victim won't exit until it tries to return to user space (see usertrap() in trap.c).
/// @param pid
/// @return
int kill(int pid) {
  for (struct proc *p = proc; p < &proc[NPROC]; p++) {
    acquire(&p->lock);
    if (p->pid == pid) {
      p->killed = 1;
      if (p->state == SLEEPING) {
        // Wake process from sleep().
        p->state = READY;
      }
      release(&p->lock);
      return 0;
    }
    release(&p->lock);
  }
  return -1;
}

/// @brief Copy to either a user address, or kernel address, depending on usr_dst.
/// @param user_dst
/// @param dst
/// @param src
/// @param len
/// @return 0 on success, -1 on error.
int either_copyout(int user_dst, uint64 dst, void *src, uint64 len) {
  struct proc *p = myproc();
  if (user_dst) {
    return copyout(p->pagetable, dst, src, len);
  } else {
    kmemmove((char *)dst, src, len);
    return 0;
  }
}

/// @brief Copy from either a user address, or kernel address,  depending on usr_src. (可选择)
/// @param dst
/// @param user_src (user_src != NULL) dst <- user_src
/// @param src (user_src == NULL) dst <- src
/// @param len
/// @return 0 on success, -1 on error.
int either_copyin(void *dst, int user_src, uint64 src, uint64 len) {
  struct proc *p = myproc();
  if (user_src) {
    return copyin(p->pagetable, dst, src, len);
  } else {
    kmemmove(dst, (char *)src, len);
    return 0;
  }
}

/// @brief Print a process listing to console.  For debugging.
/// Runs when user types ^P on console.  No lock to avoid wedging a stuck machine further.
/// @param
void procdump(void) {
  static char *states[] = {
      [UNUSED] "unused", [SLEEPING] "sleep ", [READY] "runble", [RUNNING] "run   ", [ZOMBIE] "zombie"};

  printf("\n");
  for (struct proc *p = proc; p < &proc[NPROC]; p++) {
    if (p->state == UNUSED) continue;
    char *state;
    if (p->state >= 0 && p->state < NELEM(states) && states[p->state])
      state = states[p->state];
    else
      state = "???";
    printf("%d %s %s", p->pid, state, p->name);
    printf("\n");
  }
}
