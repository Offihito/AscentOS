// ── Architecture Syscalls: arch_prctl ───────────────────────────────────────
#include "syscall.h"
#include "../cpu/msr.h"
#include "../console/klog.h"
#include "../apic/lapic_timer.h"
#include <stdint.h>

// ── arch_prctl sub-commands (Linux x86_64) ──────────────────────────────────
#define ARCH_SET_GS 0x1001
#define ARCH_SET_FS 0x1002
#define ARCH_GET_FS 0x1003
#define ARCH_GET_GS 0x1004

// ── MSR addresses ───────────────────────────────────────────────────────────
#define IA32_FS_BASE        0xC0000100
#define IA32_GS_BASE        0xC0000101
#define IA32_KERNEL_GS_BASE 0xC0000102

static int is_canonical_user_addr(uint64_t addr) {
    uint64_t sign = (addr >> 47) & 1ULL;
    uint64_t upper = addr >> 48;
    return (sign == 0) ? (upper == 0) : (upper == 0xFFFF);
}

// ── sys_arch_prctl ──────────────────────────────────────────────────────────
// Linux ABI: arch_prctl(code, addr)
//   rdi = code, rsi = addr
static uint64_t sys_arch_prctl(uint64_t code, uint64_t addr, uint64_t a2,
                                uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a2; (void)a3; (void)a4; (void)a5;

    switch (code) {
    case ARCH_SET_FS:
        if (!is_canonical_user_addr(addr)) {
            klog_puts("[ARCH_PRCTL] Reject non-canonical SET_FS = ");
            klog_uint64(addr);
            klog_puts("\n");
            return (uint64_t)-22; // -EINVAL
        }
        wrmsr(IA32_FS_BASE, addr);
        klog_puts("[ARCH_PRCTL] SET_FS = ");
        klog_uint64(addr);
        klog_puts("\n");
        return 0;

    case ARCH_GET_FS:
        // addr is a pointer to a uint64_t in userspace where we store the value.
        if (addr) {
            *(uint64_t *)addr = rdmsr(IA32_FS_BASE);
        }
        return 0;

    case ARCH_SET_GS:
        if (!is_canonical_user_addr(addr)) {
            klog_puts("[ARCH_PRCTL] Reject non-canonical SET_GS = ");
            klog_uint64(addr);
            klog_puts("\n");
            return (uint64_t)-22; // -EINVAL
        }
        // For user GS, we write to KERNEL_GS_BASE (swapgs swaps it in/out).
        // After sysret + swapgs, this becomes the active GS for userspace.
        wrmsr(IA32_KERNEL_GS_BASE, addr);
        klog_puts("[ARCH_PRCTL] SET_GS = ");
        klog_uint64(addr);
        klog_puts("\n");
        return 0;

    case ARCH_GET_GS:
        if (addr) {
            *(uint64_t *)addr = rdmsr(IA32_KERNEL_GS_BASE);
        }
        return 0;

    default:
        klog_puts("[ARCH_PRCTL] Unknown code: ");
        klog_uint64(code);
        klog_puts("\n");
        return (uint64_t)-22; // -EINVAL
    }
}

static uint64_t sys_clock_gettime(uint64_t clk_id, uint64_t tp_ptr,
                                   uint64_t a2, uint64_t a3,
                                   uint64_t a4, uint64_t a5) {
    (void)a2; (void)a3; (void)a4; (void)a5;

    if (!tp_ptr) return (uint64_t)-14; // EFAULT

    uint64_t ms = lapic_timer_get_ms();
    uint64_t sec = ms / 1000;
    uint64_t nsec = (ms % 1000) * 1000000ULL;

    // CLOCK_REALTIME and CLOCK_MONOTONIC both return uptime-like time here.
    switch (clk_id) {
    case 0: // CLOCK_REALTIME
    case 1: // CLOCK_MONOTONIC
        ((uint64_t *)tp_ptr)[0] = sec;
        ((uint64_t *)tp_ptr)[1] = nsec;
        return 0;
    default:
        return (uint64_t)-22; // EINVAL
    }
}

// nanosleep(req, rem) - sleep for specified time
// req and rem are pointers to struct timespec { tv_sec, tv_nsec }
static uint64_t sys_nanosleep(uint64_t req_ptr, uint64_t rem_ptr,
                              uint64_t a2, uint64_t a3,
                              uint64_t a4, uint64_t a5) {
    (void)a2; (void)a3; (void)a4; (void)a5; (void)rem_ptr;

    if (!req_ptr) return (uint64_t)-14; // EFAULT

    uint64_t *req = (uint64_t *)req_ptr;
    uint64_t sec = req[0];
    uint64_t nsec = req[1];

    // Convert to milliseconds
    uint64_t total_ms = sec * 1000 + nsec / 1000000;
    if (total_ms == 0 && nsec > 0) total_ms = 1; // Minimum 1ms

    // Busy-wait sleep using LAPIC timer with interrupts enabled
    uint64_t start = lapic_timer_get_ms();
    while ((lapic_timer_get_ms() - start) < total_ms) {
        __asm__ volatile("sti; pause"); // Enable interrupts and pause
    }

    return 0;
}

void syscall_register_arch(void) {
    syscall_register(SYS_ARCH_PRCTL, sys_arch_prctl);
    syscall_register(SYS_CLOCK_GETTIME, sys_clock_gettime);
    syscall_register(SYS_NANOSLEEP, sys_nanosleep);
}
