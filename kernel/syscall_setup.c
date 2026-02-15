// syscall_setup.c - MSR Configuration for SYSCALL/SYSRET
#include <stdint.h>

// External function
extern void serial_print(const char* str);
extern void syscall_entry(void);

// MSR numbers
#define MSR_EFER        0xC0000080  // Extended Feature Enable Register
#define MSR_STAR        0xC0000081  // Segment selector for SYSCALL/SYSRET
#define MSR_LSTAR       0xC0000082  // Long mode SYSCALL target address
#define MSR_CSTAR       0xC0000083  // Compatibility mode SYSCALL target (not used)
#define MSR_SFMASK      0xC0000084  // SYSCALL flag mask

// EFER bits
#define EFER_SCE        (1 << 0)    // System Call Extensions

// ===========================================
// MSR READ/WRITE FUNCTIONS
// ===========================================

static inline uint64_t rdmsr(uint32_t msr) {
    uint32_t low, high;
    __asm__ volatile (
        "rdmsr"
        : "=a"(low), "=d"(high)
        : "c"(msr)
    );
    return ((uint64_t)high << 32) | low;
}

static inline void wrmsr(uint32_t msr, uint64_t value) {
    uint32_t low = value & 0xFFFFFFFF;
    uint32_t high = (value >> 32) & 0xFFFFFFFF;
    __asm__ volatile (
        "wrmsr"
        :
        : "c"(msr), "a"(low), "d"(high)
    );
}

// ===========================================
// GDT SEGMENT SELECTORS
// ===========================================
// These must match your GDT setup!
// Typical x86-64 GDT layout:
//   0x00: Null descriptor
//   0x08: Kernel code (ring 0, 64-bit)
//   0x10: Kernel data (ring 0)
//   0x18: User data (ring 3) - MUST come before user code!
//   0x20: User code (ring 3, 64-bit)
//   0x28: TSS (optional)

#define KERNEL_CS   0x08    // Kernel code segment
#define KERNEL_DS   0x10    // Kernel data segment
#define USER_DS     0x18    // User data segment (ring 3)
#define USER_CS     0x20    // User code segment (ring 3)

// ===========================================
// SYSCALL MSR SETUP
// ===========================================

void syscall_setup_msrs(void) {
    serial_print("[SYSCALL] Setting up MSRs...\n");
    
    // 1. Enable SYSCALL/SYSRET in EFER
    uint64_t efer = rdmsr(MSR_EFER);
    efer |= EFER_SCE;  // Enable System Call Extensions
    wrmsr(MSR_EFER, efer);
    serial_print("[SYSCALL] EFER.SCE enabled\n");
    
    // 2. Setup STAR (Segment Selector Register)
    // STAR format (64 bits):
    //   Bits 63:48 - User CS selector (CS = value + 16, SS = value + 8)
    //   Bits 47:32 - Kernel CS selector
    //   Bits 31:0  - Reserved
    //
    // When SYSCALL executes:
    //   CS = STAR[47:32]       (kernel code)
    //   SS = STAR[47:32] + 8   (kernel data)
    //
    // When SYSRET executes:
    //   CS = STAR[63:48] + 16  (user code)
    //   SS = STAR[63:48] + 8   (user data)
    
    uint64_t star = 0;
    star |= ((uint64_t)KERNEL_CS << 32);   // Kernel CS for SYSCALL
    star |= ((uint64_t)(USER_DS - 8) << 48); // User base selector for SYSRET
    
    wrmsr(MSR_STAR, star);
    serial_print("[SYSCALL] STAR configured\n");
    
    // 3. Setup LSTAR (Long mode SYSCALL target address)
    // This is the address of our syscall_entry function
    uint64_t lstar = (uint64_t)syscall_entry;
    wrmsr(MSR_LSTAR, lstar);
    
    serial_print("[SYSCALL] LSTAR set to: 0x");
    char hex[17];
    for (int i = 15; i >= 0; i--) {
        int nibble = (lstar >> (i * 4)) & 0xF;
        hex[15-i] = nibble < 10 ? '0' + nibble : 'A' + (nibble - 10);
    }
    hex[16] = '\0';
    serial_print(hex);
    serial_print("\n");
    
    // 4. Setup SFMASK (SYSCALL Flag Mask)
    // Flags that will be CLEARED in RFLAGS when SYSCALL executes
    // We want to clear:
    //   - IF (bit 9) - Interrupt Flag (disable interrupts during syscall)
    //   - DF (bit 10) - Direction Flag
    //   - TF (bit 8) - Trap Flag
    
    uint64_t sfmask = 0;
    sfmask |= (1 << 9);   // Clear IF (disable interrupts)
    sfmask |= (1 << 10);  // Clear DF
    sfmask |= (1 << 8);   // Clear TF
    
    wrmsr(MSR_SFMASK, sfmask);
    serial_print("[SYSCALL] SFMASK configured\n");
    
    // 5. Compatibility mode SYSCALL target (we don't use this in 64-bit)
    wrmsr(MSR_CSTAR, 0);
    
    serial_print("[SYSCALL] MSR configuration complete\n");
}

// ===========================================
// VERIFICATION FUNCTION
// ===========================================

void syscall_verify_setup(void) {
    serial_print("\n=== SYSCALL Configuration Verification ===\n");
    
    // Read back EFER
    uint64_t efer = rdmsr(MSR_EFER);
    serial_print("EFER.SCE: ");
    if (efer & EFER_SCE) {
        serial_print("Enabled\n");
    } else {
        serial_print("DISABLED (ERROR!)\n");
    }
    
    // Read back STAR
    uint64_t star = rdmsr(MSR_STAR);
    serial_print("STAR: 0x");
    char hex[17];
    for (int i = 15; i >= 0; i--) {
        int nibble = (star >> (i * 4)) & 0xF;
        hex[15-i] = nibble < 10 ? '0' + nibble : 'A' + (nibble - 10);
    }
    hex[16] = '\0';
    serial_print(hex);
    serial_print("\n");
    
    // Read back LSTAR
    uint64_t lstar = rdmsr(MSR_LSTAR);
    serial_print("LSTAR: 0x");
    for (int i = 15; i >= 0; i--) {
        int nibble = (lstar >> (i * 4)) & 0xF;
        hex[15-i] = nibble < 10 ? '0' + nibble : 'A' + (nibble - 10);
    }
    hex[16] = '\0';
    serial_print(hex);
    serial_print("\n");
    
    // Read back SFMASK
    uint64_t sfmask = rdmsr(MSR_SFMASK);
    serial_print("SFMASK: 0x");
    for (int i = 15; i >= 0; i--) {
        int nibble = (sfmask >> (i * 4)) & 0xF;
        hex[15-i] = nibble < 10 ? '0' + nibble : 'A' + (nibble - 10);
    }
    hex[16] = '\0';
    serial_print(hex);
    serial_print("\n");
    
    serial_print("\n");
}