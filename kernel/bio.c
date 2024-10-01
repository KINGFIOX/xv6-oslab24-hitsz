// Buffer cache.
//
// The buffer cache is a linked list of buf structures holding
// cached copies of disk block contents.  Caching disk blocks
// in memory reduces the number of disk reads and also provides
// a synchronization point for disk blocks used by multiple processes.
//
// Interface:
// * To get a buffer for a particular disk block, call bread.
// * After changing buffer data, call bwrite to write it to disk.
// * When done with the buffer, call brelse.
// * Do not use the buffer after calling brelse.
// * Only one process at a time can use a buffer,
//     so do not keep them longer than necessary.

#include "types.h"
#include "param.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "riscv.h"
#include "defs.h"
#include "fs.h"
#include "buf.h"

#define NBUCKET 13

typedef struct buf buf_t;

typedef struct {
  struct spinlock lock;
  buf_t head;  // 已分配
} bucket_t;

struct {
  struct spinlock freelock;
  buf_t free;  // 空闲

  buf_t buf[NBUF];

  bucket_t buckets[NBUCKET];
} bcache;

void binit(void) {
  initlock(&bcache.freelock, "freelock");

  for (int i = 0; i < NBUCKET; i++) {
    initlock(&bcache.buckets[i].lock, "bucket");
    bcache.buckets[i].head.next = 0;
    bcache.buckets[i].head.prev = 0;
    bcache.free.next = 0;
    bcache.free.prev = 0;
  }

  // Create linked list of buffers
  for (int i = 0; i < NBUF; i++) {
    initsleeplock(&bcache.buf[i].lock, "buffer");
    bcache.buf[i].next = bcache.free.next;
    bcache.buf[i].prev = &bcache.free;
    bcache.free.next = &bcache.buf[i];
    if (bcache.buf[i].next) {
      bcache.buf[i].next->prev = &bcache.buf[i];
    }
  }
}

// Look through buffer cache for block on device dev.
// If not found, allocate a buffer.
// In either case, return locked buffer.
static struct buf *bget(uint dev, uint blockno) {
  int hash = blockno % NBUCKET;
  acquire(&bcache.buckets[hash].lock);
  for (buf_t *cur = bcache.buckets[hash].head.next; cur; cur = cur->next) {
    if (cur->dev == dev && cur->blockno == blockno) {
      cur->refcnt++;
      release(&bcache.buckets[hash].lock);  // 找到, 解锁, 返回
      acquiresleep(&cur->lock);
      return cur;
    }
  }

  while (1) {
    acquire(&bcache.freelock);
    buf_t *cur = bcache.free.next;
    if (cur == 0) {
      release(&bcache.freelock);
      continue;  // 有点自旋的意思
    }
    // pop cur from free
    if (cur->next != 0) {
      cur->next->prev = cur->prev;
    }
    bcache.free.next = cur->next;
    release(&bcache.freelock);

    cur->dev = dev;  // init
    cur->blockno = blockno;
    cur->valid = 0;
    cur->refcnt = 1;

    // push cur to head
    cur->next = bcache.buckets[blockno % NBUCKET].head.next;
    cur->prev = &bcache.buckets[blockno % NBUCKET].head;
    bcache.buckets[blockno % NBUCKET].head.next = cur;
    if (cur->next) {
      cur->next->prev = cur;
    }
    release(&bcache.buckets[blockno % NBUCKET].lock);

    acquiresleep(&cur->lock);
    return cur;
  }

  // failed
  release(&bcache.freelock);
  release(&bcache.buckets[blockno % NBUCKET].lock);
  panic("bget: no buffers");
}

// Return a locked buf with the contents of the indicated block.
struct buf *bread(uint dev, uint blockno) {
  struct buf *b = bget(dev, blockno);
  if (!b->valid) {
    virtio_disk_rw(b, 0);
    b->valid = 1;
  }
  return b;
}

// Write b's contents to disk.  Must be locked.
void bwrite(struct buf *b) {
  if (!holdingsleep(&b->lock)) panic("bwrite");
  virtio_disk_rw(b, 1);
}

// Release a locked buffer.
// Move to the head of the most-recently-used list.
void brelse(struct buf *b) {
  if (!holdingsleep(&b->lock)) panic("brelse");

  releasesleep(&b->lock);

  acquire(&bcache.buckets[b->blockno % NBUCKET].lock);
  b->refcnt--;
  if (b->refcnt == 0) {
    if (b->next) {
      b->next->prev = b->prev;
    }
    b->prev->next = b->next;

    acquire(&bcache.freelock);
    b->next = bcache.free.next;
    b->prev = &bcache.free;
    bcache.free.next = b;
    if (b->next) {
      b->next->prev = b;
    }
    release(&bcache.freelock);
  }
  release(&bcache.buckets[b->blockno % NBUCKET].lock);
}

void bpin(struct buf *b) {
  acquire(&bcache.buckets[b->blockno % NBUCKET].lock);
  b->refcnt++;
  release(&bcache.buckets[b->blockno % NBUCKET].lock);
}

void bunpin(struct buf *b) {
  acquire(&bcache.buckets[b->blockno % NBUCKET].lock);
  b->refcnt--;
  release(&bcache.buckets[b->blockno % NBUCKET].lock);
}
