// File system implementation.  Five layers:
//   + Blocks: allocator for raw disk blocks.
//   + Log: crash recovery for multi-step updates.
//   + Files: inode allocator, reading, writing, metadata.
//   + Directories: inode with special contents (list of other inodes!)
//   + Names: paths like /usr/rtm/xv6/fs.c for convenient naming.
//
// This file contains the low-level file system manipulation
// routines.  The (higher-level) system call implementations
// are in sysfile.c.

#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "stat.h"
#include "spinlock.h"
#include "proc.h"
#include "sleeplock.h"
#include "fs.h"
#include "buf.h"
#include "file.h"

#define min(a, b) ((a) < (b) ? (a) : (b))

/// @brief there should be one superblock per disk device,
/// but we run with only one device.
///
/// init in fsinit(int dev) using readsb(int dev, struct superblock *sb)
static struct superblock sb;

/// @brief Read the super block. only called by fsinit.
/// @param dev
/// @param sb (return)
static void readsb(int dev, struct superblock *sb) {
  struct buf *bp = bread(dev, 1);  // superblock 是 1 号 block, buf with locked
  kmemmove(sb, bp->data, sizeof(struct superblock));
  brelse(bp);
}

/// @brief init fs
/// @globals
/// - (mut) sb
void fsinit(int dev) {
  readsb(dev, &sb);
  if (sb.magic != FSMAGIC) panic("invalid file system");
  initlog(dev, &sb);
}

/// @brief Zero a block.
/// @param dev
/// @param bno block number
static void bzero(int dev, int bno) {
  struct buf *bp = bread(dev, bno);
  kmemset(bp->data, 0, BSIZE);
  log_write(bp);
  brelse(bp);
}

// Blocks.

/// @brief Allocate a zeroed disk block.
/// @param dev
/// @return
/// @globals
/// - (mut) sb
static uint balloc(uint dev) {
  for (int b = 0; b < sb.size; b += BPB) {  // 一个 block 一个 block 的跳过, 一个 block 里面存放的是 bitmap
    struct buf *bp = bread(dev, BBLOCK(b, sb));
    for (int bi = 0; bi < BPB && b + bi < sb.size; bi++) {  // 遍历 bitmap 的每一个 bit
      int m = 1 << (bi & 0b111);
      if ((bp->data[bi >> 3] & m) == 0) {  // Is block free?
        bp->data[bi >> 3] |= m;            // Mark block in use.
        log_write(bp);
        brelse(bp);
        bzero(dev, b + bi);  // b + bi == bno, 分配得到的空白的 block
        return b + bi;
      }
    }
    brelse(bp);
  }
  panic("balloc: out of blocks");
}

/// @brief Free a disk block.
/// @param dev
/// @param b block number
static void bfree(int dev, uint b) {
  struct buf *bp = bread(dev, BBLOCK(b, sb));
  int bi = b % BPB;
  int m = 1 << (bi % 8);
  if ((bp->data[bi >> 3] & m) == 0) panic("freeing free block");
  bp->data[bi >> 3] &= ~m;
  log_write(bp);
  brelse(bp);
}

