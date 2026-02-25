/* AscentOS -- Bash 4.4 Stubs (v11) */
/*
 * SORUN: newlib'in termios.h'i x86_64-elf hedefi icin TCSADRAIN ve
 * struct termios'u tam olarak tanimlamiyor. Manuel olarak tanimliyoruz.
 */

/* termios sabitleri -- POSIX standart degerleri */
#ifndef TCSANOW
# define TCSANOW   0
#endif
#ifndef TCSADRAIN
# define TCSADRAIN 1
#endif
#ifndef TCSAFLUSH
# define TCSAFLUSH 2
#endif

/* tcgetattr/tcsetattr prototipleri -- void* ile forward declare sorunu yok */
extern int tcgetattr(int fd, void *termios_p);
extern int tcsetattr(int fd, int optional_actions, const void *termios_p);

#include <sys/types.h>
/* setjmp/longjmp: newlib setjmp.h kullan — semboller libc.a'dan gelir */
#include <setjmp.h>
extern int  waitpid(int, int *, int);
extern void *memcpy(void *__restrict, const void *__restrict, __SIZE_TYPE__);

#ifndef _SIGJMP_BUF_DEFINED
#define _SIGJMP_BUF_DEFINED
typedef jmp_buf sigjmp_buf;
#endif
/* sigsetjmp/siglongjmp: setjmp/longjmp wrapperlari (sinyal maskesi yok, freestanding) */
int sigsetjmp(sigjmp_buf env, int savemask) { (void)savemask; return setjmp(env); }
void siglongjmp(sigjmp_buf env, int val)   { longjmp(env, val ? val : 1); }
pid_t wait(int *status) { return waitpid(-1, status, 0); }

/* pwd.h/grp.h freestanding'de yok */
struct passwd {
    char *pw_name, *pw_passwd, *pw_gecos, *pw_dir, *pw_shell;
    uid_t pw_uid; gid_t pw_gid;
};
struct group {
    char *gr_name, *gr_passwd; gid_t gr_gid; char **gr_mem;
};
static char _bash_shell[] = "/bash";
static struct passwd _root_pw;
static void _pw_init(void) {
    _root_pw.pw_name="root"; _root_pw.pw_passwd="x";
    _root_pw.pw_uid=0; _root_pw.pw_gid=0;
    _root_pw.pw_gecos="Root"; _root_pw.pw_dir="/"; _root_pw.pw_shell=_bash_shell;
}
struct passwd *getpwuid(uid_t u)             { (void)u; _pw_init(); return &_root_pw; }
struct passwd *getpwnam(const char *n)       { (void)n; _pw_init(); return &_root_pw; }
int getpwuid_r(uid_t u,struct passwd *p,char *b,size_t l,struct passwd **r)
    { (void)u;(void)b;(void)l; _pw_init(); *p=_root_pw; *r=p; return 0; }
int getpwnam_r(const char *n,struct passwd *p,char *b,size_t l,struct passwd **r)
    { (void)n;(void)b;(void)l; _pw_init(); *p=_root_pw; *r=p; return 0; }

static struct group _root_grp;
static char *_root_gr_mem[] = {(char*)0};
static void _gr_init(void) {
    _root_grp.gr_name="root"; _root_grp.gr_passwd="x";
    _root_grp.gr_gid=0; _root_grp.gr_mem=_root_gr_mem;
}
struct group *getgrgid(gid_t g)       { (void)g; _gr_init(); return &_root_grp; }
struct group *getgrnam(const char *n) { (void)n; _gr_init(); return &_root_grp; }

char *setlocale(int cat, const char *loc) { (void)cat; (void)loc; return "C"; }

#ifndef SS_DISABLE
# define SS_ONSTACK 1
# define SS_DISABLE 2
typedef struct { void *ss_sp; int ss_flags; size_t ss_size; } stack_t;
#endif
size_t confstr(int n,char *b,size_t l)    { (void)n;(void)b;(void)l; return 0; }
long pathconf(const char *p,int n)        { (void)p;(void)n; return -1; }
long fpathconf(int fd,int n)              { (void)fd;(void)n; return -1; }
long sysconf(int n)                       { (void)n; return -1; }
char *ttyname(int fd)                     { (void)fd; return "/dev/tty0"; }
int getgroups(int s,gid_t l[])            { (void)s;(void)l; return 0; }
void sync(void)                           { }
long ulimit(int c,long nl)                { (void)c;(void)nl; return -1; }

