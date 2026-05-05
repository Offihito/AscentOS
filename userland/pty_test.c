// PTY Test Program for AscentOS
// Tests the PTY (pseudoterminal) implementation

#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>

// PTY ioctl commands
#define TIOCGPTN 0x80045430    // Get PTY number
#define TIOCSPTLCK 0x40045431  // Lock/unlock PTY

// Terminal ioctl commands
#define TCGETS  0x5401
#define TCSETS  0x5402
#define TIOCGWINSZ 0x5413
#define TIOCSWINSZ 0x5414
#define TIOCGPGRP 0x540F
#define TIOCSPGRP 0x5410

struct termios {
  unsigned int c_iflag;
  unsigned int c_oflag;
  unsigned int c_cflag;
  unsigned int c_lflag;
  unsigned char c_line;
  unsigned char c_cc[32];
  unsigned int __c_ispeed;
  unsigned int __c_ospeed;
};

struct winsize {
  unsigned short ws_row;
  unsigned short ws_col;
  unsigned short ws_xpixel;
  unsigned short ws_ypixel;
};

int main(void) {
  int master_fd, slave_fd;
  int pty_num;
  char slave_path[32];
  char buf[256];
  struct termios term;
  struct winsize ws;
  
  printf("=== PTY Test Program ===\n\n");
  
  // Test 1: Open /dev/ptmx (master)
  printf("Test 1: Opening /dev/ptmx...\n");
  master_fd = open("/dev/ptmx", O_RDWR);
  if (master_fd < 0) {
    printf("  FAILED: Could not open /dev/ptmx (fd=%d)\n", master_fd);
    return 1;
  }
  printf("  PASSED: Opened /dev/ptmx (fd=%d)\n", master_fd);
  
  // Test 2: Get PTY number
  printf("\nTest 2: Getting PTY number...\n");
  if (ioctl(master_fd, TIOCGPTN, &pty_num) < 0) {
    printf("  FAILED: TIOCGPTN ioctl failed\n");
    close(master_fd);
    return 1;
  }
  printf("  PASSED: PTY number = %d\n", pty_num);
  
  // Test 3: Open slave device
  printf("\nTest 3: Opening slave device...\n");
  snprintf(slave_path, sizeof(slave_path), "/dev/pts/%d", pty_num);
  printf("  Slave path: %s\n", slave_path);
  
  slave_fd = open(slave_path, O_RDWR);
  if (slave_fd < 0) {
    printf("  FAILED: Could not open slave (fd=%d)\n", slave_fd);
    close(master_fd);
    return 1;
  }
  printf("  PASSED: Opened slave (fd=%d)\n", slave_fd);
  
  // Test 4: Get terminal settings
  printf("\nTest 4: Getting terminal settings...\n");
  if (ioctl(slave_fd, TCGETS, &term) < 0) {
    printf("  FAILED: TCGETS ioctl failed\n");
  } else {
    printf("  PASSED: Terminal settings:\n");
    printf("    c_iflag=0x%x c_oflag=0x%x c_lflag=0x%x\n",
           term.c_iflag, term.c_oflag, term.c_lflag);
  }
  
  // Test 5: Get window size
  printf("\nTest 5: Getting window size...\n");
  if (ioctl(slave_fd, TIOCGWINSZ, &ws) < 0) {
    printf("  FAILED: TIOCGWINSZ ioctl failed\n");
  } else {
    printf("  PASSED: Window size: %dx%d\n", ws.ws_col, ws.ws_row);
  }
  
  // Test 6: Write from master, read from slave
  printf("\nTest 6: Master -> Slave communication...\n");
  const char *test_msg = "Hello from master!";
  ssize_t written = write(master_fd, test_msg, strlen(test_msg));
  if (written < 0) {
    printf("  FAILED: Write to master failed\n");
  } else {
    printf("  Wrote %zd bytes to master\n", written);
    
    // Small delay for data to propagate
    usleep(10000);
    
    ssize_t n = read(slave_fd, buf, sizeof(buf) - 1);
    if (n < 0) {
      printf("  FAILED: Read from slave failed\n");
    } else {
      buf[n] = '\0';
      printf("  PASSED: Read %zd bytes from slave: \"%s\"\n", n, buf);
    }
  }
  
  // Test 7: Write from slave, read from master
  printf("\nTest 7: Slave -> Master communication...\n");
  const char *reply_msg = "Hello from slave!";
  written = write(slave_fd, reply_msg, strlen(reply_msg));
  if (written < 0) {
    printf("  FAILED: Write to slave failed\n");
  } else {
    printf("  Wrote %zd bytes to slave\n", written);
    
    usleep(10000);
    
    ssize_t n = read(master_fd, buf, sizeof(buf) - 1);
    if (n < 0) {
      printf("  FAILED: Read from master failed\n");
    } else {
      buf[n] = '\0';
      printf("  PASSED: Read %zd bytes from master: \"%s\"\n", n, buf);
    }
  }
  
  // Test 8: Set window size
  printf("\nTest 8: Setting window size...\n");
  ws.ws_col = 120;
  ws.ws_row = 40;
  if (ioctl(slave_fd, TIOCSWINSZ, &ws) < 0) {
    printf("  FAILED: TIOCSWINSZ ioctl failed\n");
  } else {
    // Verify
    struct winsize ws2;
    ioctl(slave_fd, TIOCGWINSZ, &ws2);
    if (ws2.ws_col == 120 && ws2.ws_row == 40) {
      printf("  PASSED: Window size set to 120x40\n");
    } else {
      printf("  FAILED: Window size mismatch\n");
    }
  }
  
  // Test 9: Lock PTY
  printf("\nTest 9: Locking PTY...\n");
  int lock = 1;
  if (ioctl(master_fd, TIOCSPTLCK, &lock) < 0) {
    printf("  FAILED: TIOCSPTLCK ioctl failed\n");
  } else {
    printf("  PASSED: PTY locked\n");
    
    // Try to open slave again - should fail
    int fd2 = open(slave_path, O_RDWR);
    if (fd2 < 0) {
      printf("  PASSED: Slave cannot be opened when locked\n");
    } else {
      printf("  FAILED: Slave opened despite lock!\n");
      close(fd2);
    }
    
    // Unlock
    lock = 0;
    ioctl(master_fd, TIOCSPTLCK, &lock);
  }
  
  // Cleanup
  printf("\n=== Cleanup ===\n");
  close(slave_fd);
  close(master_fd);
  printf("Closed both file descriptors\n");
  
  printf("\n=== PTY Tests Complete ===\n");
  return 0;
}
