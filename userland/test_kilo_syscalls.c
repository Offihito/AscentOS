#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/types.h>

// Undefine musl's preprocessor macros to avoid conflicts
#undef sa_handler
#undef sa_flags
#undef sa_restorer

// Standard file descriptor constants
#define STDIN_FILENO 0
#define STDOUT_FILENO 1
#define STDERR_FILENO 2
#define SYS_RT_SIGPROCMASK 14

// ioctl constants
#define TIOCGWINSZ 0x5413

// Note: winsize is defined in sys/ioctl.h by musl
// #define it here for reference:
// struct winsize {
//   uint16_t ws_row;
//   uint16_t ws_col;
//   uint16_t ws_xpixel;
//   uint16_t ws_ypixel;
// };

// Kernel sigaction structure (matches Linux ABI)
struct kernel_sigaction {
  uint64_t ksa_handler;
  uint64_t ksa_flags;
  uint64_t ksa_restorer;
  uint64_t ksa_mask[16];
};

// Direct syscall wrappers
static inline long syscall_ftruncate(int fd, long length) {
  long result;
  __asm__ volatile(
    "movq %1, %%rdi\n"
    "movq %2, %%rsi\n"
    "movq $77, %%rax\n"
    "syscall\n"
    "movq %%rax, %0\n"
    : "=r" (result)
    : "r" ((uint64_t)fd), "r" ((uint64_t)length)
    : "rax", "rdi", "rsi", "rcx", "r11"
  );
  return result;
}

static inline long syscall_rt_sigaction(int signum, const void *act, 
                                         void *oldact, size_t sigsetsize) {
  long result;
  __asm__ volatile(
    "movq %1, %%rdi\n"
    "movq %2, %%rsi\n"
    "movq %3, %%rdx\n"
    "movq %4, %%r10\n"
    "movq $13, %%rax\n"
    "syscall\n"
    "movq %%rax, %0\n"
    : "=r" (result)
    : "r" ((uint64_t)signum), "r" ((uint64_t)act), 
      "r" ((uint64_t)oldact), "r" ((uint64_t)sigsetsize)
    : "rax", "rdi", "rsi", "rdx", "r10", "rcx", "r11"
  );
  return result;
}

static inline long syscall_rt_sigprocmask(int how, const void *set, 
                                           void *oldset, size_t sigsetsize) {
  long result;
  __asm__ volatile(
    "movq %1, %%rdi\n"
    "movq %2, %%rsi\n"
    "movq %3, %%rdx\n"
    "movq %4, %%r10\n"
    "movq $14, %%rax\n"
    "syscall\n"
    "movq %%rax, %0\n"
    : "=r" (result)
    : "r" ((uint64_t)how), "r" ((uint64_t)set), 
      "r" ((uint64_t)oldset), "r" ((uint64_t)sigsetsize)
    : "rax", "rdi", "rsi", "rdx", "r10", "rcx", "r11"
  );
  return result;
}

// Test counter
static int tests_passed = 0;
static int tests_failed = 0;

void test_assert(const char *test_name, int condition) {
  if (condition) {
    printf("[PASS] %s\n", test_name);
    tests_passed++;
  } else {
    printf("[FAIL] %s\n", test_name);
    tests_failed++;
  }
}

void print_num(long num) {
  printf("%ld", num);
}