/* shtty.h beklentileri -- void* ile tanimlaniyor */
int ttgetattr(int fd, void *tp)       { return tcgetattr(fd, tp); }
int ttsetattr(int fd, void *tp)       { return tcsetattr(fd, TCSADRAIN, tp); }
void save_tty_chars(void *tp)         { (void)tp; }
int tty_setsig(int fd, int sig, int on)    { (void)fd;(void)sig;(void)on; return 0; }
int tty_cbreak(int fd, void *tp)      { (void)fd;(void)tp; return 0; }
int tty_noecho(int fd, void *tp)      { (void)fd;(void)tp; return 0; }

/* ttfd_* varyantları -- read.def bunları kullanıyor */
int ttfd_cbreak(int fd, void *otermios, void *ntermios) {
    (void)fd; (void)otermios; (void)ntermios; return 0;
}
int ttfd_noecho(int fd, void *otermios, void *ntermios) {
    (void)fd; (void)otermios; (void)ntermios; return 0;
}
int ttfd_onechar(int fd, void *otermios, void *ntermios) {
    (void)fd; (void)otermios; (void)ntermios; return 0;
}

/* umask -- AscentOS tek kullanıcılı, sabit mode_t döndür */
#ifndef mode_t
typedef unsigned int mode_t;
#endif
mode_t umask(mode_t mask) { (void)mask; return 022; }

/* setreuid / setregid / setgid -- stub, tek kullanıcılı sistem */
int setreuid(unsigned int ruid, unsigned int euid) { (void)ruid; (void)euid; return 0; }
int setregid(unsigned int rgid, unsigned int egid) { (void)rgid; (void)egid; return 0; }
int setgid(unsigned int gid) { (void)gid; return 0; }
int setuid(unsigned int uid) { (void)uid; return 0; }

/* group database iteration -- tek kullanıcılı sistem, sabit root grubu döndür */
static int _grp_iterated = 0;
void setgrent(void) { _grp_iterated = 0; }
void endgrent(void) { _grp_iterated = 1; }
struct group *getgrent(void) {
    if (_grp_iterated) return (struct group*)0;
    _grp_iterated = 1;
    _gr_init();
    return &_root_grp;
}

/* WIFCORED: ascentos.h'ta macro olarak tanımlandı (0), fonksiyon gerekmez */

/* Zaman değişkenleri -- bash lib/sh/strftime.c bunları referans alıyor.
   newlib x86_64-elf hedefi için timezone/daylight/altzone eksik olabilir.
   Weak tanım: newlib bunları tanımlıyorsa kendi tanımı öncelik kazanır. */
__attribute__((weak)) long  timezone = 0;
__attribute__((weak)) int   daylight = 0;
__attribute__((weak)) long  altzone  = 0;

/* readline terminal semboller: --without-curses ile terminal.c bunları tanımlamıyor.
   Burada stub olarak tanımlıyoruz. Function pointer NULL = readline'ın varsayılan
   davranışına fallback etmesini sağlıyor. */
typedef void (*_rl_voidfunc_t)(int);
__attribute__((weak)) _rl_voidfunc_t rl_prep_term_function   = (_rl_voidfunc_t)0;
__attribute__((weak)) _rl_voidfunc_t rl_deprep_term_function = (_rl_voidfunc_t)0;
__attribute__((weak)) void rl_tty_set_default_bindings(void *k)   { (void)k; }
__attribute__((weak)) int  rl_restart_output(int s, int x)        { (void)s; (void)x; return 0; }
__attribute__((weak)) void _rl_disable_tty_signals(void)          { }
__attribute__((weak)) void _rl_restore_tty_signals(void)          { }

/* mknod stub -- mkfifo için gerekli */
int mknod(const char *path, unsigned int mode, unsigned long long dev) {
    (void)path; (void)mode; (void)dev; return -1;
}

/* getlogin stub */
char *getlogin(void) { return "root"; }
char *ctermid(char *s) {
    static char buf[] = "/dev/tty0";
    if (s) { int i; for(i=0;i<10;i++) s[i]=buf[i]; return s; }
    return buf;
}
