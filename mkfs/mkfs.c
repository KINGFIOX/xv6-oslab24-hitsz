#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <assert.h>

#define stat xv6_stat  // avoid clash with host struct stat

typedef unsigned int uint;
typedef unsigned short ushort;
typedef unsigned char uchar;

typedef unsigned char uint8;
typedef unsigned short uint16;
typedef unsigned int uint32;
typedef unsigned long uint64;

typedef uint64 pde_t;

// On-disk file system format.
// Both the kernel and user programs use this header file.

#define ROOTINO 1   // root i-number
#define BSIZE 1024  // block size

// Disk layout:
// [ boot block | super block | log | inode blocks |
//                                          free bit map | data blocks]
//
// mkfs computes the super block and builds an initial file system. The
// super block describes the disk layout:
struct superblock {
  uint magic;       // Must be FSMAGIC
  uint size;        // Size of file system image (blocks)
  uint nblocks;     // Number of data blocks
  uint ninodes;     // Number of inodes.
  uint nlog;        // Number of log blocks
  uint logstart;    // Block number of first log block
  uint inodestart;  // Block number of first inode block
  uint bmapstart;   // Block number of first free map block
};

#define FSMAGIC 0x10203040

#define NDIRECT 12
#define NINDIRECT (BSIZE / sizeof(uint))
#define MAXFILE (NDIRECT + NINDIRECT)

// On-disk inode structure
struct dinode {
  short type;               // File type
  short major;              // Major device number (T_DEVICE only)
  short minor;              // Minor device number (T_DEVICE only)
  short nlink;              // Number of links to inode in file system
  uint size;                // Size of file (bytes)
  uint addrs[NDIRECT + 1];  // Data block addresses
};

// Inodes per block.
#define IPB (BSIZE / sizeof(struct dinode))

// Block containing inode i
#define IBLOCK(i, sb) ((i) / IPB + sb.inodestart)

// Bitmap bits per block
#define BPB (BSIZE * 8)

// Block of free map containing bit for block b
#define BBLOCK(b, sb) ((b) / BPB + sb.bmapstart)

// Directory is a file containing a sequence of dirent structures.
#define DIRSIZ 14

struct dirent {
  ushort inum;  // inode num
  char name[DIRSIZ];
};

#define T_DIR 1     // Directory
#define T_FILE 2    // File
#define T_DEVICE 3  // Device

struct stat {
  int dev;      // File system's disk device
  uint ino;     // Inode number
  short type;   // Type of file
  short nlink;  // Number of links to file
  uint64 size;  // Size of file in bytes
};

#define NPROC 64                   // maximum number of processes
#define NCPU 8                     // maximum number of CPUs
#define NOFILE 16                  // open files per process
#define NFILE 100                  // open files per system
#define NINODE 50                  // maximum number of active i-nodes
#define NDEV 10                    // maximum major device number
#define ROOTDEV 1                  // device number of file system root disk
#define MAXARG 32                  // max exec arguments
#define MAXOPBLOCKS 10             // max # of blocks any FS op writes
#define LOGSIZE (MAXOPBLOCKS * 3)  // max data blocks in on-disk log
#define NBUF (MAXOPBLOCKS * 3)     // size of disk block cache
#define FSSIZE 1000                // size of file system in blocks
#define MAXPATH 128                // maximum file path name

#ifndef static_assert
#define static_assert(a, b) \
  do {                      \
    switch (0)              \
    case 0:                 \
    case (a):;              \
  } while (0)
#endif

#define NINODES 200

// Disk layout:
// [ boot block | sb block | log | inode blocks | free bit map | data blocks ]

int nbitmap = FSSIZE / (BSIZE * 8) + 1;
int ninodeblocks = NINODES / IPB + 1;
int nlog = LOGSIZE;
int nmeta;    // Number of meta blocks (boot, sb, nlog, inode, bitmap)
int nblocks;  // Number of data blocks

int fsfd;
struct superblock sb;
char zeroes[BSIZE];
uint freeinode = 1;
uint used_block;

void balloc(int);
void wsect(uint bno, const void *buf);
void winode(uint, const struct dinode *);
void rinode(uint inum, struct dinode *ip);
void rsect(uint bno, void *buf);
uint ialloc(ushort type);
void iappend(uint inum, const void *p, int n);

