#include "klog.h"
#include "../drivers/serial.h"
#include "../lock/spinlock.h"
#include "console.h"

static spinlock_t klog_lock = SPINLOCK_INIT;

void klog_putchar(char c) {
  spinlock_acquire(&klog_lock);
  serial_putchar(c);
  spinlock_release(&klog_lock);
}

void klog_puts(const char *s) {
  spinlock_acquire(&klog_lock);
  while (*s) {
    serial_putchar(*s++);
  }
  spinlock_release(&klog_lock);
}

void klog_uint64(uint64_t num) {
  spinlock_acquire(&klog_lock);
  if (num == 0) {
    serial_putchar('0');
    spinlock_release(&klog_lock);
    return;
  }
  char buf[20];
  int i = 0;
  while (num > 0) {
    buf[i++] = '0' + (num % 10);
    num /= 10;
  }
  while (i > 0) {
    i--;
    serial_putchar(buf[i]);
  }
  spinlock_release(&klog_lock);
}

void klog_hex64(uint64_t num) {
  spinlock_acquire(&klog_lock);
  const char *hex = "0123456789ABCDEF";
  serial_putchar('0');
  serial_putchar('x');
  for (int i = 60; i >= 0; i -= 4) {
    serial_putchar(hex[(num >> i) & 0xF]);
  }
  spinlock_release(&klog_lock);
}

void klog_hex32(uint32_t num) {
  spinlock_acquire(&klog_lock);
  const char *hex = "0123456789ABCDEF";
  serial_putchar('0');
  serial_putchar('x');
  for (int i = 28; i >= 0; i -= 4) {
    serial_putchar(hex[(num >> i) & 0xF]);
  }
  spinlock_release(&klog_lock);
}
