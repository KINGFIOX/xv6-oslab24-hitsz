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

/* ---------- ---------- heap ---------- ---------- */

typedef struct {
  uint timestamp;
  buf_t *pointer;
} entry_t;

typedef struct {
  int size;
  entry_t array[NBUF];
} MinHeap;

// 交换两个 entry_t 的值
void swap(entry_t *x, entry_t *y) {
  entry_t temp = *x;
  *x = *y;
  *y = temp;
}

void heapifyUp(MinHeap *heap, int idx) {
  int parent = (idx - 1) / 2;
  if (idx && heap->array[parent].timestamp > heap->array[idx].timestamp) {
    swap(&heap->array[parent], &heap->array[idx]);
    heapifyUp(heap, parent);
  }
}

void heapifyDown(MinHeap *heap, int idx) {
  int left = 2 * idx + 1;
  int right = 2 * idx + 2;
  int smallest = idx;
  if (left < heap->size && heap->array[left].timestamp < heap->array[smallest].timestamp) smallest = left;
  if (right < heap->size && heap->array[right].timestamp < heap->array[smallest].timestamp) smallest = right;
  if (smallest != idx) {
    swap(&heap->array[smallest], &heap->array[idx]);
    heapifyDown(heap, smallest);
  }
}

void insertMinHeap(MinHeap *heap, entry_t key) {
  if (heap->size == NBUF) panic("heap full\n");
  heap->array[heap->size] = key;
  heap->size++;
  heapifyUp(heap, heap->size - 1);
}

entry_t extractMin(MinHeap *heap) {
  if (heap->size <= 0) {
    entry_t empty = {0xffffffff, 0};
    return empty;
  }
  if (heap->size == 1) {
    heap->size--;
    return heap->array[0];
  }
  entry_t root = heap->array[0];
  heap->array[0] = heap->array[heap->size - 1];
  heap->size--;
  heapifyDown(heap, 0);
  return root;
}

entry_t getMin(MinHeap *heap) {
  if (heap->size <= 0) {
    entry_t empty = {0xffffffff, 0};
    return empty;
  }
  return heap->array[0];
}

/* ---------- ---------- heap ---------- ---------- */

typedef struct {
  struct spinlock lock;
  buf_t head;  // 已分配
} bucket_t;

struct {
  struct spinlock freelock;
  MinHeap freelist;  // 空闲

  buf_t buf[NBUF];

  bucket_t buckets[NBUCKET];
} bcache;

void binit(void) {
  initlock(&bcache.freelock, "freelock");

  bcache.freelist.size = 0;

  for (int i = 0; i < NBUCKET; i++) {
    initlock(&bcache.buckets[i].lock, "bucket");
    bcache.buckets[i].head.next = 0;
    bcache.buckets[i].head.prev = 0;
  }

  // Create linked list of buffers
  for (int i = 0; i < NBUF; i++) {
    initsleeplock(&bcache.buf[i].lock, "buffer");
    entry_t entry = {0, &bcache.buf[i]};
    insertMinHeap(&bcache.freelist, entry);
  }
}

// Look through buffer cache for block on device dev.
// If not found, allocate a buffer.
// In either case, return locked buffer.
static struct buf *bget(uint dev, uint blockno) {
  int hash = blockno % NBUCKET;

  // for (buf_t *cur = bcache.buckets[hash].head.next; cur; cur = cur->next) {
  //   if (cur->dev == dev && cur->blockno == blockno) {
  //     acquire(&bcache.buckets[hash].lock);
  //     if (cur->dev == dev && cur->blockno == blockno) {
  //       cur->refcnt++;
  //       cur->timestamp = ticks;
  //       release(&bcache.buckets[hash].lock);  // 找到, 解锁, 返回
  //       acquiresleep(&cur->lock);
  //       return cur;
  //     }
  //     release(&bcache.buckets[hash].lock);
  //   }
  // }

  acquire(&bcache.buckets[hash].lock);
  for (buf_t *cur = bcache.buckets[hash].head.next; cur; cur = cur->next) {
    if (cur->dev == dev && cur->blockno == blockno) {
      cur->refcnt++;
      cur->timestamp = ticks;
      release(&bcache.buckets[hash].lock);  // 找到, 解锁, 返回
      acquiresleep(&cur->lock);
      return cur;
    }
  }

retry:
  acquire(&bcache.freelock);
  entry_t iter = getMin(&bcache.freelist);
  if (iter.pointer == 0) {
    release(&bcache.freelock);
    goto retry;  // 有点自旋的意思
  }
  iter = extractMin(&bcache.freelist);
  buf_t *cur = iter.pointer;

  // pop cur from free
  release(&bcache.freelock);

  cur->dev = dev;  // init
  cur->blockno = blockno;
  cur->valid = 0;
  cur->refcnt = 1;
  cur->timestamp = ticks;

  // push cur to head
  cur->next = bcache.buckets[hash].head.next;
  cur->prev = &bcache.buckets[hash].head;
  bcache.buckets[hash].head.next = cur;
  if (cur->next) {
    cur->next->prev = cur;
  }
  release(&bcache.buckets[hash].lock);

  acquiresleep(&cur->lock);
  return cur;

  // failed
  release(&bcache.freelock);
  release(&bcache.buckets[hash].lock);
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

  int hash = b->blockno % NBUCKET;

  releasesleep(&b->lock);

  acquire(&bcache.buckets[hash].lock);
  b->refcnt--;
  if (b->refcnt == 0) {
    if (b->next) {
      b->next->prev = b->prev;
    }
    b->prev->next = b->next;

    acquire(&bcache.freelock);
    entry_t entry = {ticks, b};
    b->next = 0;
    b->prev = 0;
    insertMinHeap(&bcache.freelist, entry);
    release(&bcache.freelock);
  }
  release(&bcache.buckets[hash].lock);
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
