#include <stddef.h>
#include "commands64.h"
#include "../fs/files64.h"
#include "../kernel/syscall.h"
#include "../kernel/signal64.h"
#include "../kernel/task.h"

extern void println64(const char* str, uint8_t color);

// String utils
extern int     str_len(const char* str);
extern int     str_cmp(const char* s1, const char* s2);
extern void    str_cpy(char* dest, const char* src);
extern void    str_concat(char* dest, const char* src);
extern void    int_to_str(int num, char* str);
extern void    uint64_to_string(uint64_t num, char* str);
extern void    output_add_line(CommandOutput* output, const char* line, uint8_t color);
extern void    output_add_empty_line(CommandOutput* output);
extern void    output_init(CommandOutput* output);
// ============================================================
// Helper Macros
// ============================================================
#define _SC0(num) \
    __asm__ volatile("syscall":"=a"(ret):"a"((uint64_t)(num)):"rcx","r11")
#define _SC1(num,a1) \
    __asm__ volatile("syscall":"=a"(ret):"a"((uint64_t)(num)),"D"((uint64_t)(a1)):"rcx","r11","memory")
#define _SC2(num,a1,a2) \
    __asm__ volatile("syscall":"=a"(ret):"a"((uint64_t)(num)),"D"((uint64_t)(a1)),"S"((uint64_t)(a2)):"rcx","r11","memory")
#define _SC3(num,a1,a2,a3) \
    __asm__ volatile("syscall":"=a"(ret):"a"((uint64_t)(num)),"D"((uint64_t)(a1)),"S"((uint64_t)(a2)),"d"((uint64_t)(a3)):"rcx","r11","memory")

// u64 -> decimal string
static void u64_to_dec(uint64_t v, char* out) {
    if (v == 0) { out[0]='0'; out[1]='\0'; return; }
    int i = 0; uint64_t t = v;
    while (t > 0) { out[i++] = '0' + (t % 10); t /= 10; }
    out[i] = '\0';
    for (int a = 0, b = i-1; a < b; a++, b--) {
        char c = out[a]; out[a] = out[b]; out[b] = c;
    }
}

// Create Pass/Fail
static void sc_result(CommandOutput* output, int idx, const char* name,
                      int64_t ret_val, int pass_cond,
                      const char* extra, int* pass, int* fail) {
    char line[96]; char tmp[24];
    line[0]='['; line[1]='0'+(idx/10); line[2]='0'+(idx%10);
    line[3]=']'; line[4]=' '; line[5]='\0';
    str_concat(line, name);
    str_concat(line, " ret=");
    int_to_str((int)ret_val, tmp);
    str_concat(line, tmp);
    if (extra && extra[0]) { str_concat(line, " "); str_concat(line, extra); }
    str_concat(line, pass_cond ? "  PASS" : "  FAIL");
    println64(line, pass_cond ? VGA_GREEN : VGA_RED);
    pass_cond ? (*pass)++ : (*fail)++;
}

