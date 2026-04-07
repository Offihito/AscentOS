#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

// Syscall numbers (Linux x86_64 ABI)
#define SYS_SET_TID_ADDRESS 218
#define SYS_EXIT_GROUP 231
#define SYS_IOCTL 16

// Common ioctl commands
#define TCGETS    0x5401   // Get terminal attributes
#define TCSETS    0x5402   // Set terminal attributes
#define FIOCLEX   0x20006601  // Set close-on-exec for fd
#define FIONREAD  0x541B   // Get number of bytes available to read

// termios structure
struct termios {
  uint32_t c_iflag;
  uint32_t c_oflag;
  uint32_t c_cflag;
  uint32_t c_lflag;
  uint8_t c_line;
  uint8_t c_cc[32];
  uint32_t c_ispeed;
  uint32_t c_ospeed;
};

// Direct syscall wrappers for testing
static inline long syscall_set_tid_address(uint64_t *tidptr) {
  long result;
  __asm__ volatile(
    "movq %1, %%rdi\n"
    "movq $218, %%rax\n"
    "syscall\n"
    "movq %%rax, %0\n"
    : "=r" (result)
    : "r" (tidptr)
    : "rax", "rdi", "rcx", "r11"
  );
  return result;
}

static inline long syscall_ioctl(int fd, unsigned long request, void *arg) {
  long result;
  __asm__ volatile(
    "movq %1, %%rdi\n"
    "movq %2, %%rsi\n"
    "movq %3, %%rdx\n"
    "movq $16, %%rax\n"
    "syscall\n"
    "movq %%rax, %0\n"
    : "=r" (result)
    : "r" ((uint64_t)fd), "r" (request), "r" ((uint64_t)arg)
    : "rax", "rdi", "rsi", "rdx", "rcx", "r11"
  );
  return result;
}

static inline void print_str(const char *str) {
  write(STDOUT_FILENO, str, strlen(str));
}

static inline void print_num(long num) {
  char buf[32];
  int len = 0;
  
  if (num == 0) {
    write(STDOUT_FILENO, "0", 1);
    return;
  }
  
  if (num < 0) {
    write(STDOUT_FILENO, "-", 1);
    num = -num;
  }
  
  while (num > 0) {
    buf[len++] = '0' + (num % 10);
    num /= 10;
  }
  
  for (int i = len - 1; i >= 0; i--) {
    write(STDOUT_FILENO, &buf[i], 1);
  }
}

static inline void print_hex(uint64_t num) {
  const char *hex = "0123456789abcdef";
  char buf[16];
  int len = 0;
  
  if (num == 0) {
    write(STDOUT_FILENO, "0x0", 3);
    return;
  }
  
  while (num > 0) {
    buf[len++] = hex[num & 0xf];
    num >>= 4;
  }
  
  write(STDOUT_FILENO, "0x", 2);
  for (int i = len - 1; i >= 0; i--) {
    write(STDOUT_FILENO, &buf[i], 1);
  }
}

// Test counter
static int tests_passed = 0;
static int tests_failed = 0;

void test_assert(const char *test_name, int condition) {
  if (condition) {
    print_str("[PASS] ");
    print_str(test_name);
    print_str("\n");
    tests_passed++;
  } else {
    print_str("[FAIL] ");
    print_str(test_name);
    print_str("\n");
    tests_failed++;
  }
}

