#include "string.h"

int strcmp(const char *s1, const char *s2) {
  while (*s1 && (*s1 == *s2)) {
    s1++;
    s2++;
  }
  return *(const unsigned char *)s1 - *(const unsigned char *)s2;
}

int strncmp(const char *s1, const char *s2, size_t n) {
  while (n && *s1 && (*s1 == *s2)) {
    s1++;
    s2++;
    n--;
  }
  if (n == 0)
    return 0;
  return *(const unsigned char *)s1 - *(const unsigned char *)s2;
}

size_t strlen(const char *s) {
  size_t len = 0;
  while (s[len])
    len++;
  return len;
}

char *strcpy(char *dest, const char *src) {
  char *original_dest = dest;
  while ((*dest++ = *src++))
    ;
  return original_dest;
}

char *strncpy(char *dest, const char *src, size_t n) {
  size_t i;
  for (i = 0; i < n && src[i] != '\0'; i++) {
    dest[i] = src[i];
  }
  for (; i < n; i++) {
    dest[i] = '\0';
  }
  return dest;
}

char *strcat(char *dest, const char *src) {
  char *ptr = dest;
  while (*ptr)
    ptr++;
  while ((*ptr++ = *src++))
    ;
  return dest;
}

char *strncat(char *dest, const char *src, size_t n) {
  char *ptr = dest;
  while (*ptr)
    ptr++;
  while (n && (*ptr++ = *src++))
    n--;
  if (n == 0)
    *ptr = '\0';
  return dest;
}

void *memset(void *s, int c, size_t n) {
  unsigned char *p = s;
  uint8_t b = (uint8_t)c;

  // Fill lead-in bytes to align to 8-byte boundary
  while (n > 0 && ((uintptr_t)p & 7) != 0) {
    *p++ = b;
    n--;
  }

  if (n >= 8) {
    uint64_t val64 = (uint64_t)b | ((uint64_t)b << 8) | ((uint64_t)b << 16) |
                     ((uint64_t)b << 24);
    val64 |= (val64 << 32);

    uint64_t *p64 = (uint64_t *)p;
    while (n >= 8) {
      *p64++ = val64;
      n -= 8;
    }
    p = (unsigned char *)p64;
  }

  while (n--) {
    *p++ = b;
  }
  return s;
}

void *memcpy(void *dest, const void *src, size_t n) {
  unsigned char *d = dest;
  const unsigned char *s = src;

  // If n is small or alignment is difficult, just byte copy
  if (n < 16 || (((uintptr_t)d & 7) != ((uintptr_t)s & 7))) {
    while (n--) {
      *d++ = *s++;
    }
    return dest;
  }

  // Align to 8-byte boundary
  while (n > 0 && ((uintptr_t)d & 7) != 0) {
    *d++ = *s++;
    n--;
  }

  uint64_t *d64 = (uint64_t *)d;
  const uint64_t *s64 = (const uint64_t *)s;
  while (n >= 8) {
    *d64++ = *s64++;
    n -= 8;
  }

  d = (unsigned char *)d64;
  s = (const unsigned char *)s64;
  while (n--) {
    *d++ = *s++;
  }

  return dest;
}

int memcmp(const void *s1, const void *s2, size_t n) {
  const unsigned char *p1 = s1, *p2 = s2;
  while (n--) {
    if (*p1 != *p2) {
      return *p1 - *p2;
    }
    p1++;
    p2++;
  }
  return 0;
}

uint32_t atoui(const char *s) {
  uint32_t res = 0;
  while (*s >= '0' && *s <= '9') {
    res = res * 10 + (*s - '0');
    s++;
  }
  return res;
}
