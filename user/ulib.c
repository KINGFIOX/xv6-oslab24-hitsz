#include "kernel/types.h"
#include "kernel/stat.h"
#include "kernel/fcntl.h"
#include "user/user.h"

char *strcpy(char *s, const char *t) {
  char *os;

  os = s;
  while ((*s++ = *t++) != 0)
    ;
  return os;
}

/// @brief
/// @return
/// - 0: equal
/// - <0: p < q
/// - >0: p > q
int strcmp(const char *p, const char *q) {
  while (*p && *p == *q) p++, q++;
  return (uchar)*p - (uchar)*q;
}

uint strlen(const char *s) {
  int n;

  for (n = 0; s[n]; n++)
    ;
  return n;
}

void *memset(void *dst, int c, uint n) {
  char *cdst = (char *)dst;
  int i;
  for (i = 0; i < n; i++) {
    cdst[i] = c;
  }
  return dst;
}

char *strchr(const char *s, char c) {
  for (; *s; s++)
    if (*s == c) return (char *)s;
  return 0;
}

char *gets(char *buf, int max) {
  int i, cc;
  char c;

  for (i = 0; i + 1 < max;) {
    cc = read(0, &c, 1);
    if (cc < 1) break;
    buf[i++] = c;
    if (c == '\n' || c == '\r') break;
  }
  buf[i] = '\0';
  return buf;
}

/// @brief
/// @param file_name
/// @param st (return)
/// @return
int stat(const char *file_name, struct stat *st) {
  int fd = open(file_name, O_RDONLY);
  if (fd < 0) return -1;
  int r = fstat(fd, st);
  close(fd);
  return r;
}

// convert string to integer
int atoi(const char *s) {
  int n;

  n = 0;
  while ('0' <= *s && *s <= '9') n = n * 10 + *s++ - '0';
  return n;
}

// convert integer to string
void itoa(int n, char s[]) {
  int i = 0;
  int is_negative = 0;

  if (n < 0) {
    is_negative = 1;
    n = -n;
  }

  do {
    s[i++] = n % 10 + '0';
    n /= 10;
  } while (n > 0);

  if (is_negative) {
    s[i++] = '-';
  }

  s[i] = '\0';

  // invert string
  for (int j = 0; j < i / 2; j++) {
    char temp = s[j];
    s[j] = s[i - j - 1];
    s[i - j - 1] = temp;
  }
}

void *memmove(void *vdst, const void *vsrc, int n) {
  char *dst;
  const char *src;

  dst = vdst;
  src = vsrc;
  if (src > dst) {
    while (n-- > 0) *dst++ = *src++;
  } else {
    dst += n;
    src += n;
    while (n-- > 0) *--dst = *--src;
  }
  return vdst;
}

int memcmp(const void *s1, const void *s2, uint n) {
  const char *p1 = s1, *p2 = s2;
  while (n-- > 0) {
    if (*p1 != *p2) {
      return *p1 - *p2;
    }
    p1++;
    p2++;
  }
  return 0;
}

void *memcpy(void *dst, const void *src, uint n) { return memmove(dst, src, n); }
