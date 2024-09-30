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
  uint size;        // Size of file system image (blocks) (这个是 image 的属性, 会被加载进来的)
  uint nblocks;     // Number of data blocks
  uint ninodes;     // Number of inodes.
  uint nlog;        // Number of log blocks
  uint logstart;    // Block number of first log block
  uint inodestart;  // Block number of first inode block
  uint bmapstart;   // Block number of first free map block
};

#define FSMAGIC 0x10203040

/// @brief 间接索引块的个数 (xv6 中只有一级间接索引)
/// 1024 / 4 == 256
#define NINDIRECT (BSIZE / sizeof(uint))

/// @brief 直接索引块的个数
#define NDIRECT 12

#define MAXFILE (NDIRECT + NINDIRECT)

/// @brief On-disk inode structure
struct dinode {
  short type;               // File type
  short major;              // Major device number (T_DEVICE only)
  short minor;              // Minor device number (T_DEVICE only)
  short nlink;              // Number of links to inode in file system
  uint size;                // Size of file (bytes)
  uint addrs[NDIRECT + 1];  // Data block addresses
};  // sizeof(struct dinode) == 64 bytes

/// @brief Inodes per block. == 16
#define IPB (BSIZE / sizeof(struct dinode))

/// @brief Block containing inode i.
/// @param i inode number, 给定 inode number, 求出这个 inode 位于第几个 block 里面
#define IBLOCK(i, sb) ((i) / IPB + sb.inodestart)

/// @brief Number of bits in a block
/// BSIZE 就表示: 一个 block 有几个 byte
/// 一个 byte 有 8 个 bit
#define BPB (BSIZE * 8)

/// @brief Block of free map containing bit for block b
/// @param b bitmap 中的: 第 b 个 bit
/// @param sb superblock, would use sb.bmapstart means bitmap 的起始 block 的 number
/// @return 根据 b 和 sb.bmapstart, 返回 b 位于的 block 的 number
#define BBLOCK(b, sb) ((b) / BPB + sb.bmapstart)

// Directory is a file containing a sequence of dirent structures.
#define DIRSIZ 14

/// @brief 目录项结构 (directory entry)
struct dirent {
  ushort inum;
  char name[DIRSIZ];
};
