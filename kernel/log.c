#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "fs.h"
#include "buf.h"

//! Simple logging that allows concurrent FS system calls.
//!
//! A log transaction contains the updates of multiple FS system calls.
//! The logging system only commits when there are no FS system calls active.
//!  Thus there is never any reasoning required about whether a commit might
//! write an uncommitted system call's updates to disk.
//!
//! A system call should call begin_op()/end_op() to mark its start and end.
//! Usually begin_op() just increments the count of in-progress FS system calls and returns.
//! But if it thinks the log is close to running out, it sleeps until the last outstanding end_op() commits.
//!
//! The log is a physical re-do log containing disk blocks.
//! The on-disk log format:
//!   header block, containing block #s for block A, B, C, ...
//!   block A
//!   block B
//!   block C
//!   ...
//! Log appends are synchronous.

/// @brief Contents of the header block,
/// used for both the on-disk header block and to keep track in memory of logged block# before commit.
struct logheader {
  int n;
  int block[LOGSIZE];
};

struct log {
  struct spinlock lock;
  int start;
  int size;
  int outstanding;  // how many FS sys calls are executing. (未解决的, 杰出的)
  int committing;   // in commit(), please wait.
  int dev;
  struct logheader lh;
};

static struct log log;

static void recover_from_log(void);
static void commit();

/// @brief called once by the master cpu by fsinit() in fs.c
/// @param dev
/// @param sb
void initlog(int dev, struct superblock *sb) {
  if (sizeof(struct logheader) >= BSIZE) panic("initlog: too big logheader");
  initlock(&log.lock, "log");
  log.start = sb->logstart;
  log.size = sb->nlog;
  log.dev = dev;
  recover_from_log();
}

/// @brief Copy committed blocks from log to their home location
/// 我发现一件事情:
/// 他会先将 home block 读出来 dbuf，然后将 log block 也读出来, 覆盖 dbuf，然后再写回磁盘
static void install_trans(void) {
  for (int i = 0; i < log.lh.n; i++) {
    // 这里的 +1 是为了跳过 header
    struct buf *lbuf = bread(log.dev, log.start + i + 1);  // read log block, 这里是 log block
    struct buf *dbuf = bread(log.dev, log.lh.block[i]);    // read dst, 这里是 home block

    kmemmove(dbuf->data, lbuf->data, BSIZE);  // copy block to dst, 将 log block 中暂存的 block 内容拷贝到 dst 中
    bwrite(dbuf);                             // write dst to disk, cause the refcnt++

    // 因为 dbuf 对应的 block, 在 log 的列表中也有引用, 看 log_write(struct buf *b) 的 bpin
    bunpin(dbuf);  // in order to release the dbuf, should decrease the refcnt

    // clean
    brelse(lbuf);
    brelse(dbuf);
  }
}

/// @brief Read the log header from disk into the in-memory log header
/// @globals
/// - (mut) log, init the log.lh.n and log.lh.block
static void read_head(void) {
  // log.start 对应的 block 里面存放有 log header
  struct buf *buf = bread(log.dev, log.start);  // in order to read the data from disk, should read the buffer first
  struct logheader *lh = (struct logheader *)(buf->data);
  log.lh.n = lh->n;  // 这个 buf 是临时的, 主要是为了服务于全局变量 log 的
  for (int i = 0; i < log.lh.n; i++) {
    log.lh.block[i] = lh->block[i];
  }
  brelse(buf);  // release the buf from bread. bread invoke the bget, bget 是构造函数, brelse 是析构函数
}

/// @brief Write in-memory log header to disk. This is the true point at which the current transaction commits.
/// @globals
/// - (mut) log
static void write_head(void) {
  struct buf *buf = bread(log.dev, log.start);
  struct logheader *hb = (struct logheader *)(buf->data);
  hb->n = log.lh.n;
  for (int i = 0; i < log.lh.n; i++) {
    hb->block[i] = log.lh.block[i];
  }
  bwrite(buf);
  brelse(buf);
}