int main(void) {
  print_str("=== AscentOS Syscall Test Suite ===\n");
  print_str("\n");

  // Test 1: set_tid_address
  print_str("--- Test 1: set_tid_address ---\n");
  uint64_t tid_var = 0;
  long tid = syscall_set_tid_address(&tid_var);
  print_str("set_tid_address returned TID: ");
  print_num(tid);
  print_str(" (should be > 0)\n");
  test_assert("set_tid_address returns valid TID", tid > 0);

  // Test 2: set_tid_address with NULL should still work
  print_str("\n--- Test 2: set_tid_address with NULL ---\n");
  long tid2 = syscall_set_tid_address(NULL);
  print_str("set_tid_address(NULL) returned: ");
  print_num(tid2);
  print_str(" (should match previous TID)\n");
  test_assert("set_tid_address(NULL) works", tid2 > 0);

  // Test 3: ioctl TCGETS
  print_str("\n--- Test 3: ioctl TCGETS ---\n");
  struct termios term;
  memset(&term, 0, sizeof(term));
  long ioctl_ret = syscall_ioctl(STDOUT_FILENO, TCGETS, &term);
  print_str("ioctl(STDOUT_FILENO, TCGETS) returned: ");
  print_num(ioctl_ret);
  print_str("\n");
  if (ioctl_ret == 0) {
    print_str("Terminal settings: c_iflag=");
    print_hex(term.c_iflag);
    print_str(" c_oflag=");
    print_hex(term.c_oflag);
    print_str("\n");
  }
  test_assert("ioctl TCGETS succeeds", ioctl_ret == 0);

  // Test 4: ioctl TCSETS
  print_str("\n--- Test 4: ioctl TCSETS ---\n");
  struct termios new_term;
  memset(&new_term, 0, sizeof(new_term));
  new_term.c_iflag = 0x1234;
  new_term.c_oflag = 0x5678;
  long tcsets_ret = syscall_ioctl(STDOUT_FILENO, TCSETS, &new_term);
  print_str("ioctl(STDOUT_FILENO, TCSETS) returned: ");
  print_num(tcsets_ret);
  print_str(" (should be 0)\n");
  test_assert("ioctl TCSETS succeeds", tcsets_ret == 0);

  // Test 5: ioctl FIOCLEX (set close-on-exec)
  print_str("\n--- Test 5: ioctl FIOCLEX ---\n");
  long fioclex_ret = syscall_ioctl(STDOUT_FILENO, FIOCLEX, NULL);
  print_str("ioctl(STDOUT_FILENO, FIOCLEX) returned: ");
  print_num(fioclex_ret);
  print_str(" (should be 0)\n");
  test_assert("ioctl FIOCLEX succeeds", fioclex_ret == 0);

  // Test 6: ioctl FIONREAD (get bytes available)
  print_str("\n--- Test 6: ioctl FIONREAD ---\n");
  uint32_t bytes_available = 0;
  long fionread_ret = syscall_ioctl(STDOUT_FILENO, FIONREAD, &bytes_available);
  print_str("ioctl(STDOUT_FILENO, FIONREAD) returned: ");
  print_num(fionread_ret);
  print_str(" bytes_available: ");
  print_num(bytes_available);
  print_str(" (should be >= 0)\n");
  test_assert("ioctl FIONREAD succeeds", fionread_ret == 0);

  // Test 7: ioctl with invalid fd
  print_str("\n--- Test 7: ioctl with invalid fd ---\n");
  long invalid_fd_ret = syscall_ioctl(99, TCGETS, &term);
  print_str("ioctl(99, TCGETS) returned: ");
  print_num(invalid_fd_ret);
  print_str(" (should be -25)\n");
  test_assert("ioctl with invalid fd returns ENOTTY", invalid_fd_ret == -25);

  // Test 8: ioctl with invalid request
  print_str("\n--- Test 8: ioctl with invalid request ---\n");
  long invalid_req_ret = syscall_ioctl(STDOUT_FILENO, 0xDEADBEEF, NULL);
  print_str("ioctl(STDOUT_FILENO, 0xDEADBEEF) returned: ");
  print_num(invalid_req_ret);
  print_str(" (should be -22)\n");
  test_assert("ioctl with invalid request returns EINVAL", invalid_req_ret == -22);

  // Summary
  print_str("\n=== Test Summary ===\n");
  print_str("Passed: ");
  print_num(tests_passed);
  print_str("\n");
  print_str("Failed: ");
  print_num(tests_failed);
  print_str("\n");

  if (tests_failed == 0) {
    print_str("\nAll tests passed!\n");
    exit(0);
  } else {
    print_str("\nSome tests failed.\n");
    exit(1);
  }
}