// Inodes.
//
// An inode describes a single unnamed file.
// The inode disk structure holds metadata: the file's type, its size,
// the number of links referring to it, and the list of blocks holding the file's content.
//
// The inodes are laid out sequentially on disk at sb.startinode.
// Each inode has a number, indicating its position on the disk.
//
// The kernel keeps a cache of in-use inodes in memory
// to provide a place for synchronizing access
// to inodes used by multiple processes. The cached
// inodes include book-keeping information that is
// not stored on disk: ip->ref and ip->valid.
//
// An inode and its in-memory representation go through a
// sequence of states before they can be used by the
// rest of the file system code.
//
// * Allocation: an inode is allocated if its type (on disk)
//   is non-zero. ialloc() allocates, and iput() frees if
//   the reference and link counts have fallen to zero.
//
// * Referencing in cache: an entry in the inode cache
//   is free if ip->ref is zero. Otherwise ip->ref tracks
//   the number of in-memory pointers to the entry (open
//   files and current directories). iget() finds or
//   creates a cache entry and increments its ref; iput()
//   decrements ref.
//
// * Valid: the information (type, size, &c) in an inode
//   cache entry is only correct when ip->valid is 1.
//   ilock() reads the inode from
//   the disk and sets ip->valid, while iput() clears
//   ip->valid if ip->ref has fallen to zero.
//
// * Locked: file system code may only examine and modify
//   the information in an inode and its content if it
//   has first locked the inode.
//
// Thus a typical sequence is:
//   ip = iget(dev, inum)
//   ilock(ip)
//   ... examine and modify ip->xxx ...
//   iunlock(ip)
//   iput(ip)
//
// ilock() is separate from iget() so that system calls can
// get a long-term reference to an inode (as for an open file)
// and only lock it for short periods (e.g., in read()).
// The separation also helps avoid deadlock and races during
// pathname lookup. iget() increments ip->ref so that the inode
// stays cached and pointers to it remain valid.
//
// Many internal file system functions expect the caller to
// have locked the inodes involved; this lets callers create
// multi-step atomic operations.
//
// The icache.lock spin-lock protects the allocation of icache
// entries. Since ip->ref indicates whether an entry is free,
// and ip->dev and ip->inum indicate which i-node an entry
// holds, one must hold icache.lock while using any of those fields.
//
// An ip->lock sleep-lock protects all ip-> fields other than ref,
// dev, and inum.  One must hold ip->lock in order to
// read or write that inode's ip->valid, ip->size, ip->type, &c.

/// @brief inode cache (in memory)
static struct {
  struct spinlock lock;
  struct inode inode[NINODE];  // 内存中, 最多只有这么多 inode
} icache;

void iinit() {
  initlock(&icache.lock, "icache");
  for (int i = 0; i < NINODE; i++) {
    initsleeplock(&icache.inode[i].lock, "inode");
  }
}

static struct inode *iget(uint dev, uint inum);

/// @brief Allocate an inode on device dev. Mark it as allocated by  giving it type type.
/// @param dev
/// @param type
/// @return an unlocked but allocated and referenced inode.
struct inode *ialloc(uint dev, short type) {
  for (int inum = 1; inum < sb.ninodes; inum++) {                 // 线性查找: 还没被使用的 inode number
    struct buf *bp = bread(dev, IBLOCK(inum, sb));                // base pointer
    struct dinode *dip = (struct dinode *)bp->data + inum % IPB;  // 获取对应的 inode entry
    if (dip->type == 0) {                                         // a free inode, disk inode
      kmemset(dip, 0, sizeof(struct dinode));                     // clear
      dip->type = type;
      log_write(bp);  // mark it allocated on the disk, 上面有对 struct buf 的 write, 下面就应该有 log_write
      brelse(bp);

      return iget(dev, inum);  // iget: 获取一个 inode, 他对应与 dinode
    }
    brelse(bp);
  }
  panic("ialloc: no inodes");
}

/// @brief Copy a modified in-memory inode to disk. Must be called after every change to an ip->xxx field that lives on disk,
/// since i-node cache is write-through. Caller must hold ip->lock.
/// @param ip
void iupdate(const struct inode *ip) {
  struct buf *bp = bread(ip->dev, IBLOCK(ip->inum, sb));
  struct dinode *dip = (struct dinode *)bp->data + ip->inum % IPB;
  dip->type = ip->type;  // 一个一个 copy, 是因为: dip 与 ip 不一样
  dip->major = ip->major;
  dip->minor = ip->minor;
  dip->nlink = ip->nlink;
  dip->size = ip->size;
  kmemmove(dip->addrs, ip->addrs, sizeof(ip->addrs));  // uint ip->addrs[NDIRECT + 1];
  log_write(bp);
  brelse(bp);
}

/// @brief Find the inode with number inum on device dev and return the in-memory copy.
/// Does not lock the inode and does not read it from disk.
/// 相当于是: inode 的构造函数
/// @param dev
/// @param inum
/// @return
static struct inode *iget(uint dev, uint inum) {
  acquire(&icache.lock);

  // Is the inode already cached?
  struct inode *empty = 0;
  struct inode *ip;
  for (ip = &icache.inode[0]; ip < &icache.inode[NINODE]; ip++) {
    if (ip->ref > 0 && ip->dev == dev && ip->inum == inum) {  // inode-cache 命中
      ip->ref++;
      release(&icache.lock);
      return ip;
    }
    if (empty == 0 && ip->ref == 0)  // Remember empty slot.
      empty = ip;
  }

