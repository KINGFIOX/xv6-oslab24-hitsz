//
// formatted console output -- kprintf, panic.
//

#include <stdarg.h>

#include "types.h"
#include "param.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "fs.h"
#include "file.h"
#include "memlayout.h"
#include "riscv.h"
#include "defs.h"
#include "proc.h"

/// @brief if call panic, then set this to 1
volatile int panicked = 0;

// lock to avoid interleaving concurrent kprintf's.
static struct {
  struct spinlock lock;
  int locking;
} pr;

static char digits[] = "0123456789abcdef";

static void printint(int xx, int base, int sign) {
  char buf[16];
  int i;
  uint x;

  if (sign && (sign = xx < 0))
    x = -xx;
  else
    x = xx;

  i = 0;
  do {
    buf[i++] = digits[x % base];
  } while ((x /= base) != 0);

  if (sign) buf[i++] = '-';

  while (--i >= 0) consputc(buf[i]);
}

static void printptr(uint64 x) {
  int i;
  consputc('0');
  consputc('x');
  for (i = 0; i < (sizeof(uint64) * 2); i++, x <<= 4) consputc(digits[x >> (sizeof(uint64) * 8 - 4)]);
}

void info_with_color(char *fmt, ...) {
  va_list ap;
  int i, c, locking;
  char *s;
#ifndef LAB_SYSCALL_TEST
  static const char *color = "\033[0;36m";
  static const char *info = "[INFO] ";
  static const char *rst_str = "\033[0m";
#endif

  locking = pr.locking;
  if (locking) acquire(&pr.lock);

  if (fmt == 0) panic("null fmt");

#ifndef LAB_SYSCALL_TEST
  for (i = 0; color[i] != 0; i++) {
    consputc(color[i]);
  }
  for (i = 0; info[i] != 0; i++) {
    consputc(info[i]);
  }
#endif

  va_start(ap, fmt);
  for (i = 0; (c = fmt[i] & 0xff) != 0; i++) {
    if (c != '%') {
      consputc(c);
      continue;
    }
    c = fmt[++i] & 0xff;
    if (c == 0) break;
    switch (c) {
      case 'd':
        printint(va_arg(ap, int), 10, 1);
        break;
      case 'x':
        printint(va_arg(ap, int), 16, 1);
        break;
      case 'p':
        printptr(va_arg(ap, uint64));
        break;
      case 's':
        if ((s = va_arg(ap, char *)) == 0) s = "(null)";
        for (; *s; s++) consputc(*s);
        break;
      case '%':
        consputc('%');
        break;
      default:
        // Print unknown % sequence to draw attention.
        consputc('%');
        consputc(c);
        break;
    }
  }

#ifndef LAB_SYSCALL_TEST
  for (i = 0; rst_str[i] != 0; i++) {
    consputc(rst_str[i]);
  }
#endif

  if (locking) release(&pr.lock);
}

// Print to the console. only understands %d, %x, %p, %s.
void
#ifdef TEST
_printf(const char *filename, unsigned int line, char *fmt, ...)
#else
_printf(char *fmt, ...)
#endif
{
#ifdef TEST
  static const char *file_color = "\033[0;36m";
  static const char *line_color = ":\033[0;35m";
  static const char *content_color = "\t\033[0;32m";
  static const char *rst_str = "\033[0m";
#endif
  va_list ap;
  int i, c, locking;
  char *s;

  locking = pr.locking;
  if (locking) acquire(&pr.lock);

  if (fmt == 0) panic("null fmt");

#ifdef TEST
  for (i = 0; file_color[i] != 0; i++) {
    consputc(file_color[i]);
  }
  for (i = 0; filename[i] != 0; i++) {
    consputc(filename[i]);
  }
  for (i = 0; line_color[i] != 0; i++) {
    consputc(line_color[i]);
  }
  printint(line, 10, 1);
  for (i = 0; content_color[i] != 0; i++) {
    consputc(content_color[i]);
  }
#endif
  va_start(ap, fmt);
  for (i = 0; (c = fmt[i] & 0xff) != 0; i++) {
    if (c != '%') {
      consputc(c);
      continue;
    }
    c = fmt[++i] & 0xff;
    if (c == 0) break;
    switch (c) {
      case 'd':
        printint(va_arg(ap, int), 10, 1);
        break;
      case 'x':
        printint(va_arg(ap, int), 16, 1);
        break;
      case 'p':
        printptr(va_arg(ap, uint64));
        break;
      case 's':
        if ((s = va_arg(ap, char *)) == 0) s = "(null)";
        for (; *s; s++) consputc(*s);
        break;
      case '%':
        consputc('%');
        break;
      default:
        // Print unknown % sequence to draw attention.
        consputc('%');
        consputc(c);
        break;
    }
  }
#ifdef TEST
  for (i = 0; rst_str[i] != 0; i++) {
    consputc(rst_str[i]);
  }
#endif
  if (locking) release(&pr.lock);
}

/// @brief
/// @param s
void panic(char *s) {
  pr.locking = 0;
  kprintf("panic: ");
  kprintf(s);
  kprintf("\n");
  panicked = 1;  // freeze uart output from other CPUs
  for (;;);      // 里面一层死循环, 防止 cpu 跑飞, 以及神秘重启的现象
}

void printfinit(void) {
  initlock(&pr.lock, "pr");
  pr.locking = 1;
}
