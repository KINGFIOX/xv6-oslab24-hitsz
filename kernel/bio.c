//! Buffer cache.
//!
//! The buffer cache is a linked list of buf structures holding
//! cached copies of disk block contents.  Caching disk blocks
//! in memory reduces the number of disk reads and also provides
//! a synchronization point for disk blocks used by multiple processes.
//!
//! Interface:
//! * To get a buffer for a particular disk block, call bread.
//! * After changing buffer data, call bwrite to write it to disk.
//! * When done with the buffer, call brelse.
//! * Do not use the buffer after calling brelse.
//! * Only one process at a time can use a buffer, so do not keep them longer than necessary.

#include "types.h"
#include "param.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "riscv.h"
#include "defs.h"
#include "fs.h"
#include "buf.h"

static struct {
  struct spinlock lock;
  struct buf buf[NBUF];

  // Linked list of all buffers, through prev/next.
  // Sorted by how recently the buffer was used.
  // head.next is most recent, head.prev is least.
  // 这个 bcache 链表在初始化的时候就将所有的节点创建好了, but with no data, no valid
  struct buf head;
} bcache;

/// @brief bcache init
/// @param
void binit(void) {
  initlock(&bcache.lock, "bcache");

  // Create linked list of buffers
  bcache.head.prev = &bcache.head;
  bcache.head.next = &bcache.head;
  // struct buf buf[NBUF]; in bcache
  for (struct buf *b = bcache.buf; b < bcache.buf + NBUF; b++) {
    b->next = bcache.head.next;  // insert to the head
    b->prev = &bcache.head;
    initsleeplock(&b->lock, "buffer");
    bcache.head.next->prev = b;
    bcache.head.next = b;
  }
}

/// @brief Look through buffer cache for block on device dev.
/// 这个相当于是: struct buf 的构造函数了
/// @param dev
/// @param blockno
/// @warning return with lock
/// @return
/// - If not found, allocate a buffer.
/// - In either case, return locked buffer.
///
/// @globals
/// - (mut) bcache
static struct buf *bget(uint dev, uint blockno) {
  acquire(&bcache.lock);

  // Is the block already cached?
  for (struct buf *b = bcache.head.next; b != &bcache.head; b = b->next) {
    if (b->dev == dev && b->blockno == blockno) {
      b->refcnt++;
      release(&bcache.lock);
      acquiresleep(&b->lock);
      return b;
    }
  }

  // Not cached.
  // Recycle the least recently used (LRU) unused buffer.
  // cache 中, 没找到
  // 回收一个没用的, 也就是 refcnt == 0 的
  for (struct buf *b = bcache.head.prev; b != &bcache.head; b = b->prev) {
    if (b->refcnt == 0) {  // 但是这里好像不会 victim 之类的
      b->dev = dev;
      b->blockno = blockno;
      b->valid = 0;
      b->refcnt = 1;
      release(&bcache.lock);
      acquiresleep(&b->lock);
      return b;
    }
  }
  panic("bget: no buffers");
}

/// @brief
/// @param dev
/// @param blockno
/// @return a locked buf with the contents of the indicated block.
/// @warning return with lock
struct buf *bread(uint dev, uint blockno) {
  struct buf *b = bget(dev, blockno);  // bget 只是分配, 而 bread 会从 disk 中读取数据
  if (!b->valid) {
    // 如果这里是 valid, 那么就说明实际上已经缓存了, 就不用再次 read 了
    virtio_disk_rw(b, 0);
    b->valid = 1;
  }
  return b;
}

/// @brief Write b's contents to disk.  Must be locked.
/// @warning should with lock
void bwrite(struct buf *b) {
  if (!holdingsleep(&b->lock)) panic("bwrite");
  virtio_disk_rw(b, 1);
}

/// @brief Release a locked buffer. Move to the head of the most-recently-used list.
/// 这个就是 struct buf 的析构函数, 内部维护引用计数
/// @param b
/// @warning should with lock
/// @globals
/// - (mut) bcache
void brelse(struct buf *b) {
  if (!holdingsleep(&b->lock)) panic("brelse");

  releasesleep(&b->lock);

  acquire(&bcache.lock);
  b->refcnt--;
  if (b->refcnt == 0) {
    // no one is waiting for it.
    b->next->prev = b->prev;  // remove from current location
    b->prev->next = b->next;
    b->next = bcache.head.next;  // push to the head
    b->prev = &bcache.head;
    bcache.head.next->prev = b;
    bcache.head.next = b;
  }

  release(&bcache.lock);
}

// this 2 function below are controlling the refcnt in struct buf

void bpin(struct buf *b) {
  acquire(&bcache.lock);  // 不过, 为什么这个要获取 bcache 的 spinlock 呢 ?
  b->refcnt++;
  release(&bcache.lock);
}

void bunpin(struct buf *b) {
  acquire(&bcache.lock);
  b->refcnt--;
  release(&bcache.lock);
}
