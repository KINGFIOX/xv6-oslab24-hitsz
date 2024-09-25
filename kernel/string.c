#include "types.h"

/// @brief kernel memory set
/// @param dst
/// @param c
/// @param n
/// @return
void *kmemset(void *dst, int c, uint n) {
  char *cdst = (char *)dst;
  for (int i = 0; i < n; i++) {
    cdst[i] = c;
  }
  return dst;
}

/// @brief
/// @param v1
/// @param v2
/// @param n
/// @return
/// 0: equal,
/// >0: v1 > v2,
/// <0: v1 < v2
int kmemcmp(const void *v1, const void *v2, uint n) {
  const uchar *s1 = v1;
  const uchar *s2 = v2;
  while (n-- > 0) {
    if (*s1 != *s2) return *s1 - *s2;
    s1++, s2++;
  }

  return 0;
}

/// @brief memcpy but care of the address and length (不会出现被覆盖的情况)
/// @param dst
/// @param src
/// @param n
/// @return dst
void *kmemmove(void *dst, const void *src, uint n) {
  const char *s = src;
  char *d = dst;
  if (s < d && s + n > d) {
    s += n;
    d += n;
    while (n-- > 0) *--d = *--s;
  } else
    while (n-- > 0) *d++ = *s++;

  return dst;
}

/// @brief memcpy exists to placate GCC.  Use memmove.
/// @param dst
/// @param src
/// @param n
/// @return
void *kmemcpy(void *dst, const void *src, uint n) { return kmemmove(dst, src, n); }

/// @brief
/// @param p
/// @param q
/// @param n
/// @return
int kstrncmp(const char *p, const char *q, uint n) {
  while (n > 0 && *p && *p == *q) n--, p++, q++;
  if (n == 0) return 0;
  return (uchar)*p - (uchar)*q;
}

/// @brief
/// @param s
/// @param t
/// @param n
/// @return
char *kstrncpy(char *s, const char *t, int n) {
  char *os = s;
  while (n-- > 0 && (*s++ = *t++) != 0);
  while (n-- > 0) *s++ = 0;
  return os;
}

/// @brief Like strncpy but guaranteed to NUL-terminate.
/// @param s
/// @param t
/// @param n
/// @return
char *ksafestrcpy(char *s, const char *t, int n) {
  char *os = s;
  if (n <= 0) return os;
  while (--n > 0 && (*s++ = *t++) != 0);
  *s = 0;
  return os;
}

/// @brief
/// @param s
/// @return
int kstrlen(const char *s) {
  int n;
  for (n = 0; s[n]; n++);
  return n;
}
