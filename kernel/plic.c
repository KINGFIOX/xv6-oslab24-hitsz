#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "defs.h"

//
// the riscv Platform Level Interrupt Controller (PLIC).
//

/// @brief plic 启用外设
/// @param
void plicinit(void) {
  // set desired IRQ priorities non-zero (otherwise disabled).
  // *4 是因为, 每个 irq 占用 4 个字节
  *(uint32 *)(PLIC + UART0_IRQ * 4) = 1;
  *(uint32 *)(PLIC + VIRTIO0_IRQ * 4) = 1;
}

/// @brief 设置 plic 对应的 hart
/// @param
void plicinithart(void) {
  int hart = cpuid();

  // set uart's enable bit for this hart's S-mode.
  *(uint32 *)PLIC_SENABLE(hart) = (1 << UART0_IRQ) | (1 << VIRTIO0_IRQ);

  // set this hart's S-mode priority threshold to 0.
  *(uint32 *)PLIC_SPRIORITY(hart) = 0;  // no threshold, not block any interrupt
}

/// @brief ask the PLIC what interrupt we should serve.
/// @param
/// @return
int plic_claim(void) {
  int hart = cpuid();
  int irq = *(uint32 *)PLIC_SCLAIM(hart);  // load 的同时, 会清除该 irq 的 claim bit
  return irq;
}

/// @brief tell the PLIC we've served this IRQ.
/// @param irq
void plic_complete(int irq) {
  int hart = cpuid();
  *(uint32 *)PLIC_SCLAIM(hart) = irq;
}
