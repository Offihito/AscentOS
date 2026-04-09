// Simple ls implementation using getdents64 syscall
// Linux x86_64 binary compatible with AscentOS

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/syscall.h>

// Linux getdents64 structure
struct linux_dirent64 {
  uint64_t d_ino;
  uint64_t d_off;
  uint16_t d_reclen;
  uint8_t  d_type;
  char     d_name[];
};

// DT_* constants
#define DT_UNKNOWN  0
#define DT_FIFO     1
#define DT_CHR      2
#define DT_DIR      4
#define DT_BLK      6
#define DT_REG      8
#define DT_LNK     10
#define DT_SOCK    12

// getdents64 syscall wrapper
static int getdents64(int fd, void *buf, size_t count) {
  return (int)syscall(SYS_getdents64, fd, buf, count);
}

static const char *file_type_str(uint8_t type) {
  switch (type) {
    case DT_DIR:  return "DIR ";
    case DT_REG:  return "FILE";
    case DT_CHR:  return "CHR ";
    case DT_BLK:  return "BLK ";
    case DT_LNK:  return "LNK ";
    case DT_FIFO: return "FIFO";
    case DT_SOCK: return "SOCK";
    default:      return "    ";
  }
}

int main(int argc, char *argv[]) {
  const char *path = (argc > 1) ? argv[1] : "/";

  int fd = open(path, O_RDONLY | O_DIRECTORY);
  if (fd < 0) {
    printf("ls: cannot open '%s'\n", path);
    return 1;
  }

  char buf[1024];
  int nread;

  printf("Directory listing of %s:\n", path);
  printf("inode       type  name\n");
  printf("----------------------------\n");

  while ((nread = getdents64(fd, buf, sizeof(buf))) > 0) {
    int pos = 0;
    while (pos < nread) {
      struct linux_dirent64 *d = (struct linux_dirent64 *)(buf + pos);
      
      printf("%-10lu  %s   %s\n", d->d_ino, file_type_str(d->d_type), d->d_name);
      
      pos += d->d_reclen;
    }
  }

  if (nread < 0) {
    printf("ls: getdents64 error\n");
    close(fd);
    return 1;
  }

  close(fd);
  return 0;
}