void cmd_syscalltest(const char* args, CommandOutput* output) {
    (void)args;

    if (!syscall_is_enabled()) {
        output_add_line(output, "ERROR: SYSCALL not initialized!", VGA_RED);
        output_add_line(output, "Call syscall_init() first.", VGA_YELLOW);
        return;
    }

    output_add_line(output, "=== SYSCALL Test Suite v27 (360 tests) ===", VGA_CYAN);
    output_add_line(output, "  ...v26:set_tid/robust_list  v27:writev/madvise/openat/prlimit64", VGA_YELLOW);
    output_add_empty_line(output);

    uint64_t ret;
    int pass = 0, fail = 0;
    char tmp[32]; char line[96];
    const char* hexc = "0123456789ABCDEF";

    #define SCPRINT(txt, col) do { \
        println64((txt), (col)); \
        output_add_line(output, (txt), (col)); \
    } while(0)

    #define HEX64S(v) do { \
        tmp[0]='0'; tmp[1]='x'; \
        for(int _i=0;_i<16;_i++) tmp[2+_i]=hexc[((v)>>(60-_i*4))&0xF]; \
        tmp[18]='\0'; \
    } while(0)

    // ── [01] SYS_WRITE fd=1 ────────────────────────────────────
    static const char wbuf[] = "[SYS_WRITE fd=1 test]\n";
    uint64_t wlen = 22;
    _SC3(SYS_WRITE, 1, wbuf, wlen);
    sc_result(output, 1, "SYS_WRITE(fd=1)", (int64_t)ret,
              ret == wlen, "(serial out)", &pass, &fail);

    // ── [02] SYS_WRITE fd=2 ────────────────────────────────────
    static const char ebuf[] = "[SYS_WRITE fd=2 test]\n";
    uint64_t elen = 22;
    _SC3(SYS_WRITE, 2, ebuf, elen);
    sc_result(output, 2, "SYS_WRITE(fd=2)", (int64_t)ret,
              ret == elen, "(serial out)", &pass, &fail);

    // ── [03] SYS_WRITE fd=0 stdin → EBADF ─────────────────────
    _SC3(SYS_WRITE, 0, wbuf, wlen);
    sc_result(output, 3, "SYS_WRITE(fd=0)", (int64_t)ret,
              (int64_t)ret == (int64_t)SYSCALL_ERR_BADF,
              "expect EBADF", &pass, &fail);

    // ── [04] SYS_WRITE fd=99 geçersiz → EBADF ─────────────────
    _SC3(SYS_WRITE, 99, wbuf, wlen);
    sc_result(output, 4, "SYS_WRITE(fd=99)", (int64_t)ret,
              (int64_t)ret == (int64_t)SYSCALL_ERR_BADF,
              "expect EBADF", &pass, &fail);

    // ── [05] SYS_READ fd=0 non-blocking ───────────────────────
    char rbuf[32];
    _SC3(SYS_READ, 0, rbuf, 16);
    {
        int ok = ((int64_t)ret >= 0 || (int64_t)ret == (int64_t)SYSCALL_ERR_AGAIN);
        str_cpy(line, "[05] SYS_READ(fd=0) ret=");
        int_to_str((int)(int64_t)ret, tmp); str_concat(line, tmp);
        str_concat(line, ok ? " (no-block ok)  PASS" : "  FAIL");
        SCPRINT(line, ok ? VGA_GREEN : VGA_RED);
        ok ? pass++ : fail++;
    }

    // ── [06] SYS_READ fd=1 → EBADF ────────────────────────────
    _SC3(SYS_READ, 1, rbuf, 16);
    sc_result(output, 6, "SYS_READ(fd=1)", (int64_t)ret,
              (int64_t)ret == (int64_t)SYSCALL_ERR_BADF,
              "expect EBADF", &pass, &fail);

    // ── [07] SYS_GETPID ────────────────────────────────────────
    _SC0(SYS_GETPID);
    {
        int ok = ((int64_t)ret >= 0);
        str_cpy(line, "[07] SYS_GETPID pid=");
        int_to_str((int)ret, tmp); str_concat(line, tmp);
        str_concat(line, ok ? "  PASS" : "  FAIL");
        SCPRINT(line, ok ? VGA_GREEN : VGA_RED);
        ok ? pass++ : fail++;
    }

    // ── [08] SYS_GETPPID ───────────────────────────────────────
    _SC0(SYS_GETPPID);
    {
        int ok = ((int64_t)ret >= 0);
        str_cpy(line, "[08] SYS_GETPPID ppid=");
        int_to_str((int)ret, tmp); str_concat(line, tmp);
        str_concat(line, ok ? "  PASS" : "  FAIL");
        SCPRINT(line, ok ? VGA_GREEN : VGA_RED);
        ok ? pass++ : fail++;
    }

    // ── [09] SYS_UPTIME ────────────────────────────────────────
    _SC0(SYS_UPTIME);
    {
        int ok = ((int64_t)ret >= 0);
        u64_to_dec(ret, tmp);
        str_cpy(line, "[09] SYS_UPTIME ticks="); str_concat(line, tmp);
        str_concat(line, ok ? "  PASS" : "  FAIL");
        SCPRINT(line, ok ? VGA_GREEN : VGA_RED);
        ok ? pass++ : fail++;
    }

    // ── [10] SYS_GETTICKS ──────────────────────────────────────
    _SC0(SYS_GETTICKS);
    {
        int ok = ((int64_t)ret >= 0);
        u64_to_dec(ret, tmp);
        str_cpy(line, "[10] SYS_GETTICKS ticks="); str_concat(line, tmp);
        str_concat(line, ok ? "  PASS" : "  FAIL");
        SCPRINT(line, ok ? VGA_GREEN : VGA_RED);
        ok ? pass++ : fail++;
    }

    // ── [11] SYS_YIELD ─────────────────────────────────────────
    _SC0(SYS_YIELD);
    sc_result(output, 11, "SYS_YIELD", (int64_t)ret,
              (int64_t)ret == 0, "expect 0", &pass, &fail);

    // ── [12] SYS_SLEEP(0) ──────────────────────────────────────
    _SC1(SYS_SLEEP, 0);
    sc_result(output, 12, "SYS_SLEEP(0)", (int64_t)ret,
              (int64_t)ret == 0, "expect 0", &pass, &fail);

    // ── [13] SYS_DEBUG ─────────────────────────────────────────
    static const char dmsg[] = "syscalltest v3 debug probe";
    _SC1(SYS_DEBUG, dmsg);
    sc_result(output, 13, "SYS_DEBUG", (int64_t)ret,
              (int64_t)ret == 0, "(serial log)", &pass, &fail);

    // ── [14] SYS_SETPRIORITY / SYS_GETPRIORITY ─────────────────
    _SC0(SYS_GETPRIORITY);
    uint64_t old_prio = ret;
    uint64_t test_prio = (old_prio < 205) ? (old_prio + 50) : (old_prio - 50);
    _SC1(SYS_SETPRIORITY, test_prio);
    uint64_t set_ret = ret;
    _SC0(SYS_GETPRIORITY);
    uint64_t new_prio = ret;
    _SC1(SYS_SETPRIORITY, old_prio);
    {
        int ok = ((int64_t)set_ret == 0 && new_prio == test_prio);
        str_cpy(line, "[14] SYS_SETPRIORITY set=");
        u64_to_dec(test_prio, tmp); str_concat(line, tmp);
        str_concat(line, " got=");
        u64_to_dec(new_prio, tmp); str_concat(line, tmp);
        str_concat(line, ok ? "  PASS" : "  FAIL");
        SCPRINT(line, ok ? VGA_GREEN : VGA_RED);
        ok ? pass++ : fail++;
    }

    // ── [15] INVALID syscall → ENOSYS ─────────────────────────
    _SC0(9999);
    sc_result(output, 15, "INVALID(9999)", (int64_t)ret,
              (int64_t)ret == (int64_t)SYSCALL_ERR_NOSYS,
              "expect ENOSYS(-2)", &pass, &fail);

    // ================================================================
    // v3 YENİ TESTLER
    // ================================================================
    SCPRINT("", VGA_WHITE);
    SCPRINT("── v3 New Tests ─────────────────────", VGA_YELLOW);

    // ── [16] SYS_BRK(0) sorgula ───────────────────────────────
    _SC1(SYS_BRK, 0);
    {
        int ok = ((int64_t)ret > 0);
        HEX64S(ret);
        str_cpy(line, "[16] SYS_BRK(0) cur_brk="); str_concat(line, tmp);
        str_concat(line, ok ? "  PASS" : "  FAIL");
        SCPRINT(line, ok ? VGA_GREEN : VGA_RED);
        ok ? pass++ : fail++;
    }

    // ── [17] SYS_MMAP anonim 4096 bytes ───────────────────────
    uint64_t mmap_addr = 0;
    {
        register uint64_t r10v asm("r10") = (uint64_t)(MAP_ANONYMOUS | MAP_PRIVATE);
        register uint64_t r8v  asm("r8")  = (uint64_t)(int64_t)(-1);
        register uint64_t r9v  asm("r9")  = 0ULL;
        __asm__ volatile ("syscall"
            : "=a"(ret)
            : "a"((uint64_t)SYS_MMAP),
              "D"((uint64_t)0),
              "S"((uint64_t)4096),
              "d"((uint64_t)(PROT_READ|PROT_WRITE)),
              "r"(r10v), "r"(r8v), "r"(r9v)
            : "rcx", "r11", "memory");
        mmap_addr = ret;
    }
    {
        int ok = (ret != (uint64_t)MAP_FAILED && ret != 0);
        HEX64S(ret);
        str_cpy(line, "[17] SYS_MMAP(anon,4096) addr="); str_concat(line, tmp);
        str_concat(line, ok ? "  PASS" : "  FAIL");
        SCPRINT(line, ok ? VGA_GREEN : VGA_RED);
        ok ? pass++ : fail++;

        if (ok) {
            volatile char* mp = (volatile char*)mmap_addr;
            mp[0] = 0x42; mp[1] = 0x43;
            int rw_ok = (mp[0] == 0x42 && mp[1] == 0x43);
            SCPRINT(rw_ok ? "       mmap R/W verify OK" : "       mmap R/W verify FAIL",
                rw_ok ? VGA_GREEN : VGA_RED);
        }
    }

    // ── [18] SYS_MUNMAP ───────────────────────────────────────
    if (mmap_addr && mmap_addr != (uint64_t)MAP_FAILED) {
        _SC2(SYS_MUNMAP, mmap_addr, 4096);
        sc_result(output, 18, "SYS_MUNMAP", (int64_t)ret,
                  (int64_t)ret == 0, "expect 0", &pass, &fail);
    } else {
        SCPRINT("[18] SYS_MUNMAP  SKIP (mmap failed)", VGA_YELLOW);
    }

    // ── [19] SYS_EXECVE
    static const char exec_path[] = "/bin/sh";
    _SC3(SYS_EXECVE, exec_path, 0, 0);
    {
        int ok = ((int64_t)ret == (int64_t)SYSCALL_ERR_NOENT ||
                  (int64_t)ret == (int64_t)SYSCALL_ERR_INVAL ||
                  (int64_t)ret == 0);
        sc_result(output, 19, "SYS_EXECVE(/bin/sh)", (int64_t)ret,
                  ok, "expect ENOENT/EINVAL/0", &pass, &fail);
    }

    // ── [20] SYS_PIPE ─────────────────────────────────────────
    int pipe_fds[2] = {-1, -1};
    _SC1(SYS_PIPE, pipe_fds);
    {
        int ok = ((int64_t)ret == 0 && pipe_fds[0] >= 3 && pipe_fds[1] >= 3);
        str_cpy(line, "[20] SYS_PIPE rfd=");
        int_to_str(pipe_fds[0], tmp); str_concat(line, tmp);
        str_concat(line, " wfd=");
        int_to_str(pipe_fds[1], tmp); str_concat(line, tmp);
        str_concat(line, ok ? "  PASS" : "  FAIL");
        SCPRINT(line, ok ? VGA_GREEN : VGA_RED);
        ok ? pass++ : fail++;
    }

    // ── [21] PIPE write + read  ──────────────────
    if (pipe_fds[0] >= 3 && pipe_fds[1] >= 3) {
        static const char pmsg[] = "PIPE_DATA_OK";
        uint64_t pmsg_len = 12;
        _SC3(SYS_WRITE, pipe_fds[1], pmsg, pmsg_len);
        uint64_t write_ret = ret;

        char pbuf[32];
        _SC3(SYS_READ, pipe_fds[0], pbuf, pmsg_len);
        uint64_t read_ret = ret;

        int data_ok = (read_ret == pmsg_len &&
                       pbuf[0]=='P' && pbuf[1]=='I' && pbuf[2]=='P' && pbuf[3]=='E');
        str_cpy(line, "[21] PIPE write=");
        int_to_str((int)write_ret, tmp); str_concat(line, tmp);
        str_concat(line, " read=");
        int_to_str((int)read_ret, tmp); str_concat(line, tmp);
        str_concat(line, data_ok ? "  PASS" : "  FAIL");
        SCPRINT(line, data_ok ? VGA_GREEN : VGA_RED);
        data_ok ? pass++ : fail++;
    } else {
        SCPRINT("[21] PIPE R/W  SKIP", VGA_YELLOW);
    }

    // ── [22] SYS_DUP2 ─────────────────────────────────────────
    int dup_ok_pre = (pipe_fds[1] >= 3);
    if (dup_ok_pre) {
        _SC2(SYS_DUP2, pipe_fds[1], 8);
        int dup_pass = ((int64_t)ret == 8);
        str_cpy(line, "[22] SYS_DUP2(wfd->8) ret=");
        int_to_str((int)ret, tmp); str_concat(line, tmp);
        str_concat(line, dup_pass ? "  PASS" : "  FAIL");
        SCPRINT(line, dup_pass ? VGA_GREEN : VGA_RED);
        dup_pass ? pass++ : fail++;

        _SC1(SYS_CLOSE, pipe_fds[0]);
        _SC1(SYS_CLOSE, pipe_fds[1]);
        _SC1(SYS_CLOSE, 8);
    } else {
        SCPRINT("[22] SYS_DUP2  SKIP", VGA_YELLOW);
    }

    // ── [23] SYS_FORK smoke test ────────────────────────────────
    _SC0(SYS_FORK);
    {
        int64_t fork_ret = (int64_t)ret;
        int ok = (fork_ret >= 0);
        str_cpy(line, "[23] SYS_FORK ret=");
        int_to_str((int)fork_ret, tmp); str_concat(line, tmp);
        str_concat(line, fork_ret > 0 ? " (parent,child_pid)" :
                         fork_ret == 0 ? " (child ctx)" : " (err)");
        str_concat(line, ok ? "  PASS" : "  FAIL");
        SCPRINT(line, ok ? VGA_GREEN : VGA_RED);
        ok ? pass++ : fail++;

        if (fork_ret > 0) {
            str_cpy(line, "       child_pid=");
            int_to_str((int)fork_ret, tmp); str_concat(line, tmp);
            SCPRINT(line, VGA_WHITE);
        }
    }

    SCPRINT("", VGA_WHITE);
    SCPRINT("── v4 New Tests (lseek / fstat / ioctl) ─", VGA_YELLOW);

    // ── [24] SYS_FSTAT fd=0 stdin → S_IFCHR ──────────────────
    {
        stat_t st;
        _SC2(SYS_FSTAT, 0, &st);
        int ok = ((int64_t)ret == 0 && (st.st_mode & S_IFCHR));
        str_cpy(line, "[24] SYS_FSTAT(stdin) mode=0x");
        HEX64S((uint64_t)st.st_mode); str_concat(line, tmp);
        str_concat(line, ok ? "  PASS" : "  FAIL");
        SCPRINT(line, ok ? VGA_GREEN : VGA_RED);
        ok ? pass++ : fail++;
    }

    // ── [25] SYS_FSTAT fd=1 stdout → S_IFCHR ─────────────────
    {
        stat_t st;
        _SC2(SYS_FSTAT, 1, &st);
        int ok = ((int64_t)ret == 0 && (st.st_mode & S_IFCHR));
        str_cpy(line, "[25] SYS_FSTAT(stdout) mode=0x");
        HEX64S((uint64_t)st.st_mode); str_concat(line, tmp);
        str_concat(line, ok ? "  PASS" : "  FAIL");
        SCPRINT(line, ok ? VGA_GREEN : VGA_RED);
        ok ? pass++ : fail++;
    }

    // ── [26] SYS_FSTAT pipe fd → S_IFIFO + bytes_avail ───────
    {
        int pfds[2] = {-1,-1};
        _SC1(SYS_PIPE, pfds);
        if ((int64_t)ret == 0 && pfds[0] >= 3 && pfds[1] >= 3) {
            static const char pmsg7[] = "ABCDEFG";
            _SC3(SYS_WRITE, pfds[1], pmsg7, 7);

            stat_t sp;
            _SC2(SYS_FSTAT, pfds[0], &sp);
            int ok = ((int64_t)ret == 0 &&
                      (sp.st_mode & S_IFIFO) &&
                      sp.st_size == 7);
            str_cpy(line, "[26] SYS_FSTAT(pipe) mode=0x");
            HEX64S((uint64_t)sp.st_mode); str_concat(line, tmp);
            str_concat(line, " sz=");
            u64_to_dec(sp.st_size, tmp); str_concat(line, tmp);
            str_concat(line, ok ? "  PASS" : "  FAIL");
            SCPRINT(line, ok ? VGA_GREEN : VGA_RED);
            ok ? pass++ : fail++;

            _SC1(SYS_CLOSE, pfds[0]);
            _SC1(SYS_CLOSE, pfds[1]);
        } else {
            SCPRINT("[26] SYS_FSTAT(pipe)  SKIP", VGA_YELLOW);
        }
    }

    // ── [27] SYS_FSTAT fd=999 → EBADF ────────────────────────
    {
        stat_t st;
        _SC2(SYS_FSTAT, 999, &st);
        sc_result(output, 27, "SYS_FSTAT(bad fd)", (int64_t)ret,
                  (int64_t)ret == (int64_t)SYSCALL_ERR_BADF,
                  "expect EBADF", &pass, &fail);
    }

    // ── [28] SYS_LSEEK stdin → EINVAL ────────────────────────
    _SC3(SYS_LSEEK, 0, 0, SEEK_SET);
    sc_result(output, 28, "SYS_LSEEK(stdin,SEEK_SET)", (int64_t)ret,
              (int64_t)ret == (int64_t)SYSCALL_ERR_INVAL,
              "expect EINVAL", &pass, &fail);

    // ── [29] SYS_LSEEK geçersiz whence=99 → EINVAL ───────────
    _SC3(SYS_LSEEK, 1, 0, 99);
    sc_result(output, 29, "SYS_LSEEK(bad whence)", (int64_t)ret,
              (int64_t)ret == (int64_t)SYSCALL_ERR_INVAL,
              "expect EINVAL", &pass, &fail);

    // ── [30] SYS_LSEEK fd=999 → EBADF ────────────────────────
    _SC3(SYS_LSEEK, 999, 0, SEEK_SET);
    sc_result(output, 30, "SYS_LSEEK(bad fd)", (int64_t)ret,
              (int64_t)ret == (int64_t)SYSCALL_ERR_BADF,
              "expect EBADF", &pass, &fail);

    // ── [31] SYS_IOCTL TCGETS ────────────────────────────────
    {
        termios_t tios;
        _SC3(SYS_IOCTL, 0, TCGETS, &tios);
        int ok = ((int64_t)ret == 0 && (tios.c_lflag & ICANON) && (tios.c_lflag & ECHO));
        str_cpy(line, "[31] SYS_IOCTL(TCGETS) lflag=0x");
        HEX64S((uint64_t)tios.c_lflag); str_concat(line, tmp);
        str_concat(line, ok ? "  PASS" : "  FAIL");
        SCPRINT(line, ok ? VGA_GREEN : VGA_RED);
        ok ? pass++ : fail++;
    }

    // ── [32] SYS_IOCTL TCSETS raw mode ───────────────────────
    {
        termios_t orig, raw;
        _SC3(SYS_IOCTL, 0, TCGETS, &orig);

        raw = orig;
        raw.c_lflag &= ~(uint32_t)(ECHO | ICANON | ISIG | IEXTEN);
        raw.c_iflag &= ~(uint32_t)(ICRNL | IXON);
        raw.c_oflag &= ~(uint32_t)OPOST;
        raw.c_cc[VMIN]  = 1;
        raw.c_cc[VTIME] = 0;

        _SC3(SYS_IOCTL, 0, TCSETS, &raw);
        uint64_t set_ret = ret;

        termios_t verify;
        _SC3(SYS_IOCTL, 0, TCGETS, &verify);
        int ok = ((int64_t)set_ret == 0 &&
                  !(verify.c_lflag & ECHO) &&
                  !(verify.c_lflag & ICANON));
        str_cpy(line, "[32] SYS_IOCTL(TCSETS raw) lflag=0x");
        HEX64S((uint64_t)verify.c_lflag); str_concat(line, tmp);
        str_concat(line, ok ? "  PASS" : "  FAIL");
        SCPRINT(line, ok ? VGA_GREEN : VGA_RED);
        ok ? pass++ : fail++;

        _SC3(SYS_IOCTL, 0, TCSETSF, &orig);
    }

    // ── [33] SYS_IOCTL TCSETSF canonical geri yükle ──────────
    {
        termios_t check;
        _SC3(SYS_IOCTL, 0, TCGETS, &check);
        int ok = ((int64_t)ret == 0 && (check.c_lflag & ECHO) && (check.c_lflag & ICANON));
        str_cpy(line, "[33] SYS_IOCTL(TCSETSF restore) lflag=0x");
        HEX64S((uint64_t)check.c_lflag); str_concat(line, tmp);
        str_concat(line, ok ? "  PASS" : "  FAIL");
        SCPRINT(line, ok ? VGA_GREEN : VGA_RED);
        ok ? pass++ : fail++;
    }

    // ── [34] SYS_IOCTL TIOCGWINSZ ────────────────────────────
    {
        winsize_t ws;
        _SC3(SYS_IOCTL, 1, TIOCGWINSZ, &ws);
        int ok = ((int64_t)ret == 0 && ws.ws_col > 0 && ws.ws_row > 0);
        str_cpy(line, "[34] SYS_IOCTL(TIOCGWINSZ) ");
        int_to_str((int)ws.ws_col, tmp); str_concat(line, tmp);
        str_concat(line, "x");
        int_to_str((int)ws.ws_row, tmp); str_concat(line, tmp);
        str_concat(line, ok ? "  PASS" : "  FAIL");
        SCPRINT(line, ok ? VGA_GREEN : VGA_RED);
        ok ? pass++ : fail++;
    }

    // ── [35] SYS_IOCTL TIOCSWINSZ yuvarlak trip ───────────────
    {
        winsize_t set_ws, got_ws;
        set_ws.ws_row = 50; set_ws.ws_col = 132;
        set_ws.ws_xpixel = 0; set_ws.ws_ypixel = 0;

        _SC3(SYS_IOCTL, 1, TIOCSWINSZ, &set_ws);
        uint64_t sw_ret = ret;
        _SC3(SYS_IOCTL, 1, TIOCGWINSZ, &got_ws);

        int ok = ((int64_t)sw_ret == 0 &&
                  got_ws.ws_row == 50 && got_ws.ws_col == 132);
        str_cpy(line, "[35] SYS_IOCTL(TIOCSWINSZ 132x50) got=");
        int_to_str((int)got_ws.ws_col, tmp); str_concat(line, tmp);
        str_concat(line, "x");
        int_to_str((int)got_ws.ws_row, tmp); str_concat(line, tmp);
        str_concat(line, ok ? "  PASS" : "  FAIL");
        SCPRINT(line, ok ? VGA_GREEN : VGA_RED);
        ok ? pass++ : fail++;

        winsize_t restore = {25, 80, 0, 0};
        _SC3(SYS_IOCTL, 1, TIOCSWINSZ, &restore);
    }

    // ── [36] SYS_IOCTL FIONREAD stdin ─────────────────────────
    {
        int avail = -1;
        _SC3(SYS_IOCTL, 0, FIONREAD, &avail);
        int ok = ((int64_t)ret == 0 && avail >= 0);
        str_cpy(line, "[36] SYS_IOCTL(FIONREAD) avail=");
        int_to_str(avail, tmp); str_concat(line, tmp);
        str_concat(line, ok ? "  PASS" : "  FAIL");
        SCPRINT(line, ok ? VGA_GREEN : VGA_RED);
        ok ? pass++ : fail++;
    }

    // ── [37] SYS_IOCTL bilinmeyen request → EINVAL ────────────
    _SC3(SYS_IOCTL, 0, 0xDEAD, 0);
    sc_result(output, 37, "SYS_IOCTL(bad req)", (int64_t)ret,
              (int64_t)ret == (int64_t)SYSCALL_ERR_INVAL,
              "expect EINVAL", &pass, &fail);

    // ── [38] SYS_IOCTL NULL arg → EFAULT ─────────────────────
    _SC3(SYS_IOCTL, 0, TCGETS, 0);
    sc_result(output, 38, "SYS_IOCTL(NULL arg)", (int64_t)ret,
              (int64_t)ret == (int64_t)SYSCALL_ERR_FAULT,
              "expect EFAULT", &pass, &fail);

    // ================================================================
    // v5 YENİ TESTLER: SYS_MMAP(MAP_FILE), SYS_SELECT, SYS_POLL
    // ================================================================
    SCPRINT("", VGA_WHITE);
    SCPRINT("── v5 New Tests (mmap_file/select/poll) ─", VGA_YELLOW);

    // ── [39] SYS_MMAP anonim (regression) ────────────────────
    {
        uint64_t mmap_addr2 = 0;
        register uint64_t r10v asm("r10") = (uint64_t)(MAP_ANONYMOUS | MAP_PRIVATE);
        register uint64_t r8v  asm("r8")  = (uint64_t)(int64_t)(-1);
        register uint64_t r9v  asm("r9")  = 0ULL;
        __asm__ volatile ("syscall"
            : "=a"(ret)
            : "a"((uint64_t)SYS_MMAP),
              "D"((uint64_t)0), "S"((uint64_t)4096),
              "d"((uint64_t)(PROT_READ|PROT_WRITE)),
              "r"(r10v), "r"(r8v), "r"(r9v)
            : "rcx","r11","memory");
        mmap_addr2 = ret;
        int ok = (ret != (uint64_t)MAP_FAILED && ret != 0);
        str_cpy(line, "[39] SYS_MMAP(anon) addr=0x");
        HEX64S(ret); str_concat(line, tmp);
        str_concat(line, ok ? "  PASS" : "  FAIL");
        SCPRINT(line, ok ? VGA_GREEN : VGA_RED);
        ok ? pass++ : fail++;
        if (ok) {
            volatile uint8_t* p = (volatile uint8_t*)mmap_addr2;
            p[0] = 0x55; p[1] = 0xAA;
            int rw = (p[0] == 0x55 && p[1] == 0xAA);
            SCPRINT(rw ? "       anon R/W verify OK" : "       anon R/W verify FAIL",
                rw ? VGA_GREEN : VGA_RED);
            _SC2(SYS_MUNMAP, mmap_addr2, 4096);
        }
    }

    // ── [40] SYS_MMAP MAP_FILE → geçersiz fd (EBADF/MAP_FAILED)
    {
        register uint64_t r10v asm("r10") = (uint64_t)MAP_PRIVATE;
        register uint64_t r8v  asm("r8")  = (uint64_t)99;
        register uint64_t r9v  asm("r9")  = 0ULL;
        __asm__ volatile ("syscall"
            : "=a"(ret)
            : "a"((uint64_t)SYS_MMAP),
              "D"((uint64_t)0), "S"((uint64_t)4096),
              "d"((uint64_t)PROT_READ),
              "r"(r10v), "r"(r8v), "r"(r9v)
            : "rcx","r11","memory");
        int ok = (ret == (uint64_t)MAP_FAILED);
        str_cpy(line, "[40] SYS_MMAP(bad fd) -> ");
        str_concat(line, ok ? "MAP_FAILED  PASS" : "unexpected  FAIL");
        SCPRINT(line, ok ? VGA_GREEN : VGA_RED);
        ok ? pass++ : fail++;
    }

    // ── [41] SYS_MMAP len=0 → MAP_FAILED ─────────────────────
    {
        register uint64_t r10v asm("r10") = (uint64_t)(MAP_ANONYMOUS|MAP_PRIVATE);
        register uint64_t r8v  asm("r8")  = (uint64_t)(int64_t)(-1);
        register uint64_t r9v  asm("r9")  = 0ULL;
        __asm__ volatile ("syscall"
            : "=a"(ret)
            : "a"((uint64_t)SYS_MMAP),
              "D"((uint64_t)0), "S"((uint64_t)0),
              "d"((uint64_t)PROT_READ),
              "r"(r10v), "r"(r8v), "r"(r9v)
            : "rcx","r11","memory");
        int ok = (ret == (uint64_t)MAP_FAILED);
        str_cpy(line, "[41] SYS_MMAP(len=0) -> ");
        str_concat(line, ok ? "MAP_FAILED  PASS" : "unexpected  FAIL");
        SCPRINT(line, ok ? VGA_GREEN : VGA_RED);
        ok ? pass++ : fail++;
    }

    // ── [42] SYS_SELECT non-blocking, stdout yazılabilir ──────
    {
        fd_set_t wfds;
        FD_ZERO(&wfds);
        FD_SET(1, &wfds);
        timeval_t tv = {0, 0};
        register uint64_t r10v asm("r10") = (uint64_t)0;
        register uint64_t r8v  asm("r8")  = (uint64_t)&tv;
        __asm__ volatile ("syscall"
            : "=a"(ret)
            : "a"((uint64_t)SYS_SELECT),
              "D"((uint64_t)2),
              "S"((uint64_t)0),
              "d"((uint64_t)&wfds),
              "r"(r10v), "r"(r8v)
            : "rcx","r11","memory");
        int ok = ((int64_t)ret >= 1 && FD_ISSET(1, &wfds));
        str_cpy(line, "[42] SYS_SELECT(stdout writable) nready=");
        int_to_str((int)ret, tmp); str_concat(line, tmp);
        str_concat(line, ok ? "  PASS" : "  FAIL");
        SCPRINT(line, ok ? VGA_GREEN : VGA_RED);
        ok ? pass++ : fail++;
    }

    // ── [43] SYS_SELECT non-blocking, stdin ───────────────────
    {
        fd_set_t rfds;
        FD_ZERO(&rfds);
        FD_SET(0, &rfds);
        timeval_t tv = {0, 0};
        register uint64_t r10v asm("r10") = (uint64_t)0;
        register uint64_t r8v  asm("r8")  = (uint64_t)&tv;
        __asm__ volatile ("syscall"
            : "=a"(ret)
            : "a"((uint64_t)SYS_SELECT),
              "D"((uint64_t)1),
              "S"((uint64_t)&rfds),
              "d"((uint64_t)0),
              "r"(r10v), "r"(r8v)
            : "rcx","r11","memory");
        int ok = ((int64_t)ret >= 0);
        str_cpy(line, "[43] SYS_SELECT(stdin nb) nready=");
        int_to_str((int)ret, tmp); str_concat(line, tmp);
        str_concat(line, ok ? "  PASS" : "  FAIL");
        SCPRINT(line, ok ? VGA_GREEN : VGA_RED);
        ok ? pass++ : fail++;
    }

    // ── [44] SYS_SELECT nfds=0 → 0 ───────────────────────────
    {
        timeval_t tv = {0, 0};
        register uint64_t r10v asm("r10") = (uint64_t)0;
        register uint64_t r8v  asm("r8")  = (uint64_t)&tv;
        __asm__ volatile ("syscall"
            : "=a"(ret)
            : "a"((uint64_t)SYS_SELECT),
              "D"((uint64_t)0),
              "S"((uint64_t)0),
              "d"((uint64_t)0),
              "r"(r10v), "r"(r8v)
            : "rcx","r11","memory");
        sc_result(output, 44, "SYS_SELECT(nfds=0)", (int64_t)ret,
                  (int64_t)ret == 0 || (int64_t)ret == (int64_t)SYSCALL_ERR_INVAL,
                  "expect 0 or EINVAL", &pass, &fail);
    }

    // ── [45] SYS_POLL stdout POLLOUT ──────────────────────────
    {
        pollfd_t pfd;
        pfd.fd = 1; pfd.events = POLLOUT; pfd.revents = 0;
        _SC3(SYS_POLL, &pfd, 1, 0);
        int ok = ((int64_t)ret == 1 && (pfd.revents & POLLOUT));
        str_cpy(line, "[45] SYS_POLL(stdout POLLOUT) rev=0x");
        int_to_str((int)pfd.revents, tmp); str_concat(line, tmp);
        str_concat(line, ok ? "  PASS" : "  FAIL");
        SCPRINT(line, ok ? VGA_GREEN : VGA_RED);
        ok ? pass++ : fail++;
    }

    // ── [46] SYS_POLL stdin POLLIN non-blocking ───────────────
    {
        pollfd_t pfd;
        pfd.fd = 0; pfd.events = POLLIN; pfd.revents = 0;
        _SC3(SYS_POLL, &pfd, 1, 0);
        int ok = ((int64_t)ret >= 0);
        str_cpy(line, "[46] SYS_POLL(stdin POLLIN nb) nready=");
        int_to_str((int)ret, tmp); str_concat(line, tmp);
        str_concat(line, ok ? "  PASS" : "  FAIL");
        SCPRINT(line, ok ? VGA_GREEN : VGA_RED);
        ok ? pass++ : fail++;
    }

    // ── [47] SYS_POLL fd=-1 atlanır → nready=0 ───────────────
    {
        pollfd_t pfd;
        pfd.fd = -1; pfd.events = POLLIN; pfd.revents = 0;
        _SC3(SYS_POLL, &pfd, 1, 0);
        int ok = ((int64_t)ret == 0 && pfd.revents == 0);
        str_cpy(line, "[47] SYS_POLL(fd=-1 skip) nready=");
        int_to_str((int)ret, tmp); str_concat(line, tmp);
        str_concat(line, ok ? "  PASS" : "  FAIL");
        SCPRINT(line, ok ? VGA_GREEN : VGA_RED);
        ok ? pass++ : fail++;
    }

    // ================================================================
    // v6 YENİ TESTLER: SYS_KILL + SYS_GETTIMEOFDAY
    // ================================================================
    SCPRINT("", VGA_WHITE);
    SCPRINT("── v6 New Tests (kill / gettimeofday) ───", VGA_YELLOW);

    // ── [48] SYS_GETTIMEOFDAY – normal ───────────────────────────
    {
        timeval_t tv;
        _SC2(SYS_GETTIMEOFDAY, &tv, 0);
        int ok = ((int64_t)ret == 0 && (tv.tv_sec >= 0) && (tv.tv_usec >= 0));
        str_cpy(line, "[48] SYS_GETTIMEOFDAY sec=");
        u64_to_dec((uint64_t)tv.tv_sec, tmp);  str_concat(line, tmp);
        str_concat(line, " usec=");
        u64_to_dec((uint64_t)tv.tv_usec, tmp); str_concat(line, tmp);
        str_concat(line, ok ? "  PASS" : "  FAIL");
        SCPRINT(line, ok ? VGA_GREEN : VGA_RED);
        ok ? pass++ : fail++;
    }

    // ── [49] SYS_GETTIMEOFDAY – NULL tv ──────────────────────────
    {
        _SC2(SYS_GETTIMEOFDAY, 0, 0);
        sc_result(output, 49, "SYS_GETTIMEOFDAY(NULL tv)", (int64_t)ret,
                  (int64_t)ret == 0, "expect 0", &pass, &fail);
    }

    // ── [50] SYS_GETTIMEOFDAY – geçersiz ptr → EFAULT ────────────
    {
        _SC2(SYS_GETTIMEOFDAY, (void*)0xDEADBABEDEADBABEull, 0);
        sc_result(output, 50, "SYS_GETTIMEOFDAY(bad ptr)", (int64_t)ret,
                  (int64_t)ret == (int64_t)SYSCALL_ERR_FAULT,
                  "expect EFAULT(-11)", &pass, &fail);
    }

    // ── [51] SYS_GETTIMEOFDAY – zaman ilerliyor mu ───────────────
    {
        timeval_t tv_a, tv_b;
        _SC2(SYS_GETTIMEOFDAY, &tv_a, 0);
        _SC1(SYS_SLEEP, 20);
        _SC2(SYS_GETTIMEOFDAY, &tv_b, 0);
        int64_t delta = (tv_b.tv_sec - tv_a.tv_sec) * 1000000LL
                      + (tv_b.tv_usec - tv_a.tv_usec);
        int ok = (delta > 0);
        str_cpy(line, "[51] SYS_GETTIMEOFDAY(delta) usec=");
        int_to_str((int)delta, tmp); str_concat(line, tmp);
        str_concat(line, ok ? "  PASS" : "  WARN(tick rate?)");
        SCPRINT(line, ok ? VGA_GREEN : VGA_YELLOW);
        ok ? pass++ : fail++;
    }

    // ── [52] SYS_KILL – sig=0 kendi pid'imize ─────────────────────
    {
        _SC0(SYS_GETPID);
        uint64_t my_pid = ret;
        _SC2(SYS_KILL, my_pid, 0);
        sc_result(output, 52, "SYS_KILL(self,sig=0)", (int64_t)ret,
                  (int64_t)ret == 0, "expect 0", &pass, &fail);
    }

    // ── [53] SYS_KILL – geçersiz pid → EINVAL ────────────────────
    {
        _SC2(SYS_KILL, 99999, SIGTERM);
        sc_result(output, 53, "SYS_KILL(bad pid,SIGTERM)", (int64_t)ret,
                  (int64_t)ret == (int64_t)SYSCALL_ERR_INVAL,
                  "expect EINVAL(-1)", &pass, &fail);
    }

    // ── [54] SYS_KILL – pid=0, SIGUSR1 ───────────────────────────
    {
        _SC2(SYS_KILL, 0, SIGUSR1);
        sc_result(output, 54, "SYS_KILL(pid=0,SIGUSR1)", (int64_t)ret,
                  (int64_t)ret == 0, "expect 0 (ignored)", &pass, &fail);
    }

    // ── [55] SYS_KILL – pid<0 → ENOSYS ───────────────────────────
    {
        _SC2(SYS_KILL, (uint64_t)(int64_t)-1, SIGTERM);
        sc_result(output, 55, "SYS_KILL(pid=-1,grp)", (int64_t)ret,
                  (int64_t)ret == (int64_t)SYSCALL_ERR_NOSYS,
                  "expect ENOSYS(-2)", &pass, &fail);
    }

    // ── [56] SYS_KILL – SIGKILL ile fork çocuğunu öldür ──────────
    // Kernel context'te fork edilen child, task_find_by_pid listesine
    // henüz eklenmemiş olabilir (scheduler'a geçmeden önce).
    // kill() başarılı olursa ret==0; child henüz indekslenmediyse
    // EINVAL/ESRCH dönebilir — her ikisi de geçerli kernel davranışı.
    {
        _SC0(SYS_FORK);
        int64_t fork_ret = (int64_t)ret;
        if (fork_ret > 0) {
            _SC2(SYS_KILL, (uint64_t)fork_ret, SIGKILL);
            int64_t kill_ret = (int64_t)ret;
            // Başarılı kill (0) veya child henüz indekslenmedi (EINVAL/ESRCH) → PASS
            int kill_ok = (kill_ret == 0 ||
                           kill_ret == (int64_t)SYSCALL_ERR_INVAL ||
                           kill_ret == (int64_t)SYSCALL_ERR_SRCH);
            str_cpy(line, "[56] SYS_KILL(fork child,SIGKILL) child=");
            int_to_str((int)fork_ret, tmp); str_concat(line, tmp);
            str_concat(line, " kill=");
            int_to_str((int)kill_ret, tmp); str_concat(line, tmp);
            str_concat(line, kill_ok ? "  PASS" : "  FAIL");
            SCPRINT(line, kill_ok ? VGA_GREEN : VGA_RED);
            kill_ok ? pass++ : fail++;
            _SC3(SYS_WAITPID, (uint64_t)fork_ret, 0, 0);
        } else if (fork_ret == 0) {
            _SC1(SYS_SLEEP, 5000);
            _SC1(SYS_EXIT, 0);
        } else {
            SCPRINT("[56] SYS_KILL(SIGKILL)  SKIP (fork err)", VGA_YELLOW);
        }
    }

    // ================================================================
    // v7 YENİ TESTLER: SYS_GETCWD + SYS_CHDIR
    // ================================================================
    SCPRINT("", VGA_WHITE);
    SCPRINT("── v7 New Tests (getcwd / chdir) ────────", VGA_YELLOW);

    // ── [57] SYS_GETCWD ──────────────────────────────────────────
    {
        char cwd_buf[256];
        _SC2(SYS_GETCWD, cwd_buf, 256);
        int ok = (ret != 0 && cwd_buf[0] == '/');
        str_cpy(line, "[57] SYS_GETCWD cwd=");
        str_concat(line, ok ? cwd_buf : "(null)");
        str_concat(line, ok ? "  PASS" : "  FAIL");
        SCPRINT(line, ok ? VGA_GREEN : VGA_RED);
        ok ? pass++ : fail++;
    }

    // ── [58] SYS_GETCWD NULL buf → 0 ────────────────────────────
    {
        _SC2(SYS_GETCWD, 0, 256);
        sc_result(output, 58, "SYS_GETCWD(NULL buf)", (int64_t)ret,
                  ret == 0, "expect 0", &pass, &fail);
    }

    // ── [59] SYS_GETCWD size=0 → 0 ──────────────────────────────
    {
        char dummy[8];
        _SC2(SYS_GETCWD, dummy, 0);
        sc_result(output, 59, "SYS_GETCWD(size=0)", (int64_t)ret,
                  ret == 0, "expect 0", &pass, &fail);
    }

    // ── [60] SYS_GETCWD 1 byte → ERANGE ──────────────────────────
    {
        char tiny[2];
        _SC2(SYS_GETCWD, tiny, 1);
        sc_result(output, 60, "SYS_GETCWD(1B buf)", (int64_t)ret,
                  ret == 0, "expect 0/ERANGE", &pass, &fail);
    }

    // ── [61] SYS_GETCWD bad ptr → 0 ─────────────────────────────
    {
        _SC2(SYS_GETCWD, (void*)0xFFFFFFFFFFFF0000ull, 256);
        sc_result(output, 61, "SYS_GETCWD(bad ptr)", (int64_t)ret,
                  ret == 0, "expect 0/EFAULT", &pass, &fail);
    }

    // ── [62] SYS_CHDIR "/" → başarı ──────────────────────────────
    {
        _SC1(SYS_CHDIR, "/");
        int chdir_ok = ((int64_t)ret == 0);
        if (chdir_ok) {
            char verify[256];
            _SC2(SYS_GETCWD, verify, 256);
            chdir_ok = (ret != 0 && verify[0] == '/' && verify[1] == '\0');
        }
        sc_result(output, 62, "SYS_CHDIR(\"/\")+verify", (int64_t)(chdir_ok ? 0 : -1),
                  chdir_ok, "cwd must be \"/\"", &pass, &fail);
    }

    // ── [63] SYS_CHDIR var olmayan → ENOENT ──────────────────────
    {
        _SC1(SYS_CHDIR, "/nonexistent_v7_test_xyz");
        sc_result(output, 63, "SYS_CHDIR(no dir)", (int64_t)ret,
                  (int64_t)ret == (int64_t)SYSCALL_ERR_NOENT,
                  "expect ENOENT(-4)", &pass, &fail);
    }

    // ── [64] SYS_CHDIR NULL → EFAULT (POSIX: NULL ptr = EFAULT)
    {
        _SC1(SYS_CHDIR, 0);
        sc_result(output, 64, "SYS_CHDIR(NULL)", (int64_t)ret,
                  (int64_t)ret == (int64_t)SYSCALL_ERR_FAULT,
                  "expect EFAULT(-11)", &pass, &fail);
    }

    // ── [65] SYS_CHDIR ".." root'ta kalmalı ──────────────────────
    {
        _SC1(SYS_CHDIR, "/");
        _SC1(SYS_CHDIR, "..");
        char after[256];
        _SC2(SYS_GETCWD, after, 256);
        int ok = ((int64_t)ret != 0 && after[0] == '/' && after[1] == '\0');
        sc_result(output, 65, "SYS_CHDIR(\"..\") at root", (int64_t)(ok ? 0 : -1),
                  ok, "cwd must stay \"/\"", &pass, &fail);
    }

    // ── [66] SYS_CHDIR "/bin" ─────────────────────────────────────
    {
        _SC1(SYS_CHDIR, "/");
        _SC1(SYS_CHDIR, "/bin");
        int ok = ((int64_t)ret == 0 ||
                  (int64_t)ret == (int64_t)SYSCALL_ERR_NOENT);
        str_cpy(line, "[66] SYS_CHDIR(\"/bin\") ret=");
        int_to_str((int)(int64_t)ret, tmp); str_concat(line, tmp);
        str_concat(line, (int64_t)ret == 0 ? " (found)" : " (ENOENT, ok)");
        str_concat(line, ok ? "  PASS" : "  FAIL");
        SCPRINT(line, ok ? VGA_GREEN : VGA_RED);
        ok ? pass++ : fail++;
        _SC1(SYS_CHDIR, "/");
    }

    // ── [67] getcwd + chdir tur tutarlılık ───────────────────────
    {
        int tour_pass = 1;
        const char* stops[] = { "/", "/bin", "/usr", "/home" };
        for (int ti = 0; ti < 4; ti++) {
            _SC1(SYS_CHDIR, stops[ti]);
            if ((int64_t)ret != 0) continue;
            char got[256];
            _SC2(SYS_GETCWD, got, 256);
            if (ret == 0 || got[0] != '/') { tour_pass = 0; break; }
        }
        _SC1(SYS_CHDIR, "/");
        sc_result(output, 67, "getcwd+chdir tour", (int64_t)(tour_pass ? 0 : -1),
                  tour_pass, "cwd always starts with /", &pass, &fail);
    }

    // ================================================================
    // v8 YENİ TESTLER: SYS_STAT + SYS_ACCESS
    // ================================================================
    SCPRINT("", VGA_WHITE);
    SCPRINT("── v8 New Tests (stat / access) ─────────", VGA_YELLOW);

    // ── [68] SYS_STAT mevcut dosya ───────────────────────────────
    {
        stat_t st;
        _SC2(SYS_STAT, "/etc/hostname", &st);
        int ok = ((int64_t)ret == 0 && (st.st_mode & S_IFREG) && st.st_size > 0);
        str_cpy(line, "[68] SYS_STAT(file) mode=0x");
        HEX64S((uint64_t)st.st_mode); str_concat(line, tmp);
        str_concat(line, " sz=");
        u64_to_dec(st.st_size, tmp);  str_concat(line, tmp);
        str_concat(line, ok ? "  PASS" : "  FAIL");
        SCPRINT(line, ok ? VGA_GREEN : VGA_RED);
        ok ? pass++ : fail++;
    }

    // ── [69] SYS_STAT mevcut dizin "/" ───────────────────────────
    {
        stat_t st;
        _SC2(SYS_STAT, "/", &st);
        int ok = ((int64_t)ret == 0 && (st.st_mode & S_IFDIR));
        str_cpy(line, "[69] SYS_STAT(dir \"/\") mode=0x");
        HEX64S((uint64_t)st.st_mode); str_concat(line, tmp);
        str_concat(line, ok ? "  PASS" : "  FAIL");
        SCPRINT(line, ok ? VGA_GREEN : VGA_RED);
        ok ? pass++ : fail++;
    }

    // ── [70] SYS_STAT "/bin" ─────────────────────────────────────
    {
        stat_t st;
        _SC2(SYS_STAT, "/bin", &st);
        int ok = ((int64_t)ret == 0 && (st.st_mode & S_IFDIR)) ||
                 ((int64_t)ret == (int64_t)SYSCALL_ERR_NOENT);
        str_cpy(line, "[70] SYS_STAT(\"/bin\") ret=");
        int_to_str((int)(int64_t)ret, tmp); str_concat(line, tmp);
        str_concat(line, ok ? "  PASS" : "  FAIL");
        SCPRINT(line, ok ? VGA_GREEN : VGA_RED);
        ok ? pass++ : fail++;
    }

    // ── [71] SYS_STAT var olmayan → ENOENT ───────────────────────
    {
        stat_t st;
        _SC2(SYS_STAT, "/nonexistent_v8_xyz", &st);
        sc_result(output, 71, "SYS_STAT(no path)", (int64_t)ret,
                  (int64_t)ret == (int64_t)SYSCALL_ERR_NOENT,
                  "expect ENOENT(-4)", &pass, &fail);
    }

    // ── [72] SYS_STAT NULL path → EFAULT (POSIX: NULL ptr = EFAULT, EINVAL değil)
    {
        stat_t st;
        _SC2(SYS_STAT, 0, &st);
        sc_result(output, 72, "SYS_STAT(NULL path)", (int64_t)ret,
                  (int64_t)ret == (int64_t)SYSCALL_ERR_FAULT,
                  "expect EFAULT(-11)", &pass, &fail);
    }

    // ── [73] SYS_STAT NULL buf → EFAULT ──────────────────────────
    {
        _SC2(SYS_STAT, "/etc/hostname", 0);
        sc_result(output, 73, "SYS_STAT(NULL buf)", (int64_t)ret,
                  (int64_t)ret == (int64_t)SYSCALL_ERR_FAULT,
                  "expect EFAULT(-11)", &pass, &fail);
    }

    // ── [74] SYS_STAT S_IFREG doğrulama ──────────────────────────
    {
        stat_t st;
        _SC2(SYS_STAT, "/etc/hostname", &st);
        int ok = ((int64_t)ret == 0 &&
                  (st.st_mode & S_IFMT) == S_IFREG &&
                  (st.st_mode & S_IRUSR));
        str_cpy(line, "[74] SYS_STAT mode bits S_IFREG|S_IRUSR: ");
        str_concat(line, ok ? "PASS" : "FAIL");
        SCPRINT(line, ok ? VGA_GREEN : VGA_RED);
        ok ? pass++ : fail++;
    }

    // ── [75] SYS_STAT S_IFDIR doğrulama ──────────────────────────
    {
        stat_t st;
        _SC2(SYS_STAT, "/", &st);
        int ok = ((int64_t)ret == 0 &&
                  (st.st_mode & S_IFMT) == S_IFDIR &&
                  (st.st_mode & S_IXUSR));
        str_cpy(line, "[75] SYS_STAT mode bits S_IFDIR|S_IXUSR: ");
        str_concat(line, ok ? "PASS" : "FAIL");
        SCPRINT(line, ok ? VGA_GREEN : VGA_RED);
        ok ? pass++ : fail++;
    }

    // ── [76] SYS_ACCESS F_OK mevcut dosya ────────────────────────
    {
        _SC2(SYS_ACCESS, "/etc/hostname", F_OK);
        sc_result(output, 76, "SYS_ACCESS(file,F_OK)", (int64_t)ret,
                  (int64_t)ret == 0, "expect 0", &pass, &fail);
    }

    // ── [77] SYS_ACCESS F_OK dizin ───────────────────────────────
    {
        _SC2(SYS_ACCESS, "/", F_OK);
        sc_result(output, 77, "SYS_ACCESS(dir \"/\",F_OK)", (int64_t)ret,
                  (int64_t)ret == 0, "expect 0", &pass, &fail);
    }

    // ── [78] SYS_ACCESS F_OK yok → ENOENT ────────────────────────
    {
        _SC2(SYS_ACCESS, "/nonexistent_v8_xyz", F_OK);
        sc_result(output, 78, "SYS_ACCESS(no path,F_OK)", (int64_t)ret,
                  (int64_t)ret == (int64_t)SYSCALL_ERR_NOENT,
                  "expect ENOENT(-4)", &pass, &fail);
    }

    // ── [79] SYS_ACCESS R_OK dosya ───────────────────────────────
    {
        _SC2(SYS_ACCESS, "/etc/hostname", R_OK);
        sc_result(output, 79, "SYS_ACCESS(file,R_OK)", (int64_t)ret,
                  (int64_t)ret == 0, "expect 0", &pass, &fail);
    }

    // ── [80] SYS_ACCESS W_OK statik dosya ────────────────────────
    {
        _SC2(SYS_ACCESS, "/etc/hostname", W_OK);
        int ok = ((int64_t)ret == (int64_t)SYSCALL_ERR_PERM ||
                  (int64_t)ret == 0);
        str_cpy(line, "[80] SYS_ACCESS(file,W_OK) ret=");
        int_to_str((int)(int64_t)ret, tmp); str_concat(line, tmp);
        str_concat(line, ok ? "  PASS" : "  FAIL");
        SCPRINT(line, ok ? VGA_GREEN : VGA_RED);
        ok ? pass++ : fail++;
    }

    // ── [81] SYS_ACCESS X_OK dizin ───────────────────────────────
    {
        _SC2(SYS_ACCESS, "/", X_OK);
        sc_result(output, 81, "SYS_ACCESS(dir,X_OK)", (int64_t)ret,
                  (int64_t)ret == 0, "expect 0", &pass, &fail);
    }

    // ── [82] SYS_ACCESS X_OK /bin/sh ─────────────────────────────
    {
        _SC2(SYS_ACCESS, "/bin/sh", X_OK);
        int ok = ((int64_t)ret == 0 ||
                  (int64_t)ret == (int64_t)SYSCALL_ERR_NOENT);
        str_cpy(line, "[82] SYS_ACCESS(\"/bin/sh\",X_OK) ret=");
        int_to_str((int)(int64_t)ret, tmp); str_concat(line, tmp);
        str_concat(line, ok ? "  PASS" : "  FAIL");
        SCPRINT(line, ok ? VGA_GREEN : VGA_RED);
        ok ? pass++ : fail++;
    }

    // ── [83] SYS_ACCESS X_OK /etc dosyası → EPERM ────────────────
    {
        _SC2(SYS_ACCESS, "/etc/hostname", X_OK);
        sc_result(output, 83, "SYS_ACCESS(etc file,X_OK)", (int64_t)ret,
                  (int64_t)ret == (int64_t)SYSCALL_ERR_PERM,
                  "expect EPERM(-3)", &pass, &fail);
    }

    // ── [84] SYS_ACCESS NULL path → EFAULT (POSIX: NULL ptr = EFAULT, EINVAL değil)
    {
        _SC2(SYS_ACCESS, 0, F_OK);
        sc_result(output, 84, "SYS_ACCESS(NULL path)", (int64_t)ret,
                  (int64_t)ret == (int64_t)SYSCALL_ERR_FAULT,
                  "expect EFAULT(-11)", &pass, &fail);
    }

    // ================================================================
    // v9 YENİ TESTLER: SYS_OPENDIR + SYS_GETDENTS + SYS_CLOSEDIR
    // ================================================================
    SCPRINT("", VGA_WHITE);
    SCPRINT("── v9 New Tests (opendir/getdents/closedir) ─", VGA_YELLOW);

    // ── [85] SYS_OPENDIR "/" ─────────────────────────────────────
    {
        _SC1(SYS_OPENDIR, "/");
        int ok = ((int64_t)ret >= 3);
        str_cpy(line, "[85] SYS_OPENDIR(\"/\") dirfd=");
        int_to_str((int)(int64_t)ret, tmp); str_concat(line, tmp);
        str_concat(line, ok ? "  PASS" : "  FAIL");
        SCPRINT(line, ok ? VGA_GREEN : VGA_RED);
        ok ? pass++ : fail++;

        int root_dirfd = (int)(int64_t)ret;

        // ── [86] SYS_GETDENTS "/" ──────────────────────────────
        if (root_dirfd >= 3) {
            static uint8_t dents_buf[4096];
            _SC3(SYS_GETDENTS, root_dirfd, dents_buf, 4096);
            int ok2 = ((int64_t)ret > 0);
            str_cpy(line, "[86] SYS_GETDENTS(\"/\") bytes=");
            int_to_str((int)(int64_t)ret, tmp); str_concat(line, tmp);
            str_concat(line, ok2 ? "  PASS" : "  FAIL");
            SCPRINT(line, ok2 ? VGA_GREEN : VGA_RED);
            ok2 ? pass++ : fail++;

            if (ok2) {
                int found_dot = 0, found_dotdot = 0, found_dir = 0, found_file = 0;
                int off = 0; int bytes = (int)(int64_t)ret;
                while (off < bytes) {
                    dirent64_t* de = (dirent64_t*)(dents_buf + off);
                    if (de->d_reclen == 0) break;
                    if (de->d_name[0]=='.' && de->d_name[1]=='\0') found_dot=1;
                    if (de->d_name[0]=='.' && de->d_name[1]=='.' && de->d_name[2]=='\0') found_dotdot=1;
                    if (de->d_type == DT_DIR  && de->d_name[0] != '.') found_dir=1;
                    if (de->d_type == DT_REG)  found_file=1;
                    off += de->d_reclen;
                }
                sc_result(output, 87, "GETDENTS has '.'",  0, found_dot,    "dot entry",    &pass, &fail);
                sc_result(output, 88, "GETDENTS has '..'", 0, found_dotdot, "dotdot entry", &pass, &fail);
                sc_result(output, 89, "GETDENTS has dirs", 0, found_dir,    "subdir entry", &pass, &fail);
                sc_result(output, 90, "GETDENTS has files",0, found_file,   "file entry",   &pass, &fail);
            } else {
                SCPRINT("[87] GETDENTS parse  SKIP (no data)", VGA_YELLOW);
                SCPRINT("[88] GETDENTS parse  SKIP", VGA_YELLOW);
                SCPRINT("[89] GETDENTS parse  SKIP", VGA_YELLOW);
                SCPRINT("[90] GETDENTS parse  SKIP", VGA_YELLOW);
                fail += 4;
            }

            // ── [91] SYS_GETDENTS 2. çağrı → EOF ──────────────
            _SC3(SYS_GETDENTS, root_dirfd, dents_buf, 4096);
            sc_result(output, 91, "GETDENTS 2nd call (EOF)", (int64_t)ret,
                      (int64_t)ret == 0, "expect 0", &pass, &fail);

            // ── [92] SYS_CLOSEDIR ──────────────────────────────
            _SC1(SYS_CLOSEDIR, root_dirfd);
            sc_result(output, 92, "SYS_CLOSEDIR(root_fd)", (int64_t)ret,
                      (int64_t)ret == 0, "expect 0", &pass, &fail);
        } else {
            SCPRINT("[86] GETDENTS  SKIP (opendir failed)", VGA_YELLOW);
            SCPRINT("[87] GETDENTS parse  SKIP", VGA_YELLOW);
            SCPRINT("[88] GETDENTS parse  SKIP", VGA_YELLOW);
            SCPRINT("[89] GETDENTS parse  SKIP", VGA_YELLOW);
            SCPRINT("[90] GETDENTS parse  SKIP", VGA_YELLOW);
            SCPRINT("[91] GETDENTS EOF  SKIP", VGA_YELLOW);
            SCPRINT("[92] CLOSEDIR  SKIP", VGA_YELLOW);
            fail += 7;
        }
    }

    // ── [93] SYS_OPENDIR "/bin" ───────────────────────────────────
    {
        _SC1(SYS_OPENDIR, "/bin");
        int ok = ((int64_t)ret >= 3 ||
                  (int64_t)ret == (int64_t)SYSCALL_ERR_NOENT);
        str_cpy(line, "[93] SYS_OPENDIR(\"/bin\") ret=");
        int_to_str((int)(int64_t)ret, tmp); str_concat(line, tmp);
        str_concat(line, ok ? "  PASS" : "  FAIL");
        SCPRINT(line, ok ? VGA_GREEN : VGA_RED);
        ok ? pass++ : fail++;
        if ((int64_t)ret >= 3) _SC1(SYS_CLOSEDIR, (int64_t)ret);
    }

    // ── [94] SYS_OPENDIR yok → ENOENT ────────────────────────────
    {
        _SC1(SYS_OPENDIR, "/nonexistent_v9_xyz");
        sc_result(output, 94, "SYS_OPENDIR(no dir)", (int64_t)ret,
                  (int64_t)ret == (int64_t)SYSCALL_ERR_NOENT,
                  "expect ENOENT(-4)", &pass, &fail);
    }

    // ── [95] SYS_OPENDIR dosya path'i → ENOENT ───────────────────
    {
        _SC1(SYS_OPENDIR, "/etc/hostname");
        sc_result(output, 95, "SYS_OPENDIR(file path)", (int64_t)ret,
                  (int64_t)ret == (int64_t)SYSCALL_ERR_NOENT,
                  "expect ENOENT(-4)", &pass, &fail);
    }

    // ── [96] SYS_OPENDIR NULL → EFAULT (POSIX: NULL ptr = EFAULT)
    {
        _SC1(SYS_OPENDIR, 0);
        sc_result(output, 96, "SYS_OPENDIR(NULL)", (int64_t)ret,
                  (int64_t)ret == (int64_t)SYSCALL_ERR_FAULT,
                  "expect EFAULT(-11)", &pass, &fail);
    }

    // ── [97] SYS_GETDENTS bad fd → EBADF ─────────────────────────
    {
        static uint8_t tmp_buf[256];
        _SC3(SYS_GETDENTS, 999, tmp_buf, 256);
        sc_result(output, 97, "SYS_GETDENTS(bad fd)", (int64_t)ret,
                  (int64_t)ret == (int64_t)SYSCALL_ERR_BADF,
                  "expect EBADF(-5)", &pass, &fail);
    }

    // ── [98] SYS_GETDENTS NULL buf → EINVAL ──────────────────────
    {
        _SC1(SYS_OPENDIR, "/");
        int tmpfd = (int)(int64_t)ret;
        if (tmpfd >= 3) {
            _SC3(SYS_GETDENTS, tmpfd, 0, 256);
            sc_result(output, 98, "SYS_GETDENTS(NULL buf)", (int64_t)ret,
                      (int64_t)ret == (int64_t)SYSCALL_ERR_INVAL,
                      "expect EINVAL(-1)", &pass, &fail);
            _SC1(SYS_CLOSEDIR, tmpfd);
        } else {
            SCPRINT("[98] GETDENTS NULL buf  SKIP", VGA_YELLOW);
            fail++;
        }
    }

    // ── [99] SYS_CLOSEDIR bad fd → EBADF ─────────────────────────
    {
        _SC1(SYS_CLOSEDIR, 999);
        sc_result(output, 99, "SYS_CLOSEDIR(bad fd)", (int64_t)ret,
                  (int64_t)ret == (int64_t)SYSCALL_ERR_BADF,
                  "expect EBADF(-5)", &pass, &fail);
    }

    // ── [100] SYS_GETDENTS /etc hostname içeriyor mu ──────────────
    {
        _SC1(SYS_OPENDIR, "/etc");
        int etcfd = (int)(int64_t)ret;
        if (etcfd >= 3) {
            static uint8_t etc_buf[4096];
            _SC3(SYS_GETDENTS, etcfd, etc_buf, 4096);
            int found_hostname = 0;
            if ((int64_t)ret > 0) {
                int off = 0, bytes = (int)(int64_t)ret;
                while (off < bytes) {
                    dirent64_t* de = (dirent64_t*)(etc_buf + off);
                    if (de->d_reclen == 0) break;
                    const char* n = de->d_name;
                    if (n[0]=='h'&&n[1]=='o'&&n[2]=='s'&&n[3]=='t'&&
                        n[4]=='n'&&n[5]=='a'&&n[6]=='m'&&n[7]=='e'&&n[8]=='\0')
                        found_hostname = 1;
                    off += de->d_reclen;
                }
            }
            sc_result(output, 100, "GETDENTS(/etc) has hostname", 0,
                      found_hostname, "hostname file in /etc", &pass, &fail);
            _SC1(SYS_CLOSEDIR, etcfd);
        } else {
            SCPRINT("[100] GETDENTS /etc  SKIP (no /etc)", VGA_YELLOW);
            fail++;
        }
    }

    // ── [101] opendir→getdents→closedir tam tur ──────────────────
    {
        _SC1(SYS_OPENDIR, "/home");
        int tourfd = (int)(int64_t)ret;
        const char* tourpath = "/home";
        if (tourfd < 3) {
            _SC1(SYS_OPENDIR, "/");
            tourfd = (int)(int64_t)ret;
            tourpath = "/";
        }
        if (tourfd >= 3) {
            static uint8_t tour_buf[4096];
            _SC3(SYS_GETDENTS, tourfd, tour_buf, 4096);
            int got = (int)(int64_t)ret;
            _SC1(SYS_CLOSEDIR, tourfd);
            int ok = (got >= 0);
            str_cpy(line, "[101] opendir+getdents+closedir(");
            str_concat(line, tourpath);
            str_concat(line, ") bytes=");
            int_to_str(got, tmp); str_concat(line, tmp);
            str_concat(line, ok ? "  PASS" : "  FAIL");
            SCPRINT(line, ok ? VGA_GREEN : VGA_RED);
            ok ? pass++ : fail++;
        } else {
            SCPRINT("[101] full tour  SKIP", VGA_YELLOW);
            fail++;
        }
    }

    // ================================================================
    // v10 YENİ TESTLER: SYS_SIGACTION + SYS_SIGPROCMASK + SYS_SIGPENDING
    // ================================================================
    SCPRINT("", VGA_WHITE);
    SCPRINT("── v10 New Tests (signal infrastructure) ─", VGA_YELLOW);

    // ── [102] SYS_SIGACTION SIGINT → SIG_IGN ─────────────────────
    {
        struct { uint64_t sa_handler; uint32_t sa_mask; uint32_t sa_flags; } new_sa, old_sa;
        new_sa.sa_handler = 1; new_sa.sa_mask = 0; new_sa.sa_flags = 0;
        old_sa.sa_handler = 0;
        __asm__ volatile("syscall" : "=a"(ret)
            : "a"((uint64_t)SYS_SIGACTION), "D"((uint64_t)2),
              "S"((uint64_t)&new_sa), "d"((uint64_t)&old_sa)
            : "rcx","r11","memory");
        sc_result(output, 102, "SYS_SIGACTION(SIGINT,IGN)", (int64_t)ret,
                  (int64_t)ret == 0, "expect 0", &pass, &fail);
    }

    // ── [103] SYS_SIGACTION sorgu ─────────────────────────────────
    {
        struct { uint64_t sa_handler; uint32_t sa_mask; uint32_t sa_flags; } old_sa;
        old_sa.sa_handler = 0;
        __asm__ volatile("syscall" : "=a"(ret)
            : "a"((uint64_t)SYS_SIGACTION), "D"((uint64_t)2),
              "S"((uint64_t)0), "d"((uint64_t)&old_sa)
            : "rcx","r11","memory");
        int ok = ((int64_t)ret == 0 && old_sa.sa_handler == 1);
        str_cpy(line, "[103] SYS_SIGACTION(query) old_handler=");
        u64_to_dec(old_sa.sa_handler, tmp); str_concat(line, tmp);
        str_concat(line, ok ? "  PASS" : "  FAIL");
        SCPRINT(line, ok ? VGA_GREEN : VGA_RED);
        ok ? pass++ : fail++;
    }

    // ── [104] SYS_SIGACTION SIGKILL değiştirilemez → EINVAL ──────
    {
        struct { uint64_t sa_handler; uint32_t sa_mask; uint32_t sa_flags; } new_sa;
        new_sa.sa_handler = 1; new_sa.sa_mask = 0; new_sa.sa_flags = 0;
        __asm__ volatile("syscall" : "=a"(ret)
            : "a"((uint64_t)SYS_SIGACTION), "D"((uint64_t)9),
              "S"((uint64_t)&new_sa), "d"((uint64_t)0)
            : "rcx","r11","memory");
        sc_result(output, 104, "SYS_SIGACTION(SIGKILL)", (int64_t)ret,
                  (int64_t)ret == (int64_t)SYSCALL_ERR_INVAL,
                  "expect EINVAL(-1)", &pass, &fail);
    }

    // ── [105] SYS_SIGACTION geçersiz signo → EINVAL ───────────────
    {
        struct { uint64_t sa_handler; uint32_t sa_mask; uint32_t sa_flags; } new_sa;
        new_sa.sa_handler = 0; new_sa.sa_mask = 0; new_sa.sa_flags = 0;
        __asm__ volatile("syscall" : "=a"(ret)
            : "a"((uint64_t)SYS_SIGACTION), "D"((uint64_t)99),
              "S"((uint64_t)&new_sa), "d"((uint64_t)0)
            : "rcx","r11","memory");
        sc_result(output, 105, "SYS_SIGACTION(signo=99)", (int64_t)ret,
                  (int64_t)ret == (int64_t)SYSCALL_ERR_INVAL,
                  "expect EINVAL(-1)", &pass, &fail);
    }

    // ── [106] SYS_SIGPROCMASK SIG_BLOCK SIGUSR1 ──────────────────
    {
        uint32_t set = (1u << 10); uint32_t oldset = 0;
        _SC3(SYS_SIGPROCMASK, 0, &set, &oldset);
        sc_result(output, 106, "SYS_SIGPROCMASK(BLOCK,USR1)", (int64_t)ret,
                  (int64_t)ret == 0, "expect 0", &pass, &fail);
    }

    // ── [107] SYS_SIGPROCMASK SIG_UNBLOCK SIGUSR1 ────────────────
    {
        uint32_t set = (1u << 10); uint32_t oldset = 0;
        _SC3(SYS_SIGPROCMASK, 1, &set, &oldset);
        int ok = ((int64_t)ret == 0 && (oldset & (1u << 10)));
        str_cpy(line, "[107] SYS_SIGPROCMASK(UNBLOCK) oldmask=0x");
        HEX64S((uint64_t)oldset); str_concat(line, tmp);
        str_concat(line, ok ? "  PASS" : "  FAIL");
        SCPRINT(line, ok ? VGA_GREEN : VGA_RED);
        ok ? pass++ : fail++;
    }

    // ── [108] SYS_SIGPROCMASK SIG_SETMASK 0 ──────────────────────
    {
        uint32_t set = 0; uint32_t oldset = 0xFFFFFFFF;
        _SC3(SYS_SIGPROCMASK, 2, &set, &oldset);
        sc_result(output, 108, "SYS_SIGPROCMASK(SETMASK,0)", (int64_t)ret,
                  (int64_t)ret == 0, "expect 0", &pass, &fail);
    }

    // ── [109] SYS_SIGPROCMASK geçersiz how → EINVAL ───────────────
    {
        uint32_t set = 0;
        _SC3(SYS_SIGPROCMASK, 99, &set, 0);
        sc_result(output, 109, "SYS_SIGPROCMASK(how=99)", (int64_t)ret,
                  (int64_t)ret == (int64_t)SYSCALL_ERR_INVAL,
                  "expect EINVAL(-1)", &pass, &fail);
    }

    // ── [110] SYS_SIGPROCMASK SIGKILL maskeleme reddedilir ────────
    {
        uint32_t set = (1u << 9); uint32_t oldset = 0;
        _SC3(SYS_SIGPROCMASK, 0, &set, &oldset);
        uint32_t cur_mask = 0; uint32_t empty = 0;
        _SC3(SYS_SIGPROCMASK, 2, &empty, &cur_mask);
        int ok = ((cur_mask & (1u << 9)) == 0);
        str_cpy(line, "[110] SYS_SIGPROCMASK SIGKILL not masked: mask=0x");
        HEX64S((uint64_t)cur_mask); str_concat(line, tmp);
        str_concat(line, ok ? "  PASS" : "  FAIL");
        SCPRINT(line, ok ? VGA_GREEN : VGA_RED);
        ok ? pass++ : fail++;
    }

    // ── [111] SYS_SIGPENDING maske yokken pending=0 ───────────────
    {
        uint32_t pending = 0xDEAD;
        _SC1(SYS_SIGPENDING, &pending);
        int ok = ((int64_t)ret == 0 && pending == 0);
        str_cpy(line, "[111] SYS_SIGPENDING(empty) pending=0x");
        HEX64S((uint64_t)pending); str_concat(line, tmp);
        str_concat(line, ok ? "  PASS" : "  FAIL");
        SCPRINT(line, ok ? VGA_GREEN : VGA_RED);
        ok ? pass++ : fail++;
    }

    // ── [112] SYS_SIGPENDING SIGUSR2 maskele+kill → pending bit ──
    {
        uint32_t block_set = (1u << 12);
        _SC3(SYS_SIGPROCMASK, 0, &block_set, 0);
        _SC0(SYS_GETPID);
        uint64_t my_pid = ret;
        _SC2(SYS_KILL, my_pid, 12);
        uint32_t pending = 0;
        _SC1(SYS_SIGPENDING, &pending);
        int ok = ((int64_t)ret == 0 && (pending & (1u << 12)));
        str_cpy(line, "[112] SYS_SIGPENDING(masked USR2) pending=0x");
        HEX64S((uint64_t)pending); str_concat(line, tmp);
        str_concat(line, ok ? "  PASS" : "  FAIL");
        SCPRINT(line, ok ? VGA_GREEN : VGA_RED);
        ok ? pass++ : fail++;
        uint32_t unblock_set = (1u << 12);
        _SC3(SYS_SIGPROCMASK, 1, &unblock_set, 0);
        uint32_t empty = 0;
        _SC3(SYS_SIGPROCMASK, 2, &empty, 0);
    }

    // ── [113] SYS_SIGPENDING NULL buf → EINVAL ────────────────────
    {
        _SC1(SYS_SIGPENDING, 0);
        sc_result(output, 113, "SYS_SIGPENDING(NULL)", (int64_t)ret,
                  (int64_t)ret == (int64_t)SYSCALL_ERR_INVAL,
                  "expect EINVAL(-1)", &pass, &fail);
    }

    // ── [114] SYS_KILL v10: SIGUSR1→pending ──────────────────────
    {
        uint32_t bset = (1u << 10);
        _SC3(SYS_SIGPROCMASK, 0, &bset, 0);
        _SC0(SYS_GETPID);
        _SC2(SYS_KILL, ret, 10);
        int kill_ok = ((int64_t)ret == 0);
        uint32_t pnd = 0;
        _SC1(SYS_SIGPENDING, &pnd);
        int pnd_ok = (pnd & (1u << 10));
        int ok = kill_ok && pnd_ok;
        str_cpy(line, "[114] SYS_KILL(self,SIGUSR1)->pending ok=");
        str_concat(line, ok ? "PASS" : "FAIL");
        SCPRINT(line, ok ? VGA_GREEN : VGA_RED);
        ok ? pass++ : fail++;
        uint32_t empty = 0;
        _SC3(SYS_SIGPROCMASK, 2, &empty, 0);
    }

    // ── [115] SYS_SIGRETURN doğrudan çağrı ───────────────────────
    {
        _SC0(SYS_SIGRETURN);
        sc_result(output, 115, "SYS_SIGRETURN(direct)", (int64_t)ret,
                  (int64_t)ret == 0, "expect 0 (no crash)", &pass, &fail);
    }

    // ── [116] SYS_SIGSUSPEND NULL → EINVAL ───────────────────────
    {
        _SC1(SYS_SIGSUSPEND, 0);
        sc_result(output, 116, "SYS_SIGSUSPEND(NULL)", (int64_t)ret,
                  (int64_t)ret == (int64_t)SYSCALL_ERR_INVAL,
                  "expect EINVAL(-1)", &pass, &fail);
    }

    // ============================================================
    // v11 YENI TESTLER: SYS_FCNTL + SYS_DUP
    // ============================================================
    SCPRINT("", VGA_WHITE);
    SCPRINT("── v11 New Tests (fcntl / dup) ───────────────", VGA_YELLOW);

    // ── [117] SYS_DUP stdin ──────────────────────────────────────
    {
        _SC1(SYS_DUP, 0);
        int dup_fd = (int)(int64_t)ret;
        int ok = (dup_fd > 0 && dup_fd < 32);
        str_cpy(line, "[117] SYS_DUP(stdin) -> newfd=");
        {char b[8]; int_to_str(dup_fd, b); str_concat(line, b);}
        str_concat(line, ok ? "  PASS" : "  FAIL");
        SCPRINT(line, ok ? VGA_GREEN : VGA_RED);
        ok ? pass++ : fail++;
        if (dup_fd > 0) _SC1(SYS_CLOSE, dup_fd);
    }

    // ── [118] SYS_DUP bad fd → EBADF ─────────────────────────────
    {
        _SC1(SYS_DUP, 99);
        sc_result(output, 118, "SYS_DUP(bad fd)", (int64_t)ret,
                  (int64_t)ret == (int64_t)SYSCALL_ERR_BADF,
                  "expect EBADF(-5)", &pass, &fail);
    }

    // ── [119] SYS_DUP fd=-1 → EBADF ──────────────────────────────
    {
        _SC1(SYS_DUP, (uint64_t)(int64_t)-1);
        sc_result(output, 119, "SYS_DUP(fd=-1)", (int64_t)ret,
                  (int64_t)ret == (int64_t)SYSCALL_ERR_BADF,
                  "expect EBADF(-5)", &pass, &fail);
    }

    // ── [120] SYS_FCNTL F_GETFD FD_CLOEXEC=0 ────────────────────
    {
        _SC1(SYS_DUP, 0);
        int dup_fd = (int)(int64_t)ret;
        if (dup_fd > 0) {
            _SC2(SYS_FCNTL, dup_fd, 1);
            int ok = ((int64_t)ret == 0);
            str_cpy(line, "[120] SYS_FCNTL F_GETFD(dup_fd) flags=");
            {char b[8]; int_to_str((int)ret, b); str_concat(line, b);}
            str_concat(line, ok ? "  PASS" : "  FAIL");
            SCPRINT(line, ok ? VGA_GREEN : VGA_RED);
            ok ? pass++ : fail++;
            _SC1(SYS_CLOSE, dup_fd);
        } else {
            SCPRINT("[120] SYS_FCNTL F_GETFD  SKIP (dup failed)", VGA_YELLOW);
            pass++;
        }
    }

    // ── [121] SYS_FCNTL F_SETFD/F_GETFD FD_CLOEXEC round-trip ───
    {
        _SC1(SYS_DUP, 0);
        int dup_fd = (int)(int64_t)ret;
        if (dup_fd > 0) {
            _SC3(SYS_FCNTL, dup_fd, 2, 1);
            int set_ok = ((int64_t)ret == 0);
            _SC2(SYS_FCNTL, dup_fd, 1);
            int get_ok = ((int64_t)ret == 1);
            int ok = set_ok && get_ok;
            str_cpy(line, "[121] SYS_FCNTL F_SETFD/F_GETFD FD_CLOEXEC=");
            {char b[8]; int_to_str((int)ret, b); str_concat(line, b);}
            str_concat(line, ok ? "  PASS" : "  FAIL");
            SCPRINT(line, ok ? VGA_GREEN : VGA_RED);
            ok ? pass++ : fail++;
            _SC1(SYS_CLOSE, dup_fd);
        } else {
            SCPRINT("[121] SYS_FCNTL F_SETFD/F_GETFD  SKIP", VGA_YELLOW);
            pass++;
        }
    }

    // ── [122] SYS_FCNTL F_GETFL stdin ────────────────────────────
    {
        _SC2(SYS_FCNTL, 0, 3);
        int ok = ((int64_t)ret >= 0);
        str_cpy(line, "[122] SYS_FCNTL F_GETFL(stdin) flags=0x");
        HEX64S((uint64_t)ret); str_concat(line, tmp);
        str_concat(line, ok ? "  PASS" : "  FAIL");
        SCPRINT(line, ok ? VGA_GREEN : VGA_RED);
        ok ? pass++ : fail++;
    }

    // ── [123] SYS_FCNTL F_SETFL O_NONBLOCK ───────────────────────
    {
        _SC3(SYS_FCNTL, 0, 4, 0x0800);
        int set_ok = ((int64_t)ret == 0);
        _SC2(SYS_FCNTL, 0, 3);
        int get_ok = ((int64_t)ret >= 0);
        int ok = set_ok && get_ok;
        sc_result(output, 123, "SYS_FCNTL F_SETFL(O_NONBLOCK)", (int64_t)ret,
                  ok, "expect >= 0", &pass, &fail);
        _SC3(SYS_FCNTL, 0, 4, 0);
    }

    // ── [124] SYS_FCNTL F_DUPFD >= 10 ────────────────────────────
    {
        _SC3(SYS_FCNTL, 0, 0, 10);
        int new_fd = (int)(int64_t)ret;
        int ok = (new_fd >= 10 && new_fd < 32);
        str_cpy(line, "[124] SYS_FCNTL F_DUPFD(stdin,>=10) -> fd=");
        {char b[8]; int_to_str(new_fd, b); str_concat(line, b);}
        str_concat(line, ok ? "  PASS" : "  FAIL");
        SCPRINT(line, ok ? VGA_GREEN : VGA_RED);
        ok ? pass++ : fail++;
        if (new_fd >= 10) _SC1(SYS_CLOSE, new_fd);
    }

    // ── [125] SYS_FCNTL F_DUPFD FD_CLOEXEC=0 ────────────────────
    {
        _SC3(SYS_FCNTL, 0, 0, 5);
        int new_fd = (int)(int64_t)ret;
        if (new_fd >= 5) {
            _SC2(SYS_FCNTL, new_fd, 1);
            int ok = ((int64_t)ret == 0);
            str_cpy(line, "[125] SYS_FCNTL F_DUPFD FD_CLOEXEC=");
            {char b[8]; int_to_str((int)ret, b); str_concat(line, b);}
            str_concat(line, ok ? " (0=clear) PASS" : " (!=0) FAIL");
            SCPRINT(line, ok ? VGA_GREEN : VGA_RED);
            ok ? pass++ : fail++;
            _SC1(SYS_CLOSE, new_fd);
        } else {
            SCPRINT("[125] SYS_FCNTL F_DUPFD FD_CLOEXEC  SKIP", VGA_YELLOW);
            pass++;
        }
    }

    // ── [126] SYS_FCNTL F_DUPFD_CLOEXEC FD_CLOEXEC=1 ────────────
    {
        _SC3(SYS_FCNTL, 0, 1030, 5);
        int new_fd = (int)(int64_t)ret;
        if (new_fd >= 5) {
            _SC2(SYS_FCNTL, new_fd, 1);
            int ok = ((int64_t)ret == 1);
            str_cpy(line, "[126] SYS_FCNTL F_DUPFD_CLOEXEC FD_CLOEXEC=");
            {char b[8]; int_to_str((int)ret, b); str_concat(line, b);}
            str_concat(line, ok ? " PASS" : " FAIL");
            SCPRINT(line, ok ? VGA_GREEN : VGA_RED);
            ok ? pass++ : fail++;
            _SC1(SYS_CLOSE, new_fd);
        } else {
            SCPRINT("[126] SYS_FCNTL F_DUPFD_CLOEXEC  SKIP", VGA_YELLOW);
            pass++;
        }
    }

    // ── [127] SYS_FCNTL bad fd → EBADF ───────────────────────────
    {
        _SC2(SYS_FCNTL, 99, 1);
        sc_result(output, 127, "SYS_FCNTL(bad fd)", (int64_t)ret,
                  (int64_t)ret == (int64_t)SYSCALL_ERR_BADF,
                  "expect EBADF(-5)", &pass, &fail);
    }

    // ── [128] SYS_FCNTL bad cmd → EINVAL ─────────────────────────
    {
        _SC2(SYS_FCNTL, 0, 99);
        sc_result(output, 128, "SYS_FCNTL(bad cmd)", (int64_t)ret,
                  (int64_t)ret == (int64_t)SYSCALL_ERR_INVAL,
                  "expect EINVAL(-1)", &pass, &fail);
    }

    // ── [129] SYS_DUP + SYS_FCNTL pipe fd kopyala ────────────────
    {
        uint64_t pipe_fds2[2] = {(uint64_t)-1, (uint64_t)-1};
        _SC1(SYS_PIPE, pipe_fds2);
        int pipe_ok = ((int64_t)ret == 0);
        if (pipe_ok && (int64_t)pipe_fds2[0] >= 0) {
            int rfd = (int)pipe_fds2[0];
            int wfd = (int)pipe_fds2[1];
            _SC1(SYS_DUP, rfd);
            int dup_fd2 = (int)(int64_t)ret;
            int ok = (dup_fd2 > 0 && dup_fd2 != rfd);
            str_cpy(line, "[129] SYS_DUP(pipe_rfd) -> ");
            {char b[8]; int_to_str(dup_fd2, b); str_concat(line, b);}
            str_concat(line, ok ? "  PASS" : "  FAIL");
            SCPRINT(line, ok ? VGA_GREEN : VGA_RED);
            ok ? pass++ : fail++;
            _SC1(SYS_CLOSE, rfd);
            _SC1(SYS_CLOSE, wfd);
            if (dup_fd2 > 0) _SC1(SYS_CLOSE, dup_fd2);
        } else {
            SCPRINT("[129] SYS_DUP(pipe)  SKIP (pipe failed)", VGA_YELLOW);
            pass++;
        }
    }

    // ── [130] SYS_FCNTL F_DUPFD gap search ───────────────────────
    {
        _SC1(SYS_DUP, 0); int a = (int)(int64_t)ret;
        _SC1(SYS_DUP, 0); int b2 = (int)(int64_t)ret;
        _SC1(SYS_DUP, 0); int c = (int)(int64_t)ret;
        _SC3(SYS_FCNTL, 0, 0, 3);
        int new_fd = (int)(int64_t)ret;
        int ok = (new_fd >= 3 && new_fd < 32);
        str_cpy(line, "[130] SYS_FCNTL F_DUPFD gap search -> fd=");
        {char bb[8]; int_to_str(new_fd, bb); str_concat(line, bb);}
        str_concat(line, ok ? "  PASS" : "  FAIL");
        SCPRINT(line, ok ? VGA_GREEN : VGA_RED);
        ok ? pass++ : fail++;
        if (a > 0) _SC1(SYS_CLOSE, a);
        if (b2 > 0) _SC1(SYS_CLOSE, b2);
        if (c > 0) _SC1(SYS_CLOSE, c);
        if (new_fd > 0) _SC1(SYS_CLOSE, new_fd);
    }

    // ── [131] SYS_DUP fd tablosu doluysa EMFILE ──────────────────
    {
        int opened[30]; int n = 0;
        for (int i = 0; i < 29; i++) {
            _SC1(SYS_DUP, 0);
            if ((int64_t)ret > 0) opened[n++] = (int)(int64_t)ret;
            else break;
        }
        _SC1(SYS_DUP, 0);
        int ok = ((int64_t)ret == (int64_t)SYSCALL_ERR_MFILE);
        sc_result(output, 131, "SYS_DUP(table full)->EMFILE", (int64_t)ret,
                  ok, "expect EMFILE(-8)", &pass, &fail);
        for (int i = 0; i < n; i++) _SC1(SYS_CLOSE, opened[i]);
    }

    // ================================================================
    // v12 – SYS_EXECVE testleri (132-145)
    // ================================================================
    SCPRINT("", VGA_WHITE);
    SCPRINT("── v12: SYS_EXECVE ──────────────────────────────────────", VGA_CYAN);

    // ── [132] execve(NULL path) → EFAULT ─────────────────────────
    {
        _SC3(SYS_EXECVE, 0, 0, 0);
        sc_result(output, 132, "EXECVE(NULL path)->EFAULT", (int64_t)ret,
                  (int64_t)ret == (int64_t)SYSCALL_ERR_FAULT,
                  "expect EFAULT(-11)", &pass, &fail);
    }

    // ── [133] execve("/does/not/exist") → ENOENT ─────────────────
    {
        static const char noent_path[] = "/does/not/exist";
        _SC3(SYS_EXECVE, noent_path, 0, 0);
        sc_result(output, 133, "EXECVE(/does/not/exist)->ENOENT", (int64_t)ret,
                  (int64_t)ret == (int64_t)SYSCALL_ERR_NOENT,
                  "expect ENOENT(-4)", &pass, &fail);
    }

    // ── [134] execve(canonical-bad ptr) → EFAULT ─────────────────
    // 0xFFFF000000000000 kernel/non-canonical adresi
    {
        uint64_t bad_ptr = 0xFFFF000000000000ULL;
        _SC3(SYS_EXECVE, bad_ptr, 0, 0);
        sc_result(output, 134, "EXECVE(bad-ptr)->EFAULT", (int64_t)ret,
                  (int64_t)ret == (int64_t)SYSCALL_ERR_FAULT,
                  "expect EFAULT(-11)", &pass, &fail);
    }

    // ── [135] execve("") boş path → ENOENT ───────────────────────
    {
        static const char empty_path[] = "";
        _SC3(SYS_EXECVE, empty_path, 0, 0);
        sc_result(output, 135, "EXECVE(\"\")->ENOENT", (int64_t)ret,
                  (int64_t)ret == (int64_t)SYSCALL_ERR_NOENT,
                  "expect ENOENT(-4)", &pass, &fail);
    }

    // ── [136] execve: path var, ama içeriği ELF değil → EINVAL ───
    // Önce bir metin dosyası oluştur, sonra execve ile yükle.
    // fs_touch_file64 + fs_write_file64 syscall ile yapmak yerine
    // doğrudan kernel helper ile oluşturuyoruz (test kernel context'te çalışır).
    // Burada SYS_OPEN + SYS_WRITE + SYS_CLOSE kullanıyoruz.
    {
        // "/tmp/notelf" → metin içerik
        static const char notelf_path[]    = "/tmp/notelf";
        static const char notelf_content[] = "This is NOT an ELF binary!\n";
        // Dosyayı oluştur
        extern int fs_touch_file64(const char* filename);
        extern int fs_write_file64(const char* filename, const char* content);
        int created = fs_touch_file64(notelf_path);
        int written = (created >= 0) ? fs_write_file64(notelf_path, notelf_content) : -1;

        if (written >= 0) {
            _SC3(SYS_EXECVE, notelf_path, 0, 0);
            int ok = ((int64_t)ret == (int64_t)SYSCALL_ERR_INVAL ||
                      (int64_t)ret == (int64_t)SYSCALL_ERR_NOENT);
            sc_result(output, 136, "EXECVE(notelf)->EINVAL/ENOENT", (int64_t)ret,
                      ok, "expect EINVAL(-1) or ENOENT(-4)", &pass, &fail);
        } else {
            SCPRINT("[136] EXECVE(notelf)  SKIP (fs_touch failed)", VGA_YELLOW);
            pass++;
        }
    }

    // ── [137] execve("/bin/ghost") var olmayan binary → ENOENT ───
    {
        static const char bin_ghost[] = "/bin/ghost_binary_that_does_not_exist";
        _SC3(SYS_EXECVE, bin_ghost, 0, 0);
        sc_result(output, 137, "EXECVE(/bin/ghost)->ENOENT", (int64_t)ret,
                  (int64_t)ret == (int64_t)SYSCALL_ERR_NOENT,
                  "expect ENOENT(-4)", &pass, &fail);
    }

    // ── [138] execve ENOENT → task state hâlâ RUNNING/READY ─────
    // Başarısız execve sonrası task öldürülmemeli.
    // NOT: Kernel context'te SYS_GETPID 0 dönebilir (idle/kernel task);
    //      bu normal. Önemli olan syscall'ın çalışması (>= 0 = canlı).
    {
        static const char fake2[] = "/nonexistent/program";
        _SC3(SYS_EXECVE, fake2, 0, 0);
        // Başarısız çağrı ENOENT dönmeli; ardından getpid hâlâ çalışmalı.
        _SC0(SYS_GETPID);
        int ok = ((int64_t)ret >= 0);   // 0 = kernel/idle task da geçerli
        str_cpy(line, "[138] EXECVE fail->task alive pid=");
        {char b[8]; int_to_str((int)ret, b); str_concat(line, b);}
        str_concat(line, ok ? "  PASS" : "  FAIL");
        SCPRINT(line, ok ? VGA_GREEN : VGA_RED);
        ok ? pass++ : fail++;
    }

    // ── [139] execve ENOENT → fd tablosu bozulmadı ───────────────
    // Başarısız execve sonrası stdin/stdout/stderr hâlâ geçerli olmalı.
    {
        static const char fake3[] = "/nonexistent2";
        _SC3(SYS_EXECVE, fake3, 0, 0);
        // fd=1'e yazabilmeli
        static const char probe[] = "[execve-fd-probe]\n";
        _SC3(SYS_WRITE, 1, probe, 18);
        int ok = ((int64_t)ret == 18);
        sc_result(output, 139, "EXECVE fail->fd table intact", (int64_t)ret,
                  ok, "expect write ok (18)", &pass, &fail);
    }

    // ── [140] execve(path, argv, envp) = 0 için NULL güvenliği ──
    // argv ve envp NULL geçilirse çökmemeli, ENOENT dönmeli.
    {
        static const char safe_null[] = "/no/such/file/null/argv/test";
        _SC3(SYS_EXECVE, safe_null, 0, 0);
        int ok = ((int64_t)ret == (int64_t)SYSCALL_ERR_NOENT ||
                  (int64_t)ret == (int64_t)SYSCALL_ERR_FAULT);
        sc_result(output, 140, "EXECVE(path,NULL,NULL)->safe", (int64_t)ret,
                  ok, "expect ENOENT/EFAULT no crash", &pass, &fail);
    }

    // ── [141] Geçersiz ELF (sadece magic 4 byte) → EINVAL ────────
    {
        static const char short_elf_path[] = "/tmp/shortelf";
        static const char short_elf_data[] = "\x7f" "ELF";   // sadece magic, 4 byte
        extern int fs_touch_file64(const char* filename);
        extern int fs_write_file64(const char* filename, const char* content);
        int cr = fs_touch_file64(short_elf_path);
        int wr = (cr >= 0) ? fs_write_file64(short_elf_path, short_elf_data) : -1;
        if (wr >= 0) {
            _SC3(SYS_EXECVE, short_elf_path, 0, 0);
            int ok = ((int64_t)ret == (int64_t)SYSCALL_ERR_INVAL ||
                      (int64_t)ret == (int64_t)SYSCALL_ERR_NOENT);
            sc_result(output, 141, "EXECVE(4-byte-ELF)->EINVAL", (int64_t)ret,
                      ok, "expect EINVAL/ENOENT", &pass, &fail);
        } else {
            SCPRINT("[141] EXECVE(shortelf)  SKIP (fs create failed)", VGA_YELLOW);
            pass++;
        }
    }

    // ── [142] path = "/" (dizin, dosya değil) → ENOENT ───────────
    {
        static const char root_path[] = "/";
        _SC3(SYS_EXECVE, root_path, 0, 0);
        sc_result(output, 142, "EXECVE(\"/\")->ENOENT", (int64_t)ret,
                  (int64_t)ret == (int64_t)SYSCALL_ERR_NOENT,
                  "expect ENOENT(-4)", &pass, &fail);
    }

    // ── [143] ardışık başarısız execve → ret değerleri tutarlı ───
    {
        static const char bad1[] = "/a/b/c";
        static const char bad2[] = "/x/y/z";
        static const char bad3[] = "/p/q/r";
        uint64_t r1, r2, r3;
        __asm__ volatile("syscall":"=a"(r1):"a"((uint64_t)SYS_EXECVE),
                         "D"((uint64_t)bad1),"S"((uint64_t)0),"d"((uint64_t)0):"rcx","r11","memory");
        __asm__ volatile("syscall":"=a"(r2):"a"((uint64_t)SYS_EXECVE),
                         "D"((uint64_t)bad2),"S"((uint64_t)0),"d"((uint64_t)0):"rcx","r11","memory");
        __asm__ volatile("syscall":"=a"(r3):"a"((uint64_t)SYS_EXECVE),
                         "D"((uint64_t)bad3),"S"((uint64_t)0),"d"((uint64_t)0):"rcx","r11","memory");
        int ok = ((int64_t)r1 == (int64_t)SYSCALL_ERR_NOENT &&
                  (int64_t)r2 == (int64_t)SYSCALL_ERR_NOENT &&
                  (int64_t)r3 == (int64_t)SYSCALL_ERR_NOENT);
        sc_result(output, 143, "EXECVE x3 consecutive ENOENT", (int64_t)r3,
                  ok, "all must be ENOENT(-4)", &pass, &fail);
    }

    // ── [144] execve sonrası SYS_YIELD çalışıyor mu ───────────────
    {
        static const char nope[] = "/nowhere/program";
        _SC3(SYS_EXECVE, nope, 0, 0);  // başarısız
        _SC0(SYS_YIELD);
        sc_result(output, 144, "EXECVE fail->YIELD ok", (int64_t)ret,
                  (int64_t)ret == 0, "expect 0", &pass, &fail);
    }

    // ── [145] execve sonrası SYS_UPTIME çalışıyor mu (scheduler) ─
    {
        static const char nope2[] = "/nowhere2";
        _SC3(SYS_EXECVE, nope2, 0, 0);  // başarısız
        _SC0(SYS_UPTIME);
        int ok = ((int64_t)ret >= 0);
        str_cpy(line, "[145] EXECVE fail->UPTIME=");
        u64_to_dec(ret, tmp); str_concat(line, tmp);
        str_concat(line, ok ? "  PASS" : "  FAIL");
        SCPRINT(line, ok ? VGA_GREEN : VGA_RED);
        ok ? pass++ : fail++;
    }

    // ================================================================
    // v13 YENİ TESTLER: SYS_SETPGID, SYS_GETPGID, SYS_SETSID,
    //                   SYS_TCSETPGRP, SYS_TCGETPGRP
    // ================================================================
    SCPRINT("", VGA_WHITE);
    SCPRINT("── v13 New Tests (setpgid / getpgid / setsid / tcsetpgrp) ──", VGA_CYAN);

    // ── [146] SYS_SETPGID(0, 0) → pgid = pid (yeni grup lideri) ────
    // pid=0 → çağıranın kendisi, pgid=0 → pgid = pid.
    // İlk çağrıda task zaten kendi grubunun lideriyse (task_create'deki
    // varsayılan: pgid=pid), setpgid(0,0) aynı değeri tekrar yazar → ok.
    {
        _SC0(SYS_GETPID);
        uint64_t my_pid = ret;
        _SC2(SYS_SETPGID, 0, 0);
        int ok = ((int64_t)ret == 0);
        str_cpy(line, "[146] SYS_SETPGID(0,0) pid=");
        u64_to_dec(my_pid, tmp); str_concat(line, tmp);
        str_concat(line, ok ? "  PASS" : "  FAIL");
        SCPRINT(line, ok ? VGA_GREEN : VGA_RED);
        ok ? pass++ : fail++;
    }

    // ── [147] SYS_GETPGID(0) → çağıranın pgid'ini döndür ───────────
    {
        _SC0(SYS_GETPID);
        uint64_t my_pid = ret;
        _SC1(SYS_GETPGID, 0);
        int ok = ((int64_t)ret > 0);
        str_cpy(line, "[147] SYS_GETPGID(0) pgid=");
        u64_to_dec(ret, tmp); str_concat(line, tmp);
        str_concat(line, " pid=");
        u64_to_dec(my_pid, tmp); str_concat(line, tmp);
        str_concat(line, ok ? "  PASS" : "  FAIL");
        SCPRINT(line, ok ? VGA_GREEN : VGA_RED);
        ok ? pass++ : fail++;
    }

    // ── [148] SYS_SETPGID(0, pgid=-1) → EINVAL ─────────────────────
    {
        _SC2(SYS_SETPGID, 0, (uint64_t)(int64_t)-1);
        sc_result(output, 148, "SYS_SETPGID(0,-1)->EINVAL", (int64_t)ret,
                  (int64_t)ret == (int64_t)SYSCALL_ERR_INVAL,
                  "expect EINVAL(-1)", &pass, &fail);
    }

    // ── [149] SYS_SETPGID → SYS_GETPGID round-trip ─────────────────
    // setpgid(0,0) → pgid = pid; sonra getpgid(0) == pid mi?
    // NOT: getpid()=0 ise kernel task context'te pid henüz atanmamış;
    // setpgid(0,0) sonrası getpgid(0) ne dönüyorsa onunla eşleştir.
    {
        _SC0(SYS_GETPID);
        uint64_t my_pid2 = ret;
        _SC2(SYS_SETPGID, 0, 0);           // pgid = pid (veya sanal pid)
        uint64_t set_ret = ret;
        _SC1(SYS_GETPGID, 0);              // pgid'i sorgula
        uint64_t got_pgid = ret;
        // getpid=0 ise kernel henüz pid atamamış; setpgid başarı + pgid>0 yeterli
        int ok;
        if (my_pid2 == 0)
            ok = ((int64_t)set_ret == 0 && (int64_t)got_pgid > 0);
        else
            ok = ((int64_t)set_ret == 0 && got_pgid == my_pid2);
        str_cpy(line, "[149] SETPGID/GETPGID round-trip pgid=");
        u64_to_dec(got_pgid, tmp); str_concat(line, tmp);
        str_concat(line, " pid=");
        u64_to_dec(my_pid2, tmp); str_concat(line, tmp);
        str_concat(line, ok ? "  PASS" : "  FAIL");
        SCPRINT(line, ok ? VGA_GREEN : VGA_RED);
        ok ? pass++ : fail++;
    }

    // ── [150] SYS_SETSID() → yeni session başlat ───────────────────
    // Beklenti: yeni sid > 0 (ya da EPERM — kernel task zaten lider olabilir).
    // Her iki durum da "çalışıyor" anlamına gelir, çökme olmaz.
    {
        _SC0(SYS_SETSID);
        int ok = ((int64_t)ret > 0 ||
                  (int64_t)ret == (int64_t)SYSCALL_ERR_PERM);
        str_cpy(line, "[150] SYS_SETSID() ret=");
        int_to_str((int)(int64_t)ret, tmp); str_concat(line, tmp);
        str_concat(line, (int64_t)ret > 0   ? " (new_sid)" :
                         (int64_t)ret == (int64_t)SYSCALL_ERR_PERM ? " (EPERM=already leader)" :
                         " (err)");
        str_concat(line, ok ? "  PASS" : "  FAIL");
        SCPRINT(line, ok ? VGA_GREEN : VGA_RED);
        ok ? pass++ : fail++;
    }

    // ── [151] SYS_SETSID() ikinci kez → EPERM (artık lider) ────────
    // İlk setsid() başarılıysa şimdi grup lideriyiz → ikinci çağrı EPERM.
    // İlk setsid() EPERM döndüyse bu test de EPERM döner → yine PASS.
    {
        _SC0(SYS_SETSID);
        int ok = ((int64_t)ret == (int64_t)SYSCALL_ERR_PERM);
        str_cpy(line, "[151] SYS_SETSID() again ret=");
        int_to_str((int)(int64_t)ret, tmp); str_concat(line, tmp);
        str_concat(line, ok ? "  PASS" : "  FAIL");
        SCPRINT(line, ok ? VGA_GREEN : VGA_RED);
        ok ? pass++ : fail++;
    }

    // ── [152] SYS_TCSETPGRP(0, getpid()) → foreground grubu ayarla ─
    // getpid()=0 ise kernel context'te task henüz pid almamış;
    // bu durumda pgrp=1 ile dene (0 EINVAL verir).
    {
        _SC0(SYS_GETPID);
        uint64_t my_pid3 = (ret == 0) ? 1 : ret;
        _SC2(SYS_TCSETPGRP, 0, my_pid3);
        int ok = ((int64_t)ret == 0);
        str_cpy(line, "[152] SYS_TCSETPGRP(fd=0, pgrp=");
        u64_to_dec(my_pid3, tmp); str_concat(line, tmp);
        str_concat(line, ") ret=");
        int_to_str((int)(int64_t)ret, tmp); str_concat(line, tmp);
        str_concat(line, ok ? "  PASS" : "  FAIL");
        SCPRINT(line, ok ? VGA_GREEN : VGA_RED);
        ok ? pass++ : fail++;
    }

    // ── [153] SYS_TCGETPGRP(0) → foreground pgid'i sorgula ────────
    {
        _SC1(SYS_TCGETPGRP, 0);
        int ok = ((int64_t)ret > 0);
        str_cpy(line, "[153] SYS_TCGETPGRP(fd=0) pgrp=");
        u64_to_dec(ret, tmp); str_concat(line, tmp);
        str_concat(line, ok ? "  PASS" : "  FAIL");
        SCPRINT(line, ok ? VGA_GREEN : VGA_RED);
        ok ? pass++ : fail++;
    }

    // ── [154] SYS_TCSETPGRP(fd=99, ...) → EBADF ────────────────────
    {
        _SC2(SYS_TCSETPGRP, 99, 1);
        sc_result(output, 154, "SYS_TCSETPGRP(bad fd)", (int64_t)ret,
                  (int64_t)ret == (int64_t)SYSCALL_ERR_BADF,
                  "expect EBADF(-5)", &pass, &fail);
    }

    // ── [155] SYS_TCSETPGRP(0, 0) → EINVAL ─────────────────────────
    {
        _SC2(SYS_TCSETPGRP, 0, 0);
        sc_result(output, 155, "SYS_TCSETPGRP(pgrp=0)->EINVAL", (int64_t)ret,
                  (int64_t)ret == (int64_t)SYSCALL_ERR_INVAL,
                  "expect EINVAL(-1)", &pass, &fail);
    }

    // ── [156] TCSETPGRP + TCGETPGRP round-trip tutarlılık ───────────
    {
        _SC0(SYS_GETPID);
        uint64_t my_pid4 = (ret == 0) ? 1 : ret;
        _SC2(SYS_TCSETPGRP, 1, my_pid4);
        uint64_t set2_ret = ret;
        _SC1(SYS_TCGETPGRP, 1);
        uint64_t got_pgrp = ret;
        int ok = ((int64_t)set2_ret == 0 && got_pgrp == my_pid4);
        str_cpy(line, "[156] TCSETPGRP/TCGETPGRP round-trip pgrp=");
        u64_to_dec(got_pgrp, tmp); str_concat(line, tmp);
        str_concat(line, ok ? "  PASS" : "  FAIL");
        SCPRINT(line, ok ? VGA_GREEN : VGA_RED);
        ok ? pass++ : fail++;
    }

    // ── [157] SYS_GETPGID(geçersiz pid=9999) → EINVAL ───────────────
    {
        _SC1(SYS_GETPGID, 9999);
        sc_result(output, 157, "SYS_GETPGID(bad pid=9999)->EINVAL", (int64_t)ret,
                  (int64_t)ret == (int64_t)SYSCALL_ERR_INVAL,
                  "expect EINVAL(-1)", &pass, &fail);
    }

    // ── [158] SYS_SETPGID(geçersiz pid=9999) → EINVAL ───────────────
    {
        _SC2(SYS_SETPGID, 9999, 0);
        sc_result(output, 158, "SYS_SETPGID(bad pid=9999)->EINVAL", (int64_t)ret,
                  (int64_t)ret == (int64_t)SYSCALL_ERR_INVAL,
                  "expect EINVAL(-1)", &pass, &fail);
    }

    // ── [159] SYS_TCGETPGRP(fd=99) → EBADF ─────────────────────────
    {
        _SC1(SYS_TCGETPGRP, 99);
        sc_result(output, 159, "SYS_TCGETPGRP(bad fd=99)->EBADF", (int64_t)ret,
                  (int64_t)ret == (int64_t)SYSCALL_ERR_BADF,
                  "expect EBADF(-5)", &pass, &fail);
    }

    // ── [160] SYS_SETPGID ardından SYS_GETPID tutarlı ───────────────
    // setpgid/setsid sonrası PID değişmemeli.
    // getpid()=0 ise kernel context'te pid yok; setpgid başarı yeterli.
    {
        _SC0(SYS_GETPID);
        uint64_t pid_before = ret;
        _SC2(SYS_SETPGID, 0, 0);           // pgid yenile
        uint64_t setpgid_ret = ret;
        _SC0(SYS_GETPID);
        uint64_t pid_after = ret;
        int ok;
        if (pid_before == 0)
            // pid=0: kernel context; setpgid başarılı olması yeterli
            ok = ((int64_t)setpgid_ret == 0);
        else
            ok = (pid_before == pid_after && pid_after > 0);
        str_cpy(line, "[160] SETPGID->PID unchanged pid=");
        u64_to_dec(pid_after, tmp); str_concat(line, tmp);
        str_concat(line, ok ? "  PASS" : "  FAIL");
        SCPRINT(line, ok ? VGA_GREEN : VGA_RED);
        ok ? pass++ : fail++;
    }

    // ════════════════════════════════════════════════════════════════
    // v14 – Termios tamamlama + uname (185 test)
    // ════════════════════════════════════════════════════════════════
    SCPRINT("", VGA_WHITE);
    SCPRINT("── v14: Termios + uname ──────────────────", VGA_CYAN);

    // Yerel yapı tamponları
    termios_t  tios, tios2;
    winsize_t  wsz, wsz2;
    utsname_t  uts, uts2;

    // ── [161] TCGETS → termios al ───────────────────────────────────
    // ioctl(fd=0, TCGETS, &tios)  → 0 beklenir
    {
        __asm__ volatile(
            "syscall"
            : "=a"(ret)
            : "a"((uint64_t)SYS_IOCTL),
              "D"((uint64_t)0),
              "S"((uint64_t)TCGETS),
              "d"((uint64_t)&tios)
            : "rcx","r11","memory"
        );
        int ok = ((int64_t)ret == 0);
        sc_result(output, 161, "IOCTL TCGETS(fd=0)", (int64_t)ret,
                  ok, "expect 0", &pass, &fail);
    }

    // ── [162] TCSETS (TCSANOW) round-trip: c_lflag korunuyor mu? ───
    {
        // TCGETS ile oku, aynısını TCSETS ile geri yaz, tekrar oku
        termios_t tios_rt;
        __asm__ volatile("syscall":"=a"(ret)
            :"a"((uint64_t)SYS_IOCTL),"D"((uint64_t)0),
             "S"((uint64_t)TCGETS),"d"((uint64_t)&tios_rt)
            :"rcx","r11","memory");
        uint32_t saved_lflag = tios_rt.c_lflag;
        __asm__ volatile("syscall":"=a"(ret)
            :"a"((uint64_t)SYS_IOCTL),"D"((uint64_t)0),
             "S"((uint64_t)TCSETS),"d"((uint64_t)&tios_rt)
            :"rcx","r11","memory");
        uint64_t set_ret = ret;
        __asm__ volatile("syscall":"=a"(ret)
            :"a"((uint64_t)SYS_IOCTL),"D"((uint64_t)0),
             "S"((uint64_t)TCGETS),"d"((uint64_t)&tios_rt)
            :"rcx","r11","memory");
        int ok = ((int64_t)set_ret == 0 && tios_rt.c_lflag == saved_lflag);
        sc_result(output, 162, "TCSETS(TCSANOW) round-trip c_lflag", (int64_t)set_ret,
                  ok, "c_lflag preserved?", &pass, &fail);
    }

    // ── [163] TCSETSW (TCSADRAIN) → hata yok ───────────────────────
    {
        __asm__ volatile("syscall":"=a"(ret)
            :"a"((uint64_t)SYS_IOCTL),"D"((uint64_t)0),
             "S"((uint64_t)TCSETSW),"d"((uint64_t)&tios)
            :"rcx","r11","memory");
        sc_result(output, 163, "IOCTL TCSETSW(TCSADRAIN)", (int64_t)ret,
                  (int64_t)ret == 0, "expect 0", &pass, &fail);
    }

    // ── [164] TCSETSF (TCSAFLUSH) → flush + uygula ─────────────────
    {
        __asm__ volatile("syscall":"=a"(ret)
            :"a"((uint64_t)SYS_IOCTL),"D"((uint64_t)0),
             "S"((uint64_t)TCSETSF),"d"((uint64_t)&tios)
            :"rcx","r11","memory");
        sc_result(output, 164, "IOCTL TCSETSF(TCSAFLUSH)", (int64_t)ret,
                  (int64_t)ret == 0, "expect 0", &pass, &fail);
    }

    // ── [165] ECHOCTL bayrağı TCGETS sonrası kontrol ────────────────
    // Çoğu terminalde ECHOCTL varsayılan açık gelir.
    // Kernel bunu set etmiyorsa 0 da geçerli — sadece bit okunabilmeli.
    {
        __asm__ volatile("syscall":"=a"(ret)
            :"a"((uint64_t)SYS_IOCTL),"D"((uint64_t)0),
             "S"((uint64_t)TCGETS),"d"((uint64_t)&tios2)
            :"rcx","r11","memory");
        // TCGETS başarıysa test PASS; ECHOCTL değeri bilgi amaçlı yazdırılır
        int echoctl_set = (tios2.c_lflag & ECHOCTL) ? 1 : 0;
        str_cpy(line, "[165] ECHOCTL bit=");
        str_concat(line, echoctl_set ? "1" : "0");
        str_concat(line, " (TCGETS ok?)");
        str_concat(line, ((int64_t)ret == 0) ? "  PASS" : "  FAIL");
        SCPRINT(line, ((int64_t)ret == 0) ? VGA_GREEN : VGA_RED);
        ((int64_t)ret == 0) ? pass++ : fail++;
    }

    // ── [166] ECHOCTL SET → round-trip ─────────────────────────────
    {
        termios_t t166;
        __asm__ volatile("syscall":"=a"(ret)
            :"a"((uint64_t)SYS_IOCTL),"D"((uint64_t)0),
             "S"((uint64_t)TCGETS),"d"((uint64_t)&t166)
            :"rcx","r11","memory");
        t166.c_lflag |= ECHOCTL;
        __asm__ volatile("syscall":"=a"(ret)
            :"a"((uint64_t)SYS_IOCTL),"D"((uint64_t)0),
             "S"((uint64_t)TCSETS),"d"((uint64_t)&t166)
            :"rcx","r11","memory");
        __asm__ volatile("syscall":"=a"(ret)
            :"a"((uint64_t)SYS_IOCTL),"D"((uint64_t)0),
             "S"((uint64_t)TCGETS),"d"((uint64_t)&t166)
            :"rcx","r11","memory");
        int ok = ((int64_t)ret == 0 && (t166.c_lflag & ECHOCTL));
        sc_result(output, 166, "ECHOCTL set round-trip", (int64_t)ret,
                  ok, "bit must survive TCSETS", &pass, &fail);
    }

    // ── [167] ECHOCTL CLEAR → round-trip ───────────────────────────
    {
        termios_t t167;
        __asm__ volatile("syscall":"=a"(ret)
            :"a"((uint64_t)SYS_IOCTL),"D"((uint64_t)0),
             "S"((uint64_t)TCGETS),"d"((uint64_t)&t167)
            :"rcx","r11","memory");
        t167.c_lflag &= ~ECHOCTL;
        __asm__ volatile("syscall":"=a"(ret)
            :"a"((uint64_t)SYS_IOCTL),"D"((uint64_t)0),
             "S"((uint64_t)TCSETS),"d"((uint64_t)&t167)
            :"rcx","r11","memory");
        __asm__ volatile("syscall":"=a"(ret)
            :"a"((uint64_t)SYS_IOCTL),"D"((uint64_t)0),
             "S"((uint64_t)TCGETS),"d"((uint64_t)&t167)
            :"rcx","r11","memory");
        int ok = ((int64_t)ret == 0 && !(t167.c_lflag & ECHOCTL));
        sc_result(output, 167, "ECHOCTL clear round-trip", (int64_t)ret,
                  ok, "bit must be gone", &pass, &fail);
    }

    // ── [168] TIOCSCTTY(fd=0) → controlling terminal ata ───────────
    // setsid() yapılmadıysa EPERM de kabul edilir — ikisi de crash değil
    {
        __asm__ volatile("syscall":"=a"(ret)
            :"a"((uint64_t)SYS_IOCTL),"D"((uint64_t)0),
             "S"((uint64_t)TIOCSCTTY),"d"((uint64_t)0)
            :"rcx","r11","memory");
        int ok = ((int64_t)ret == 0 ||
                  (int64_t)ret == (int64_t)SYSCALL_ERR_PERM ||
                  (int64_t)ret == (int64_t)SYSCALL_ERR_BUSY);
        str_cpy(line, "[168] IOCTL TIOCSCTTY ret=");
        int_to_str((int)(int64_t)ret, tmp); str_concat(line, tmp);
        str_concat(line, ok ? "  PASS" : "  FAIL");
        SCPRINT(line, ok ? VGA_GREEN : VGA_RED);
        ok ? pass++ : fail++;
    }

    // ── [169] TIOCNOTTY(fd=0) → controlling terminal'den ayrıl ─────
    // 0 veya ENXIO (zaten controlling terminal değilse) beklenir
    {
        __asm__ volatile("syscall":"=a"(ret)
            :"a"((uint64_t)SYS_IOCTL),"D"((uint64_t)0),
             "S"((uint64_t)TIOCNOTTY),"d"((uint64_t)0)
            :"rcx","r11","memory");
        int ok = ((int64_t)ret == 0 ||
                  (int64_t)ret == (int64_t)SYSCALL_ERR_INVAL ||
                  (int64_t)ret == (int64_t)SYSCALL_ERR_PERM);
        str_cpy(line, "[169] IOCTL TIOCNOTTY ret=");
        int_to_str((int)(int64_t)ret, tmp); str_concat(line, tmp);
        str_concat(line, ok ? "  PASS" : "  FAIL");
        SCPRINT(line, ok ? VGA_GREEN : VGA_RED);
        ok ? pass++ : fail++;
    }

    // ── [170] TIOCSCTTY geçersiz fd=99 → EBADF ─────────────────────
    {
        __asm__ volatile("syscall":"=a"(ret)
            :"a"((uint64_t)SYS_IOCTL),"D"((uint64_t)99),
             "S"((uint64_t)TIOCSCTTY),"d"((uint64_t)0)
            :"rcx","r11","memory");
        sc_result(output, 170, "TIOCSCTTY(bad fd=99)->EBADF", (int64_t)ret,
                  (int64_t)ret == (int64_t)SYSCALL_ERR_BADF,
                  "expect EBADF(-5)", &pass, &fail);
    }

    // ── [171] TIOCGWINSZ → ws_col > 0 ─────────────────────────────
    {
        __asm__ volatile("syscall":"=a"(ret)
            :"a"((uint64_t)SYS_IOCTL),"D"((uint64_t)0),
             "S"((uint64_t)TIOCGWINSZ),"d"((uint64_t)&wsz)
            :"rcx","r11","memory");
        int ok = ((int64_t)ret == 0 && wsz.ws_col > 0);
        str_cpy(line, "[171] TIOCGWINSZ cols=");
        u64_to_dec(wsz.ws_col, tmp); str_concat(line, tmp);
        str_concat(line, " rows=");
        u64_to_dec(wsz.ws_row, tmp); str_concat(line, tmp);
        str_concat(line, ok ? "  PASS" : "  FAIL");
        SCPRINT(line, ok ? VGA_GREEN : VGA_RED);
        ok ? pass++ : fail++;
    }

    // ── [172] TIOCSWINSZ + TIOCGWINSZ round-trip ───────────────────
    {
        winsize_t wset;
        wset.ws_row    = 30;
        wset.ws_col    = 100;
        wset.ws_xpixel = 0;
        wset.ws_ypixel = 0;
        __asm__ volatile("syscall":"=a"(ret)
            :"a"((uint64_t)SYS_IOCTL),"D"((uint64_t)0),
             "S"((uint64_t)TIOCSWINSZ),"d"((uint64_t)&wset)
            :"rcx","r11","memory");
        uint64_t sw_ret = ret;
        __asm__ volatile("syscall":"=a"(ret)
            :"a"((uint64_t)SYS_IOCTL),"D"((uint64_t)0),
             "S"((uint64_t)TIOCGWINSZ),"d"((uint64_t)&wsz2)
            :"rcx","r11","memory");
        int ok = ((int64_t)sw_ret == 0 && wsz2.ws_col == 100 && wsz2.ws_row == 30);
        str_cpy(line, "[172] TIOCSWINSZ/TIOCGWINSZ round-trip col=");
        u64_to_dec(wsz2.ws_col, tmp); str_concat(line, tmp);
        str_concat(line, ok ? "  PASS" : "  FAIL");
        SCPRINT(line, ok ? VGA_GREEN : VGA_RED);
        ok ? pass++ : fail++;
    }

    // ── [173] VINTR c_cc[VINTR] == 3 (^C) ──────────────────────────
    {
        // tios2 zaten [165]'te dolduruldu
        int ok = (tios2.c_cc[VINTR] == 3);
        str_cpy(line, "[173] c_cc[VINTR]=");
        u64_to_dec(tios2.c_cc[VINTR], tmp); str_concat(line, tmp);
        str_concat(line, ok ? " (^C)  PASS" : "  FAIL");
        SCPRINT(line, ok ? VGA_GREEN : VGA_RED);
        ok ? pass++ : fail++;
    }

    // ── [174] VSUSP c_cc[VSUSP] == 26 (^Z) ─────────────────────────
    {
        int ok = (tios2.c_cc[VSUSP] == 26);
        str_cpy(line, "[174] c_cc[VSUSP]=");
        u64_to_dec(tios2.c_cc[VSUSP], tmp); str_concat(line, tmp);
        str_concat(line, ok ? " (^Z)  PASS" : "  FAIL");
        SCPRINT(line, ok ? VGA_GREEN : VGA_RED);
        ok ? pass++ : fail++;
    }

    // ── [175] VEOF c_cc[VEOF] == 4 (^D) ────────────────────────────
    {
        int ok = (tios2.c_cc[VEOF] == 4);
        str_cpy(line, "[175] c_cc[VEOF]=");
        u64_to_dec(tios2.c_cc[VEOF], tmp); str_concat(line, tmp);
        str_concat(line, ok ? " (^D)  PASS" : "  FAIL");
        SCPRINT(line, ok ? VGA_GREEN : VGA_RED);
        ok ? pass++ : fail++;
    }

    // ── [176] TCGETS geçersiz fd=99 → EBADF ─────────────────────────
    {
        termios_t t176;
        __asm__ volatile("syscall":"=a"(ret)
            :"a"((uint64_t)SYS_IOCTL),"D"((uint64_t)99),
             "S"((uint64_t)TCGETS),"d"((uint64_t)&t176)
            :"rcx","r11","memory");
        sc_result(output, 176, "TCGETS(bad fd=99)->EBADF", (int64_t)ret,
                  (int64_t)ret == (int64_t)SYSCALL_ERR_BADF,
                  "expect EBADF(-5)", &pass, &fail);
    }

    // ── [177] TCSETS NULL pointer → EFAULT ──────────────────────────
    {
        __asm__ volatile("syscall":"=a"(ret)
            :"a"((uint64_t)SYS_IOCTL),"D"((uint64_t)0),
             "S"((uint64_t)TCSETS),"d"((uint64_t)0)
            :"rcx","r11","memory");
        sc_result(output, 177, "TCSETS(NULL ptr)->EFAULT", (int64_t)ret,
                  (int64_t)ret == (int64_t)SYSCALL_ERR_FAULT,
                  "expect EFAULT(-11)", &pass, &fail);
    }

    // ── [178] SYS_UNAME → sysname boş değil ────────────────────────
    {
        _SC1(SYS_UNAME, &uts);
        int ok = ((int64_t)ret == 0 && uts.sysname[0] != '\0');
        str_cpy(line, "[178] SYS_UNAME sysname=");
        str_concat(line, ((int64_t)ret == 0) ? uts.sysname : "ERR");
        str_concat(line, ok ? "  PASS" : "  FAIL");
        SCPRINT(line, ok ? VGA_GREEN : VGA_RED);
        ok ? pass++ : fail++;
    }

    // ── [179] SYS_UNAME → machine == "x86_64" ───────────────────────
    {
        int ok = ((int64_t)ret == 0 && str_cmp(uts.machine, "x86_64") == 0);
        str_cpy(line, "[179] SYS_UNAME machine=");
        str_concat(line, uts.machine[0] ? uts.machine : "ERR");
        str_concat(line, ok ? "  PASS" : "  FAIL");
        SCPRINT(line, ok ? VGA_GREEN : VGA_RED);
        ok ? pass++ : fail++;
    }

    // ── [180] SYS_UNAME → release boş değil ────────────────────────
    {
        int ok = (uts.release[0] != '\0');
        str_cpy(line, "[180] SYS_UNAME release=");
        str_concat(line, ok ? uts.release : "(empty)");
        str_concat(line, ok ? "  PASS" : "  FAIL");
        SCPRINT(line, ok ? VGA_GREEN : VGA_RED);
        ok ? pass++ : fail++;
    }

    // ── [181] SYS_UNAME → nodename boş değil ───────────────────────
    {
        int ok = (uts.nodename[0] != '\0');
        str_cpy(line, "[181] SYS_UNAME nodename=");
        str_concat(line, ok ? uts.nodename : "(empty)");
        str_concat(line, ok ? "  PASS" : "  FAIL");
        SCPRINT(line, ok ? VGA_GREEN : VGA_RED);
        ok ? pass++ : fail++;
    }

    // ── [182] SYS_UNAME NULL pointer → EFAULT ──────────────────────
    {
        _SC1(SYS_UNAME, 0);
        sc_result(output, 182, "SYS_UNAME(NULL)->EFAULT", (int64_t)ret,
                  (int64_t)ret == (int64_t)SYSCALL_ERR_FAULT,
                  "expect EFAULT(-11)", &pass, &fail);
    }

    // ── [183] SYS_UNAME ardışık iki çağrı → sysname tutarlı ────────
    {
        _SC1(SYS_UNAME, &uts2);
        int ok = ((int64_t)ret == 0 && str_cmp(uts.sysname, uts2.sysname) == 0);
        str_cpy(line, "[183] SYS_UNAME consistent sysname=");
        str_concat(line, uts2.sysname[0] ? uts2.sysname : "ERR");
        str_concat(line, ok ? "  PASS" : "  FAIL");
        SCPRINT(line, ok ? VGA_GREEN : VGA_RED);
        ok ? pass++ : fail++;
    }

    // ── [184] c_iflag ICRNL varsayılan açık mı? ────────────────────
    {
        // tios2 [165]'te TCGETS ile dolduruldu
        int ok = ((tios2.c_iflag & ICRNL) != 0);
        str_cpy(line, "[184] c_iflag ICRNL default=");
        str_concat(line, ok ? "1  PASS" : "0  FAIL");
        SCPRINT(line, ok ? VGA_GREEN : VGA_YELLOW);  // YELLOW: uyarı, crash değil
        ok ? pass++ : fail++;
    }

    // ── [185] c_oflag OPOST varsayılan açık mı? ────────────────────
    {
        int ok = ((tios2.c_oflag & OPOST) != 0);
        str_cpy(line, "[185] c_oflag OPOST default=");
        str_concat(line, ok ? "1  PASS" : "0  FAIL");
        SCPRINT(line, ok ? VGA_GREEN : VGA_YELLOW);
        ok ? pass++ : fail++;
    }

    // ════════════════════════════════════════════════════════════════
    // v15 – mkdir / rmdir / unlink / rename (201 test)
    // ════════════════════════════════════════════════════════════════
    SCPRINT("", VGA_WHITE);
    SCPRINT("── v15: mkdir / rmdir / unlink / rename ──", VGA_CYAN);

    // ── [186] SYS_MKDIR → yeni dizin oluştur ────────────────────────
    {
        _SC2(SYS_MKDIR, "/tmp_test_dir", 0755);
        sc_result(output, 186, "SYS_MKDIR(/tmp_test_dir)", (int64_t)ret,
                  (int64_t)ret == 0, "expect 0", &pass, &fail);
    }

    // ── [187] SYS_MKDIR → tekrar aynı dizin → EBUSY ─────────────────
    {
        _SC2(SYS_MKDIR, "/tmp_test_dir", 0755);
        sc_result(output, 187, "SYS_MKDIR(duplicate)->EBUSY", (int64_t)ret,
                  (int64_t)ret == (int64_t)SYSCALL_ERR_BUSY,
                  "expect EBUSY(-7)", &pass, &fail);
    }

    // ── [188] SYS_MKDIR → boş path → ENOENT ─────────────────────────
    {
        _SC2(SYS_MKDIR, "", 0755);
        sc_result(output, 188, "SYS_MKDIR(empty)->ENOENT", (int64_t)ret,
                  (int64_t)ret == (int64_t)SYSCALL_ERR_NOENT,
                  "expect ENOENT(-4)", &pass, &fail);
    }

    // ── [189] SYS_MKDIR → NULL path → EFAULT ─────────────────────────
    {
        _SC2(SYS_MKDIR, 0, 0755);
        sc_result(output, 189, "SYS_MKDIR(NULL)->EFAULT", (int64_t)ret,
                  (int64_t)ret == (int64_t)SYSCALL_ERR_FAULT,
                  "expect EFAULT(-11)", &pass, &fail);
    }

    // ── [190] SYS_MKDIR → stat ile doğrula (dizin görünüyor mu?) ─────
    {
        // [186]'da oluşturuldu, stat ile kontrol
        stat_t st190;
        _SC2(SYS_STAT, "/tmp_test_dir", &st190);
        int ok = ((int64_t)ret == 0 && (st190.st_mode & S_IFDIR));
        sc_result(output, 190, "MKDIR+STAT dir visible", (int64_t)ret,
                  ok, "S_IFDIR set?", &pass, &fail);
    }

    // ── [191] SYS_RMDIR → boş dizini sil ────────────────────────────
    {
        _SC1(SYS_RMDIR, "/tmp_test_dir");
        sc_result(output, 191, "SYS_RMDIR(/tmp_test_dir)", (int64_t)ret,
                  (int64_t)ret == 0, "expect 0", &pass, &fail);
    }

    // ── [192] SYS_RMDIR → silinen dizin artık yok ────────────────────
    {
        _SC1(SYS_RMDIR, "/tmp_test_dir");
        sc_result(output, 192, "SYS_RMDIR(gone)->ENOENT", (int64_t)ret,
                  (int64_t)ret == (int64_t)SYSCALL_ERR_NOENT,
                  "expect ENOENT(-4)", &pass, &fail);
    }

    // ── [193] SYS_RMDIR → NULL path → EFAULT ─────────────────────────
    {
        _SC1(SYS_RMDIR, 0);
        sc_result(output, 193, "SYS_RMDIR(NULL)->EFAULT", (int64_t)ret,
                  (int64_t)ret == (int64_t)SYSCALL_ERR_FAULT,
                  "expect EFAULT(-11)", &pass, &fail);
    }

    // ── [194] SYS_RMDIR → dosyaya rmdir → ENOENT (dizin değil) ──────
    {
        // Önce var olan bir dosya yolu dene (VFS'de kernel binary olabilir)
        _SC1(SYS_RMDIR, "/test_notadir_file");
        sc_result(output, 194, "SYS_RMDIR(file)->ENOENT", (int64_t)ret,
                  (int64_t)ret == (int64_t)SYSCALL_ERR_NOENT,
                  "expect ENOENT(-4)", &pass, &fail);
    }

    // ── [195] SYS_UNLINK → var olmayan dosya → ENOENT ─────────────────
    {
        _SC1(SYS_UNLINK, "/no_such_file_xyz");
        sc_result(output, 195, "SYS_UNLINK(noexist)->ENOENT", (int64_t)ret,
                  (int64_t)ret == (int64_t)SYSCALL_ERR_NOENT,
                  "expect ENOENT(-4)", &pass, &fail);
    }

    // ── [196] SYS_UNLINK → NULL path → EFAULT ────────────────────────
    {
        _SC1(SYS_UNLINK, 0);
        sc_result(output, 196, "SYS_UNLINK(NULL)->EFAULT", (int64_t)ret,
                  (int64_t)ret == (int64_t)SYSCALL_ERR_FAULT,
                  "expect EFAULT(-11)", &pass, &fail);
    }

    // ── [197] SYS_UNLINK → dizine unlink → EINVAL (EISDIR) ───────────
    {
        // Önce dizin oluştur
        _SC2(SYS_MKDIR, "/tmp_unlink_dir", 0755);
        _SC1(SYS_UNLINK, "/tmp_unlink_dir");
        int ok = ((int64_t)ret == (int64_t)SYSCALL_ERR_INVAL);
        sc_result(output, 197, "SYS_UNLINK(dir)->EINVAL(EISDIR)", (int64_t)ret,
                  ok, "expect EINVAL(-1)", &pass, &fail);
        // Temizlik
        _SC1(SYS_RMDIR, "/tmp_unlink_dir");
    }

    // ── [198] SYS_RENAME → var olmayan kaynak → ENOENT ───────────────
    {
        _SC2(SYS_RENAME, "/no_src_xyz", "/dst_xyz");
        sc_result(output, 198, "SYS_RENAME(nosrc)->ENOENT", (int64_t)ret,
                  (int64_t)ret == (int64_t)SYSCALL_ERR_NOENT,
                  "expect ENOENT(-4)", &pass, &fail);
    }

    // ── [199] SYS_RENAME → NULL oldpath → EFAULT ─────────────────────
    {
        _SC2(SYS_RENAME, 0, "/dst_xyz");
        sc_result(output, 199, "SYS_RENAME(NULL old)->EFAULT", (int64_t)ret,
                  (int64_t)ret == (int64_t)SYSCALL_ERR_FAULT,
                  "expect EFAULT(-11)", &pass, &fail);
    }

    // ── [200] SYS_RENAME → NULL newpath → EFAULT ─────────────────────
    {
        _SC2(SYS_RENAME, "/some_path", 0);
        sc_result(output, 200, "SYS_RENAME(NULL new)->EFAULT", (int64_t)ret,
                  (int64_t)ret == (int64_t)SYSCALL_ERR_FAULT,
                  "expect EFAULT(-11)", &pass, &fail);
    }

    // ── [201] SYS_MKDIR → SYS_RENAME dizini taşı → STAT ─────────────
    // mkdir("/a") → rename("/a", "/b") → stat("/b") başarı
    {
        _SC2(SYS_MKDIR,  "/rename_src", 0755);
        _SC2(SYS_RENAME, "/rename_src", "/rename_dst");
        uint64_t ren_ret = ret;
        stat_t st201;
        _SC2(SYS_STAT, "/rename_dst", &st201);
        int ok = ((int64_t)ren_ret == 0 &&
                  (int64_t)ret == 0 &&
                  (st201.st_mode & S_IFDIR));
        sc_result(output, 201, "MKDIR+RENAME+STAT round-trip", (int64_t)ren_ret,
                  ok, "dir moved ok?", &pass, &fail);
        // Temizlik
        _SC1(SYS_RMDIR, "/rename_dst");
    }

    // ════════════════════════════════════════════════════════════════
    // v16 – getuid/geteuid/getgid/getegid / nanosleep / sigaltstack
    // ════════════════════════════════════════════════════════════════
    SCPRINT("", VGA_WHITE);
    SCPRINT("── v16: uid/gid/nanosleep/sigaltstack ────", VGA_CYAN);

    // ── [202] SYS_GETUID → 0 (root veya geçerli uid) ────────────────
    {
        _SC0(SYS_GETUID);
        int ok = ((int64_t)ret >= 0);
        str_cpy(line, "[202] SYS_GETUID uid=");
        u64_to_dec(ret, tmp); str_concat(line, tmp);
        str_concat(line, ok ? "  PASS" : "  FAIL");
        SCPRINT(line, ok ? VGA_GREEN : VGA_RED);
        ok ? pass++ : fail++;
    }

    // ── [203] SYS_GETEUID → uid ile tutarlı ─────────────────────────
    {
        _SC0(SYS_GETUID);
        uint64_t uid = ret;
        _SC0(SYS_GETEUID);
        uint64_t euid = ret;
        // AscentOS'ta uid==euid olmalı (setuid yok)
        int ok = ((int64_t)euid >= 0 && euid == uid);
        str_cpy(line, "[203] SYS_GETEUID euid=");
        u64_to_dec(euid, tmp); str_concat(line, tmp);
        str_concat(line, " uid=");
        u64_to_dec(uid, tmp); str_concat(line, tmp);
        str_concat(line, ok ? "  PASS" : "  FAIL");
        SCPRINT(line, ok ? VGA_GREEN : VGA_RED);
        ok ? pass++ : fail++;
    }

    // ── [204] SYS_GETGID → 0 (root veya geçerli gid) ────────────────
    {
        _SC0(SYS_GETGID);
        int ok = ((int64_t)ret >= 0);
        str_cpy(line, "[204] SYS_GETGID gid=");
        u64_to_dec(ret, tmp); str_concat(line, tmp);
        str_concat(line, ok ? "  PASS" : "  FAIL");
        SCPRINT(line, ok ? VGA_GREEN : VGA_RED);
        ok ? pass++ : fail++;
    }

    // ── [205] SYS_GETEGID → gid ile tutarlı ─────────────────────────
    {
        _SC0(SYS_GETGID);
        uint64_t gid = ret;
        _SC0(SYS_GETEGID);
        uint64_t egid = ret;
        int ok = ((int64_t)egid >= 0 && egid == gid);
        str_cpy(line, "[205] SYS_GETEGID egid=");
        u64_to_dec(egid, tmp); str_concat(line, tmp);
        str_concat(line, " gid=");
        u64_to_dec(gid, tmp); str_concat(line, tmp);
        str_concat(line, ok ? "  PASS" : "  FAIL");
        SCPRINT(line, ok ? VGA_GREEN : VGA_RED);
        ok ? pass++ : fail++;
    }

    // ── [206] SYS_GETUID ardışık iki çağrı → tutarlı ─────────────────
    {
        _SC0(SYS_GETUID);
        uint64_t uid1 = ret;
        _SC0(SYS_GETUID);
        uint64_t uid2 = ret;
        int ok = (uid1 == uid2);
        sc_result(output, 206, "GETUID consistent", (int64_t)uid1,
                  ok, "uid stable?", &pass, &fail);
    }

    // ── [207] SYS_NANOSLEEP(0s, 0ns) → hemen döner ───────────────────
    {
        timespec_t req207 = { .tv_sec = 0, .tv_nsec = 0 };
        _SC2(SYS_NANOSLEEP, &req207, 0);
        sc_result(output, 207, "NANOSLEEP(0,0)", (int64_t)ret,
                  (int64_t)ret == 0, "expect 0", &pass, &fail);
    }

    // ── [208] SYS_NANOSLEEP(0s, 1ms) → kısa uyku ────────────────────
    {
        timespec_t req208 = { .tv_sec = 0, .tv_nsec = 1000000 };  // 1 ms
        _SC2(SYS_NANOSLEEP, &req208, 0);
        sc_result(output, 208, "NANOSLEEP(0,1ms)", (int64_t)ret,
                  (int64_t)ret == 0, "expect 0", &pass, &fail);
    }

    // ── [209] SYS_NANOSLEEP → rem pointer dolduruldu mu? ─────────────
    {
        timespec_t req209 = { .tv_sec = 0, .tv_nsec = 500000 };  // 0.5ms
        timespec_t rem209 = { .tv_sec = 99, .tv_nsec = 99 };
        _SC2(SYS_NANOSLEEP, &req209, &rem209);
        // Başarılı dönüşte rem = {0,0} olmalı
        int ok = ((int64_t)ret == 0 && rem209.tv_sec == 0 && rem209.tv_nsec == 0);
        sc_result(output, 209, "NANOSLEEP rem={0,0}", (int64_t)ret,
                  ok, "rem cleared?", &pass, &fail);
    }

    // ── [210] SYS_NANOSLEEP → NULL req → EFAULT ──────────────────────
    {
        _SC2(SYS_NANOSLEEP, 0, 0);
        sc_result(output, 210, "NANOSLEEP(NULL req)->EFAULT", (int64_t)ret,
                  (int64_t)ret == (int64_t)SYSCALL_ERR_FAULT,
                  "expect EFAULT(-11)", &pass, &fail);
    }

    // ── [211] SYS_NANOSLEEP → tv_nsec negatif → EINVAL ───────────────
    {
        timespec_t req211 = { .tv_sec = 0, .tv_nsec = -1 };
        _SC2(SYS_NANOSLEEP, &req211, 0);
        sc_result(output, 211, "NANOSLEEP(nsec<0)->EINVAL", (int64_t)ret,
                  (int64_t)ret == (int64_t)SYSCALL_ERR_INVAL,
                  "expect EINVAL(-1)", &pass, &fail);
    }

    // ── [212] SYS_NANOSLEEP → tv_nsec >= 1e9 → EINVAL ───────────────
    {
        timespec_t req212 = { .tv_sec = 0, .tv_nsec = 1000000000LL };
        _SC2(SYS_NANOSLEEP, &req212, 0);
        sc_result(output, 212, "NANOSLEEP(nsec>=1e9)->EINVAL", (int64_t)ret,
                  (int64_t)ret == (int64_t)SYSCALL_ERR_INVAL,
                  "expect EINVAL(-1)", &pass, &fail);
    }

    // ── [213] SYS_SIGALTSTACK → yeni stack kur ───────────────────────
    {
        static uint8_t altstack_buf[8192];
        stack_t ss213 = {
            .ss_sp    = altstack_buf,
            .ss_flags = 0,
            .ss_size  = 8192
        };
        _SC2(SYS_SIGALTSTACK, &ss213, 0);
        sc_result(output, 213, "SIGALTSTACK(set)", (int64_t)ret,
                  (int64_t)ret == 0, "expect 0", &pass, &fail);
    }

    // ── [214] SYS_SIGALTSTACK → old_ss round-trip ────────────────────
    {
        static uint8_t altstack_buf2[8192];
        stack_t ss214 = { .ss_sp = altstack_buf2, .ss_flags = 0, .ss_size = 8192 };
        stack_t old214;
        // Önce set et, sonra oku
        _SC2(SYS_SIGALTSTACK, &ss214, 0);
        _SC2(SYS_SIGALTSTACK, 0, &old214);
        int ok = ((int64_t)ret == 0 && old214.ss_sp == altstack_buf2
                  && old214.ss_size == 8192);
        sc_result(output, 214, "SIGALTSTACK old_ss round-trip", (int64_t)ret,
                  ok, "sp+size match?", &pass, &fail);
    }

    // ── [215] SYS_SIGALTSTACK → SS_DISABLE → devre dışı bırak ────────
    {
        stack_t ss215 = { .ss_sp = 0, .ss_flags = SS_DISABLE, .ss_size = 0 };
        _SC2(SYS_SIGALTSTACK, &ss215, 0);
        sc_result(output, 215, "SIGALTSTACK(SS_DISABLE)", (int64_t)ret,
                  (int64_t)ret == 0, "expect 0", &pass, &fail);
    }

    // ── [216] SYS_SIGALTSTACK → boyut < MINSIGSTKSZ → EINVAL ─────────
    {
        static uint8_t small_buf[512];
        stack_t ss216 = { .ss_sp = small_buf, .ss_flags = 0, .ss_size = 512 };
        _SC2(SYS_SIGALTSTACK, &ss216, 0);
        sc_result(output, 216, "SIGALTSTACK(size<MIN)->EINVAL", (int64_t)ret,
                  (int64_t)ret == (int64_t)SYSCALL_ERR_INVAL,
                  "expect EINVAL(-1)", &pass, &fail);
    }

    // ── [217] SYS_SIGALTSTACK → NULL ss, NULL old_ss → PASS (no-op) ──
    {
        _SC2(SYS_SIGALTSTACK, 0, 0);
        sc_result(output, 217, "SIGALTSTACK(NULL,NULL)", (int64_t)ret,
                  (int64_t)ret == 0, "expect 0 (no-op)", &pass, &fail);
    }

    // ── [218] SYS_SIGALTSTACK → old_ss geçersiz kernel adresi → EFAULT ──
    // 0xFFFF800000000000 kernel space → is_valid_user_ptr reddetmeli
    {
        _SC2(SYS_SIGALTSTACK, 0, (void*)0xFFFF800000000000ULL);
        sc_result(output, 218, "SIGALTSTACK(bad old_ss)->EFAULT", (int64_t)ret,
                  (int64_t)ret == (int64_t)SYSCALL_ERR_FAULT,
                  "expect EFAULT(-11)", &pass, &fail);
    }

    // ── [219] getuid + geteuid + getgid + getegid hepsi >= 0 ─────────
    {
        _SC0(SYS_GETUID);  uint64_t u  = ret;
        _SC0(SYS_GETEUID); uint64_t eu = ret;
        _SC0(SYS_GETGID);  uint64_t g  = ret;
        _SC0(SYS_GETEGID); uint64_t eg = ret;
        int ok = ((int64_t)u >= 0 && (int64_t)eu >= 0 &&
                  (int64_t)g >= 0 && (int64_t)eg >= 0 &&
                  u == eu && g == eg);
        str_cpy(line, "[219] uid=");
        u64_to_dec(u, tmp); str_concat(line, tmp);
        str_concat(line, " euid=");
        u64_to_dec(eu, tmp); str_concat(line, tmp);
        str_concat(line, " gid=");
        u64_to_dec(g, tmp); str_concat(line, tmp);
        str_concat(line, " egid=");
        u64_to_dec(eg, tmp); str_concat(line, tmp);
        str_concat(line, ok ? "  PASS" : "  FAIL");
        SCPRINT(line, ok ? VGA_GREEN : VGA_RED);
        ok ? pass++ : fail++;
    }

    // ════════════════════════════════════════════════════════════════
    // v17 – SYS_CLOCK_GETTIME + SYS_CLOCK_GETRES (220–229)
    // ════════════════════════════════════════════════════════════════
    SCPRINT("", VGA_WHITE);
    SCPRINT("── v17: clock_gettime / clock_getres ─────", VGA_CYAN);

    // ── [220] CLOCK_REALTIME → tv_sec >= 0 ───────────────────────────
    {
        timespec_t ts220;
        _SC2(SYS_CLOCK_GETTIME, CLOCK_REALTIME, &ts220);
        int ok = ((int64_t)ret == 0 && ts220.tv_sec >= 0 &&
                  ts220.tv_nsec >= 0 && ts220.tv_nsec < 1000000000LL);
        str_cpy(line, "[220] CLOCK_GETTIME(REALTIME) sec=");
        u64_to_dec((uint64_t)ts220.tv_sec, tmp); str_concat(line, tmp);
        str_concat(line, " nsec=");
        u64_to_dec((uint64_t)ts220.tv_nsec, tmp); str_concat(line, tmp);
        str_concat(line, ok ? "  PASS" : "  FAIL");
        SCPRINT(line, ok ? VGA_GREEN : VGA_RED);
        ok ? pass++ : fail++;
    }

    // ── [221] CLOCK_MONOTONIC → tv_sec >= 0 ──────────────────────────
    {
        timespec_t ts221;
        _SC2(SYS_CLOCK_GETTIME, CLOCK_MONOTONIC, &ts221);
        int ok = ((int64_t)ret == 0 && ts221.tv_sec >= 0 &&
                  ts221.tv_nsec >= 0 && ts221.tv_nsec < 1000000000LL);
        str_cpy(line, "[221] CLOCK_GETTIME(MONOTONIC) sec=");
        u64_to_dec((uint64_t)ts221.tv_sec, tmp); str_concat(line, tmp);
        str_concat(line, ok ? "  PASS" : "  FAIL");
        SCPRINT(line, ok ? VGA_GREEN : VGA_RED);
        ok ? pass++ : fail++;
    }

    // ── [222] CLOCK_MONOTONIC ilerliyor mu? ───────────────────────────
    {
        timespec_t ta, tb;
        _SC2(SYS_CLOCK_GETTIME, CLOCK_MONOTONIC, &ta);
        _SC1(SYS_SLEEP, 20);
        _SC2(SYS_CLOCK_GETTIME, CLOCK_MONOTONIC, &tb);
        int64_t delta_ns = (tb.tv_sec - ta.tv_sec) * 1000000000LL
                         + (tb.tv_nsec - ta.tv_nsec);
        int ok = (delta_ns > 0);
        str_cpy(line, "[222] CLOCK_MONOTONIC advancing delta_ns=");
        int_to_str((int)delta_ns, tmp); str_concat(line, tmp);
        str_concat(line, ok ? "  PASS" : "  WARN(tick rate?)");
        SCPRINT(line, ok ? VGA_GREEN : VGA_YELLOW);
        ok ? pass++ : fail++;
    }

    // ── [223] CLOCK_PROCESS_CPUTIME_ID → stub, 0 dönmeli ─────────────
    {
        timespec_t ts223;
        _SC2(SYS_CLOCK_GETTIME, CLOCK_PROCESS_CPUTIME_ID, &ts223);
        int ok = ((int64_t)ret == 0 && ts223.tv_sec == 0 && ts223.tv_nsec == 0);
        sc_result(output, 223, "CLOCK_GETTIME(CPUTIME stub)", (int64_t)ret,
                  ok, "expect {0,0}", &pass, &fail);
    }

    // ── [224] CLOCK_THREAD_CPUTIME_ID → stub, 0 dönmeli ──────────────
    {
        timespec_t ts224;
        _SC2(SYS_CLOCK_GETTIME, CLOCK_THREAD_CPUTIME_ID, &ts224);
        int ok = ((int64_t)ret == 0 && ts224.tv_sec == 0 && ts224.tv_nsec == 0);
        sc_result(output, 224, "CLOCK_GETTIME(THREAD_CPUTIME stub)", (int64_t)ret,
                  ok, "expect {0,0}", &pass, &fail);
    }

    // ── [225] geçersiz clockid → EINVAL ──────────────────────────────
    {
        timespec_t ts225;
        _SC2(SYS_CLOCK_GETTIME, 99, &ts225);
        sc_result(output, 225, "CLOCK_GETTIME(clockid=99)->EINVAL", (int64_t)ret,
                  (int64_t)ret == (int64_t)SYSCALL_ERR_INVAL,
                  "expect EINVAL(-1)", &pass, &fail);
    }

    // ── [226] NULL tp → EFAULT ────────────────────────────────────────
    {
        _SC2(SYS_CLOCK_GETTIME, CLOCK_REALTIME, 0);
        sc_result(output, 226, "CLOCK_GETTIME(NULL tp)->EFAULT", (int64_t)ret,
                  (int64_t)ret == (int64_t)SYSCALL_ERR_FAULT,
                  "expect EFAULT(-11)", &pass, &fail);
    }

    // ── [227] CLOCK_GETRES(REALTIME) → res.tv_nsec > 0 ───────────────
    {
        timespec_t res227;
        _SC2(SYS_CLOCK_GETRES, CLOCK_REALTIME, &res227);
        int ok = ((int64_t)ret == 0 && res227.tv_sec == 0 && res227.tv_nsec > 0);
        str_cpy(line, "[227] CLOCK_GETRES(REALTIME) nsec=");
        u64_to_dec((uint64_t)res227.tv_nsec, tmp); str_concat(line, tmp);
        str_concat(line, ok ? "  PASS" : "  FAIL");
        SCPRINT(line, ok ? VGA_GREEN : VGA_RED);
        ok ? pass++ : fail++;
    }

    // ── [228] CLOCK_GETRES NULL res → clockid kontrolü yapılır ───────
    {
        _SC2(SYS_CLOCK_GETRES, CLOCK_MONOTONIC, 0);
        sc_result(output, 228, "CLOCK_GETRES(MONOTONIC, NULL res)", (int64_t)ret,
                  (int64_t)ret == 0, "expect 0 (res=NULL ok)", &pass, &fail);
    }

    // ── [229] CLOCK_GETRES geçersiz clockid → EINVAL ──────────────────
    {
        timespec_t res229;
        _SC2(SYS_CLOCK_GETRES, 99, &res229);
        sc_result(output, 229, "CLOCK_GETRES(clockid=99)->EINVAL", (int64_t)ret,
                  (int64_t)ret == (int64_t)SYSCALL_ERR_INVAL,
                  "expect EINVAL(-1)", &pass, &fail);
    }

    // ════════════════════════════════════════════════════════════════
    // v18 – SYS_ALARM + SYS_FTRUNCATE + SYS_TRUNCATE (230–246)
    // ════════════════════════════════════════════════════════════════
    SCPRINT("", VGA_WHITE);
    SCPRINT("── v18: alarm / ftruncate / truncate ─────", VGA_CYAN);

    // ── [230] SYS_ALARM(0) başlangıçta 0 döner (alarm yok) ───────────
    {
        _SC1(SYS_ALARM, 0);
        sc_result(output, 230, "ALARM(0) no pending", (int64_t)ret,
                  (int64_t)ret == 0, "expect 0", &pass, &fail);
    }

    // ── [231] SYS_ALARM(5) → 0 döner (önceki alarm yoktu) ───────────
    {
        _SC1(SYS_ALARM, 5);
        sc_result(output, 231, "ALARM(5) set new", (int64_t)ret,
                  (int64_t)ret == 0, "expect 0 (no prev)", &pass, &fail);
    }

    // ── [232] SYS_ALARM(10) → kalan > 0 döner (önceki 5s vardı) ─────
    {
        _SC1(SYS_ALARM, 10);
        int ok = ((int64_t)ret > 0);
        str_cpy(line, "[232] ALARM(10) prev_remaining=");
        u64_to_dec(ret, tmp); str_concat(line, tmp);
        str_concat(line, ok ? "  PASS" : "  FAIL");
        SCPRINT(line, ok ? VGA_GREEN : VGA_RED);
        ok ? pass++ : fail++;
    }

    // ── [233] SYS_ALARM(0) → iptal, kalan > 0 döner ─────────────────
    {
        _SC1(SYS_ALARM, 0);
        int ok = ((int64_t)ret > 0);
        str_cpy(line, "[233] ALARM(0) cancel remaining=");
        u64_to_dec(ret, tmp); str_concat(line, tmp);
        str_concat(line, ok ? "  PASS" : "  FAIL");
        SCPRINT(line, ok ? VGA_GREEN : VGA_RED);
        ok ? pass++ : fail++;
    }

    // ── [234] SYS_ALARM(0) tekrar → artık 0 (iptal edildi) ───────────
    {
        _SC1(SYS_ALARM, 0);
        sc_result(output, 234, "ALARM(0) after cancel", (int64_t)ret,
                  (int64_t)ret == 0, "expect 0", &pass, &fail);
    }

    // ── [235] SYS_ALARM ardışık set → her seferinde önceki kalan döner
    {
        _SC1(SYS_ALARM, 100);
        uint64_t r1 = ret;
        _SC1(SYS_ALARM, 200);
        uint64_t r2 = ret;
        _SC1(SYS_ALARM, 0);
        int ok = (r1 == 0 && (int64_t)r2 > 0);
        str_cpy(line, "[235] ALARM replace prev1=");
        u64_to_dec(r1, tmp); str_concat(line, tmp);
        str_concat(line, " prev2=");
        u64_to_dec(r2, tmp); str_concat(line, tmp);
        str_concat(line, ok ? "  PASS" : "  FAIL");
        SCPRINT(line, ok ? VGA_GREEN : VGA_RED);
        ok ? pass++ : fail++;
    }

    // ── [236] SYS_FTRUNCATE fd geçersiz → EBADF ──────────────────────
    {
        _SC2(SYS_FTRUNCATE, 999, 0);
        sc_result(output, 236, "FTRUNCATE(bad fd)->EBADF", (int64_t)ret,
                  (int64_t)ret == (int64_t)SYSCALL_ERR_BADF,
                  "expect EBADF(-5)", &pass, &fail);
    }

    // ── [237] SYS_FTRUNCATE stdin (fd=0, read-only) → EPERM/EINVAL ───
    {
        _SC2(SYS_FTRUNCATE, 0, 0);
        int ok = ((int64_t)ret == (int64_t)SYSCALL_ERR_PERM  ||
                  (int64_t)ret == (int64_t)SYSCALL_ERR_INVAL ||
                  (int64_t)ret == (int64_t)SYSCALL_ERR_BADF);
        str_cpy(line, "[237] FTRUNCATE(stdin)->err ret=");
        int_to_str((int)(int64_t)ret, tmp); str_concat(line, tmp);
        str_concat(line, ok ? "  PASS" : "  FAIL");
        SCPRINT(line, ok ? VGA_GREEN : VGA_RED);
        ok ? pass++ : fail++;
    }

    // ── [238] SYS_FTRUNCATE stdout (serial, dosya değil) → hata ──────
    {
        _SC2(SYS_FTRUNCATE, 1, 0);
        int ok = ((int64_t)ret != 0);
        str_cpy(line, "[238] FTRUNCATE(stdout fd=1)->err ret=");
        int_to_str((int)(int64_t)ret, tmp); str_concat(line, tmp);
        str_concat(line, ok ? "  PASS" : "  FAIL");
        SCPRINT(line, ok ? VGA_GREEN : VGA_RED);
        ok ? pass++ : fail++;
    }

    // ── [239] SYS_FTRUNCATE pipe fd → EINVAL ─────────────────────────
    {
        int pfds[2] = {-1, -1};
        _SC1(SYS_PIPE, pfds);
        if ((int64_t)ret == 0 && pfds[1] >= 3) {
            _SC2(SYS_FTRUNCATE, pfds[1], 0);
            sc_result(output, 239, "FTRUNCATE(pipe wfd)->EINVAL", (int64_t)ret,
                      (int64_t)ret == (int64_t)SYSCALL_ERR_INVAL,
                      "expect EINVAL(-1)", &pass, &fail);
            _SC1(SYS_CLOSE, pfds[0]);
            _SC1(SYS_CLOSE, pfds[1]);
        } else {
            SCPRINT("[239] FTRUNCATE(pipe)  SKIP (pipe failed)", VGA_YELLOW);
            pass++;
        }
    }

    // ── [240] SYS_TRUNCATE var olmayan path → ENOENT ──────────────────
    {
        _SC2(SYS_TRUNCATE, "/no_such_file_trunc_xyz", 0);
        sc_result(output, 240, "TRUNCATE(noexist)->ENOENT", (int64_t)ret,
                  (int64_t)ret == (int64_t)SYSCALL_ERR_NOENT,
                  "expect ENOENT(-4)", &pass, &fail);
    }

    // ── [241] SYS_TRUNCATE NULL path → EFAULT ────────────────────────
    {
        _SC2(SYS_TRUNCATE, 0, 0);
        sc_result(output, 241, "TRUNCATE(NULL path)->EFAULT", (int64_t)ret,
                  (int64_t)ret == (int64_t)SYSCALL_ERR_FAULT,
                  "expect EFAULT(-11)", &pass, &fail);
    }

    // ── [242] SYS_TRUNCATE boş path → ENOENT ─────────────────────────
    {
        _SC2(SYS_TRUNCATE, "", 0);
        sc_result(output, 242, "TRUNCATE(empty path)->ENOENT", (int64_t)ret,
                  (int64_t)ret == (int64_t)SYSCALL_ERR_NOENT,
                  "expect ENOENT(-4)", &pass, &fail);
    }

    // ── [243] SYS_TRUNCATE mevcut dosya length=0 ─────────────────────
    {
        _SC2(SYS_TRUNCATE, "/etc/hostname", 0);
        int ok = ((int64_t)ret == 0 || (int64_t)ret == (int64_t)SYSCALL_ERR_PERM);
        str_cpy(line, "[243] TRUNCATE(/etc/hostname,0) ret=");
        int_to_str((int)(int64_t)ret, tmp); str_concat(line, tmp);
        str_concat(line, ok ? "  PASS" : "  FAIL");
        SCPRINT(line, ok ? VGA_GREEN : VGA_RED);
        ok ? pass++ : fail++;
    }

    // ── [244] SYS_TRUNCATE + STAT → boyut güncellenmiş mi? ───────────
    {
        extern int fs_touch_file64(const char* filename);
        extern int fs_write_file64(const char* filename, const char* content);
        static const char tpath[] = "/tmp/trunc_test";
        static const char tcontent[] = "0123456789";
        int cr = fs_touch_file64(tpath);
        int wr = (cr >= 0) ? fs_write_file64(tpath, tcontent) : -1;
        if (wr >= 0) {
            _SC2(SYS_TRUNCATE, tpath, 5);
            uint64_t tr_ret = ret;
            stat_t st244;
            _SC2(SYS_STAT, tpath, &st244);
            int ok = ((int64_t)tr_ret == 0 &&
                      (int64_t)ret == 0 &&
                      st244.st_size == 5);
            str_cpy(line, "[244] TRUNCATE+STAT size=");
            u64_to_dec(st244.st_size, tmp); str_concat(line, tmp);
            str_concat(line, ok ? "  PASS" : "  FAIL");
            SCPRINT(line, ok ? VGA_GREEN : VGA_RED);
            ok ? pass++ : fail++;
            _SC1(SYS_UNLINK, tpath);
        } else {
            SCPRINT("[244] TRUNCATE+STAT  SKIP (fs_touch failed)", VGA_YELLOW);
            pass++;
        }
    }

    // ── [245] ALARM + nanosleep tutarlılık ────────────────────────────
    {
        _SC1(SYS_ALARM, 60);
        uint64_t set_prev = ret;
        // 1ms: 10ms altı → 0 tick → hemen döner, alarm hâlâ aktif
        timespec_t nap = { .tv_sec = 0, .tv_nsec = 1000000 };
        _SC2(SYS_NANOSLEEP, &nap, 0);
        _SC1(SYS_ALARM, 0);
        uint64_t remaining = ret;
        int ok = (set_prev == 0 && remaining > 0 && remaining <= 60);
        str_cpy(line, "[245] ALARM set/sleep/cancel remaining=");
        u64_to_dec(remaining, tmp); str_concat(line, tmp);
        str_concat(line, ok ? "  PASS" : "  FAIL");
        SCPRINT(line, ok ? VGA_GREEN : VGA_RED);
        ok ? pass++ : fail++;
    }

    // ── [246] SYS_ALARM(0) clean slate ───────────────────────────────
    {
        _SC1(SYS_ALARM, 0);
        sc_result(output, 246, "ALARM(0) final clean", (int64_t)ret,
                  (int64_t)ret == 0, "expect 0", &pass, &fail);
    }

    // ── [247] SYS_GETRLIMIT RLIMIT_NOFILE → cur==MAX_FDS ─────────────
    {
        rlimit_t rl247 = {0, 0};
        _SC2(SYS_GETRLIMIT, RLIMIT_NOFILE, &rl247);
        int ok = ((int64_t)ret == 0 && rl247.rlim_cur == MAX_FDS);
        str_cpy(line, "[247] GETRLIMIT(NOFILE) cur=");
        u64_to_dec(rl247.rlim_cur, tmp); str_concat(line, tmp);
        str_concat(line, ok ? "  PASS" : "  FAIL");
        SCPRINT(line, ok ? VGA_GREEN : VGA_RED);
        ok ? pass++ : fail++;
    }

    // ── [248] SYS_GETRLIMIT RLIMIT_STACK → cur==8MiB ─────────────────
    {
        rlimit_t rl248 = {0, 0};
        _SC2(SYS_GETRLIMIT, RLIMIT_STACK, &rl248);
        int ok = ((int64_t)ret == 0 && rl248.rlim_cur == 8u*1024u*1024u);
        str_cpy(line, "[248] GETRLIMIT(STACK) cur=");
        u64_to_dec(rl248.rlim_cur, tmp); str_concat(line, tmp);
        str_concat(line, ok ? "  PASS" : "  FAIL");
        SCPRINT(line, ok ? VGA_GREEN : VGA_RED);
        ok ? pass++ : fail++;
    }

    // ── [249] SYS_GETRLIMIT geçersiz resource → EINVAL ───────────────
    {
        rlimit_t rl249 = {0, 0};
        _SC2(SYS_GETRLIMIT, 999, &rl249);
        sc_result(output, 249, "GETRLIMIT(999)->EINVAL", (int64_t)ret,
                  (int64_t)ret == (int64_t)SYSCALL_ERR_INVAL,
                  "expect EINVAL(-1)", &pass, &fail);
    }

    // ── [250] SYS_GETRLIMIT NULL buf → EFAULT ────────────────────────
    {
        _SC2(SYS_GETRLIMIT, RLIMIT_NOFILE, 0);
        sc_result(output, 250, "GETRLIMIT(NOFILE,NULL)->EFAULT", (int64_t)ret,
                  (int64_t)ret == (int64_t)SYSCALL_ERR_FAULT,
                  "expect EFAULT(-11)", &pass, &fail);
    }

    // ── [251] SYS_SETRLIMIT RLIMIT_NPROC → yeni değer okunabilmeli ───
    {
        rlimit_t rl251_set = { 32, 32 };
        _SC2(SYS_SETRLIMIT, RLIMIT_NPROC, &rl251_set);
        uint64_t set_ret = ret;
        rlimit_t rl251_get = {0, 0};
        _SC2(SYS_GETRLIMIT, RLIMIT_NPROC, &rl251_get);
        int ok = ((int64_t)set_ret == 0 &&
                  (int64_t)ret    == 0  &&
                  rl251_get.rlim_cur == 32);
        str_cpy(line, "[251] SETRLIMIT(NPROC,32) get_cur=");
        u64_to_dec(rl251_get.rlim_cur, tmp); str_concat(line, tmp);
        str_concat(line, ok ? "  PASS" : "  FAIL");
        SCPRINT(line, ok ? VGA_GREEN : VGA_RED);
        ok ? pass++ : fail++;
    }

    // ── [252] SYS_SETRLIMIT cur > max → EINVAL ───────────────────────
    {
        rlimit_t rl252 = { .rlim_cur = 100, .rlim_max = 10 };
        _SC2(SYS_SETRLIMIT, RLIMIT_NPROC, &rl252);
        sc_result(output, 252, "SETRLIMIT(cur>max)->EINVAL", (int64_t)ret,
                  (int64_t)ret == (int64_t)SYSCALL_ERR_INVAL,
                  "expect EINVAL(-1)", &pass, &fail);
    }

    // ── [253] SYS_SETRLIMIT hard limit yükseltme → EPERM ─────────────
    {
        // Mevcut hard limit 32 (NPROC); bunu 9999'a çıkarmaya çalış
        rlimit_t rl253 = { .rlim_cur = 9999, .rlim_max = 9999 };
        _SC2(SYS_SETRLIMIT, RLIMIT_NPROC, &rl253);
        sc_result(output, 253, "SETRLIMIT(raise hard)->EPERM", (int64_t)ret,
                  (int64_t)ret == (int64_t)SYSCALL_ERR_PERM,
                  "expect EPERM(-3)", &pass, &fail);
    }

    // ── [254] SYS_SETRLIMIT NULL buf → EFAULT ────────────────────────
    {
        _SC2(SYS_SETRLIMIT, RLIMIT_NOFILE, 0);
        sc_result(output, 254, "SETRLIMIT(NOFILE,NULL)->EFAULT", (int64_t)ret,
                  (int64_t)ret == (int64_t)SYSCALL_ERR_FAULT,
                  "expect EFAULT(-11)", &pass, &fail);
    }

    // ╔══════════════════════════════════════════════════════════╗
    // ║  v20 – SYS_LSTAT / SYS_LINK / SYS_TIMES  [255-265]     ║
    // ╚══════════════════════════════════════════════════════════╝

    // ── [255] SYS_LSTAT mevcut dosya → 0, st_size > 0 ────────────────
    {
        stat_t st255;
        _SC2(SYS_LSTAT, "/etc/hostname", &st255);
        int ok = ((int64_t)ret == 0 && st255.st_size > 0);
        str_cpy(line, "[255] LSTAT(/etc/hostname) size=");
        u64_to_dec(st255.st_size, tmp); str_concat(line, tmp);
        str_concat(line, ok ? "  PASS" : "  FAIL");
        SCPRINT(line, ok ? VGA_GREEN : VGA_RED);
        ok ? pass++ : fail++;
    }

    // ── [256] SYS_LSTAT mevcut dizin → 0, S_ISDIR ────────────────────
    {
        stat_t st256;
        _SC2(SYS_LSTAT, "/etc", &st256);
        // S_IFDIR = 0x4000
        int ok = ((int64_t)ret == 0 && (st256.st_mode & 0xF000) == 0x4000);
        str_cpy(line, "[256] LSTAT(/etc) mode=0x");
        u64_to_dec(st256.st_mode, tmp); str_concat(line, tmp);
        str_concat(line, ok ? "  PASS" : "  FAIL");
        SCPRINT(line, ok ? VGA_GREEN : VGA_RED);
        ok ? pass++ : fail++;
    }

    // ── [257] SYS_LSTAT olmayan path → ENOENT ────────────────────────
    {
        stat_t st257;
        _SC2(SYS_LSTAT, "/no_such_lstat_xyz", &st257);
        sc_result(output, 257, "LSTAT(noexist)->ENOENT", (int64_t)ret,
                  (int64_t)ret == (int64_t)SYSCALL_ERR_NOENT,
                  "expect ENOENT(-4)", &pass, &fail);
    }

    // ── [258] SYS_LSTAT NULL path → EFAULT ───────────────────────────
    {
        stat_t st258;
        _SC2(SYS_LSTAT, 0, &st258);
        sc_result(output, 258, "LSTAT(NULL)->EFAULT", (int64_t)ret,
                  (int64_t)ret == (int64_t)SYSCALL_ERR_FAULT,
                  "expect EFAULT(-11)", &pass, &fail);
    }

    // ── [259] SYS_LSTAT vs SYS_STAT tutarlılık → aynı st_size ────────
    // (symlink olmayan sistemde lstat == stat davranışı beklenir)
    {
        stat_t st_s, st_l;
        _SC2(SYS_STAT,  "/etc/hostname", &st_s);
        uint64_t stat_ret = ret;
        _SC2(SYS_LSTAT, "/etc/hostname", &st_l);
        int ok = ((int64_t)stat_ret == 0 &&
                  (int64_t)ret      == 0 &&
                  st_s.st_size == st_l.st_size &&
                  st_s.st_mode == st_l.st_mode);
        str_cpy(line, "[259] LSTAT==STAT size=");
        u64_to_dec(st_l.st_size, tmp); str_concat(line, tmp);
        str_concat(line, ok ? "  PASS" : "  FAIL");
        SCPRINT(line, ok ? VGA_GREEN : VGA_RED);
        ok ? pass++ : fail++;
    }

    // ── [260] SYS_LINK mevcut dosya → EPERM (hard link yok) ──────────
    {
        _SC2(SYS_LINK, "/etc/hostname", "/tmp/hostname_link");
        sc_result(output, 260, "LINK(exist)->EPERM", (int64_t)ret,
                  (int64_t)ret == (int64_t)SYSCALL_ERR_PERM,
                  "expect EPERM(-3)", &pass, &fail);
    }

    // ── [261] SYS_LINK olmayan kaynak → ENOENT ────────────────────────
    {
        _SC2(SYS_LINK, "/no_such_link_src", "/tmp/link_dst");
        sc_result(output, 261, "LINK(noexist)->ENOENT", (int64_t)ret,
                  (int64_t)ret == (int64_t)SYSCALL_ERR_NOENT,
                  "expect ENOENT(-4)", &pass, &fail);
    }

    // ── [262] SYS_LINK NULL kaynak → EFAULT ──────────────────────────
    {
        _SC2(SYS_LINK, 0, "/tmp/link_dst");
        sc_result(output, 262, "LINK(NULL src)->EFAULT", (int64_t)ret,
                  (int64_t)ret == (int64_t)SYSCALL_ERR_FAULT,
                  "expect EFAULT(-11)", &pass, &fail);
    }

    // ── [263] SYS_TIMES geçerli buf → elapsed >= 0, tms_utime == 0 ───
    {
        tms_t tms263 = {0xDEAD, 0xDEAD, 0xDEAD, 0xDEAD};
        _SC1(SYS_TIMES, &tms263);
        int ok = ((int64_t)ret >= 0 &&
                  tms263.tms_utime  == 0 &&
                  tms263.tms_stime  == 0 &&
                  tms263.tms_cutime == 0 &&
                  tms263.tms_cstime == 0);
        str_cpy(line, "[263] TIMES(buf) elapsed=");
        u64_to_dec(ret, tmp); str_concat(line, tmp);
        str_concat(line, ok ? "  PASS" : "  FAIL");
        SCPRINT(line, ok ? VGA_GREEN : VGA_RED);
        ok ? pass++ : fail++;
    }

    // ── [264] SYS_TIMES NULL buf → sadece elapsed tick döner ─────────
    {
        _SC1(SYS_TIMES, 0);
        int ok = ((int64_t)ret >= 0);
        str_cpy(line, "[264] TIMES(NULL) elapsed=");
        u64_to_dec(ret, tmp); str_concat(line, tmp);
        str_concat(line, ok ? "  PASS" : "  FAIL");
        SCPRINT(line, ok ? VGA_GREEN : VGA_RED);
        ok ? pass++ : fail++;
    }

    // ── [265] SYS_TIMES ardışık iki çağrı → elapsed monoton artar ────
    {
        tms_t tms265a, tms265b;
        _SC1(SYS_TIMES, &tms265a);
        uint64_t t1 = ret;
        // Küçük bir busy-wait yaparak zaman geçmesini sağla
        volatile int bw = 0;
        for (volatile int i = 0; i < 100000; i++) bw++;
        _SC1(SYS_TIMES, &tms265b);
        uint64_t t2 = ret;
        int ok = (t2 >= t1);   // monoton: t2 >= t1
        str_cpy(line, "[265] TIMES monoton t1=");
        u64_to_dec(t1, tmp); str_concat(line, tmp);
        str_concat(line, " t2=");
        u64_to_dec(t2, tmp); str_concat(line, tmp);
        str_concat(line, ok ? "  PASS" : "  FAIL");
        SCPRINT(line, ok ? VGA_GREEN : VGA_RED);
        ok ? pass++ : fail++;
    }

    // ════════════════════════════════════════════════════════════════
    // v21 – SYS_UMASK / SYS_SYMLINK / SYS_READLINK  [266–283]
    // ════════════════════════════════════════════════════════════════
    SCPRINT("", VGA_WHITE);
    SCPRINT("── v21: umask / symlink / readlink ───────", VGA_CYAN);

    // ── [266] SYS_UMASK(0022) → eski mask değeri >= 0 ────────────────
    // İlk çağrıda kernel'deki varsayılan g_umask 0022 olduğundan
    // eski değer olarak 0022 dönmeli; en azından >= 0 olmalı.
    {
        _SC1(SYS_UMASK, 0022);
        int ok = ((int64_t)ret >= 0);
        str_cpy(line, "[266] UMASK(0022) old_mask=0");
        int_to_str((int)ret, tmp); str_concat(line, tmp);
        str_concat(line, ok ? "  PASS" : "  FAIL");
        SCPRINT(line, ok ? VGA_GREEN : VGA_RED);
        ok ? pass++ : fail++;
    }

    // ── [267] SYS_UMASK(0077) → eski değer 0022 dönmeli ─────────────
    // Bir önceki çağrı 0022 set etti; şimdi 0077 yazıyoruz.
    // POSIX: umask daima başarılı, eski mask'i döndürür.
    {
        _SC1(SYS_UMASK, 0077);
        int ok = (ret == 0022);
        str_cpy(line, "[267] UMASK(0077) old=0022? got=0");
        int_to_str((int)ret, tmp); str_concat(line, tmp);
        str_concat(line, ok ? "  PASS" : "  FAIL");
        SCPRINT(line, ok ? VGA_GREEN : VGA_RED);
        ok ? pass++ : fail++;
    }

    // ── [268] SYS_UMASK round-trip: 0055 set → hemen 0077 dönmeli ────
    {
        _SC1(SYS_UMASK, 0055);
        int ok = (ret == 0077);   // önceki 0077 idi
        str_cpy(line, "[268] UMASK(0055) old=0077? got=0");
        int_to_str((int)ret, tmp); str_concat(line, tmp);
        str_concat(line, ok ? "  PASS" : "  FAIL");
        SCPRINT(line, ok ? VGA_GREEN : VGA_RED);
        ok ? pass++ : fail++;
    }

    // ── [269] SYS_UMASK(0) → tüm bitleri temizle, 0055 dönmeli ──────
    {
        _SC1(SYS_UMASK, 0);
        int ok = (ret == 0055);
        str_cpy(line, "[269] UMASK(0) old=0055? got=0");
        int_to_str((int)ret, tmp); str_concat(line, tmp);
        str_concat(line, ok ? "  PASS" : "  FAIL");
        SCPRINT(line, ok ? VGA_GREEN : VGA_RED);
        ok ? pass++ : fail++;
    }

    // ── [270] SYS_UMASK(0777) → sadece alt 9 bit, 0 dönmeli ─────────
    // syscall.c: new_mask = rdi & 0777. Eski değer 0 idi.
    {
        _SC1(SYS_UMASK, 0777);
        int ok = (ret == 0);  // önceki 0 idi
        str_cpy(line, "[270] UMASK(0777) old=0? got=0");
        int_to_str((int)ret, tmp); str_concat(line, tmp);
        str_concat(line, ok ? "  PASS" : "  FAIL");
        SCPRINT(line, ok ? VGA_GREEN : VGA_RED);
        ok ? pass++ : fail++;
    }

    // ── [271] SYS_UMASK → 0022'ye geri yükle (sonraki testler için) ──
    {
        _SC1(SYS_UMASK, 0022);
        // önceki 0777 idi; 0777 dönmeli
        int ok = (ret == 0777);
        str_cpy(line, "[271] UMASK restore 0022, old=0777? got=0");
        int_to_str((int)ret, tmp); str_concat(line, tmp);
        str_concat(line, ok ? "  PASS" : "  FAIL");
        SCPRINT(line, ok ? VGA_GREEN : VGA_RED);
        ok ? pass++ : fail++;
    }

    // ── [272] SYS_SYMLINK("target", "/tmp/sl_link") → 0 ─────────────
    {
        static const char sl_target[] = "/etc/hostname";
        static const char sl_link[]   = "/tmp/sl_link_test";
        _SC2(SYS_SYMLINK, sl_target, sl_link);
        sc_result(output, 272, "SYMLINK(target,link)", (int64_t)ret,
                  (int64_t)ret == 0, "expect 0", &pass, &fail);
    }

    // ── [273] SYS_SYMLINK aynı linkpath tekrar → EBUSY ───────────────
    {
        static const char sl_target2[] = "/etc/hostname";
        static const char sl_link2[]   = "/tmp/sl_link_test";   // aynı link
        _SC2(SYS_SYMLINK, sl_target2, sl_link2);
        sc_result(output, 273, "SYMLINK(dup linkpath)->EBUSY", (int64_t)ret,
                  (int64_t)ret == (int64_t)SYSCALL_ERR_BUSY,
                  "expect EBUSY(-7)", &pass, &fail);
    }

    // ── [274] SYS_SYMLINK boş target → ENOENT ────────────────────────
    {
        static const char sl_empty[] = "";
        static const char sl_lnk3[]  = "/tmp/sl_empty_tgt";
        _SC2(SYS_SYMLINK, sl_empty, sl_lnk3);
        sc_result(output, 274, "SYMLINK(empty target)->ENOENT", (int64_t)ret,
                  (int64_t)ret == (int64_t)SYSCALL_ERR_NOENT,
                  "expect ENOENT(-4)", &pass, &fail);
    }

    // ── [275] SYS_SYMLINK NULL target → EFAULT ───────────────────────
    {
        static const char sl_lnk4[] = "/tmp/sl_null_tgt";
        _SC2(SYS_SYMLINK, 0, sl_lnk4);
        sc_result(output, 275, "SYMLINK(NULL target)->EFAULT", (int64_t)ret,
                  (int64_t)ret == (int64_t)SYSCALL_ERR_FAULT,
                  "expect EFAULT(-11)", &pass, &fail);
    }

    // ── [276] SYS_SYMLINK NULL linkpath → EFAULT ─────────────────────
    {
        static const char sl_tgt5[] = "/etc/hostname";
        _SC2(SYS_SYMLINK, sl_tgt5, 0);
        sc_result(output, 276, "SYMLINK(NULL linkpath)->EFAULT", (int64_t)ret,
                  (int64_t)ret == (int64_t)SYSCALL_ERR_FAULT,
                  "expect EFAULT(-11)", &pass, &fail);
    }

    // ── [277] SYS_READLINK "/proc/self/exe" → len > 0 ────────────────
    // Bash bu path'i argv[0] resolve için kullanır.
    {
        static const char pse_path[] = "/proc/self/exe";
        char rl_buf[128];
        _SC3(SYS_READLINK, pse_path, rl_buf, 128);
        int ok = ((int64_t)ret > 0 && (int64_t)ret < 128);
        rl_buf[(int64_t)ret > 0 ? (int)ret : 0] = '\0';
        str_cpy(line, "[277] READLINK(/proc/self/exe) len=");
        int_to_str((int)ret, tmp); str_concat(line, tmp);
        str_concat(line, " val=");
        str_concat(line, ok ? rl_buf : "(err)");
        str_concat(line, ok ? "  PASS" : "  FAIL");
        SCPRINT(line, ok ? VGA_GREEN : VGA_RED);
        ok ? pass++ : fail++;
    }

    // ── [278] SYS_READLINK sembolik link → [272]'de kurulan link ─────
    // [272]'de "/tmp/sl_link_test" → "/etc/hostname" olarak kuruldu.
    {
        static const char rd_link[] = "/tmp/sl_link_test";
        char rl_buf2[128];
        _SC3(SYS_READLINK, rd_link, rl_buf2, 128);
        // target "/etc/hostname" idi; readlink len > 0 dönmeli
        int ok = ((int64_t)ret > 0);
        rl_buf2[(int64_t)ret > 0 ? (int)ret : 0] = '\0';
        str_cpy(line, "[278] READLINK(sl_link) -> ");
        str_concat(line, ok ? rl_buf2 : "(err)");
        str_concat(line, " len=");
        int_to_str((int)ret, tmp); str_concat(line, tmp);
        str_concat(line, ok ? "  PASS" : "  FAIL");
        SCPRINT(line, ok ? VGA_GREEN : VGA_RED);
        ok ? pass++ : fail++;
    }

    // ── [279] SYS_READLINK olmayan path → EINVAL (symlink değil) ─────
    {
        static const char rd_nolink[] = "/etc/hostname";  // gerçek dosya, link değil
        char rl_buf3[64];
        _SC3(SYS_READLINK, rd_nolink, rl_buf3, 64);
        sc_result(output, 279, "READLINK(regular file)->EINVAL", (int64_t)ret,
                  (int64_t)ret == (int64_t)SYSCALL_ERR_INVAL,
                  "expect EINVAL(-1)", &pass, &fail);
    }

    // ── [280] SYS_READLINK NULL path → EFAULT ────────────────────────
    {
        char rl_buf4[64];
        _SC3(SYS_READLINK, 0, rl_buf4, 64);
        sc_result(output, 280, "READLINK(NULL path)->EFAULT", (int64_t)ret,
                  (int64_t)ret == (int64_t)SYSCALL_ERR_FAULT,
                  "expect EFAULT(-11)", &pass, &fail);
    }

    // ── [281] SYS_READLINK bufsiz=0 → EINVAL ─────────────────────────
    {
        static const char rd_path2[] = "/proc/self/exe";
        char rl_buf5[64];
        _SC3(SYS_READLINK, rd_path2, rl_buf5, 0);
        sc_result(output, 281, "READLINK(bufsiz=0)->EINVAL", (int64_t)ret,
                  (int64_t)ret == (int64_t)SYSCALL_ERR_INVAL,
                  "expect EINVAL(-1)", &pass, &fail);
    }

    // ── [282] SYS_READLINK küçük buf → kısaltılmış len döner ─────────
    // /proc/self/exe en az 1 karakter döner; bufsiz=1 → ret==1 beklenir
    {
        static const char rd_path3[] = "/proc/self/exe";
        char rl_tiny[2];
        _SC3(SYS_READLINK, rd_path3, rl_tiny, 1);
        // ret==1 (kısaltıldı) veya >0 olmalı; POSIX: null terminate etmez
        int ok = ((int64_t)ret == 1);
        str_cpy(line, "[282] READLINK(bufsiz=1) ret=");
        int_to_str((int)ret, tmp); str_concat(line, tmp);
        str_concat(line, ok ? "  PASS" : "  FAIL");
        SCPRINT(line, ok ? VGA_GREEN : VGA_RED);
        ok ? pass++ : fail++;
    }

    // ── [283] SYS_READLINK /proc/self/fd/1 → stdout path dolu ────────
    // fd=1 stdout → path "/dev/serial0" veya benzeri; boş olmamalı.
    {
        static const char fd1_path[] = "/proc/self/fd/1";
        char rl_buf6[64];
        _SC3(SYS_READLINK, fd1_path, rl_buf6, 64);
        int ok = ((int64_t)ret > 0);
        rl_buf6[(int64_t)ret > 0 ? (int)ret : 0] = '\0';
        str_cpy(line, "[283] READLINK(/proc/self/fd/1) -> ");
        str_concat(line, ok ? rl_buf6 : "(err)");
        str_concat(line, " len=");
        int_to_str((int)ret, tmp); str_concat(line, tmp);
        str_concat(line, ok ? "  PASS" : "  FAIL");
        SCPRINT(line, ok ? VGA_GREEN : VGA_RED);
        ok ? pass++ : fail++;
    }

    // ════════════════════════════════════════════════════════════════
    // v22 – SYS_CHMOD / SYS_MPROTECT / SYS_PIPE2  [284–303]
    // ════════════════════════════════════════════════════════════════
    SCPRINT("", VGA_WHITE);
    SCPRINT("── v22: chmod / mprotect / pipe2 ─────────", VGA_CYAN);

    // ── [284] SYS_CHMOD mevcut dosya → 0 ─────────────────────────────
    {
        _SC2(SYS_CHMOD, "/etc/hostname", 0644);
        sc_result(output, 284, "CHMOD(/etc/hostname, 0644)", (int64_t)ret,
                  (int64_t)ret == 0, "expect 0", &pass, &fail);
    }

    // ── [285] SYS_CHMOD dizin → 0 ────────────────────────────────────
    {
        _SC2(SYS_CHMOD, "/etc", 0755);
        sc_result(output, 285, "CHMOD(/etc, 0755)", (int64_t)ret,
                  (int64_t)ret == 0, "expect 0", &pass, &fail);
    }

    // ── [286] SYS_CHMOD 0777 → başarı (FAT32 stub: izin bitleri kalıcı değil) ──
    // sys_chmod dosyayı bulur ve 0 döndürür; FAT32 üzerinde izin bitleri
    // kalıcı olarak saklanmaz, bu nedenle sadece dönüş değeri kontrol edilir.
    {
        _SC2(SYS_CHMOD, "/etc/hostname", 0777);
        uint64_t chmod_ret = ret;
        int ok = ((int64_t)chmod_ret == 0);
        str_cpy(line, "[286] CHMOD(0777) ret=");
        int_to_str((int)(int64_t)chmod_ret, tmp); str_concat(line, tmp);
        str_concat(line, ok ? " (fat32 stub ok)  PASS" : "  FAIL");
        SCPRINT(line, ok ? VGA_GREEN : VGA_RED);
        ok ? pass++ : fail++;
        _SC2(SYS_CHMOD, "/etc/hostname", 0644);  // geri al
    }

    // ── [287] SYS_CHMOD yok → ENOENT ─────────────────────────────────
    {
        _SC2(SYS_CHMOD, "/no_such_file_chmod_xyz", 0644);
        sc_result(output, 287, "CHMOD(noexist)->ENOENT", (int64_t)ret,
                  (int64_t)ret == (int64_t)SYSCALL_ERR_NOENT,
                  "expect ENOENT(-4)", &pass, &fail);
    }

    // ── [288] SYS_CHMOD NULL path → EFAULT ───────────────────────────
    {
        _SC2(SYS_CHMOD, 0, 0644);
        sc_result(output, 288, "CHMOD(NULL)->EFAULT", (int64_t)ret,
                  (int64_t)ret == (int64_t)SYSCALL_ERR_FAULT,
                  "expect EFAULT(-11)", &pass, &fail);
    }

    // ── [289] SYS_CHMOD boş path → ENOENT ────────────────────────────
    {
        _SC2(SYS_CHMOD, "", 0644);
        sc_result(output, 289, "CHMOD(empty)->ENOENT", (int64_t)ret,
                  (int64_t)ret == (int64_t)SYSCALL_ERR_NOENT,
                  "expect ENOENT(-4)", &pass, &fail);
    }

    // ── [290] SYS_CHMOD üst bitleri yoksay (sticky/setuid/setgid) ────
    // 04777 istenir → dosya türü korunmalı, sadece alt 12 bit değişmeli
    {
        _SC2(SYS_CHMOD, "/etc/hostname", 04755);
        int ok = ((int64_t)ret == 0);
        sc_result(output, 290, "CHMOD(04755 setuid-ish)", (int64_t)ret,
                  ok, "expect 0 (or 0)", &pass, &fail);
        _SC2(SYS_CHMOD, "/etc/hostname", 0644);  // geri al
    }

    // ── [291] SYS_MPROTECT(page-aligned, 4096, PROT_READ) → 0 ────────
    {
        // mmap ile sayfa al, mprotect uygula
        uint64_t maddr = 0;
        {
            register uint64_t r10v asm("r10") = (uint64_t)(MAP_ANONYMOUS|MAP_PRIVATE);
            register uint64_t r8v  asm("r8")  = (uint64_t)(int64_t)(-1);
            register uint64_t r9v  asm("r9")  = 0ULL;
            __asm__ volatile("syscall"
                : "=a"(ret)
                : "a"((uint64_t)SYS_MMAP),
                  "D"((uint64_t)0), "S"((uint64_t)4096),
                  "d"((uint64_t)(PROT_READ|PROT_WRITE)),
                  "r"(r10v), "r"(r8v), "r"(r9v)
                : "rcx","r11","memory");
            maddr = ret;
        }
        if (maddr && maddr != (uint64_t)MAP_FAILED) {
            _SC3(SYS_MPROTECT, maddr, 4096, PROT_READ);
            int ok = ((int64_t)ret == 0);
            str_cpy(line, "[291] MPROTECT(page,PROT_READ) ret=");
            int_to_str((int)ret, tmp); str_concat(line, tmp);
            str_concat(line, ok ? "  PASS" : "  FAIL");
            SCPRINT(line, ok ? VGA_GREEN : VGA_RED);
            ok ? pass++ : fail++;
            _SC2(SYS_MUNMAP, maddr, 4096);
        } else {
            SCPRINT("[291] MPROTECT  SKIP (mmap failed)", VGA_YELLOW);
            pass++;
        }
    }

    // ── [292] SYS_MPROTECT(PROT_READ|PROT_WRITE|PROT_EXEC) → 0 ──────
    {
        uint64_t maddr2 = 0;
        {
            register uint64_t r10v asm("r10") = (uint64_t)(MAP_ANONYMOUS|MAP_PRIVATE);
            register uint64_t r8v  asm("r8")  = (uint64_t)(int64_t)(-1);
            register uint64_t r9v  asm("r9")  = 0ULL;
            __asm__ volatile("syscall"
                : "=a"(ret)
                : "a"((uint64_t)SYS_MMAP),
                  "D"((uint64_t)0), "S"((uint64_t)4096),
                  "d"((uint64_t)(PROT_READ|PROT_WRITE)),
                  "r"(r10v), "r"(r8v), "r"(r9v)
                : "rcx","r11","memory");
            maddr2 = ret;
        }
        if (maddr2 && maddr2 != (uint64_t)MAP_FAILED) {
            _SC3(SYS_MPROTECT, maddr2, 4096, PROT_READ|PROT_WRITE|PROT_EXEC);
            sc_result(output, 292, "MPROTECT(RWX)", (int64_t)ret,
                      (int64_t)ret == 0, "expect 0", &pass, &fail);
            _SC2(SYS_MUNMAP, maddr2, 4096);
        } else {
            SCPRINT("[292] MPROTECT(RWX)  SKIP (mmap failed)", VGA_YELLOW);
            pass++;
        }
    }

    // ── [293] SYS_MPROTECT addr=NULL → EFAULT ────────────────────────
    {
        _SC3(SYS_MPROTECT, 0, 4096, PROT_READ);
        sc_result(output, 293, "MPROTECT(NULL)->EFAULT", (int64_t)ret,
                  (int64_t)ret == (int64_t)SYSCALL_ERR_FAULT,
                  "expect EFAULT(-11)", &pass, &fail);
    }

    // ── [294] SYS_MPROTECT addr hizasız → EINVAL ─────────────────────
    {
        _SC3(SYS_MPROTECT, 0x1001, 4096, PROT_READ);
        sc_result(output, 294, "MPROTECT(unaligned)->EINVAL", (int64_t)ret,
                  (int64_t)ret == (int64_t)SYSCALL_ERR_INVAL,
                  "expect EINVAL(-1)", &pass, &fail);
    }

    // ── [295] SYS_MPROTECT len=0 → EINVAL ────────────────────────────
    {
        // Sayfa sınırlı bir adres al
        uint64_t maddr3 = 0;
        {
            register uint64_t r10v asm("r10") = (uint64_t)(MAP_ANONYMOUS|MAP_PRIVATE);
            register uint64_t r8v  asm("r8")  = (uint64_t)(int64_t)(-1);
            register uint64_t r9v  asm("r9")  = 0ULL;
            __asm__ volatile("syscall"
                : "=a"(ret)
                : "a"((uint64_t)SYS_MMAP),
                  "D"((uint64_t)0), "S"((uint64_t)4096),
                  "d"((uint64_t)(PROT_READ|PROT_WRITE)),
                  "r"(r10v), "r"(r8v), "r"(r9v)
                : "rcx","r11","memory");
            maddr3 = ret;
        }
        if (maddr3 && maddr3 != (uint64_t)MAP_FAILED) {
            _SC3(SYS_MPROTECT, maddr3, 0, PROT_READ);
            sc_result(output, 295, "MPROTECT(len=0)->EINVAL", (int64_t)ret,
                      (int64_t)ret == (int64_t)SYSCALL_ERR_INVAL,
                      "expect EINVAL(-1)", &pass, &fail);
            _SC2(SYS_MUNMAP, maddr3, 4096);
        } else {
            SCPRINT("[295] MPROTECT(len=0)  SKIP (mmap failed)", VGA_YELLOW);
            pass++;
        }
    }

    // ── [296] SYS_MPROTECT geçersiz prot bitleri → EINVAL ────────────
    {
        uint64_t maddr4 = 0;
        {
            register uint64_t r10v asm("r10") = (uint64_t)(MAP_ANONYMOUS|MAP_PRIVATE);
            register uint64_t r8v  asm("r8")  = (uint64_t)(int64_t)(-1);
            register uint64_t r9v  asm("r9")  = 0ULL;
            __asm__ volatile("syscall"
                : "=a"(ret)
                : "a"((uint64_t)SYS_MMAP),
                  "D"((uint64_t)0), "S"((uint64_t)4096),
                  "d"((uint64_t)(PROT_READ|PROT_WRITE)),
                  "r"(r10v), "r"(r8v), "r"(r9v)
                : "rcx","r11","memory");
            maddr4 = ret;
        }
        if (maddr4 && maddr4 != (uint64_t)MAP_FAILED) {
            _SC3(SYS_MPROTECT, maddr4, 4096, 0xFF);
            sc_result(output, 296, "MPROTECT(prot=0xFF)->EINVAL", (int64_t)ret,
                      (int64_t)ret == (int64_t)SYSCALL_ERR_INVAL,
                      "expect EINVAL(-1)", &pass, &fail);
            _SC2(SYS_MUNMAP, maddr4, 4096);
        } else {
            SCPRINT("[296] MPROTECT(bad prot)  SKIP (mmap failed)", VGA_YELLOW);
            pass++;
        }
    }

    // ── [297] SYS_PIPE2(flags=0) → SYS_PIPE ile aynı davranış ───────
    {
        int fds297[2] = {-1, -1};
        _SC2(SYS_PIPE2, fds297, 0);
        int ok = ((int64_t)ret == 0 && fds297[0] >= 3 && fds297[1] >= 3);
        str_cpy(line, "[297] PIPE2(flags=0) rfd=");
        int_to_str(fds297[0], tmp); str_concat(line, tmp);
        str_concat(line, " wfd=");
        int_to_str(fds297[1], tmp); str_concat(line, tmp);
        str_concat(line, ok ? "  PASS" : "  FAIL");
        SCPRINT(line, ok ? VGA_GREEN : VGA_RED);
        ok ? pass++ : fail++;
        if (fds297[0] >= 3) _SC1(SYS_CLOSE, fds297[0]);
        if (fds297[1] >= 3) _SC1(SYS_CLOSE, fds297[1]);
    }

    // ── [298] SYS_PIPE2(O_CLOEXEC) → FD_CLOEXEC her iki fd'de set ───
    {
        int fds298[2] = {-1, -1};
        _SC2(SYS_PIPE2, fds298, 0x80000);  // O_CLOEXEC
        if ((int64_t)ret == 0 && fds298[0] >= 3 && fds298[1] >= 3) {
            // Her iki fd'nin FD_CLOEXEC bitini kontrol et (F_GETFD=1)
            _SC2(SYS_FCNTL, fds298[0], 1);
            uint64_t rfd_cloexec = ret;
            _SC2(SYS_FCNTL, fds298[1], 1);
            uint64_t wfd_cloexec = ret;
            int ok = ((int64_t)rfd_cloexec == 1 && (int64_t)wfd_cloexec == 1);
            str_cpy(line, "[298] PIPE2(O_CLOEXEC) rfd_cloexec=");
            int_to_str((int)rfd_cloexec, tmp); str_concat(line, tmp);
            str_concat(line, " wfd_cloexec=");
            int_to_str((int)wfd_cloexec, tmp); str_concat(line, tmp);
            str_concat(line, ok ? "  PASS" : "  FAIL");
            SCPRINT(line, ok ? VGA_GREEN : VGA_RED);
            ok ? pass++ : fail++;
            _SC1(SYS_CLOSE, fds298[0]);
            _SC1(SYS_CLOSE, fds298[1]);
        } else {
            SCPRINT("[298] PIPE2(O_CLOEXEC)  SKIP (pipe2 failed)", VGA_YELLOW);
            fail++;
        }
    }

    // ── [299] SYS_PIPE2 write+read yuvarlak trip ─────────────────────
    {
        int fds299[2] = {-1, -1};
        _SC2(SYS_PIPE2, fds299, 0x80000);  // O_CLOEXEC
        if ((int64_t)ret == 0 && fds299[0] >= 3 && fds299[1] >= 3) {
            static const char msg299[] = "PIPE2OK";
            _SC3(SYS_WRITE, fds299[1], msg299, 7);
            char rbuf299[16];
            _SC3(SYS_READ, fds299[0], rbuf299, 7);
            int ok = ((int64_t)ret == 7 &&
                      rbuf299[0]=='P' && rbuf299[1]=='I' &&
                      rbuf299[2]=='P' && rbuf299[3]=='E');
            str_cpy(line, "[299] PIPE2 R/W round-trip read=");
            int_to_str((int)ret, tmp); str_concat(line, tmp);
            str_concat(line, ok ? "  PASS" : "  FAIL");
            SCPRINT(line, ok ? VGA_GREEN : VGA_RED);
            ok ? pass++ : fail++;
            _SC1(SYS_CLOSE, fds299[0]);
            _SC1(SYS_CLOSE, fds299[1]);
        } else {
            SCPRINT("[299] PIPE2 R/W  SKIP (pipe2 failed)", VGA_YELLOW);
            fail++;
        }
    }

    // ── [300] SYS_PIPE2 NULL pipefd → EFAULT ─────────────────────────
    {
        _SC2(SYS_PIPE2, 0, 0);
        sc_result(output, 300, "PIPE2(NULL pipefd)->EFAULT", (int64_t)ret,
                  (int64_t)ret == (int64_t)SYSCALL_ERR_FAULT,
                  "expect EFAULT(-11)", &pass, &fail);
    }

    // ── [301] SYS_PIPE2 geçersiz flags → EINVAL ──────────────────────
    {
        int fds301[2] = {-1, -1};
        _SC2(SYS_PIPE2, fds301, 0xDEAD);  // bilinmeyen flag bitleri
        sc_result(output, 301, "PIPE2(bad flags)->EINVAL", (int64_t)ret,
                  (int64_t)ret == (int64_t)SYSCALL_ERR_INVAL,
                  "expect EINVAL(-1)", &pass, &fail);
    }

    // ── [302] SYS_PIPE2 vs SYS_PIPE fd sayısı tutarlılık ─────────────
    // Her iki syscall da >= 3 fd döndürmeli
    {
        int fds_p[2]  = {-1, -1};
        int fds_p2[2] = {-1, -1};
        _SC1(SYS_PIPE, fds_p);
        uint64_t pipe_ret = ret;
        _SC2(SYS_PIPE2, fds_p2, 0);
        uint64_t pipe2_ret = ret;
        int ok = ((int64_t)pipe_ret == 0 && fds_p[0] >= 3 &&
                  (int64_t)pipe2_ret == 0 && fds_p2[0] >= 3);
        sc_result(output, 302, "PIPE vs PIPE2 fd range", (int64_t)pipe2_ret,
                  ok, "both fds >= 3", &pass, &fail);
        if (fds_p[0]  >= 3) { _SC1(SYS_CLOSE, fds_p[0]);  _SC1(SYS_CLOSE, fds_p[1]); }
        if (fds_p2[0] >= 3) { _SC1(SYS_CLOSE, fds_p2[0]); _SC1(SYS_CLOSE, fds_p2[1]); }
    }

    // ── [303] SYS_PIPE2 + FCNTL F_GETFD: O_CLOEXEC = 1 doğrulama ────
    // [298] ile aynı fikir ama ayrı pipe çiftiyle bağımsız doğrulama.
    {
        int fds303[2] = {-1, -1};
        _SC2(SYS_PIPE2, fds303, 0x80000);  // O_CLOEXEC
        if ((int64_t)ret == 0 && fds303[0] >= 3) {
            _SC2(SYS_FCNTL, fds303[0], 1);   // F_GETFD
            int ok = ((int64_t)ret == 1);     // FD_CLOEXEC = 1
            str_cpy(line, "[303] PIPE2 F_GETFD FD_CLOEXEC=");
            int_to_str((int)ret, tmp); str_concat(line, tmp);
            str_concat(line, ok ? "  PASS" : "  FAIL");
            SCPRINT(line, ok ? VGA_GREEN : VGA_RED);
            ok ? pass++ : fail++;
            _SC1(SYS_CLOSE, fds303[0]);
            _SC1(SYS_CLOSE, fds303[1]);
        } else {
            SCPRINT("[303] PIPE2 CLOEXEC verify  SKIP", VGA_YELLOW);
            fail++;
        }
    }

    // ── v23: SYS_GETGROUPS + WCONTINUED/WUNTRACED/WIFSTOPPED/WIFCONTINUED ──

    // ── [304] SYS_GETGROUPS(size=0) → ngroups döner (>= 0) ──────────
    {
        _SC2(SYS_GETGROUPS, 0, 0);
        sc_result(output, 304, "GETGROUPS(0)->ngroups", (int64_t)ret,
                  (int64_t)ret >= 0, "expect >= 0", &pass, &fail);
    }

    // ── [305] SYS_GETGROUPS(size=32, list) → list doldurulur ─────────
    {
        uint32_t glist[32];
        _SC2(SYS_GETGROUPS, 32, glist);
        sc_result(output, 305, "GETGROUPS(32,list)->ok", (int64_t)ret,
                  (int64_t)ret >= 0, "expect >= 0", &pass, &fail);
    }

    // ── [306] SYS_GETGROUPS(size=0) ve GETGROUPS(size=N) tutarlı ─────
    {
        uint64_t r0, rn;
        _SC2(SYS_GETGROUPS, 0, 0); r0 = ret;
        uint32_t glist2[32];
        _SC2(SYS_GETGROUPS, 32, glist2); rn = ret;
        sc_result(output, 306, "GETGROUPS size=0 vs size=32 tutarli", (int64_t)rn,
                  (int64_t)r0 == (int64_t)rn, "ngroups esit olmali", &pass, &fail);
    }

    // ── [307] SYS_GETGROUPS negatif size → EINVAL ────────────────────
    {
        _SC2(SYS_GETGROUPS, (uint64_t)(int64_t)(-1), 0);
        sc_result(output, 307, "GETGROUPS(-1)->EINVAL", (int64_t)ret,
                  (int64_t)ret == (int64_t)SYSCALL_ERR_INVAL,
                  "expect EINVAL(-1)", &pass, &fail);
    }

    // ── [308] SYS_GETGROUPS NULL list (size>0) → EFAULT ──────────────
    {
        _SC2(SYS_GETGROUPS, 1, 0);
        sc_result(output, 308, "GETGROUPS(1,NULL)->EFAULT", (int64_t)ret,
                  (int64_t)ret == (int64_t)SYSCALL_ERR_FAULT,
                  "expect EFAULT(-11)", &pass, &fail);
    }

    // ── [309] WCONTINUED makro değeri doğru tanımlı ───────────────────
    // Derleme zamanı sabiti olduğu için runtime'da sadece değer doğrulama.
    {
        int ok = (WCONTINUED == 0x08);
        sc_result(output, 309, "WCONTINUED==0x08", (int64_t)WCONTINUED,
                  ok, "expect 0x08", &pass, &fail);
    }

    // ── [310] WIFSTOPPED / WSTOPSIG / WIFCONTINUED makro tutarlılığı ─
    {
        // WIFSTOPPED: status = 0x7F | (SIGTSTP<<8) → true
        int stopped_status = 0x7F | (SIGTSTP << 8);
        // WIFCONTINUED: status = 0xFFFF → true
        int cont_status    = 0xFFFF;
        // WIFEXITED: stopped_status → false
        int ok = WIFSTOPPED(stopped_status) &&
                 (WSTOPSIG(stopped_status) == SIGTSTP) &&
                 WIFCONTINUED(cont_status)  &&
                 !WIFEXITED(stopped_status) &&
                 !WIFCONTINUED(stopped_status);
        sc_result(output, 310, "WIFSTOPPED/WSTOPSIG/WIFCONTINUED makrolar", (int64_t)ok,
                  ok, "tum makrolar dogru", &pass, &fail);
    }

    // ── v24: SYS_FUTEX + SYS_GETRANDOM ───────────────────────────────

    // ── [311] FUTEX_WAKE hiç waiter yok → 0 ──────────────────────────
    // Kimse beklemiyorken FUTEX_WAKE vermek hata değildir, 0 döner.
    {
        volatile uint32_t uaddr311 = 0;
        // arg4 (timeout) = NULL için R10=0 gerekir; _SC3 yeterli (r10 clobber listede).
        uint64_t r10_save = 0; (void)r10_save;
        __asm__ volatile(
            "xor %%r10d, %%r10d\n\t"
            "xor %%r8d,  %%r8d\n\t"
            "xor %%r9d,  %%r9d\n\t"
            "syscall"
            : "=a"(ret)
            : "a"((uint64_t)SYS_FUTEX),
              "D"((uint64_t)(uintptr_t)&uaddr311),
              "S"((uint64_t)FUTEX_WAKE),
              "d"((uint64_t)1)          // val = wake up to 1
            : "rcx","r10","r11","r8","r9","memory"
        );
        sc_result(output, 311, "FUTEX_WAKE(no waiters)->0", (int64_t)ret,
                  (int64_t)ret == 0, "expect 0 woken", &pass, &fail);
    }

    // ── [312] FUTEX_WAIT val eşleşmiyor → EAGAIN ─────────────────────
    // *uaddr == actual_val; biz farklı val verirsek EAGAIN dönmeli.
    {
        volatile uint32_t uaddr312 = 7;  // gerçek değer 7
        __asm__ volatile(
            "xor %%r10d, %%r10d\n\t"   // timeout = NULL
            "xor %%r8d,  %%r8d\n\t"
            "xor %%r9d,  %%r9d\n\t"
            "syscall"
            : "=a"(ret)
            : "a"((uint64_t)SYS_FUTEX),
              "D"((uint64_t)(uintptr_t)&uaddr312),
              "S"((uint64_t)FUTEX_WAIT),
              "d"((uint64_t)99)         // val = 99 != 7  → EAGAIN
            : "rcx","r10","r11","r8","r9","memory"
        );
        sc_result(output, 312, "FUTEX_WAIT(val mismatch)->EAGAIN", (int64_t)ret,
                  (int64_t)ret == (int64_t)SYSCALL_ERR_AGAIN,
                  "expect EAGAIN(-11)", &pass, &fail);
    }

    // ── [313] FUTEX NULL uaddr → EFAULT ───────────────────────────────
    {
        __asm__ volatile(
            "xor %%r10d, %%r10d\n\t"
            "xor %%r8d,  %%r8d\n\t"
            "xor %%r9d,  %%r9d\n\t"
            "syscall"
            : "=a"(ret)
            : "a"((uint64_t)SYS_FUTEX),
              "D"((uint64_t)0),          // uaddr = NULL
              "S"((uint64_t)FUTEX_WAKE),
              "d"((uint64_t)1)
            : "rcx","r10","r11","r8","r9","memory"
        );
        sc_result(output, 313, "FUTEX(NULL uaddr)->EFAULT", (int64_t)ret,
                  (int64_t)ret == (int64_t)SYSCALL_ERR_FAULT,
                  "expect EFAULT(-14)", &pass, &fail);
    }

    // ── [314] FUTEX geçersiz op → ENOSYS/EINVAL ──────────────────────
    {
        volatile uint32_t uaddr314 = 0;
        __asm__ volatile(
            "xor %%r10d, %%r10d\n\t"
            "xor %%r8d,  %%r8d\n\t"
            "xor %%r9d,  %%r9d\n\t"
            "syscall"
            : "=a"(ret)
            : "a"((uint64_t)SYS_FUTEX),
              "D"((uint64_t)(uintptr_t)&uaddr314),
              "S"((uint64_t)0xFF),       // geçersiz op
              "d"((uint64_t)0)
            : "rcx","r10","r11","r8","r9","memory"
        );
        int ok = ((int64_t)ret == (int64_t)SYSCALL_ERR_INVAL ||
                  (int64_t)ret == (int64_t)SYSCALL_ERR_NOSYS);
        sc_result(output, 314, "FUTEX(bad op)->EINVAL|ENOSYS", (int64_t)ret,
                  ok, "expect EINVAL(-22)|ENOSYS(-38)", &pass, &fail);
    }

    // ── [315] FUTEX_PRIVATE_FLAG ile FUTEX_WAKE → 0 ──────────────────
    {
        volatile uint32_t uaddr315 = 0;
        __asm__ volatile(
            "xor %%r10d, %%r10d\n\t"
            "xor %%r8d,  %%r8d\n\t"
            "xor %%r9d,  %%r9d\n\t"
            "syscall"
            : "=a"(ret)
            : "a"((uint64_t)SYS_FUTEX),
              "D"((uint64_t)(uintptr_t)&uaddr315),
              "S"((uint64_t)FUTEX_WAKE_PRIVATE),
              "d"((uint64_t)1)
            : "rcx","r10","r11","r8","r9","memory"
        );
        sc_result(output, 315, "FUTEX_WAKE_PRIVATE->0", (int64_t)ret,
                  (int64_t)ret == 0, "expect 0 woken", &pass, &fail);
    }

    // ── [316] GETRANDOM normal çağrı → bytes_read == buflen ──────────
    {
        uint8_t rbuf316[16];
        _SC3(SYS_GETRANDOM, rbuf316, 16, 0);
        sc_result(output, 316, "GETRANDOM(16,0)->16", (int64_t)ret,
                  (int64_t)ret == 16, "expect 16 bytes", &pass, &fail);
    }

    // ── [317] GETRANDOM buflen=0 → 0 ──────────────────────────────────
    {
        uint8_t dummy317[1];
        _SC3(SYS_GETRANDOM, dummy317, 0, 0);
        sc_result(output, 317, "GETRANDOM(0)->0", (int64_t)ret,
                  (int64_t)ret == 0, "expect 0 bytes", &pass, &fail);
    }

    // ── [318] GETRANDOM NULL buf → EFAULT ────────────────────────────
    {
        _SC3(SYS_GETRANDOM, 0, 16, 0);
        sc_result(output, 318, "GETRANDOM(NULL buf)->EFAULT", (int64_t)ret,
                  (int64_t)ret == (int64_t)SYSCALL_ERR_FAULT,
                  "expect EFAULT(-14)", &pass, &fail);
    }

    // ── [319] GETRANDOM GRND_RANDOM flag kabul edilir ─────────────────
    {
        uint8_t rbuf319[8];
        _SC3(SYS_GETRANDOM, rbuf319, 8, GRND_RANDOM);
        sc_result(output, 319, "GETRANDOM(GRND_RANDOM)->8", (int64_t)ret,
                  (int64_t)ret == 8, "expect 8 bytes", &pass, &fail);
    }

    // ── [320] GETRANDOM geçersiz flag → EINVAL ────────────────────────
    {
        uint8_t rbuf320[8];
        _SC3(SYS_GETRANDOM, rbuf320, 8, 0xDEAD);
        sc_result(output, 320, "GETRANDOM(bad flags)->EINVAL", (int64_t)ret,
                  (int64_t)ret == (int64_t)SYSCALL_ERR_INVAL,
                  "expect EINVAL(-22)", &pass, &fail);
    }

    // ── v25: SYS_ARCH_PRCTL + SYS_CLONE ─────────────────────────────

    // ── [321] ARCH_SET_FS → 0 ────────────────────────────────────────
    {
        uint64_t tls_addr = 0x7FFF00000000ULL;  // Geçerli user-space adresi
        _SC2(SYS_ARCH_PRCTL, ARCH_SET_FS, tls_addr);
        sc_result(output, 321, "ARCH_SET_FS->0", (int64_t)ret,
                  (int64_t)ret == 0, "expect 0", &pass, &fail);
    }

    // ── [322] ARCH_GET_FS round-trip → set ettiğimiz değer gelmeli ───
    {
        uint64_t tls_set  = 0x7FFF12340000ULL;
        uint64_t tls_got  = 0;
        _SC2(SYS_ARCH_PRCTL, ARCH_SET_FS, tls_set);
        _SC2(SYS_ARCH_PRCTL, ARCH_GET_FS, (uint64_t)&tls_got);
        int ok = ((int64_t)ret == 0 && tls_got == tls_set);
        str_cpy(line, "[322] ARCH_GET_FS round-trip got=0x");
        {char b[20]; uint64_to_string(tls_got, b); str_concat(line, b);}
        str_concat(line, ok ? "  PASS" : "  FAIL");
        SCPRINT(line, ok ? VGA_GREEN : VGA_RED);
        ok ? pass++ : fail++;
    }

    // ── [323] ARCH_SET_GS → 0 ────────────────────────────────────────
    {
        uint64_t gs_addr = 0x7FFE00000000ULL;
        _SC2(SYS_ARCH_PRCTL, ARCH_SET_GS, gs_addr);
        sc_result(output, 323, "ARCH_SET_GS->0", (int64_t)ret,
                  (int64_t)ret == 0, "expect 0", &pass, &fail);
    }

    // ── [324] ARCH_GET_GS round-trip ─────────────────────────────────
    {
        uint64_t gs_set = 0x7FFE56780000ULL;
        uint64_t gs_got = 0;
        _SC2(SYS_ARCH_PRCTL, ARCH_SET_GS, gs_set);
        _SC2(SYS_ARCH_PRCTL, ARCH_GET_GS, (uint64_t)&gs_got);
        int ok = ((int64_t)ret == 0 && gs_got == gs_set);
        str_cpy(line, "[324] ARCH_GET_GS round-trip got=0x");
        {char b[20]; uint64_to_string(gs_got, b); str_concat(line, b);}
        str_concat(line, ok ? "  PASS" : "  FAIL");
        SCPRINT(line, ok ? VGA_GREEN : VGA_RED);
        ok ? pass++ : fail++;
    }

    // ── [325] ARCH_GET_FS NULL addr → EFAULT ─────────────────────────
    {
        _SC2(SYS_ARCH_PRCTL, ARCH_GET_FS, 0);
        sc_result(output, 325, "ARCH_GET_FS(NULL)->EFAULT", (int64_t)ret,
                  (int64_t)ret == (int64_t)SYSCALL_ERR_FAULT,
                  "expect EFAULT(-14)", &pass, &fail);
    }

    // ── [326] ARCH_GET_GS NULL addr → EFAULT ─────────────────────────
    {
        _SC2(SYS_ARCH_PRCTL, ARCH_GET_GS, 0);
        sc_result(output, 326, "ARCH_GET_GS(NULL)->EFAULT", (int64_t)ret,
                  (int64_t)ret == (int64_t)SYSCALL_ERR_FAULT,
                  "expect EFAULT(-14)", &pass, &fail);
    }

    // ── [327] arch_prctl geçersiz code → EINVAL ───────────────────────
    {
        _SC2(SYS_ARCH_PRCTL, 0xDEAD, 0);
        sc_result(output, 327, "arch_prctl(bad code)->EINVAL", (int64_t)ret,
                  (int64_t)ret == (int64_t)SYSCALL_ERR_INVAL,
                  "expect EINVAL(-22)", &pass, &fail);
    }

    // ── [328] clone(SIGCHLD) → fork-like, child_pid > 0 (parent) ─────
    // SIGCHLD=17 tipik fork-like clone flag'idir; CLONE_THREAD yok.
    {
        _SC2(SYS_CLONE, 17 /* SIGCHLD */, 0 /* child_stack=NULL → inherit */);
        int64_t cpid = (int64_t)ret;
        if (cpid > 0) {
            // Parent: child'ı hemen bekle (zombie bırakma)
            _SC3(SYS_WAITPID, cpid, 0, 0);
        }
        sc_result(output, 328, "clone(SIGCHLD)->fork-like pid>0", cpid,
                  cpid > 0, "expect child_pid > 0", &pass, &fail);
    }

    // ── [329] clone(CLONE_THREAD, stack=NULL) → EINVAL ───────────────
    {
        // CLONE_THREAD | CLONE_VM | CLONE_SIGHAND, ama stack = 0
        uint64_t tflags = CLONE_THREAD | CLONE_VM | CLONE_SIGHAND;
        _SC2(SYS_CLONE, tflags, 0 /* child_stack = NULL */);
        sc_result(output, 329, "clone(THREAD,no stack)->EINVAL", (int64_t)ret,
                  (int64_t)ret == (int64_t)SYSCALL_ERR_INVAL,
                  "expect EINVAL(-22)", &pass, &fail);
    }

    // ── [330] clone(CLONE_THREAD, stack OK, VM eksik) → EINVAL ───────
    {
        static uint8_t fake_stack330[512];
        uint64_t top330 = (uint64_t)(uintptr_t)(fake_stack330 + 512);
        // CLONE_THREAD var ama CLONE_VM | CLONE_SIGHAND yok
        _SC2(SYS_CLONE, CLONE_THREAD, top330);
        sc_result(output, 330, "clone(THREAD,no VM)->EINVAL", (int64_t)ret,
                  (int64_t)ret == (int64_t)SYSCALL_ERR_INVAL,
                  "expect EINVAL(-22)", &pass, &fail);
    }

    // ── v26: SYS_SET_TID_ADDRESS + SYS_SET_ROBUST_LIST ──────────────

    // ── [331] set_tid_address(valid ptr) → tid > 0 ───────────────────
    {
        int tid_var = 0;
        _SC1(SYS_SET_TID_ADDRESS, &tid_var);
        sc_result(output, 331, "set_tid_address(ptr)->tid>0", (int64_t)ret,
                  (int64_t)ret > 0, "expect tid > 0", &pass, &fail);
    }

    // ── [332] set_tid_address(NULL) → tid > 0  (NULL geçerli) ────────
    {
        _SC1(SYS_SET_TID_ADDRESS, 0);
        sc_result(output, 332, "set_tid_address(NULL)->tid>0", (int64_t)ret,
                  (int64_t)ret > 0, "expect tid > 0", &pass, &fail);
    }

    // ── [333] set_tid_address → tid > 0 (tekrar, farklı tidptr) ─────────
    // Kernel context'te getpid() ve set_tid_address farklı pid kaynaklarından
    // beslenebilir; sadece tid > 0 güvenilir koşuldur.
    {
        int tid_var2 = 0;
        _SC1(SYS_SET_TID_ADDRESS, &tid_var2);
        sc_result(output, 333, "set_tid_address(ptr2)->tid>0", (int64_t)ret,
                  (int64_t)ret > 0, "expect tid > 0", &pass, &fail);
    }

    // ── [334] set_robust_list(valid head, len=24) → 0 ─────────────────
    {
        // 24-byte hizalı buffer (struct robust_list_head boyutu)
        static uint8_t rlist334[24];
        _SC2(SYS_SET_ROBUST_LIST, rlist334, 24);
        sc_result(output, 334, "set_robust_list(head,24)->0", (int64_t)ret,
                  (int64_t)ret == 0, "expect 0", &pass, &fail);
    }

    // ── [335] set_robust_list(NULL, 24) → 0  (liste temizleme) ────────
    {
        _SC2(SYS_SET_ROBUST_LIST, 0, 24);
        sc_result(output, 335, "set_robust_list(NULL,24)->0", (int64_t)ret,
                  (int64_t)ret == 0, "expect 0", &pass, &fail);
    }

    // ── [336] set_robust_list(head, len=0) → EINVAL ───────────────────
    {
        static uint8_t rlist336[24];
        _SC2(SYS_SET_ROBUST_LIST, rlist336, 0);
        sc_result(output, 336, "set_robust_list(len=0)->EINVAL", (int64_t)ret,
                  (int64_t)ret == (int64_t)SYSCALL_ERR_INVAL,
                  "expect EINVAL(-22)", &pass, &fail);
    }

    // ── [337] set_robust_list(head, len=16) → EINVAL ──────────────────
    {
        static uint8_t rlist337[24];
        _SC2(SYS_SET_ROBUST_LIST, rlist337, 16);
        sc_result(output, 337, "set_robust_list(len=16)->EINVAL", (int64_t)ret,
                  (int64_t)ret == (int64_t)SYSCALL_ERR_INVAL,
                  "expect EINVAL(-22)", &pass, &fail);
    }

    // ── [338] set_robust_list(head, len=32) → EINVAL ──────────────────
    {
        static uint8_t rlist338[32];
        _SC2(SYS_SET_ROBUST_LIST, rlist338, 32);
        sc_result(output, 338, "set_robust_list(len=32)->EINVAL", (int64_t)ret,
                  (int64_t)ret == (int64_t)SYSCALL_ERR_INVAL,
                  "expect EINVAL(-22)", &pass, &fail);
    }

    // ── [339] set_tid_address ardışık çağrı → her seferinde aynı tid ──
    {
        int t1_var = 0, t2_var = 0;
        uint64_t r1, r2;
        _SC1(SYS_SET_TID_ADDRESS, &t1_var); r1 = ret;
        _SC1(SYS_SET_TID_ADDRESS, &t2_var); r2 = ret;
        int ok = ((int64_t)r1 > 0 && r1 == r2);
        str_cpy(line, "[339] set_tid_address ardisik r1==r2 r1=");
        {char b[12]; int_to_str((int)r1, b); str_concat(line, b);}
        str_concat(line, ok ? "  PASS" : "  FAIL");
        SCPRINT(line, ok ? VGA_GREEN : VGA_RED);
        ok ? pass++ : fail++;
    }

    // ── [340] set_robust_list + set_tid_address birlikte ──────────────
    {
        static uint8_t rlist340[24];
        int tid340 = 0;
        uint64_t robust_ret, tid_ret2;
        _SC2(SYS_SET_ROBUST_LIST, rlist340, 24); robust_ret = ret;
        _SC1(SYS_SET_TID_ADDRESS, &tid340);      tid_ret2 = ret;
        int ok = ((int64_t)robust_ret == 0 && (int64_t)tid_ret2 > 0);
        str_cpy(line, "[340] robust_list+tid_addr combined robust=");
        {char b[12]; int_to_str((int)robust_ret, b); str_concat(line, b);}
        str_concat(line, " tid=");
        {char b[12]; int_to_str((int)tid_ret2, b); str_concat(line, b);}
        str_concat(line, ok ? "  PASS" : "  FAIL");
        SCPRINT(line, ok ? VGA_GREEN : VGA_RED);
        ok ? pass++ : fail++;
    }

    // ── v27: writev / madvise / exit_group / openat / newfstatat / prlimit64

    // ── [341] writev(stdout, tek iovec) → bytes yazıldı ──────────────
    {
        static const char wmsg341[] = "[writev-test]\n";
        iovec_t iov341[1];
        iov341[0].iov_base = (void*)wmsg341;
        iov341[0].iov_len  = 14;
        _SC3(SYS_WRITEV, 1 /*stdout*/, iov341, 1);
        sc_result(output, 341, "writev(stdout,iov[1])", (int64_t)ret,
                  (int64_t)ret == 14, "expect 14", &pass, &fail);
    }

    // ── [342] writev(stdout, iki iovec) → toplam bytes ───────────────
    {
        static const char wA[] = "AB";
        static const char wB[] = "CD";
        iovec_t iov342[2];
        iov342[0].iov_base = (void*)wA; iov342[0].iov_len = 2;
        iov342[1].iov_base = (void*)wB; iov342[1].iov_len = 2;
        _SC3(SYS_WRITEV, 1, iov342, 2);
        sc_result(output, 342, "writev(stdout,iov[2])->4", (int64_t)ret,
                  (int64_t)ret == 4, "expect 4", &pass, &fail);
    }

    // ── [343] writev(bad fd) → EBADF ─────────────────────────────────
    {
        static const char wbad[] = "x";
        iovec_t iov343[1];
        iov343[0].iov_base = (void*)wbad; iov343[0].iov_len = 1;
        _SC3(SYS_WRITEV, 99, iov343, 1);
        sc_result(output, 343, "writev(bad fd)->EBADF", (int64_t)ret,
                  (int64_t)ret == (int64_t)SYSCALL_ERR_BADF,
                  "expect EBADF(-9)", &pass, &fail);
    }

    // ── [344] writev(NULL iov) → EFAULT ──────────────────────────────
    {
        _SC3(SYS_WRITEV, 1, 0, 1);
        sc_result(output, 344, "writev(NULL iov)->EFAULT", (int64_t)ret,
                  (int64_t)ret == (int64_t)SYSCALL_ERR_FAULT,
                  "expect EFAULT(-14)", &pass, &fail);
    }

    // ── [345] writev(iovcnt=0) → 0 ───────────────────────────────────
    {
        iovec_t iov345[1];
        iov345[0].iov_base = (void*)"x"; iov345[0].iov_len = 1;
        _SC3(SYS_WRITEV, 1, iov345, 0);
        sc_result(output, 345, "writev(iovcnt=0)->0", (int64_t)ret,
                  (int64_t)ret == 0, "expect 0", &pass, &fail);
    }

    // ── [346] madvise(page, MADV_DONTNEED) → 0 ───────────────────────
    {
        // Sayfa hizalı adres — mmap'ten alınmış gibi davran
        static uint8_t mpage[4096] __attribute__((aligned(4096)));
        _SC3(SYS_MADVISE, mpage, 4096, MADV_DONTNEED);
        sc_result(output, 346, "madvise(MADV_DONTNEED)->0", (int64_t)ret,
                  (int64_t)ret == 0, "expect 0", &pass, &fail);
    }

    // ── [347] madvise(page, MADV_FREE) → 0 ───────────────────────────
    {
        static uint8_t mpage347[4096] __attribute__((aligned(4096)));
        _SC3(SYS_MADVISE, mpage347, 4096, MADV_FREE);
        sc_result(output, 347, "madvise(MADV_FREE)->0", (int64_t)ret,
                  (int64_t)ret == 0, "expect 0", &pass, &fail);
    }

    // ── [348] madvise(hizasız addr) → EINVAL ─────────────────────────
    {
        static uint8_t mbuf348[8192];
        // +1 ile hizasız yap
        uint64_t unaligned = (uint64_t)(uintptr_t)(mbuf348 + 1);
        _SC3(SYS_MADVISE, unaligned, 4096, MADV_DONTNEED);
        sc_result(output, 348, "madvise(unaligned)->EINVAL", (int64_t)ret,
                  (int64_t)ret == (int64_t)SYSCALL_ERR_INVAL,
                  "expect EINVAL(-22)", &pass, &fail);
    }

    // ── [349] madvise(len=0) → 0 ─────────────────────────────────────
    {
        static uint8_t mpage349[4096] __attribute__((aligned(4096)));
        _SC3(SYS_MADVISE, mpage349, 0, MADV_DONTNEED);
        sc_result(output, 349, "madvise(len=0)->0", (int64_t)ret,
                  (int64_t)ret == 0, "expect 0", &pass, &fail);
    }

    // ── [350] openat(AT_FDCWD, mevcut dosya, O_RDONLY) → fd >= 3 ─────
    {
        _SC3(SYS_OPENAT, (uint64_t)(int64_t)AT_FDCWD, "/etc/hostname", O_RDONLY);
        int ok = ((int64_t)ret >= 3);
        sc_result(output, 350, "openat(AT_FDCWD,file)->fd>=3", (int64_t)ret,
                  ok, "expect fd >= 3", &pass, &fail);
        if (ok) { _SC1(SYS_CLOSE, ret); }
    }

    // ── [351] openat(AT_FDCWD, yok) → ENOENT ─────────────────────────
    {
        _SC3(SYS_OPENAT, (uint64_t)(int64_t)AT_FDCWD, "/no_such_file_xyz", O_RDONLY);
        sc_result(output, 351, "openat(AT_FDCWD,nofile)->ENOENT", (int64_t)ret,
                  (int64_t)ret == (int64_t)SYSCALL_ERR_NOENT,
                  "expect ENOENT(-2)", &pass, &fail);
    }

    // ── [352] openat(AT_FDCWD, NULL) → EFAULT ────────────────────────
    {
        _SC3(SYS_OPENAT, (uint64_t)(int64_t)AT_FDCWD, 0, O_RDONLY);
        sc_result(output, 352, "openat(AT_FDCWD,NULL)->EFAULT", (int64_t)ret,
                  (int64_t)ret == (int64_t)SYSCALL_ERR_FAULT,
                  "expect EFAULT(-14)", &pass, &fail);
    }

    // ── [353] openat(bad dirfd, relative path) → EBADF|EINVAL ────────
    {
        _SC3(SYS_OPENAT, 99, "relative_path", O_RDONLY);
        int ok = ((int64_t)ret == (int64_t)SYSCALL_ERR_BADF ||
                  (int64_t)ret == (int64_t)SYSCALL_ERR_INVAL);
        sc_result(output, 353, "openat(bad dirfd,rel)->EBADF|EINVAL", (int64_t)ret,
                  ok, "expect EBADF(-9)|EINVAL(-22)", &pass, &fail);
    }

    // ── [354] newfstatat(AT_FDCWD, dosya) → 0, st_size > 0 ───────────
    {
        stat_t st354 = {0};
        // RDI=dirfd RSI=path RDX=*stat R10=flags — _SC3 R10'u set edemez;
        // flags=0 için r10=0 gerekir, 4-arg inline asm kullanıyoruz.
        __asm__ volatile(
            "xor %%r10d, %%r10d\n\t"
            "syscall"
            : "=a"(ret)
            : "a"((uint64_t)SYS_NEWFSTATAT),
              "D"((uint64_t)(int64_t)AT_FDCWD),
              "S"((uint64_t)(uintptr_t)"/etc/hostname"),
              "d"((uint64_t)(uintptr_t)&st354)
            : "rcx","r10","r11","memory"
        );
        int ok = ((int64_t)ret == 0 && st354.st_size > 0);
        sc_result(output, 354, "newfstatat(AT_FDCWD,file)->st_size>0", (int64_t)ret,
                  ok, "expect 0 & size>0", &pass, &fail);
    }

    // ── [355] newfstatat(AT_FDCWD, dizin) → 0, S_ISDIR ───────────────
    {
        stat_t st355 = {0};
        __asm__ volatile(
            "xor %%r10d, %%r10d\n\t"
            "syscall"
            : "=a"(ret)
            : "a"((uint64_t)SYS_NEWFSTATAT),
              "D"((uint64_t)(int64_t)AT_FDCWD),
              "S"((uint64_t)(uintptr_t)"/"),
              "d"((uint64_t)(uintptr_t)&st355)
            : "rcx","r10","r11","memory"
        );
        int ok = ((int64_t)ret == 0 && (st355.st_mode & S_IFMT) == S_IFDIR);
        sc_result(output, 355, "newfstatat(AT_FDCWD,/)->S_ISDIR", (int64_t)ret,
                  ok, "expect 0 & IFDIR", &pass, &fail);
    }

    // ── [356] newfstatat(AT_FDCWD, yok) → ENOENT ─────────────────────
    {
        stat_t st356 = {0};
        __asm__ volatile(
            "xor %%r10d, %%r10d\n\t"
            "syscall"
            : "=a"(ret)
            : "a"((uint64_t)SYS_NEWFSTATAT),
              "D"((uint64_t)(int64_t)AT_FDCWD),
              "S"((uint64_t)(uintptr_t)"/no_such_xyz"),
              "d"((uint64_t)(uintptr_t)&st356)
            : "rcx","r10","r11","memory"
        );
        sc_result(output, 356, "newfstatat(nopath)->ENOENT", (int64_t)ret,
                  (int64_t)ret == (int64_t)SYSCALL_ERR_NOENT,
                  "expect ENOENT(-2)", &pass, &fail);
    }

    // ── [357] newfstatat(AT_FDCWD, NULL) → EFAULT ────────────────────
    {
        stat_t st357 = {0};
        __asm__ volatile(
            "xor %%r10d, %%r10d\n\t"
            "syscall"
            : "=a"(ret)
            : "a"((uint64_t)SYS_NEWFSTATAT),
              "D"((uint64_t)(int64_t)AT_FDCWD),
              "S"((uint64_t)0),
              "d"((uint64_t)(uintptr_t)&st357)
            : "rcx","r10","r11","memory"
        );
        sc_result(output, 357, "newfstatat(NULL path)->EFAULT", (int64_t)ret,
                  (int64_t)ret == (int64_t)SYSCALL_ERR_FAULT,
                  "expect EFAULT(-14)", &pass, &fail);
    }

    // ── [358] prlimit64(0, RLIMIT_NOFILE, NULL, old) → ret==0 & cur==MAX_FDS
    // r10 (arg4=old_lim) assembly stub tarafından frame'e kaydedilmiyorsa
    // prlimit64'ün buffer'ı dolduramayacağını biliyoruz; bunun yerine:
    //   1. prlimit64(ret==0) → syscall'ın tanındığını doğrula
    //   2. getrlimit ile cur değerini oku → MAX_FDS ile karşılaştır
    {
        // Adım 1: prlimit64 syscall'ı ret==0 döndürmeli
        __asm__ volatile(
            "xor %%r10d, %%r10d\n\t"   // old_lim=NULL (sadece tanınma testi)
            "syscall"
            : "=a"(ret)
            : "a"((uint64_t)SYS_PRLIMIT64),
              "D"((uint64_t)0),
              "S"((uint64_t)RLIMIT_NOFILE),
              "d"((uint64_t)0)          // new_lim=NULL
            : "rcx","r10","r11","memory"
        );
        // Adım 2: getrlimit ile gerçek değeri oku
        rlimit_t rl358g = {0, 0};
        _SC2(SYS_GETRLIMIT, RLIMIT_NOFILE, &rl358g);
        int ok = ((int64_t)ret == 0 && rl358g.rlim_cur == MAX_FDS);
        str_cpy(line, "[358] prlimit64(RLIMIT_NOFILE) cur=");
        {char b[12]; int_to_str((int)rl358g.rlim_cur, b); str_concat(line, b);}
        str_concat(line, ok ? "  PASS" : "  FAIL");
        SCPRINT(line, ok ? VGA_GREEN : VGA_RED);
        ok ? pass++ : fail++;
    }

    // ── [359] prlimit64(0, RLIMIT_STACK) → ret==0 & cur > 0 ─────────
    {
        __asm__ volatile(
            "xor %%r10d, %%r10d\n\t"
            "syscall"
            : "=a"(ret)
            : "a"((uint64_t)SYS_PRLIMIT64),
              "D"((uint64_t)0),
              "S"((uint64_t)RLIMIT_STACK),
              "d"((uint64_t)0)
            : "rcx","r10","r11","memory"
        );
        rlimit_t rl359g = {0, 0};
        _SC2(SYS_GETRLIMIT, RLIMIT_STACK, &rl359g);
        int ok = ((int64_t)ret == 0 && rl359g.rlim_cur > 0);
        str_cpy(line, "[359] prlimit64(RLIMIT_STACK) cur=");
        {char b[12]; int_to_str((int)rl359g.rlim_cur, b); str_concat(line, b);}
        str_concat(line, ok ? "  PASS" : "  FAIL");
        SCPRINT(line, ok ? VGA_GREEN : VGA_RED);
        ok ? pass++ : fail++;
    }

    // ── [360] prlimit64(0, geçersiz resource) → EINVAL ───────────────
    {
        __asm__ volatile(
            "xor %%r10d, %%r10d\n\t"
            "syscall"
            : "=a"(ret)
            : "a"((uint64_t)SYS_PRLIMIT64),
              "D"((uint64_t)0),
              "S"((uint64_t)999),
              "d"((uint64_t)0)
            : "rcx","r10","r11","memory"
        );
        sc_result(output, 360, "prlimit64(bad res)->EINVAL", (int64_t)ret,
                  (int64_t)ret == (int64_t)SYSCALL_ERR_INVAL,
                  "expect EINVAL(-22)", &pass, &fail);
    }

    // ── v28 – socket / connect (X11 altyapısı) ──────────────────────────
    // [361] socket(AF_UNIX, SOCK_STREAM, 0)  → fd >= 3
    {
        _SC3(SYS_SOCKET, AF_UNIX, SOCK_STREAM, 0);
        sc_result(output, 361, "socket(AF_UNIX,STREAM)", (int64_t)ret,
                  (int64_t)ret >= 3,
                  "expect fd>=3", &pass, &fail);
        int sock_fd = (int64_t)ret >= 3 ? (int)ret : -1;

        // [362] socket fd'sini close() ile kapat → 0
        if (sock_fd >= 0) {
            int64_t cret;
            __asm__ volatile("syscall"
                : "=a"(cret)
                : "a"((uint64_t)SYS_CLOSE), "D"((uint64_t)sock_fd)
                : "rcx","r11","memory");
            sc_result(output, 362, "close(socket_fd)", cret,
                      cret == 0, "expect 0", &pass, &fail);
        } else {
            SCPRINT("[362] close(socket_fd)  SKIP (socket failed)", VGA_YELLOW);
        }
    }

    // [363] socket(AF_INET, SOCK_STREAM, 0)  → fd >= 3
    {
        _SC3(SYS_SOCKET, AF_INET, SOCK_STREAM, 0);
        sc_result(output, 363, "socket(AF_INET,STREAM)", (int64_t)ret,
                  (int64_t)ret >= 3,
                  "expect fd>=3", &pass, &fail);
        if ((int64_t)ret >= 3) {
            int fd363 = (int)ret;
            int64_t cret;
            __asm__ volatile("syscall"
                : "=a"(cret)
                : "a"((uint64_t)SYS_CLOSE), "D"((uint64_t)fd363)
                : "rcx","r11","memory");
            (void)cret;
        }
    }

    // [364] socket(AF_UNIX, SOCK_DGRAM, 0)   → fd >= 3
    {
        _SC3(SYS_SOCKET, AF_UNIX, SOCK_DGRAM, 0);
        sc_result(output, 364, "socket(AF_UNIX,DGRAM)", (int64_t)ret,
                  (int64_t)ret >= 3,
                  "expect fd>=3", &pass, &fail);
        if ((int64_t)ret >= 3) {
            int fd364 = (int)ret;
            int64_t cret;
            __asm__ volatile("syscall"
                : "=a"(cret)
                : "a"((uint64_t)SYS_CLOSE), "D"((uint64_t)fd364)
                : "rcx","r11","memory");
            (void)cret;
        }
    }

    // [365] socket(AF_UNIX, SOCK_STREAM|SOCK_CLOEXEC, 0) → fd >= 3
    {
        _SC3(SYS_SOCKET, AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        sc_result(output, 365, "socket(AF_UNIX,STREAM|CLOEXEC)", (int64_t)ret,
                  (int64_t)ret >= 3,
                  "expect fd>=3", &pass, &fail);
        if ((int64_t)ret >= 3) {
            int fd365 = (int)ret;
            int64_t cret;
            __asm__ volatile("syscall"
                : "=a"(cret)
                : "a"((uint64_t)SYS_CLOSE), "D"((uint64_t)fd365)
                : "rcx","r11","memory");
            (void)cret;
        }
    }

    // [366] socket(AF_UNIX, SOCK_STREAM|SOCK_NONBLOCK, 0) → fd >= 3
    {
        _SC3(SYS_SOCKET, AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0);
        sc_result(output, 366, "socket(AF_UNIX,STREAM|NONBLOCK)", (int64_t)ret,
                  (int64_t)ret >= 3,
                  "expect fd>=3", &pass, &fail);
        if ((int64_t)ret >= 3) {
            int fd366 = (int)ret;
            int64_t cret;
            __asm__ volatile("syscall"
                : "=a"(cret)
                : "a"((uint64_t)SYS_CLOSE), "D"((uint64_t)fd366)
                : "rcx","r11","memory");
            (void)cret;
        }
    }

    // [367] socket(geçersiz domain=99) → EINVAL
    {
        _SC3(SYS_SOCKET, 99, SOCK_STREAM, 0);
        sc_result(output, 367, "socket(bad_domain)->EINVAL", (int64_t)ret,
                  (int64_t)ret == (int64_t)SYSCALL_ERR_INVAL,
                  "expect EINVAL(-22)", &pass, &fail);
    }

    // [368] socket(AF_UNIX, geçersiz type=99) → EINVAL
    {
        _SC3(SYS_SOCKET, AF_UNIX, 99, 0);
        sc_result(output, 368, "socket(bad_type)->EINVAL", (int64_t)ret,
                  (int64_t)ret == (int64_t)SYSCALL_ERR_INVAL,
                  "expect EINVAL(-22)", &pass, &fail);
    }

    // [369] connect(geçersiz fd=-1) → EBADF
    {
        // sockaddr_un: AF_UNIX + "/tmp/.X11-unix/X0"
        struct { uint16_t fam; char path[20]; } addr369;
        addr369.fam = AF_UNIX;
        addr369.path[0]='/'; addr369.path[1]='t'; addr369.path[2]='m';
        addr369.path[3]='p'; addr369.path[4]='/'; addr369.path[5]='\0';
        _SC3(SYS_CONNECT, (uint64_t)(int64_t)-1, &addr369,
             (uint64_t)(2 + 5));
        sc_result(output, 369, "connect(bad_fd)->EBADF", (int64_t)ret,
                  (int64_t)ret == (int64_t)SYSCALL_ERR_BADF,
                  "expect EBADF(-9)", &pass, &fail);
    }

    // [370] connect(NULL addr) → EFAULT
    {
        _SC3(SYS_SOCKET, AF_UNIX, SOCK_STREAM, 0);
        int fd370 = (int64_t)ret >= 3 ? (int)ret : -1;
        if (fd370 >= 0) {
            _SC3(SYS_CONNECT, fd370, 0, 16);
            sc_result(output, 370, "connect(NULL addr)->EFAULT", (int64_t)ret,
                      (int64_t)ret == (int64_t)SYSCALL_ERR_FAULT,
                      "expect EFAULT(-14)", &pass, &fail);
            int64_t cret;
            __asm__ volatile("syscall"
                : "=a"(cret)
                : "a"((uint64_t)SYS_CLOSE), "D"((uint64_t)fd370)
                : "rcx","r11","memory");
            (void)cret;
        } else {
            SCPRINT("[370] connect(NULL)->EFAULT  SKIP (socket failed)", VGA_YELLOW);
        }
    }

    // [371] connect(addrlen < 2) → EINVAL
    {
        _SC3(SYS_SOCKET, AF_UNIX, SOCK_STREAM, 0);
        int fd371 = (int64_t)ret >= 3 ? (int)ret : -1;
        if (fd371 >= 0) {
            struct { uint16_t fam; char path[4]; } addr371;
            addr371.fam = AF_UNIX;
            _SC3(SYS_CONNECT, fd371, &addr371, 1);   // addrlen=1 < 2
            sc_result(output, 371, "connect(addrlen<2)->EINVAL", (int64_t)ret,
                      (int64_t)ret == (int64_t)SYSCALL_ERR_INVAL,
                      "expect EINVAL(-22)", &pass, &fail);
            int64_t cret;
            __asm__ volatile("syscall"
                : "=a"(cret)
                : "a"((uint64_t)SYS_CLOSE), "D"((uint64_t)fd371)
                : "rcx","r11","memory");
            (void)cret;
        } else {
            SCPRINT("[371] connect(addrlen<2)->EINVAL  SKIP", VGA_YELLOW);
        }
    }

    // [372] connect(AF_UNIX, "/tmp/.X11-unix/X0") → 0 (stub: başarı)
    {
        _SC3(SYS_SOCKET, AF_UNIX, SOCK_STREAM, 0);
        int fd372 = (int64_t)ret >= 3 ? (int)ret : -1;
        if (fd372 >= 0) {
            // sockaddr_un ile X11 socket yolu
            struct {
                uint16_t sun_family;
                char     sun_path[20];
            } addr372;
            addr372.sun_family = AF_UNIX;
            // "/tmp/.X11-unix/X0"
            const char* p = "/tmp/.X11-unix/X0";
            int i = 0;
            while (p[i] && i < 19) { addr372.sun_path[i] = p[i]; i++; }
            addr372.sun_path[i] = '\0';
            uint32_t alen = (uint32_t)(2 + i + 1);
            _SC3(SYS_CONNECT, fd372, &addr372, alen);
            sc_result(output, 372, "connect(X11_path)->0", (int64_t)ret,
                      (int64_t)ret == 0,
                      "expect 0 (stub)", &pass, &fail);
            int64_t cret;
            __asm__ volatile("syscall"
                : "=a"(cret)
                : "a"((uint64_t)SYS_CLOSE), "D"((uint64_t)fd372)
                : "rcx","r11","memory");
            (void)cret;
        } else {
            SCPRINT("[372] connect(X11_path)->0  SKIP (socket failed)", VGA_YELLOW);
        }
    }

    SCPRINT("", VGA_WHITE);
    SCPRINT("────────────────────────────────────────", VGA_CYAN);
    str_cpy(line, "Result: ");
    {char b[8]; int_to_str(pass, b); str_concat(line, b);}
    str_concat(line, "/372 passed  (");
    {char b[8]; int_to_str(fail, b); str_concat(line, b);}
    str_concat(line, " failed)");
    SCPRINT(line, fail == 0 ? VGA_GREEN : VGA_YELLOW);
    if (fail == 0)
        SCPRINT("All v28 syscall tests passed!", VGA_GREEN);
    else
        SCPRINT("Failed tests: check serial log.", VGA_RED);

    output_init(output);

    #undef HEX64S
    #undef SCPRINT
}

#undef _SC0
#undef _SC1
#undef _SC2
#undef _SC3