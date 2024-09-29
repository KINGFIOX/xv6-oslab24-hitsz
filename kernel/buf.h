/// @brief
struct buf {
  int valid;  // has data been read from disk? 1: `data` contains `blockno`'s data
  int disk;   // does disk "own" buf? 1: disk r/w is operation is in progress
  uint dev;
  uint blockno;
  struct sleeplock lock;
  uint refcnt;
  struct buf *prev;  // LRU cache list
  struct buf *next;
  uchar data[BSIZE];  // 1024, 实际保存数据
};