/// @brief
/// @globals
/// - (mut) log
static void recover_from_log(void) {
  read_head();  // init log header
  install_trans();  // if committed, copy from log to disk. 将在 log block 中暂存的 block 拷贝到 普通的 block 中
  // 这里是为了: 可能关机前, 还有一些 log block 没有 copy 到普通 block 中, 所以这里需要将这些 log block copy 到普通
  // block 中

  log.lh.n = 0;
  write_head();  // clear the log
}

/// @brief called at the start of each FS system call.
void begin_op(void) {
  acquire(&log.lock);
  while (1) {
    if (log.committing) {  // 如果当前 log 系统正在 committing, 那么阻塞
      sleep(&log, &log.lock);
    } else if (log.lh.n + (log.outstanding + 1) * MAXOPBLOCKS > LOGSIZE) {
      // 如果当前日志条目数量 + 即将进行的操作可能占用的日志空间大小 > LOGSIZE, 则等待
      // this op might exhaust log space; wait for commit.
      sleep(&log, &log.lock);
    } else {
      log.outstanding += 1;
      release(&log.lock);
      break;
    }
  }
}

// 发现一个规律: 都是: wakeup(&log); release(&log.lock); 的

/// @brief called at the end of each FS system call.
/// commits if this was the last outstanding operation.
void end_op(void) {
  acquire(&log.lock);
  log.outstanding -= 1;
  if (log.committing) panic("log.committing");
  if (log.outstanding == 0) {
    log.committing = 1;
    release(&log.lock);
    // call commit w/o holding locks, since not allowed
    // to sleep with locks.
    commit();

    acquire(&log.lock);
    log.committing = 0;
    wakeup(&log);
    release(&log.lock);
  } else {
    // begin_op() may be waiting for log space,
    // and decrementing log.outstanding has decreased the amount of reserved space.
    // 说明, 并没有全部完成, 即 still outstanding
    // 但是我们已经取得了一些进展: log.outstanding -= 1;
    // 可能会有 begin_op 被满足, 因此 wakeup. 发现这是一组同步关系
    wakeup(&log);
    release(&log.lock);
  }
}

/// @brief Copy modified blocks from cache to log.
/// 将: home block 对应的 buf, 搬到 log block 中
static void write_log(void) {
  for (int i = 0; i < log.lh.n; i++) {
    struct buf *to = bread(log.dev, log.start + i + 1);  // log block
    struct buf *from = bread(log.dev, log.lh.block[i]);  // cache block (memory) (home)
    kmemmove(to->data, from->data, BSIZE);
    bwrite(to);  // write the log
    brelse(from);
    brelse(to);
  }
}

static void commit() {
  if (log.lh.n > 0) {
    write_log();      // Write modified blocks from cache to log
    write_head();     // Write header(metadata) to disk -- the real commit
    install_trans();  // Now install writes to home locations
    log.lh.n = 0;
    write_head();  // Erase the transaction from the log
  }
}

/// @brief Caller has modified b->data and is done with the buffer.
/// Record the block number and pin in the cache by increasing refcnt.
/// commit()/write_log() will do the disk write.
/// 做的事情就是: 如果 modify bp->data[], 那么就一定要调用 log_write(bp), 在 log 中写一条记录
/// 如果记录已经存在, 那么啥也不干 (log absorb) ; 如果不存在, 那么追加一条记录.
///
/// log_write() replaces bwrite(); a typical use is:
///   bp = bread(...)
///   modify bp->data[]
///   log_write(bp)
///   brelse(bp)
void log_write(struct buf *b) {
  if (log.lh.n >= LOGSIZE || log.lh.n >= log.size - 1) panic("too big a transaction");
  if (log.outstanding < 1) panic("log_write outside of trans");

  acquire(&log.lock);
  for (int i = 0; i < log.lh.n; i++) {
    if (log.lh.block[i] == b->blockno) {  // log absorbtion, 记录已经存在, 啥也不干
      release(&log.lock);
      return;
    }
  }
  // 如果记录不存在
  log.lh.block[log.lh.n] = b->blockno;
  bpin(b);  // 这个是为了防止: buf 被拿走的情况, 在没有 commit 前, buf 需要驻留在 bcache 中
  log.lh.n++;
  release(&log.lock);
}