  // 说明 inode-cache 没有命中

  // Recycle an inode cache entry.
  if (empty == 0) panic("iget: no inodes");

  // 分配
  ip = empty;
  ip->dev = dev;
  ip->inum = inum;
  ip->ref = 1;
  ip->valid = 0;
  release(&icache.lock);

  return ip;
}

/// @brief Increment reference count for ip.
/// @param ip
/// @return ip to enable ip = idup(ip1) idiom.
struct inode *idup(struct inode *ip) {
  acquire(&icache.lock);
  ip->ref++;
  release(&icache.lock);
  return ip;
}

/// @brief Lock the given inode.
/// @param ip Reads the inode from disk if necessary.
void ilock(struct inode *ip) {
  if (ip == 0 || ip->ref < 1) panic("ilock");

  acquiresleep(&ip->lock);

  if (ip->valid == 0) {  // 如果上锁醒来后, 发现 invalid 了, 那么就重新从 disk 中读取
    struct buf *bp = bread(ip->dev, IBLOCK(ip->inum, sb));
    struct dinode *dip = (struct dinode *)bp->data + ip->inum % IPB;
    ip->type = dip->type;
    ip->major = dip->major;
    ip->minor = dip->minor;
    ip->nlink = dip->nlink;
    ip->size = dip->size;
    kmemmove(ip->addrs, dip->addrs, sizeof(ip->addrs));
    brelse(bp);  // bp 的生命周期至少要比 dip 长
    ip->valid = 1;
    if (ip->type == 0) panic("ilock: no type");
  }
}

// Unlock the given inode.
void iunlock(struct inode *ip) {
  if (ip == 0 || !holdingsleep(&ip->lock) || ip->ref < 1) panic("iunlock");

  releasesleep(&ip->lock);
}

/// @brief Drop a reference to an in-memory inode. If that was the last reference, the inode cache entry can be
/// recycled. If that was the last reference and the inode has no links to it, free the inode (and its content) on disk.
/// All calls to iput() must be inside a transaction in case it has to free the inode.
/// put 的含义就是: 放下, 丢弃. 如果 inode->nlink 都是 0 了, 也就可以丢弃了.
/// @warning 因为里面会用到 log_write, 因此要记得: begin_op 和 end_op
/// @param ip
void iput(struct inode *ip) {
  acquire(&icache.lock);

  if (ip->ref == 1 && ip->valid && ip->nlink == 0) {
    // inode has no links and no other references: truncate and free.

    // ip->ref == 1 means no other process can have ip locked,
    // so this acquiresleep() won't block (or deadlock).
    acquiresleep(&ip->lock);

    release(&icache.lock);  // 因为接下来是操作 disk 了, 不会用到内存中的缓存

    itrunc(ip);
    ip->type = 0;
    iupdate(ip);  // 更新 inode 对应的 dinode
    ip->valid = 0;

    releasesleep(&ip->lock);

    acquire(&icache.lock);
  }

  ip->ref--;
  release(&icache.lock);
}

// Common idiom: unlock, then put.
void iunlockput(struct inode *ip) {
  iunlock(ip);
  iput(ip);
}

// Inode content
//
// The content (data) associated with each inode is stored in blocks on the disk.
// The first NDIRECT block numbers are listed in ip->addrs[].  The next NINDIRECT blocks are
// listed in block ip->addrs[NDIRECT].

/// @brief
/// @param ip
/// @param bn
/// @return the disk block address of the nth block in inode ip. If there is no such block, bmap allocates one.
static uint bmap(struct inode *ip, uint bn) {
  if (bn < NDIRECT) {  // 直接索引块
    uint addr;
    if ((addr = ip->addrs[bn]) == 0) {
      addr = balloc(ip->dev);  // 分配
      ip->addrs[bn] = addr;
    }
    return addr;
  }

  uint bnn = bn - NDIRECT;  // 第一级间接索引块的 index
  if (bnn < NINDIRECT) {
    // Load indirect block, allocating if necessary.
    uint addr;
    if ((addr = ip->addrs[NDIRECT]) == 0) {
      addr = balloc(ip->dev);  // 分配一个 block, 用来做表
      ip->addrs[NDIRECT] = addr;
    }
    struct buf *bp = bread(ip->dev, addr);
    uint *a = (uint *)bp->data;
    if ((addr = a[bnn]) == 0) {
      addr = balloc(ip->dev);  // 分配
      a[bnn] = addr;
      log_write(bp);
    }
    brelse(bp);
    return addr;
  }

  panic("bmap: out of range");
}

