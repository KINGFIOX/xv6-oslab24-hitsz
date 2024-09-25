// Physical memory layout

// qemu -machine virt is set up like this,
// based on qemu's hw/riscv/virt.c:
//
// 00001000 -- boot ROM, provided by qemu
// 02000000 -- CLINT
// 0C000000 -- PLIC
// 10000000 -- uart0
// 10001000 -- virtio disk
// 80000000 -- boot ROM jumps here in machine mode
//             -kernel loads the kernel here
// unused RAM after 80000000.

// the kernel uses physical memory thus:
// 80000000 -- entry.S, then kernel text and data
// end -- start of kernel page allocation area
// PHYSTOP -- end RAM used by the kernel

/* ---------- ---------- uart ---------- ---------- */

// serial@10000000 {
//   interrupts = <0x0a>;          // 中断号
//   interrupt - parent = <&plic>;
//   clock - frequency = "\08@";
//   reg = <0x00 0x10000000 0x00 0x100>;  // < base, size >
//   compatible = "ns16550a";
// };

#define UART0 0x10000000L

/// @brief the number of int
#define UART0_IRQ 10

/* ---------- ---------- virt io ---------- ---------- */

// virtio_mmio@10001000 {
//   interrupts = <0x01>;
//   interrupt - parent = <&plic>;
//   reg = <0x00 0x10001000 0x00 0x1000>;
//   compatible = "virtio,mmio";
// };

// virtio mmio interface
#define VIRTIO0 0x10001000
#define VIRTIO0_IRQ 1

/* ---------- ---------- clint ---------- ---------- */

// clint@2000000 {
//   // 0x03 -> machine software interrupt, 0x07 -> machine timer interrupt
//   interrupts - extended = <&cpu@0/interrupt-controller 0x03 &cpu@0/interrupt-controller 0x07>;
//   reg = <0x00 0x2000000 0x00 0x10000>;
//   compatible = "sifive,clint0", "riscv,clint0";
// };

// local interrupt controller, which contains the timer.
#define CLINT 0x2000000L
#define CLINT_MTIMECMP(hartid) (CLINT + 0x4000 + 8 * (hartid))
#define CLINT_MTIME (CLINT + 0xBFF8)  // cycles since boot.

/* ---------- ---------- plic ---------- ---------- */

// plic@c000000 {
//   phandle = <0x03>;
//   riscv, ndev = <0x5f>;                  // 支持 0x5f 个设备
//   reg = <0x00 0xc000000 0x00 0x600000>;  // (base, len) < 基地址, 大小 >
//   // 这个 0x02 对应于 /cpus/cpu@0/interrupt-controller
//   // 0x0b -> machine external interrupt , 0x09 -> supervisor external interrupt
//   interrupts - extended = <0x02 0x0b 0x02 0x09>;  // (中断源, 中断号)
//   interrupt - controller;                         // annotate this node as an interrupt controller
//   compatible = "sifive,plic-1.0.0\0riscv,plic0";
// #address-cells = <0x00>;  // 定义子节点地址部分的单元数。在 PLIC 节点下，通常不需要地址映射，因此设置为 0
// #interrupt-cells = <0x01>;  // 指向 plic 的 interrupts-cell 的数量
// };

// qemu puts programmable interrupt controller here.
// lookup the uartlite documentation for the sifive fu540-c000 manual.
#define PLIC 0x0c000000L  // look up the dts
#define PLIC_PRIORITY (PLIC + 0x0)
#define PLIC_PENDING (PLIC + 0x1000)
#define PLIC_MENABLE(hart) (PLIC + 0x2000 + (hart) * 0x100)
#define PLIC_SENABLE(hart) (PLIC + 0x2080 + (hart) * 0x100)
#define PLIC_MPRIORITY(hart) (PLIC + 0x200000 + (hart) * 0x2000)
#define PLIC_SPRIORITY(hart) (PLIC + 0x201000 + (hart) * 0x2000)
#define PLIC_MCLAIM(hart) (PLIC + 0x200004 + (hart) * 0x2000)
#define PLIC_SCLAIM(hart) (PLIC + 0x201004 + (hart) * 0x2000)

/* ---------- ---------- kernel ---------- ---------- */

// the kernel expects there to be RAM
// for use by the kernel and user pages
// from physical address 0x80000000 to PHYSTOP.
#define KERNBASE 0x80000000L
#define PHYSTOP (KERNBASE + (((128) << 10) << 10))

// map the trampoline page to the highest address,
// in both user and kernel space.
#define TRAMPOLINE (MAXVA - PGSIZE)

// map kernel stacks beneath the trampoline,
// each surrounded by invalid guard pages.
#define KSTACK(p) (TRAMPOLINE - ((p) + 1) * 2 * PGSIZE)

// User memory layout.
// Address zero first:
//   text
//   original data and bss
//   fixed-size stack
//   expandable heap
//   ...
//   TRAPFRAME (p->trapframe, used by the trampoline)
//   TRAMPOLINE (the same page as in the kernel)
#define TRAPFRAME (TRAMPOLINE - PGSIZE)