// convert to intel byte order
ushort xshort(ushort x) {
  ushort y;
  uchar *a = (uchar *)&y;
  a[0] = x;
  a[1] = x >> 8;
  return y;
}

uint xint(uint x) {
  uint y;
  uchar *a = (uchar *)&y;
  a[0] = x;
  a[1] = x >> 8;
  a[2] = x >> 16;
  a[3] = x >> 24;
  return y;
}

int main(int argc, char *argv[]) {
  char buf[BSIZE];

  static_assert(sizeof(int) == 4, "Integers must be 4 bytes!");

  if (argc < 2) {
    fprintf(stderr, "Usage: mkfs fs.img files...\n");
    exit(1);
  }

  assert((BSIZE % sizeof(struct dinode)) == 0);
  assert((BSIZE % sizeof(struct dirent)) == 0);

  // open fs.img
  fsfd = open(argv[1], O_RDWR | O_CREAT | O_TRUNC, 0666);
  if (fsfd < 0) {
    perror(argv[1]);
    exit(1);
  }

  // 1 fs block = 1 disk sector
  nmeta = 1 /* boot */ + 1 /* superblock */ + nlog /* log_hdr + logs */ + ninodeblocks /* inodes */ + nbitmap /* bitmap(1) */;
  nblocks = FSSIZE - nmeta;  // data_blocks

  sb.magic = FSMAGIC;
  sb.size = xint(FSSIZE);
  sb.nblocks = xint(nblocks);
  sb.ninodes = xint(NINODES);
  sb.nlog = xint(nlog);
  sb.logstart = xint(2);
  sb.inodestart = xint(2 + nlog);
  sb.bmapstart = xint(2 + nlog + ninodeblocks);

  printf("nmeta %d (boot, super, log blocks %u inode blocks %u, bitmap blocks %u) blocks %d total %d\n", nmeta, nlog, ninodeblocks, nbitmap, nblocks, FSSIZE);

  used_block = nmeta;  // the first free block that we can allocate

  for (int i = 0; i < FSSIZE; i++) wsect(i, zeroes);  // clear all blocks

  memset(buf, 0, sizeof(buf));  // write superblock to fs.img
  memmove(buf, &sb, sizeof(sb));
  wsect(1, buf);

  uint rootino = ialloc(T_DIR);  // write inode and return the ino
  assert(rootino == ROOTINO);

  struct dirent de;
  bzero(&de, sizeof(de));
  de.inum = xshort(rootino);
  strcpy(de.name, ".");
  iappend(rootino, &de, sizeof(de));

  bzero(&de, sizeof(de));
  de.inum = xshort(rootino);
  strcpy(de.name, "..");
  iappend(rootino, &de, sizeof(de));

  struct dinode din;
  rinode(rootino, &din);
  din.nlink++;
  winode(rootino, &din);

  for (int i = 2; i < argc; i++) {
    // get rid of "user/"
    char *shortname;
    if (strncmp(argv[i], "user/", 5) == 0)
      shortname = argv[i] + 5;
    else
      shortname = argv[i];

    assert(index(shortname, '/') == 0);

    int fd = open(argv[i], 0);
    if (fd < 0) {
      perror(argv[i]);
      exit(1);
    }

    // Skip leading _ in name when writing to file system.
    // The binaries are named _rm, _cat, etc. to keep the
    // build operating system from trying to execute them
    // in place of system binaries like rm and cat.
    if (shortname[0] == '_') shortname += 1;  // get rid of the prefix _

    uint inum = ialloc(T_FILE);

    bzero(&de, sizeof(de));
    de.inum = xshort(inum);
    strncpy(de.name, shortname, DIRSIZ);
    iappend(rootino, &de, sizeof(de));

    int cc;
    while ((cc = read(fd, buf, sizeof(buf))) > 0) iappend(inum, buf, cc);

    close(fd);
  }

  // fix size of root inode dir
  // struct dinode din;
  rinode(rootino, &din);
  uint off = xint(din.size);
  off = ((off / BSIZE) + 1) * BSIZE;
  din.size = xint(off);
  winode(rootino, &din);

  balloc(used_block);

  exit(0);
}

