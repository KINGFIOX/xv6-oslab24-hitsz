//
// low-level driver routines for 16550a UART.
//

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"

// the UART control registers are memory-mapped
// at address UART0. this macro returns the
// address of one of the registers.
#define Reg(reg) ((volatile unsigned char *)(UART0 + reg))

// the UART control registers.
// some have different meanings for
// read vs write.
// see http://byterunner.com/16550.html
#define RHR 0b000  // receive holding register (for input bytes) (read mode)
#define THR 0b000  // transmit holding register (for output bytes) (write mode)
#define IER 0b001  // interrupt enable register (for write)
#define IER_RX_ENABLE (1 << 0)
#define IER_TX_ENABLE (1 << 1)
#define FCR 0b010  // FIFO control register (for write)
#define FCR_FIFO_ENABLE (1 << 0)
#define FCR_FIFO_CLEAR (3 << 1)  // clear the content of the two FIFOs
#define ISR 0b010                // interrupt status register (for read)
#define LCR 0b011                // line control register
#define LCR_EIGHT_BITS (3 << 0)
#define LCR_BAUD_LATCH (1 << 7)  // special mode to set baud rate
#define LSR 0b101                // line status register
#define LSR_RX_READY (1 << 0)    // input is waiting to be read from RHR
#define LSR_TX_IDLE (1 << 5)     // THR can accept another character to send

#define ReadReg(reg) (*(Reg(reg)))
#define WriteReg(reg, v) (*(Reg(reg)) = (v))

// the transmit output buffer.
static struct spinlock uart_tx_lock;
#define UART_TX_BUF_SIZE 32

/// @brief the buffer to store the char to be sent. it's a queue.
static char uart_tx_buf[UART_TX_BUF_SIZE];
/// @brief queue tail
static int uart_tx_w;  // write next to uart_tx_buf[uart_tx_w++]
/// @brief queue head
static int uart_tx_r;  // read next from uart_tx_buf[uar_tx_r++]

extern volatile int panicked;  // from printf.c

static void __uartstart();

/// @brief called in consoleinit. set the control register
/// @param
void uartinit(void) {
  // disable interrupts.
  WriteReg(IER, 0x00);

  // special mode to set baud rate.
  WriteReg(LCR, LCR_BAUD_LATCH);

  // LSB for baud rate of 38.4K.
  WriteReg(0, 0x03);

  // MSB for baud rate of 38.4K.
  WriteReg(1, 0x00);

  // leave set-baud mode,
  // and set word length to 8 bits, no parity.
  WriteReg(LCR, LCR_EIGHT_BITS);

  // reset and enable FIFOs.
  WriteReg(FCR, FCR_FIFO_ENABLE | FCR_FIFO_CLEAR);

  // enable transmit and receive interrupts.
  WriteReg(IER, IER_TX_ENABLE | IER_RX_ENABLE);

  initlock(&uart_tx_lock, "uart");
}

/// @brief add a character to the output buffer and tell the.
/// UART to start sending if it isn't already.
/// blocks if the output buffer is full.
/// because it may block, it can't be called from interrupts; it's only suitable for use by write().
/// @param c the char to be sent
///
/// @globals
/// - (mut) uart_tx_lock
/// - (mut) uart_tx_buf
void uartputc(int c) {
  acquire(&uart_tx_lock);

  // panic -> block
  if (panicked) {
    for (;;);
  }

  while (1) {
    if (((uart_tx_w + 1) % UART_TX_BUF_SIZE) == uart_tx_r) {
      // buffer is full.
      // wait for __uartstart() to open up space in the buffer.
      sleep(&uart_tx_r, &uart_tx_lock);
    } else {
      uart_tx_buf[uart_tx_w] = c;
      uart_tx_w = (uart_tx_w + 1) % UART_TX_BUF_SIZE;
      __uartstart();
      release(&uart_tx_lock);
      return;
    }
  }
}

/// @brief alternate version of uartputc() that doesn't use interrupts, for use by kernel printf() and to echo char.
/// it spins waiting for the uart's output register to be empty.
/// @param c
void uartputc_sync(int c) {
  push_off();

  if (panicked) {
    for (;;);
  }

  // wait for Transmit Holding Empty to be set in LSR(line status register).
  while ((ReadReg(LSR) & LSR_TX_IDLE) == 0);  // transmit holding empty -> transmit buf 是否 空, 空了才能发射
  WriteReg(THR, c);

  pop_off();
}

/// @brief if the UART is idle, and a character is waiting in the transmit buffer, send it.
/// @warning caller must hold uart_tx_lock. called from both the top- and bottom-half.
///
/// @globals
/// - (mut) uart_tx_buf
/// - (mut) uart_tx_r
/// - uart_tx_w
static void __uartstart() {
  while (1) {
    if (uart_tx_w == uart_tx_r) {
      // transmit buffer is empty.
      return;
    }

    if ((ReadReg(LSR) & LSR_TX_IDLE) == 0) {  // transmit buf 是否空, 空了才能发射
      // the UART transmit holding register is full,
      // so we cannot give it another byte.
      // it will interrupt when it's ready for a new byte.
      return;
    }

    // c <- pop a char
    int c = uart_tx_buf[uart_tx_r];
    uart_tx_r = (uart_tx_r + 1) % UART_TX_BUF_SIZE;

    // maybe uartputc() is waiting for space in the buffer.
    // 主要是唤醒上面那个 uartputc
    wakeup(&uart_tx_r);

    WriteReg(THR, c);
  }
}

/// @brief read one input character from the UART.
/// @return -1 if none is waiting.
int uartgetc(void) {
  if (ReadReg(LSR) & LSR_RX_READY) {
    // input data is ready.
    return ReadReg(RHR);
  } else {
    return -1;
  }
}

/// @brief handle a uart interrupt, raised because input has arrived, or the uart is ready for more output, or both.
/// called from trap.c.
void uartintr(void) {
  // read and process incoming characters.
  while (1) {
    int c = uartgetc();
    if (c == -1) break;  // exit loop
    consoleintr(c);
  }

  // send buffered characters.
  acquire(&uart_tx_lock);
  __uartstart();
  release(&uart_tx_lock);
}