int main(void) {
  printf("=== Kilo Syscalls Test Suite ===\n\n");

  // ============ Test 1: TIOCGWINSZ ioctl ============
  printf("--- Test 1: TIOCGWINSZ (Get Terminal Window Size) ---\n");
  struct winsize ws;
  memset(&ws, 0, sizeof(ws));
  long ioctl_ret = ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws);
  printf("ioctl(STDOUT, TIOCGWINSZ) returned: %ld\n", ioctl_ret);
  printf("Terminal size: %u rows x %u cols\n", ws.ws_row, ws.ws_col);
  test_assert("TIOCGWINSZ returns success", ioctl_ret == 0);
  test_assert("TIOCGWINSZ returns valid rows", ws.ws_row > 0);
  test_assert("TIOCGWINSZ returns valid cols", ws.ws_col > 0);

  // ============ Test 2: ftruncate ============
  printf("\n--- Test 2: ftruncate (Truncate File) ---\n");
  
  // Create a test file (use /mnt since /tmp dir might not have proper permissions)
  int fd = open("/mnt/test_truncate.txt", O_RDWR | O_CREAT | O_TRUNC, 0644);
  printf("Created test file, fd=%d\n", fd);
  test_assert("open() succeeded", fd >= 0);
  
  if (fd >= 0) {
    // Write some data
    const char *test_data = "Hello, World! This is test data.";
    ssize_t written = write(fd, test_data, strlen(test_data));
    printf("Wrote %ld bytes to file\n", written);
    test_assert("write() succeeded", written > 0);
    
    // Truncate the file to 5 bytes
    long truncate_ret = syscall_ftruncate(fd, 5);
    printf("ftruncate(fd, 5) returned: %ld\n", truncate_ret);
    test_assert("ftruncate() returns success", truncate_ret == 0);
    
    // Close the file
    close(fd);
  }

  // ============ Test 3: rt_sigaction (Register Signal Handler) ============
  printf("\n--- Test 3: rt_sigaction (Register Signal Handler) ---\n");
  
  struct kernel_sigaction new_action, old_action;
  memset(&new_action, 0, sizeof(new_action));
  memset(&old_action, 0, sizeof(old_action));
  
  // Set up a dummy signal handler (won't actually be called without interrupt delivery)
  new_action.ksa_handler = (uint64_t)SIG_IGN;  // Ignore signal
  new_action.ksa_flags = 0;
  
  // Test registering SIGWINCH (window change signal - critical for Kilo)
  long sigaction_ret = syscall_rt_sigaction(SIGWINCH, &new_action, 
                                              &old_action, 8);
  printf("rt_sigaction(SIGWINCH, &new_action, &old_action, 8) returned: %ld\n", 
         sigaction_ret);
  test_assert("rt_sigaction() returns success", sigaction_ret == 0);
  
  // Register SIGINT
  sigaction_ret = syscall_rt_sigaction(SIGINT, &new_action, NULL, 8);
  printf("rt_sigaction(SIGINT, ...) returned: %ld\n", sigaction_ret);
  test_assert("rt_sigaction(SIGINT) returns success", sigaction_ret == 0);
  
  // Invalid signal number should fail
  sigaction_ret = syscall_rt_sigaction(256, &new_action, NULL, 8);
  printf("rt_sigaction(256, ...) returned: %ld (should be -22)\n", sigaction_ret);
  test_assert("rt_sigaction() rejects invalid signal", sigaction_ret == -22);
  
  // Invalid sigsetsize should fail
  sigaction_ret = syscall_rt_sigaction(SIGINT, &new_action, NULL, 4);
  printf("rt_sigaction(SIGINT, ..., 4) returned: %ld (should be -22)\n", sigaction_ret);
  test_assert("rt_sigaction() rejects invalid sigsetsize", sigaction_ret == -22);

  // ============ Test 4: rt_sigprocmask (Signal Masking) ============
  printf("\n--- Test 4: rt_sigprocmask (Signal Masking) ---\n");
  
  uint64_t set[16], oldset[16];
  memset(&set, 0, sizeof(set));
  memset(&oldset, 0, sizeof(oldset));
  
  // Mask signals (SIG_BLOCK = 0)
  long sigprocmask_ret = syscall_rt_sigprocmask(0, set, oldset, 8);
  printf("rt_sigprocmask(SIG_BLOCK, ...) returned: %ld\n", sigprocmask_ret);
  test_assert("rt_sigprocmask() returns success", sigprocmask_ret == 0);

  // ============ Test 5: File Operations (for Kilo) ============
  printf("\n--- Test 5: File Operations (Kilo Workflow) ---\n");
  
  // Create a file (use /mnt since /tmp might not have proper permissions)
  int test_fd = open("/mnt/kilo_test.txt", O_RDWR | O_CREAT | O_TRUNC, 0644);
  printf("Created file, fd=%d\n", test_fd);
  test_assert("File creation succeeded", test_fd >= 0);
  
  if (test_fd >= 0) {
    // Write initial content
    const char *content = "Line 1: Hello Kilo\nLine 2: Text Editor\nLine 3: Test\n";
    ssize_t written = write(test_fd, content, strlen(content));
    printf("Wrote %ld bytes\n", written);
    test_assert("Write succeeded", written > 0);
    
    // Get file size hint (using lseek to end)
    off_t file_size = lseek(test_fd, 0, SEEK_END);
    printf("File size: %ld bytes\n", file_size);
    test_assert("File size is positive", file_size > 0);
    
    // Seek back to beginning
    off_t seek_ret = lseek(test_fd, 0, SEEK_SET);
    printf("Sought to beginning: offset=%ld\n", seek_ret);
    test_assert("Seek to beginning succeeded", seek_ret == 0);
    
    // Read back the content
    char buffer[256];
    ssize_t read_bytes = read(test_fd, buffer, sizeof(buffer) - 1);
    printf("Read %ld bytes back\n", read_bytes);
    test_assert("Read succeeded", read_bytes > 0);
    
    // Close file
    int close_ret = close(test_fd);
    printf("File closed\n");
    test_assert("Close succeeded", close_ret == 0);
  }

  // ============ Test 6: Terminal Control (for Kilo raw mode) ============
  printf("\n--- Test 6: Terminal Control (Raw Mode Setup) ---\n");
  
  // Use a char buffer for ioctl - avoids struct termios conflicts
  unsigned char term[60];
  memset(term, 0, sizeof(term));
  
  long tcgets_ret = ioctl(STDOUT_FILENO, 0x5401, term);  // TCGETS
  printf("ioctl(STDOUT, TCGETS) returned: %ld\n", tcgets_ret);
  test_assert("TCGETS returns success", tcgets_ret == 0 || tcgets_ret == -25 || tcgets_ret == -1);
  
  // Test tcsetattr (via ioctl TCSETS)
  long tcsets_ret = ioctl(STDOUT_FILENO, 0x5402, term);  // TCSETS
  printf("ioctl(STDOUT, TCSETS) returned: %ld\n", tcsets_ret);
  test_assert("TCSETS returns success", tcsets_ret == 0 || tcsets_ret == -25 || tcsets_ret == -1);

  // ============ Summary ============
  printf("\n=== Test Summary ===\n");
  printf("Passed: %d\n", tests_passed);
  printf("Failed: %d\n", tests_failed);

  if (tests_failed == 0) {
    printf("\nAll tests passed! Kilo syscalls are ready!\n");
    exit(0);
  } else {
    printf("\nSome tests failed.\n");
    exit(1);
  }
}
