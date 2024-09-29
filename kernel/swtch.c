#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"

void swtch(struct context *old, struct context *new) {
  __asm__ volatile(
      // 保存当前上下文到 *old
      "sd ra, 0(a0)\n"
      "sd sp, 8(a0)\n"
      "sd s0, 16(a0)\n"
      "sd s1, 24(a0)\n"
      "sd s2, 32(a0)\n"
      "sd s3, 40(a0)\n"
      "sd s4, 48(a0)\n"
      "sd s5, 56(a0)\n"
      "sd s6, 64(a0)\n"
      "sd s7, 72(a0)\n"
      "sd s8, 80(a0)\n"
      "sd s9, 88(a0)\n"
      "sd s10, 96(a0)\n"
      "sd s11, 104(a0)\n"

      // 恢复新上下文从 *new
      "ld ra, 0(a1)\n"
      "ld sp, 8(a1)\n"
      "ld s0, 16(a1)\n"
      "ld s1, 24(a1)\n"
      "ld s2, 32(a1)\n"
      "ld s3, 40(a1)\n"
      "ld s4, 48(a1)\n"
      "ld s5, 56(a1)\n"
      "ld s6, 64(a1)\n"
      "ld s7, 72(a1)\n"
      "ld s8, 80(a1)\n"
      "ld s9, 88(a1)\n"
      "ld s10, 96(a1)\n"
      "ld s11, 104(a1)\n"

      // 返回到新上下文
      "ret\n");
}