/// @brief Truncate inode (discard contents).
/// 就是将 ip 对应的 inode 的所有 block 都清空, 包括直接索引 block, 一级间接索引 block
/// @param ip
/// @warning Caller must hold ip->lock
void itrunc(struct inode *ip) {
  for (int i = 0; i < NDIRECT; i++) {  // 直接索引
    if (ip->addrs[i]) {
      bfree(ip->dev, ip->addrs[i]);
      ip->addrs[i] = 0;
    }
  }

  if (ip->addrs[NDIRECT]) {
    struct buf *bp = bread(ip->dev, ip->addrs[NDIRECT]);
    uint *a = (uint *)bp->data;
    for (int j = 0; j < NINDIRECT; j++) {
      if (a[j]) {
        bfree(ip->dev, a[j]);
      }
    }
    brelse(bp);
    bfree(ip->dev, ip->addrs[NDIRECT]);
    ip->addrs[NDIRECT] = 0;
  }

  ip->size = 0;
  iupdate(ip);
}

/// @brief Copy stat information from inode.
/// @param ip
/// @param st
/// @warning Caller must hold ip->lock.
void stati(const struct inode *ip, struct stat *st) {
  st->dev = ip->dev;
  st->ino = ip->inum;
  st->type = ip->type;
  st->nlink = ip->nlink;
  st->size = ip->size;
}

/// @brief Read data from inode.
/// @param ip
/// @param user_dst If user_dst==1, then dst is a user virtual address; otherwise, dst is a kernel address.
/// @param dst 目标地址, 数据将被读取到 该地址
/// @param off
/// @param n
/// @return
/// @warning Caller must hold ip->lock.
int readi(struct inode *ip, int user_dst, uint64 dst, uint off, uint n) {
  if (off > ip->size || off + n < off) return 0;
  if (off + n > ip->size) n = ip->size - off;

  uint tot = 0, m = 0;
  for (; tot < n; tot += m, off += m, dst += m) {
    struct buf *bp = bread(ip->dev, bmap(ip, off / BSIZE));  // bmap 会改 ip
    m = min(n - tot, BSIZE - off % BSIZE);
    uchar *src = bp->data + (off % BSIZE);  // 将 user_dst/dst 复制到 src
    if (either_copyout(user_dst, dst, src, m) == -1) {
      brelse(bp);
      break;
    }
    brelse(bp);
  }
  return tot;
}

/// @brief Write data to inode.
/// @param ip
/// @param user_src If user_src==1, then src is a user virtual address; otherwise, src is a kernel address.
/// @param src
/// @param off
/// @param n
/// @return n in success, -1 in failure
/// @warning Caller must hold ip->lock.
int writei(struct inode *ip, int user_src, uint64 src, uint off, uint n) {
  if (off > ip->size || off + n < off) return -1;
  if (off + n > MAXFILE * BSIZE) return -1;

  uint tot = 0, m = 0;
  for (; tot < n; tot += m, off += m, src += m) {
    struct buf *bp = bread(ip->dev, bmap(ip, off / BSIZE));
    m = min(n - tot, BSIZE - off % BSIZE);
    uchar *dst = bp->data + (off % BSIZE);  // 将 user_src/src 复制到 dst
    if (either_copyin(dst, user_src, src, m) == -1) {
      brelse(bp);
      break;
    }
    log_write(bp);
    brelse(bp);
  }

  if (n > 0) {
    if (off > ip->size) {
      ip->size = off;  // 这个 off 最后会是 size
    }
    // write the i-node back to disk even if the size didn't change
    // because the loop above might have called bmap() and added a new
    // block to ip->addrs[].
    iupdate(ip);
  }

  return n;
}

// Directories

/// @brief
/// @param s
/// @param t
/// @return
int namecmp(const char *s, const char *t) { return kstrncmp(s, t, DIRSIZ); }

//
//

