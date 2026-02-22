// syscall_test.c - Syscall altyapısı kapsamlı test rutinleri
// Ayrı dosyada tutulur; üretim build'lerine dahil edilmeyebilir.

#include "syscall.h"
#include <stdint.h>

extern void serial_print(const char* str);
extern void serial_putchar(char c);
extern void int_to_str(int num, char* str);
extern int  syscall_is_enabled(void);

static void print_hex64(uint64_t v) {
    const char* h = "0123456789ABCDEF";
    char buf[17];
    for (int i = 0; i < 16; i++)
        buf[i] = h[(v >> (60 - i * 4)) & 0xF];
    buf[16] = '\0';
    serial_print(buf);
}

static void print_uint64(uint64_t v) {
    if (v == 0) { serial_print("0"); return; }
    char buf[21]; int i = 0; uint64_t t = v;
    while (t > 0) { buf[i++] = '0' + (t % 10); t /= 10; }
    buf[i] = '\0';
    for (int a = 0, b = i - 1; a < b; a++, b--) {
        char c = buf[a]; buf[a] = buf[b]; buf[b] = c;
    }
    serial_print(buf);
}

// ============================================================
// SYSCALL_TEST – v4 testleri dahil
// ============================================================
void syscall_test(void) {
    if (!syscall_is_enabled()) {
        serial_print("[SYSCALL TEST] SYSCALL not enabled!\n");
        return;
    }
    serial_print("\n========================================\n");
    serial_print("[SYSCALL TEST] v4 comprehensive tests\n");
    serial_print("========================================\n");

    uint64_t ret = 0;

#define DO_SYSCALL0(num) \
    __asm__ volatile ("syscall" : "=a"(ret) : "a"((uint64_t)(num)) : "rcx","r11")

#define DO_SYSCALL1(num, a1) \
    __asm__ volatile ("syscall" : "=a"(ret) \
        : "a"((uint64_t)(num)), "D"((uint64_t)(a1)) : "rcx","r11","memory")

#define DO_SYSCALL2(num, a1, a2) \
    __asm__ volatile ("syscall" : "=a"(ret) \
        : "a"((uint64_t)(num)), "D"((uint64_t)(a1)), "S"((uint64_t)(a2)) \
        : "rcx","r11","memory")

#define DO_SYSCALL3(num, a1, a2, a3) \
    __asm__ volatile ("syscall" : "=a"(ret) \
        : "a"((uint64_t)(num)), "D"((uint64_t)(a1)), "S"((uint64_t)(a2)), \
          "d"((uint64_t)(a3)) : "rcx","r11","memory")

    // ── Önceki testler (v1/v2) – kısa özet ─────────────────────
    serial_print("\n[T01] SYS_WRITE stdout:\n");
    const char* wmsg = "  Hello from SYS_WRITE!\n";
    DO_SYSCALL3(SYS_WRITE, 1, wmsg, 24);
    serial_print("  ret="); { char b[8]; int_to_str((int)ret,b); serial_print(b); } serial_print("\n");

    serial_print("\n[T02] SYS_GETPID:\n");
    DO_SYSCALL0(SYS_GETPID);
    serial_print("  pid="); print_uint64(ret); serial_print("\n");

    serial_print("\n[T03] SYS_SBRK(0) query:\n");
    DO_SYSCALL1(SYS_SBRK, 0);
    serial_print("  brk=0x"); print_hex64(ret); serial_print("\n");

    // ── SYS_BRK (18) ────────────────────────────────────────────
    serial_print("\n[T04] SYS_BRK(0) query:\n");
    DO_SYSCALL1(SYS_BRK, 0);
    uint64_t initial_brk = ret;
    serial_print("  brk=0x"); print_hex64(ret); serial_print("\n");

    serial_print("\n[T05] SYS_BRK(brk+8192) extend:\n");
    DO_SYSCALL1(SYS_BRK, initial_brk + 8192);
    serial_print("  new_brk=0x"); print_hex64(ret); serial_print("\n");

    // ── SYS_MMAP (16) ───────────────────────────────────────────
    serial_print("\n[T06] SYS_MMAP anonymous 4096 bytes:\n");
    register uint64_t r10_flags asm("r10") = MAP_ANONYMOUS | MAP_PRIVATE;
    register uint64_t r8_fd     asm("r8")  = (uint64_t)(int64_t)-1;
    register uint64_t r9_off    asm("r9")  = 0;
    __asm__ volatile ("syscall"
        : "=a"(ret)
        : "a"((uint64_t)SYS_MMAP),
          "D"((uint64_t)0),
          "S"((uint64_t)4096),
          "d"((uint64_t)(PROT_READ|PROT_WRITE)),
          "r"(r10_flags), "r"(r8_fd), "r"(r9_off)
        : "rcx", "r11", "memory");
    serial_print("  mmap_addr=0x"); print_hex64(ret); serial_print("\n");
    uint64_t mmap_addr = ret;

    if (ret != (uint64_t)MAP_FAILED) {
        volatile char* p = (volatile char*)ret;
        p[0] = 'A'; p[1] = 'B'; p[2] = '\0';
        serial_print("  mmap[0]="); serial_putchar(p[0]);
        serial_print(" mmap[1]="); serial_putchar(p[1]);
        serial_print("\n");
    }

    // ── SYS_MUNMAP (17) ─────────────────────────────────────────
    if (mmap_addr != (uint64_t)MAP_FAILED) {
        serial_print("\n[T07] SYS_MUNMAP:\n");
        DO_SYSCALL2(SYS_MUNMAP, mmap_addr, 4096);
        serial_print("  ret="); { char b[8]; int_to_str((int)ret,b); serial_print(b); }
        serial_print(" (expect 0)\n");
    }

    // ── SYS_PIPE (22) ───────────────────────────────────────────
    serial_print("\n[T08] SYS_PIPE:\n");
    int pipe_fds[2] = {-1, -1};
    DO_SYSCALL1(SYS_PIPE, pipe_fds);
    serial_print("  ret="); { char b[8]; int_to_str((int)ret,b); serial_print(b); }
    serial_print(" rfd="); { char b[8]; int_to_str(pipe_fds[0],b); serial_print(b); }
    serial_print(" wfd="); { char b[8]; int_to_str(pipe_fds[1],b); serial_print(b); }
    serial_print("\n");

    if (ret == (uint64_t)SYSCALL_OK && pipe_fds[0] > 0 && pipe_fds[1] > 0) {
        serial_print("\n[T09] PIPE write/read:\n");
        const char* pmsg = "pipe_test_data";
        DO_SYSCALL3(SYS_WRITE, pipe_fds[1], pmsg, 14);
        serial_print("  write_ret="); { char b[8]; int_to_str((int)ret,b); serial_print(b); }
        serial_print("\n");

        char rbuf[32] = {0};
        DO_SYSCALL3(SYS_READ, pipe_fds[0], rbuf, 14);
        serial_print("  read_ret="); { char b[8]; int_to_str((int)ret,b); serial_print(b); }
        serial_print(" data=\""); serial_print(rbuf); serial_print("\"\n");

        // ── SYS_DUP2 (23) ─────────────────────────────────────
        serial_print("\n[T10] SYS_DUP2 (wfd -> fd 8):\n");
        DO_SYSCALL2(SYS_DUP2, pipe_fds[1], 8);
        serial_print("  ret="); { char b[8]; int_to_str((int)ret,b); serial_print(b); }
        serial_print(" (expect 8)\n");

        DO_SYSCALL3(SYS_WRITE, 8, "dup2_ok\n", 8);

        DO_SYSCALL1(SYS_CLOSE, pipe_fds[0]);
        DO_SYSCALL1(SYS_CLOSE, pipe_fds[1]);
        DO_SYSCALL1(SYS_CLOSE, 8);
    }

    // ── SYS_EXECVE (20) – stub ──────────────────────────────────
    serial_print("\n[T11] SYS_EXECVE stub (expect ENOSYS):\n");
    DO_SYSCALL3(SYS_EXECVE, "/bin/sh", 0, 0);
    serial_print("  ret="); { char b[8]; int_to_str((int)ret,b); serial_print(b); }
    serial_print(" (expect -2)\n");

    // ── SYS_FORK (19) ───────────────────────────────────────────
    serial_print("\n[T12] SYS_FORK (kernel context smoke test):\n");
    DO_SYSCALL0(SYS_FORK);
    serial_print("  fork_ret="); { char b[16]; int_to_str((int)ret,b); serial_print(b); }
    serial_print("\n");
    if ((int64_t)ret > 0) {
        serial_print("  [parent] child_pid="); print_uint64(ret); serial_print("\n");

        serial_print("\n[T13] SYS_WAITPID(child, WNOHANG):\n");
        int ws = 0;
        uint64_t child_pid = ret;
        __asm__ volatile ("syscall"
            : "=a"(ret)
            : "a"((uint64_t)SYS_WAITPID),
              "D"(child_pid),
              "S"((uint64_t)&ws),
              "d"((uint64_t)WNOHANG)
            : "rcx","r11","memory");
        serial_print("  waitpid_ret="); { char b[16]; int_to_str((int)ret,b); serial_print(b); }
        serial_print(" status="); { char b[16]; int_to_str(ws,b); serial_print(b); }
        serial_print("\n");
    }

    // ── Bilinmeyen syscall ───────────────────────────────────────
    serial_print("\n[T14] Unknown syscall 999 (expect ENOSYS=-2):\n");
    DO_SYSCALL0(999);
    serial_print("  ret="); { char b[8]; int_to_str((int)ret,b); serial_print(b); }
    serial_print(" (expect -2)\n");

    // ============================================================
    // v4 YENİ TESTLER
    // ============================================================
    serial_print("\n----------------------------------------\n");
    serial_print("[SYSCALL TEST] v4 tests begin\n");
    serial_print("----------------------------------------\n");

    // ── SYS_FSTAT – stdin (fd=0) ────────────────────────────────
    serial_print("\n[T15] SYS_FSTAT fd=0 (stdin, expect S_IFCHR):\n");
    stat_t st;
    DO_SYSCALL2(SYS_FSTAT, 0, &st);
    serial_print("  ret="); { char b[8]; int_to_str((int)ret,b); serial_print(b); }
    serial_print(" st_mode=0x"); print_hex64((uint64_t)st.st_mode);
    serial_print(" st_size="); print_uint64(st.st_size);
    serial_print("\n");
    if (st.st_mode & S_IFCHR)
        serial_print("  [OK] S_IFCHR set (character device)\n");
    else
        serial_print("  [FAIL] S_IFCHR not set!\n");

    // ── SYS_FSTAT – stdout (fd=1) ───────────────────────────────
    serial_print("\n[T16] SYS_FSTAT fd=1 (stdout, expect S_IFCHR):\n");
    stat_t st2;
    DO_SYSCALL2(SYS_FSTAT, 1, &st2);
    serial_print("  ret="); { char b[8]; int_to_str((int)ret,b); serial_print(b); }
    serial_print(" st_mode=0x"); print_hex64((uint64_t)st2.st_mode);
    serial_print("\n");

    // ── SYS_FSTAT – pipe fd ──────────────────────────────────────
    serial_print("\n[T17] SYS_FSTAT üzerinden pipe (S_IFIFO bekleniyor):\n");
    int pstat_fds[2] = {-1, -1};
    DO_SYSCALL1(SYS_PIPE, pstat_fds);
    if (ret == (uint64_t)SYSCALL_OK && pstat_fds[0] > 0) {
        // Pipe'a birkaç byte yaz (bytes_avail için)
        DO_SYSCALL3(SYS_WRITE, pstat_fds[1], "hello", 5);
        stat_t sp;
        DO_SYSCALL2(SYS_FSTAT, pstat_fds[0], &sp);
        serial_print("  ret="); { char b[8]; int_to_str((int)ret,b); serial_print(b); }
        serial_print(" st_mode=0x"); print_hex64((uint64_t)sp.st_mode);
        serial_print(" st_size(bytes_avail)="); print_uint64(sp.st_size);
        serial_print("\n");
        if (sp.st_mode & S_IFIFO)
            serial_print("  [OK] S_IFIFO set\n");
        else
            serial_print("  [FAIL] S_IFIFO not set!\n");
        if (sp.st_size == 5)
            serial_print("  [OK] bytes_avail=5\n");
        else
            serial_print("  [WARN] bytes_avail mismatch\n");
        DO_SYSCALL1(SYS_CLOSE, pstat_fds[0]);
        DO_SYSCALL1(SYS_CLOSE, pstat_fds[1]);
    } else {
        serial_print("  [SKIP] pipe allocation failed\n");
    }

    // ── SYS_FSTAT – geçersiz fd ──────────────────────────────────
    serial_print("\n[T18] SYS_FSTAT fd=999 (expect EBADF=-5):\n");
    stat_t st_bad;
    DO_SYSCALL2(SYS_FSTAT, 999, &st_bad);
    serial_print("  ret="); { char b[8]; int_to_str((int)(int64_t)ret,b); serial_print(b); }
    serial_print(" (expect -5)\n");

    // ── SYS_LSEEK – open bir dosyada SEEK_SET ────────────────────
    // Not: SYS_OPEN /dev/serial0'a seek yapılabilmesi için FD_TYPE_SERIAL
    // seek'i desteklemiyor; bu yüzden geçersiz test yapıyoruz.
    serial_print("\n[T19] SYS_LSEEK stdin (expect EINVAL=-1, serial):\n");
    DO_SYSCALL3(SYS_LSEEK, 0, 0, SEEK_SET);
    serial_print("  ret="); { char b[8]; int_to_str((int)(int64_t)ret,b); serial_print(b); }
    serial_print(" (expect -1, stdin not seekable)\n");

    // Gerçek dosya fd'si açılabilirse lseek testi
    serial_print("\n[T20] SYS_OPEN + SYS_LSEEK (file fd):\n");
    DO_SYSCALL2(SYS_OPEN, "/dev/serial0", O_RDWR);
    int test_fd = (int)(int64_t)ret;
    serial_print("  open ret="); { char b[8]; int_to_str(test_fd,b); serial_print(b); }
    serial_print("\n");
    if (test_fd >= 3) {
        // Serial fd seek desteklemez → EINVAL bekliyoruz
        DO_SYSCALL3(SYS_LSEEK, test_fd, 0, SEEK_SET);
        serial_print("  lseek SEEK_SET ret="); { char b[8]; int_to_str((int)(int64_t)ret,b); serial_print(b); }
        serial_print(" (expect -1 for serial)\n");
        // SEEK_CUR
        DO_SYSCALL3(SYS_LSEEK, test_fd, 10, SEEK_CUR);
        serial_print("  lseek SEEK_CUR+10 ret="); { char b[8]; int_to_str((int)(int64_t)ret,b); serial_print(b); }
        serial_print("\n");
        DO_SYSCALL1(SYS_CLOSE, test_fd);
    }

    // ── SYS_LSEEK – geçersiz whence ─────────────────────────────
    serial_print("\n[T21] SYS_LSEEK geçersiz whence=99 (expect EINVAL=-1):\n");
    DO_SYSCALL3(SYS_LSEEK, 1, 0, 99);
    serial_print("  ret="); { char b[8]; int_to_str((int)(int64_t)ret,b); serial_print(b); }
    serial_print(" (expect -1)\n");

    // ── SYS_IOCTL – TCGETS ──────────────────────────────────────
    serial_print("\n[T22] SYS_IOCTL TCGETS (fd=0):\n");
    termios_t tios;
    DO_SYSCALL3(SYS_IOCTL, 0, TCGETS, &tios);
    serial_print("  ret="); { char b[8]; int_to_str((int)ret,b); serial_print(b); }
    serial_print(" c_iflag=0x"); print_hex64((uint64_t)tios.c_iflag);
    serial_print(" c_lflag=0x"); print_hex64((uint64_t)tios.c_lflag);
    serial_print("\n");
    if (tios.c_lflag & ECHO)
        serial_print("  [OK] ECHO flag set\n");
    if (tios.c_lflag & ICANON)
        serial_print("  [OK] ICANON flag set\n");

    // ── SYS_IOCTL – TCSETS: raw mode geçişi ─────────────────────
    serial_print("\n[T23] SYS_IOCTL TCSETS (raw mode):\n");
    termios_t raw = tios;
    // Raw mod: ECHO + ICANON + ISIG + IEXTEN kapat
    raw.c_lflag &= ~(ECHO | ECHOE | ECHOK | ICANON | ISIG | IEXTEN);
    raw.c_iflag &= ~(ICRNL | IXON);
    raw.c_oflag &= ~OPOST;
    raw.c_cc[VMIN]  = 1;
    raw.c_cc[VTIME] = 0;
    DO_SYSCALL3(SYS_IOCTL, 0, TCSETS, &raw);
    serial_print("  ret="); { char b[8]; int_to_str((int)ret,b); serial_print(b); }
    serial_print(" (expect 0)\n");

    // Doğrula: geri oku
    termios_t verify;
    DO_SYSCALL3(SYS_IOCTL, 0, TCGETS, &verify);
    serial_print("  verify c_lflag=0x"); print_hex64((uint64_t)verify.c_lflag);
    if (!(verify.c_lflag & ECHO))
        serial_print(" [OK] ECHO off\n");
    else
        serial_print(" [FAIL] ECHO still on!\n");

    // Canonical moda geri dön
    serial_print("\n[T24] SYS_IOCTL TCSETSF (canonical'a geri dön):\n");
    DO_SYSCALL3(SYS_IOCTL, 0, TCSETSF, &tios);
    serial_print("  ret="); { char b[8]; int_to_str((int)ret,b); serial_print(b); }
    serial_print(" (expect 0)\n");
    termios_t verify2;
    DO_SYSCALL3(SYS_IOCTL, 0, TCGETS, &verify2);
    if (verify2.c_lflag & ECHO)
        serial_print("  [OK] ECHO restored\n");
    else
        serial_print("  [FAIL] ECHO not restored!\n");

    // ── SYS_IOCTL – TIOCGWINSZ ──────────────────────────────────
    serial_print("\n[T25] SYS_IOCTL TIOCGWINSZ:\n");
    winsize_t ws;
    DO_SYSCALL3(SYS_IOCTL, 1, TIOCGWINSZ, &ws);
    serial_print("  ret="); { char b[8]; int_to_str((int)ret,b); serial_print(b); }
    serial_print(" rows="); { char b[8]; int_to_str((int)ws.ws_row,b); serial_print(b); }
    serial_print(" cols="); { char b[8]; int_to_str((int)ws.ws_col,b); serial_print(b); }
    serial_print("\n");
    if (ws.ws_row == 25 && ws.ws_col == 80)
        serial_print("  [OK] Default 80x25\n");
    else
        serial_print("  [WARN] Unexpected window size\n");

    // ── SYS_IOCTL – TIOCSWINSZ ──────────────────────────────────
    serial_print("\n[T26] SYS_IOCTL TIOCSWINSZ (132x50):\n");
    winsize_t new_ws = { .ws_row = 50, .ws_col = 132, .ws_xpixel = 0, .ws_ypixel = 0 };
    DO_SYSCALL3(SYS_IOCTL, 1, TIOCSWINSZ, &new_ws);
    serial_print("  ret="); { char b[8]; int_to_str((int)ret,b); serial_print(b); }
    serial_print(" (expect 0)\n");
    // Geri oku
    winsize_t ws2;
    DO_SYSCALL3(SYS_IOCTL, 1, TIOCGWINSZ, &ws2);
    serial_print("  verify rows="); { char b[8]; int_to_str((int)ws2.ws_row,b); serial_print(b); }
    serial_print(" cols="); { char b[8]; int_to_str((int)ws2.ws_col,b); serial_print(b); }
    serial_print("\n");
    if (ws2.ws_row == 50 && ws2.ws_col == 132)
        serial_print("  [OK] 132x50 set\n");
    else
        serial_print("  [FAIL] winsize mismatch!\n");
    // Orijinale geri dön
    winsize_t restore_ws = { .ws_row = 25, .ws_col = 80 };
    DO_SYSCALL3(SYS_IOCTL, 1, TIOCSWINSZ, &restore_ws);

    // ── SYS_IOCTL – FIONREAD ────────────────────────────────────
    serial_print("\n[T27] SYS_IOCTL FIONREAD (stdin):\n");
    int avail = -1;
    DO_SYSCALL3(SYS_IOCTL, 0, FIONREAD, &avail);
    serial_print("  ret="); { char b[8]; int_to_str((int)ret,b); serial_print(b); }
    serial_print(" avail="); { char b[8]; int_to_str(avail,b); serial_print(b); }
    serial_print(" (0 if no input pending)\n");

    // ── SYS_IOCTL – TIOCGPGRP / TIOCSPGRP ──────────────────────
    serial_print("\n[T28] SYS_IOCTL TIOCGPGRP/TIOCSPGRP:\n");
    int pgrp = -1;
    DO_SYSCALL3(SYS_IOCTL, 0, TIOCGPGRP, &pgrp);
    serial_print("  TIOCGPGRP ret="); { char b[8]; int_to_str((int)ret,b); serial_print(b); }
    serial_print(" pgrp="); { char b[8]; int_to_str(pgrp,b); serial_print(b); }
    serial_print("\n");
    int new_pgrp = 42;
    DO_SYSCALL3(SYS_IOCTL, 0, TIOCSPGRP, &new_pgrp);
    serial_print("  TIOCSPGRP(42) ret="); { char b[8]; int_to_str((int)ret,b); serial_print(b); }
    serial_print("\n");
    int pgrp2 = 0;
    DO_SYSCALL3(SYS_IOCTL, 0, TIOCGPGRP, &pgrp2);
    if (pgrp2 == 42)
        serial_print("  [OK] pgrp=42\n");
    else
        serial_print("  [FAIL] pgrp mismatch!\n");
    // Geri dön
    DO_SYSCALL3(SYS_IOCTL, 0, TIOCSPGRP, &pgrp);

    // ── SYS_IOCTL – bilinmeyen request ──────────────────────────
    serial_print("\n[T29] SYS_IOCTL unknown request 0xDEAD (expect EINVAL=-1):\n");
    DO_SYSCALL3(SYS_IOCTL, 0, 0xDEAD, 0);
    serial_print("  ret="); { char b[8]; int_to_str((int)(int64_t)ret,b); serial_print(b); }
    serial_print(" (expect -1)\n");

    // ── SYS_IOCTL – NULL arg ile TCGETS (expect EFAULT) ─────────
    serial_print("\n[T30] SYS_IOCTL TCGETS NULL arg (expect EFAULT=-11):\n");
    DO_SYSCALL3(SYS_IOCTL, 0, TCGETS, 0);
    serial_print("  ret="); { char b[8]; int_to_str((int)(int64_t)ret,b); serial_print(b); }
    serial_print(" (expect -11)\n");

    serial_print("\n========================================\n");
    serial_print("[SYSCALL TEST] All v4 tests completed.\n");
    serial_print("========================================\n\n");

    // ============================================================
    // v6 Testleri: SYS_KILL + SYS_GETTIMEOFDAY
    // ============================================================
    serial_print("\n========================================\n");
    serial_print("[SYSCALL TEST] v6: kill + gettimeofday\n");
    serial_print("========================================\n");

    // ── SYS_GETTIMEOFDAY – normal kullanım ───────────────────────
    serial_print("\n[T31] SYS_GETTIMEOFDAY (normal):\n");
    timeval_t tv1;
    DO_SYSCALL2(SYS_GETTIMEOFDAY, &tv1, 0);
    serial_print("  ret="); { char b[8]; int_to_str((int)ret,b); serial_print(b); }
    serial_print(" tv_sec="); print_uint64((uint64_t)tv1.tv_sec);
    serial_print(" tv_usec="); print_uint64((uint64_t)tv1.tv_usec);
    serial_print("\n");
    if (ret == SYSCALL_OK)
        serial_print("  [OK] gettimeofday başarılı\n");
    else
        serial_print("  [FAIL] gettimeofday hata döndü!\n");

    // ── SYS_GETTIMEOFDAY – kısa sleep sonrası zaman ilerliyor mu? ─
    serial_print("\n[T32] SYS_GETTIMEOFDAY – sleep(50) sonrası zaman ilerlemeli:\n");
    timeval_t tv_before, tv_after;
    DO_SYSCALL2(SYS_GETTIMEOFDAY, &tv_before, 0);
    DO_SYSCALL1(SYS_SLEEP, 50);   // 50 tick bekle
    DO_SYSCALL2(SYS_GETTIMEOFDAY, &tv_after, 0);
    int64_t delta_sec  = tv_after.tv_sec  - tv_before.tv_sec;
    int64_t delta_usec = tv_after.tv_usec - tv_before.tv_usec;
    int64_t delta_ms   = delta_sec * 1000LL + delta_usec / 1000LL;
    serial_print("  before: sec="); print_uint64((uint64_t)tv_before.tv_sec);
    serial_print(" usec=");  print_uint64((uint64_t)tv_before.tv_usec);
    serial_print("\n  after:  sec="); print_uint64((uint64_t)tv_after.tv_sec);
    serial_print(" usec=");  print_uint64((uint64_t)tv_after.tv_usec);
    serial_print("\n  delta_ms="); { char b[16]; int_to_str((int)delta_ms,b); serial_print(b); }
    serial_print("\n");
    if (delta_ms > 0)
        serial_print("  [OK] zaman ilerledi\n");
    else
        serial_print("  [WARN] zaman ilerlemedi (tick hızı?\n");

    // ── SYS_GETTIMEOFDAY – NULL tv (tz parametresini yoksay) ────
    serial_print("\n[T33] SYS_GETTIMEOFDAY NULL tv (tz yoksayılmalı):\n");
    DO_SYSCALL2(SYS_GETTIMEOFDAY, 0, 0);
    serial_print("  ret="); { char b[8]; int_to_str((int)ret,b); serial_print(b); }
    serial_print(" (expect 0, NULL tv ok)\n");
    if (ret == SYSCALL_OK)
        serial_print("  [OK]\n");
    else
        serial_print("  [FAIL]\n");

    // ── SYS_GETTIMEOFDAY – geçersiz pointer ─────────────────────
    serial_print("\n[T34] SYS_GETTIMEOFDAY geçersiz ptr (expect EFAULT=-11):\n");
    DO_SYSCALL2(SYS_GETTIMEOFDAY, (void*)0xDEADBABEDEADBABEull, 0);
    serial_print("  ret="); { char b[8]; int_to_str((int)(int64_t)ret,b); serial_print(b); }
    serial_print(" (expect -11)\n");
    if ((int64_t)ret == -11)
        serial_print("  [OK]\n");
    else
        serial_print("  [FAIL]\n");

    // ── SYS_KILL – sinyal 0 (process var mı kontrolü) ───────────
    serial_print("\n[T35] SYS_KILL sig=0 kendi pid'imize (process kontrolü):\n");
    DO_SYSCALL0(SYS_GETPID);
    uint64_t my_pid = ret;
    DO_SYSCALL2(SYS_KILL, my_pid, 0);
    serial_print("  ret="); { char b[8]; int_to_str((int)ret,b); serial_print(b); }
    serial_print(" (expect 0)\n");
    if (ret == SYSCALL_OK)
        serial_print("  [OK] process bulundu\n");
    else
        serial_print("  [FAIL]\n");

    // ── SYS_KILL – geçersiz pid ──────────────────────────────────
    serial_print("\n[T36] SYS_KILL geçersiz pid=9999 (expect EINVAL=-1):\n");
    DO_SYSCALL2(SYS_KILL, 9999, SIGTERM);
    serial_print("  ret="); { char b[8]; int_to_str((int)(int64_t)ret,b); serial_print(b); }
    serial_print(" (expect -1)\n");
    if ((int64_t)ret == -1)
        serial_print("  [OK]\n");
    else
        serial_print("  [FAIL]\n");

    // ── SYS_KILL – pid=0 (mevcut task'a SIGUSR1, yoksanır) ──────
    serial_print("\n[T37] SYS_KILL pid=0 SIGUSR1 (yoksanmalı, ret=0):\n");
    DO_SYSCALL2(SYS_KILL, 0, SIGUSR1);
    serial_print("  ret="); { char b[8]; int_to_str((int)ret,b); serial_print(b); }
    serial_print(" (expect 0)\n");
    if (ret == SYSCALL_OK)
        serial_print("  [OK]\n");
    else
        serial_print("  [FAIL]\n");

    // ── SYS_KILL – grup sinyal (pid<0, expect ENOSYS) ────────────
    serial_print("\n[T38] SYS_KILL pid=-1 (grup sinyal, expect ENOSYS=-2):\n");
    DO_SYSCALL2(SYS_KILL, (uint64_t)(int64_t)-1, SIGTERM);
    serial_print("  ret="); { char b[8]; int_to_str((int)(int64_t)ret,b); serial_print(b); }
    serial_print(" (expect -2)\n");
    if ((int64_t)ret == -2)
        serial_print("  [OK]\n");
    else
        serial_print("  [FAIL]\n");

    // ── SYS_KILL – SIGKILL ile fork çocuğunu sonlandır ──────────
    serial_print("\n[T39] SYS_FORK + SYS_KILL(SIGKILL) çocuğu öldür:\n");
    DO_SYSCALL0(SYS_FORK);
    int64_t fork_ret = (int64_t)ret;
    serial_print("  fork ret="); { char b[12]; int_to_str((int)fork_ret,b); serial_print(b); }
    serial_print("\n");
    if (fork_ret > 0) {
        // Ebeveyndeyiz, çocuğu öldür
        DO_SYSCALL2(SYS_KILL, (uint64_t)fork_ret, SIGKILL);
        serial_print("  kill(child, SIGKILL) ret=");
        { char b[8]; int_to_str((int)ret,b); serial_print(b); }
        serial_print("\n");
        if (ret == SYSCALL_OK)
            serial_print("  [OK] çocuk sonlandırıldı\n");
        else
            serial_print("  [FAIL] kill başarısız\n");
        // Çocuğu topla
        DO_SYSCALL3(SYS_WAITPID, (uint64_t)fork_ret, 0, 0);
        serial_print("  waitpid ret="); { char b[12]; int_to_str((int)(int64_t)ret,b); serial_print(b); }
        serial_print("\n");
    } else if (fork_ret == 0) {
        // Çocuktayız: uyuyalım, ebeveyn bizi öldürecek
        DO_SYSCALL1(SYS_SLEEP, 1000);
        DO_SYSCALL1(SYS_EXIT, 0);
    } else {
        serial_print("  [SKIP] fork başarısız\n");
    }

    serial_print("\n========================================\n");
    serial_print("[SYSCALL TEST] v6 testleri tamamlandı.\n");
    serial_print("========================================\n\n");

#undef DO_SYSCALL0
#undef DO_SYSCALL1
#undef DO_SYSCALL2
#undef DO_SYSCALL3
}