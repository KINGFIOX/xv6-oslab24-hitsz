#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "spinlock.h"
#include "proc.h"
#include "fs.h"
#include "sleeplock.h"
#include "file.h"

#define PIPESIZE 512

struct pipe {
  struct spinlock lock;
  char data[PIPESIZE];
  uint nread;     // number of bytes read. 指针, 队头
  uint nwrite;    // number of bytes written. 指针, 队尾
  int readopen;   // (bool) read fd is still open
  int writeopen;  // write fd is still open
};

/// @brief
/// @param f0
/// @param f1
/// @return
int pipealloc(struct file **f0, struct file **f1) {
  struct pipe *pi = 0;
  *f0 = *f1 = 0;
  *f0 = filealloc();
  *f1 = filealloc();
  if (*f0 == 0 || *f1 == 0) {  // failed
    if (pi) kfree((char *)pi);
    if (*f0) fileclose(*f0);
    if (*f1) fileclose(*f1);
    return -1;
  };
  pi = (struct pipe *)kalloc();
  if (pi == 0) {  // failed
    if (pi) kfree((char *)pi);
    if (*f0) fileclose(*f0);
    if (*f1) fileclose(*f1);
    return -1;
  };
  pi->readopen = 1;
  pi->writeopen = 1;
  pi->nwrite = 0;
  pi->nread = 0;
  initlock(&pi->lock, "pipe");
  (*f0)->type = FD_PIPE;
  (*f0)->readable = 1;
  (*f0)->writable = 0;
  (*f0)->pipe = pi;
  (*f1)->type = FD_PIPE;
  (*f1)->readable = 0;
  (*f1)->writable = 1;
  (*f1)->pipe = pi;
  return 0;
}

/// @brief
/// @param pi
/// @param writable bool
void pipeclose(struct pipe *pi, int writable) {
  acquire(&pi->lock);
  if (writable) {
    pi->writeopen = 0;
    wakeup(&pi->nread);
  } else {
    pi->readopen = 0;
    wakeup(&pi->nwrite);
  }
  if (pi->readopen == 0 && pi->writeopen == 0) {
    release(&pi->lock);
    kfree((char *)pi);
  } else {
    release(&pi->lock);
  }
}

/// @brief
/// @param pi
/// @param addr
/// @param n
/// @return
int pipewrite(struct pipe *pi, uint64 addr, int n) {
  struct proc *pr = myproc();
  acquire(&pi->lock);
  int i = 0;
  for (; i < n; i++) {
    // 队列满了
    while (pi->nwrite == pi->nread + PIPESIZE) {  // DOC: pipewrite-full
      if (pi->readopen == 0 || pr->killed) {
        release(&pi->lock);
        return -1;
      }
      wakeup(&pi->nread);
      sleep(&pi->nwrite, &pi->lock);
    }
    char ch;
    if (copyin(pr->pagetable, &ch, addr + i, 1) == -1) break;
    pi->data[pi->nwrite++ % PIPESIZE] = ch;
  }
  wakeup(&pi->nread);
  release(&pi->lock);
  return i;
}

/// @brief
/// @param pi 指向管道结构的指针, 表示要读取的管道
/// @param addr 用户空间的虚拟地址, 数据将被复制到此处
/// @param n 要读取的最大字节数
/// @return
int piperead(struct pipe *pi, uint64 addr, int n) {
  struct proc *pr = myproc();
  acquire(&pi->lock);
  // 队列空
  while (pi->nread == pi->nwrite && pi->writeopen) {  // DOC: pipe-empty
    if (pr->killed) {
      release(&pi->lock);
      return -1;
    }
    sleep(&pi->nread, &pi->lock);  // DOC: piperead-sleep
  }
  int i = 0;
  for (; i < n; i++) {  // DOC: piperead-copy
    if (pi->nread == pi->nwrite) break;
    char ch = pi->data[pi->nread++ % PIPESIZE];
    if (copyout(pr->pagetable, addr + i, &ch, 1) == -1) break;
  }
  wakeup(&pi->nwrite);  // DOC: piperead-wakeup
  release(&pi->lock);
  return i;
}