/// @brief Look for a directory entry in a directory.
/// @param dp
/// @param name
/// @param poff (return) If found, set *poff to byte offset of entry.
/// @return inode of the entry; 0 failed
struct inode *dirlookup(struct inode *dp, const char *name, uint *poff) {
  if (dp->type != T_DIR) panic("dirlookup not DIR");
  for (uint off = 0; off < dp->size; off += sizeof(struct dirent)) {
    struct dirent de;
    if (readi(dp, 0, (uint64)&de, off, sizeof(de)) != sizeof(de)) panic("dirlookup read");
    if (de.inum == 0) continue;
    if (namecmp(name, de.name) == 0) {  // name == de.name
      // entry matches path element
      if (poff) *poff = off;  // 如果 poff 不是 NULL, 就会作为返回值
      uint inum = de.inum;
      return iget(dp->dev, inum);  // inode
    }
  }

  return 0;
}

/// @brief Write a new directory entry (name, inum) into the directory dp.
/// @param dp
/// @param name
/// @param inum
/// @return
int dirlink(struct inode *dp, char *name, uint inum) {
  struct inode *ip;
  // Check that name is not present.
  if ((ip = dirlookup(dp, name, 0)) != 0) {
    iput(ip);
    return -1;
  }

  // Look for an empty dirent.
  int off = 0;
  struct dirent de;
  for (; off < dp->size; off += sizeof(de)) {
    if (readi(dp, 0, (uint64)&de, off, sizeof(de)) != sizeof(de)) panic("dirlink read");
    if (de.inum == 0) break;
  }

  kstrncpy(de.name, name, DIRSIZ);
  de.inum = inum;  // 复制 de -> off
  if (writei(dp, 0, (uint64)&de, off, sizeof(de)) != sizeof(de)) panic("dirlink");

  return 0;
}

// Paths

// Copy the next path element from path into name.
// Return a pointer to the element following the copied one.
// The returned path has no leading slashes,
// so the caller can check *path=='\0' to see if the name is the last one.
// If no name to remove, return 0.
//
// Examples:
//   skipelem("a/bb/c", name) = "bb/c", setting name = "a"
//   skipelem("///a//bb", name) = "bb", setting name = "a"
//   skipelem("a", name) = "", setting name = "a"
//   skipelem("", name) = skipelem("////", name) = 0
//

/// @brief
/// @param path
/// @param name (return)
/// @return
static char *skipelem(const char *path, char *name) {
  while (*path == '/') path++; /* ///a/b, 跳过前面一连串的 `/` */
  if (*path == 0) return 0;    /* 说明是 /// */
  const char *s = path;
  while (*path != '/' && *path != 0) path++;
  int len = path - s;
  if (len >= DIRSIZ)
    kmemmove(name, s, DIRSIZ);  // 截断
  else {
    kmemmove(name, s, len);
    name[len] = 0;
  }
  while (*path == '/') path++;
  return (char *)path;
}

/// @brief Look up and return the inode for a path name.
/// If parent != 0, return the inode for the parent and copy the final
/// path element into name, which must have room for DIRSIZ bytes.
/// Must be called inside a transaction since it calls iput().
/// @param path
/// @param nameiparent (bool)
/// @param name (return)
/// @return
static struct inode *namex(const char *path, int nameiparent, char *name) {
  struct inode *ip;

  if (*path == '/')
    ip = iget(ROOTDEV, ROOTINO);  // 绝对地址
  else
    ip = idup(myproc()->cwd);  // 相对地址

  while ((path = skipelem(path, name)) != 0) {
    ilock(ip);
    if (ip->type != T_DIR) {
      iunlockput(ip);
      return 0;
    }
    if (nameiparent && *path == '\0') {
      // Stop one level early.
      iunlock(ip);
      return ip;
    }

    struct inode *next;
    if ((next = dirlookup(ip, name, 0)) == 0) {
      iunlockput(ip);
      return 0;
    }
    iunlockput(ip);
    ip = next;
  }
  if (nameiparent) {
    iput(ip);
    return 0;
  }
  return ip;
}

/// @brief 根据 path, 返回 path 对应的 inode
/// @param path
/// @return
struct inode *namei(const char *path) {
  char name[DIRSIZ];
  return namex(path, 0, name);
}

/// @brief
/// @param path
/// @param name
/// @return
struct inode *nameiparent(const char *path, char *name) { return namex(path, 1, name); }
