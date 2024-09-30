//
// Support functions for system calls that involve file descriptors.
//

#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "fs.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "file.h"
#include "stat.h"
#include "proc.h"

struct devsw devsw[NDEV];

/// @brief 这个就是: 打开文件表
static struct {
  struct spinlock lock;
  struct file file[NFILE];
} ftable;

void fileinit(void) { initlock(&ftable.lock, "ftable"); }

/// @brief Allocate a file structure.
struct file *filealloc(void) {
  acquire(&ftable.lock);
  for (struct file *f = ftable.file; f < ftable.file + NFILE; f++) {
    if (f->ref == 0) {  // 分配
      f->ref = 1;
      release(&ftable.lock);
      return f;
    }
  }
  release(&ftable.lock);
  return 0;
}

/// @brief Increment ref count for file f.
/// @param f
struct file *filedup(struct file *f) {
  acquire(&ftable.lock);
  if (f->ref < 1) panic("filedup");
  f->ref++;
  release(&ftable.lock);
  return f;
}

/// @brief Close file f.  (Decrement ref count, close when reaches 0.)
/// @param f
void fileclose(struct file *f) {
  acquire(&ftable.lock);
  if (f->ref < 1) panic("fileclose");
  if (--f->ref > 0) {
    release(&ftable.lock);  // 引用计数还有东西
    return;
  }
  // f->ref == 0
  struct file ff = *f;  // copy (old)
  f->ref = 0;
  f->type = FD_NONE;
  release(&ftable.lock);

  if (ff.type == FD_PIPE) {
    pipeclose(ff.pipe, ff.writable);
  } else if (ff.type == FD_INODE || ff.type == FD_DEVICE) {
    begin_op();
    iput(ff.ip);
    end_op();
  }
}

/// @brief Get metadata about file f.
/// @param f
/// @param addr a user virtual address, pointing to a struct stat.
/// @return
int filestat(struct file *f, uint64 addr) {
  if (f->type == FD_INODE || f->type == FD_DEVICE) {
    ilock(f->ip);
    struct stat st;
    stati(f->ip, &st);
    iunlock(f->ip);
    struct proc *p = myproc();
    if (copyout(p->pagetable, addr, (char *)&st, sizeof(st)) < 0) return -1;
    return 0;
  }
  return -1;
}

/// @brief Read from file f.
/// @param f
/// @param addr a user virtual address
/// @param n
/// @return
int fileread(struct file *f, uint64 addr, int n) {
  if (f->readable == 0) return -1;
  int r = 0;
  if (f->type == FD_PIPE) {
    r = piperead(f->pipe, addr, n);
  } else if (f->type == FD_DEVICE) {
    if (f->major < 0 || f->major >= NDEV || !devsw[f->major].read) return -1;  // check
    r = devsw[f->major].read(1, addr, n);
  } else if (f->type == FD_INODE) {
    ilock(f->ip);
    if ((r = readi(f->ip, 1, addr, f->off, n)) > 0) f->off += r;  // seek, 偏移
    iunlock(f->ip);
  } else {
    panic("fileread");
  }

  return r;
}

/// @brief Write to file f.
/// @param f
/// @param addr a user virtual address.
/// @param n
/// @return
int filewrite(struct file *f, uint64 addr, int n) {
  if (f->writable == 0) return -1;

  int ret = 0;
  if (f->type == FD_PIPE) {
    ret = pipewrite(f->pipe, addr, n);
  } else if (f->type == FD_DEVICE) {
    if (f->major < 0 || f->major >= NDEV || !devsw[f->major].write) return -1;
    ret = devsw[f->major].write(1, addr, n);
  } else if (f->type == FD_INODE) {
    // write a few blocks at a time to avoid exceeding
    // the maximum log transaction size, including
    // i-node, indirect block, allocation blocks,
    // and 2 blocks of slop for non-aligned writes.
    // this really belongs lower down, since writei()
    // might be writing a device like the console.
    int max = ((MAXOPBLOCKS - 1 - 1 - 2) / 2) * BSIZE;
    int i = 0;
    while (i < n) {
      int n1 = n - i;
      if (n1 > max) n1 = max;

      begin_op();
      ilock(f->ip);
      int r;
      if ((r = writei(f->ip, 1, addr + i, f->off, n1)) > 0) f->off += r;
      iunlock(f->ip);
      end_op();

      if (r < 0) break;
      if (r != n1) panic("short filewrite");
      i += r;
    }
    ret = (i == n ? n : -1);
  } else {
    panic("filewrite");
  }

  return ret;
}
