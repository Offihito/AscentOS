#include "klog.h"
#include "console.h"
#include "../drivers/serial.h"
#include "../lock/spinlock.h"

static spinlock_t klog_lock = SPINLOCK_INIT;

void klog_putchar(char c) {
    spinlock_acquire(&klog_lock);
    console_putchar(c);
    spinlock_release(&klog_lock);
}

void klog_puts(const char *s) {
    spinlock_acquire(&klog_lock);
    console_puts(s);
    spinlock_release(&klog_lock);
}

void klog_uint64(uint64_t num) {
    if (num == 0) {
        klog_puts("0");
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
        klog_putchar(buf[i]);
    }
}
