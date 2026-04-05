// ── Architecture Syscalls: arch_prctl ───────────────────────────────────────
#include "syscall.h"
#include "../cpu/msr.h"
#include "../console/klog.h"
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

// ── sys_arch_prctl ──────────────────────────────────────────────────────────
// Linux ABI: arch_prctl(code, addr)
//   rdi = code, rsi = addr
static uint64_t sys_arch_prctl(uint64_t code, uint64_t addr, uint64_t a2,
                                uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a2; (void)a3; (void)a4; (void)a5;

    switch (code) {
    case ARCH_SET_FS:
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

void syscall_register_arch(void) {
    syscall_register(SYS_ARCH_PRCTL, sys_arch_prctl);
}
