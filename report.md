# net

## 内容分析

这个实验, 文档很长, 代码很短. 跟着 mit's xv6 实验指导书 一步一步做就行了.
甚至不太需要看 intel e1000 的文档, 其实还是需要看的, mmio 的设置之类的.

文档中有提到并发控制, 但是我实现的过程中, 不用并发控制也能通过实验.

注意, 国内环境下, make grade 的 dns 测试可能不太行.

我的评价是: 感觉可以看 _TCP/IP 详解_ 了

### 代码实现 e1000_transmit

这个跟着实验指导书一步步做就行了, 他会让我们查 `tx_rint[rear].cmd` 应该怎么赋值, 查手册就完事儿了.

```c
// kernel/net.c
int e1000_transmit(struct mbuf *m) {
  //
  // Your code here.
  //
  // the mbuf contains an ethernet frame; program it into
  // the TX descriptor ring so that the e1000 sends it. Stash
  // a pointer so that it can be freed after sending.
  //
  int rear = regs[E1000_TDT];   // 可用位置
  int front = regs[E1000_TDH];  // 头
  if ((rear + 1) % TX_RING_SIZE == front) {
    printf("%s:%d transmit queue has been full.\n", __FILE__, __LINE__);
    return -1;
  }
  if (!(tx_ring[rear].status & E1000_TXD_STAT_DD)) {
    printf("%s:%d previous transmission not finished.\n", __FILE__, __LINE__);
    return -1;
  }
  if (tx_mbufs[rear] != 0) {
    mbuffree(tx_mbufs[rear]);
    tx_mbufs[rear] = 0;
  }
  tx_ring[rear].addr = (uint64)m->head;
  tx_ring[rear].length = m->len;
  tx_ring[rear].cmd = E1000_TXD_CMD_RS | E1000_TXD_CMD_EOP;
  tx_mbufs[rear] = m;  // stash a pointer for later freeing. it is above mbuffree(tx_mbufs[rear])
  regs[E1000_TDT] = (regs[E1000_TDT] + 1) % TX_RING_SIZE;

  return 0;
}
```

### 代码实现 e1000_recv

也是查手册

```c
static void e1000_recv(void) {
  //
  // Your code here.
  //
  // Check for packets that have arrived from the e1000
  // Create and deliver an mbuf for each packet (using net_rx()).
  //
  int rear = (regs[E1000_RDT] + 1) % RX_RING_SIZE;
  // 循环是为了: 因为可能: 一次收到了多个包, 这些包都需要处理完, 否则: testing multi-process pings will fail.
  while (rx_ring[rear].status & E1000_RXD_STAT_DD) {
    struct mbuf *m = rx_mbufs[rear];
    m->len = rx_ring[rear].length;
    net_rx(m);
    rx_mbufs[rear] = mbufalloc(0);
    if (!rx_mbufs[rear]) panic("e1000_recv");
    rx_ring[rear].addr = (uint64)rx_mbufs[rear]->head;  // 不过, 这里多核居然没有问题 😝
    rx_ring[rear].status = 0;
    regs[E1000_RDT] = rear;
    rear = (rear + 1) % RX_RING_SIZE;
  }
}
```

## make grade

国内环境似乎没法 `ping pdos.csail.mit.edu`

![alt text](image/grade.png)
