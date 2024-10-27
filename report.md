# fs

## Large files

### 内容分析

支持一个二级间接索引.

mit's xv6 实验指导书中有提示: bmap. bmap 的功能是: 将文件的逻辑块地址转换成物理块地址.
如果逻辑块对应的物理块不存在, 那么就会分配相应的物理块, 并挂在文件的 block 表中,
这个表也是类似于 pagetable(基数树). 如果一级简介索引的 dir 不存在, 那么就相应的创建.
因此: 二级间接索引的 dir 不存在, 也要相应的创建. 这里的逻辑块地址就是: 一个文件的第几个块.

但是仅完善 bmap 是不能通过测试的. 发现: itrunc 也有涉及到间接索引的代码, 这里我们也要改.
itrunc 就是用来清空一个文件的内容的, 释放一个文件所有的逻辑块.

### 设计方法

参考 bmap 和 itrunc 的一级间接索引的写法, 仿写二级间接索引

### 代码

```c
// kernel/fs.c
static uint bmap(struct inode *ip, uint bn) {
  if (bn < NDIRECT) {
    uint addr = ip->addrs[bn];
    if (addr == 0) {
      addr = balloc(ip->dev);
      if (addr == 0) return 0;
      ip->addrs[bn] = addr;
    }
    return addr;
  }
  bn -= NDIRECT;

  if (bn < NINDIRECT) {
    // Load indirect block, allocating if necessary.
    uint addr = ip->addrs[NDIRECT];
    if (addr == 0) {
      addr = balloc(ip->dev);
      if (addr == 0) return 0;
      ip->addrs[NDIRECT] = addr;
    }
    struct buf *bp = bread(ip->dev, addr);
    uint *a = (uint *)bp->data;
    if ((addr = a[bn]) == 0) {
      addr = balloc(ip->dev);
      if (addr) {
        a[bn] = addr;
        log_write(bp);
      }
    }
    brelse(bp);
    return addr;
  }
  bn -= NINDIRECT;

  if (bn < NININDIRECT) {
    // Load indirect block, allocating if necessary.
    uint addr = ip->addrs[NDIRECT + 1];
    if (addr == 0) {
      addr = balloc(ip->dev);
      if (addr == 0) return 0;
      ip->addrs[NDIRECT + 1] = addr;
    }
    struct buf *bp = bread(ip->dev, addr);
    uint *a = (uint *)bp->data;  // dir1

    addr = a[bn / NINDIRECT];
    if (!addr) {
      addr = balloc(ip->dev);
      if (!addr) return 0;
      a[bn / NINDIRECT] = addr;
      log_write(bp);
    }
    brelse(bp);

    bp = bread(ip->dev, addr);
    a = (uint *)bp->data;  // dir2

    addr = a[bn % NINDIRECT];
    if (!addr) {
      addr = balloc(ip->dev);
      if (addr) {
        a[bn % NINDIRECT] = addr;
        log_write(bp);
      }
    }
    brelse(bp);

    return addr;
  }

  panic("bmap: out of range");
}
```

```c
// kernel/fs.c
void itrunc(struct inode *ip) {
  for (int i = 0; i < NDIRECT; i++) {
    if (ip->addrs[i]) {
      bfree(ip->dev, ip->addrs[i]);
      ip->addrs[i] = 0;
    }
  }

  if (ip->addrs[NDIRECT]) {
    struct buf *bp = bread(ip->dev, ip->addrs[NDIRECT]);
    uint *a = (uint *)bp->data;
    for (int i = 0; i < NINDIRECT; i++) {
      if (a[i]) bfree(ip->dev, a[i]);
    }
    brelse(bp);
    bfree(ip->dev, ip->addrs[NDIRECT]);
    ip->addrs[NDIRECT] = 0;
  }

  if (ip->addrs[NDIRECT + 1]) {
    struct buf *bp = bread(ip->dev, ip->addrs[NDIRECT + 1]);
    uint *a = (uint *)bp->data;  // dir1
    for (int i = 0; i < NINDIRECT; i++) {
      if (a[i]) {
        struct buf *bp = bread(ip->dev, a[i]);
        uint *b = (uint *)bp->data;  // dir2
        for (int j = 0; j < NINDIRECT; j++) {
          if (b[j]) bfree(ip->dev, b[j]);
        }
        brelse(bp);
        bfree(ip->dev, a[i]);
      }
    }
    brelse(bp);
    bfree(ip->dev, ip->addrs[NDIRECT + 1]);
    ip->addrs[NDIRECT + 1] = 0;
  }

  ip->size = 0;
  iupdate(ip);
}
```

## Symbolic links

符号链接
