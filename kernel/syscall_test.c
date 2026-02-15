// syscall_test.c - Test functions for syscall system
#include "syscall.h"

extern void serial_print(const char* str);
extern void int_to_str(int num, char* str);

// ===========================================
// KERNEL-SIDE TEST FUNCTION
// ===========================================
// This is a kernel function that tests the syscall infrastructure
// It will be called from a command (e.g., "testsyscall")

void syscall_kernel_test(void) {
    serial_print("\n=== Testing Syscall Infrastructure ===\n\n");
    
    // Test 1: Check if syscalls are enabled
    serial_print("Test 1: Checking syscall status...\n");
    if (syscall_is_enabled()) {
        serial_print("  [PASS] Syscalls are enabled\n");
    } else {
        serial_print("  [FAIL] Syscalls are NOT enabled!\n");
        return;
    }
    
    // Test 2: Verify MSR configuration
    serial_print("\nTest 2: Verifying MSR configuration...\n");
    extern void syscall_verify_setup(void);
    syscall_verify_setup();
    
    // Test 3: Print statistics
    serial_print("\nTest 3: Current syscall statistics...\n");
    syscall_print_stats();
    
    // Test 4: Manual syscall test (from kernel space - won't work, but demonstrates)
    serial_print("\nTest 4: Syscall handler direct call test...\n");
    serial_print("  Calling sys_getpid() directly...\n");
    int64_t pid = sys_getpid();
    serial_print("  Current PID: ");
    char buf[16];
    int_to_str((int)pid, buf);
    serial_print(buf);
    serial_print("\n");
    
    serial_print("  Calling sys_ascent_gettime()...\n");
    int64_t time = sys_ascent_gettime();
    serial_print("  System uptime: ");
    int_to_str((int)time, buf);
    serial_print(buf);
    serial_print(" ticks\n");
    
    serial_print("\n=== Syscall Infrastructure Test Complete ===\n\n");
    serial_print("NOTE: To fully test syscalls, we need usermode code (Ring 3).\n");
    serial_print("This will be implemented in Phase 2.\n\n");
}

// ===========================================
// USERMODE TEST TASK (for Phase 2)
// ===========================================
// This will be used in Phase 2 when we have usermode support

#ifdef USERSPACE
void usermode_syscall_test(void) {
    // This will run in ring 3
    
    // Test write syscall
    const char* msg = "Hello from userspace!\n";
    write(1, msg, 22);
    
    // Test getpid
    int64_t pid = getpid();
    
    // Test debug syscall
    ascent_debug("Testing debug syscall from userspace");
    
    // Test yield
    ascent_yield();
    
    // Exit
    exit(0);
}
#endif