void wsect(uint bno, const void *buf) {
  // 提笔到: bno * BSIZE 的位置
  if (lseek(fsfd, bno * BSIZE, 0) != bno * BSIZE) {
    perror("lseek");
    exit(1);
  }
  if (write(fsfd, buf, BSIZE) != BSIZE) {
    perror("write");
    exit(1);
  }
}

/**
 * @brief write inode
 *
 * @param inum
 * @param ip
 */
void winode(uint inum, const struct dinode *ip) {
  char buf[BSIZE];

  uint bn = IBLOCK(inum, sb);  // get the block number which contains the inode
  rsect(bn, buf);              // read the block
  struct dinode *dip = ((struct dinode *)buf) + (inum % IPB);
  *dip = *ip;
  wsect(bn, buf);
}

/**
 * @brief read inode
 *
 * @param inum
 * @param ip (return)
 */
void rinode(uint inum, struct dinode *ip) {
  char buf[BSIZE];

  uint bn = IBLOCK(inum, sb);
  rsect(bn, buf);
  struct dinode *dip = ((struct dinode *)buf) + (inum % IPB);
  *ip = *dip;
}

/**
 * @brief
 *
 * @param bno
 * @param buf (mut)
 */
void rsect(uint bno, void *buf) {
  // 提笔到: bno * BSIZE 的位置
  if (lseek(fsfd, bno * BSIZE, 0) != bno * BSIZE) {
    perror("lseek");
    exit(1);
  }
  if (read(fsfd, buf, BSIZE) != BSIZE) {
    perror("read");
    exit(1);
  }
}

/**
 * @brief
 *
 * @param type
 * @return uint
 */
uint ialloc(ushort type) {
  uint inum = freeinode++;  // freeinode init with 1
  struct dinode din;

  bzero(&din, sizeof(din));  // clear the din
  din.type = xshort(type);
  din.nlink = xshort(1);
  din.size = xint(0);
  winode(inum, &din);
  return inum;
}

/**
 * @brief set bitmap
 *
 * @param used
 */
void balloc(int used) {
  uchar buf[BSIZE];

  printf("balloc: first %d blocks have been allocated\n", used);
  assert(used < BSIZE * 8);
  bzero(buf, BSIZE);
  for (int i = 0; i < used; i++) {
    buf[i / 8] = buf[i / 8] | (0x1 << (i % 8));
  }
  printf("balloc: write bitmap block at sector %d\n", sb.bmapstart);
  wsect(sb.bmapstart, buf);
}

#define min(a, b) ((a) < (b) ? (a) : (b))

/**
 * @brief
 *
 * @param inum
 * @param xp
 * @param n
 */
void iappend(uint inum, const void *xp, int n) {
  char buf[BSIZE];

  const char *p = xp;

  struct dinode din;
  rinode(inum, &din);
  uint off = xint(din.size);
  // printf("append inum %d at off %d sz %d\n", inum, off, n);
  while (n > 0) {
    uint fbn = off / BSIZE;
    assert(fbn < MAXFILE);
    uint x;
    if (fbn < NDIRECT) {
      if (xint(din.addrs[fbn]) == 0) {
        din.addrs[fbn] = xint(used_block++);
      }
      x = xint(din.addrs[fbn]);
    } else {
      if (xint(din.addrs[NDIRECT]) == 0) {
        din.addrs[NDIRECT] = xint(used_block++);
      }
      uint indirect[NINDIRECT];
      rsect(xint(din.addrs[NDIRECT]), (char *)indirect);
      if (indirect[fbn - NDIRECT] == 0) {
        indirect[fbn - NDIRECT] = xint(used_block++);
        wsect(xint(din.addrs[NDIRECT]), (char *)indirect);
      }
      x = xint(indirect[fbn - NDIRECT]);
    }
    uint n1 = min(n, (fbn + 1) * BSIZE - off);
    rsect(x, buf);

    bcopy(p /*src*/, buf + off - (fbn * BSIZE) /*dst*/, n1);
    wsect(x, buf);
    n -= n1;
    off += n1;
    p += n1;
  }
  din.size = xint(off);
  winode(inum, &din);
}
