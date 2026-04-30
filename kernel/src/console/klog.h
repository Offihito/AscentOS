#ifndef CONSOLE_KLOG_H
#define CONSOLE_KLOG_H

#include <stdint.h>

void klog_putchar(char c);
void klog_puts(const char *s);
void klog_uint64(uint64_t num);
void klog_hex64(uint64_t num);
void klog_hex32(uint32_t num);

#endif
