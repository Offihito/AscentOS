#ifndef SIGNAL64_H
#define SIGNAL64_H

#include <stdint.h>

#define NSIG        32      

#define SIGHUP       1      // Hang-up
#define SIGINT       2      // Keyboard interrupt (^C)
#define SIGQUIT      3      // Keyboard quit (^\ )
#define SIGILL       4      // Invalid instruction
#define SIGTRAP      5      // Trace/breakpoint
#define SIGABRT      6      // Abort
#define SIGBUS       7      // Bus error (alignment fault on x86-64)
#define SIGFPE       8      // Floating-point exception
#define SIGKILL      9      // Kill the Process
#define SIGUSR1     10      // User defined 1
#define SIGSEGV     11      // Segmentation fault
#define SIGUSR2     12      // User defined 2
#define SIGPIPE     13      // Write end of pipe has no readers
#define SIGALRM     14      // Timer expired
#define SIGTERM     15      // Terminate request
#define SIGSTKFLT   16      // Stack fault (x87)
#define SIGCHLD     17      // Child status has changed
#define SIGCONT     18      // Continue the process
#define SIGSTOP     19      // Stop the process
#define SIGTSTP     20      // Keyboard stop
#define SIGTTIN     21      // Background process trying to read
#define SIGTTOU     22      // Background process trying to write
#define SIGURG      23      // Socket urgent data
#define SIGXCPU     24      // CPU time limit exceeded
#define SIGXFSZ     25      // File size limit exceeded
#define SIGVTALRM   26      // Virtual timer expired
#define SIGPROF     27      // Profiling timer expired
#define SIGWINCH    28      // Terminal window size change
#define SIGIO       29      // I/O ready
#define SIGPWR      30      // Power failure
#define SIGSYS      31      // Unknown system call

typedef uint32_t sigset_t;

// Signal number - bitmap conversion
#define SIG_BIT(signo)       (1u << (signo))

// sigset_t operators (POSIX sigemptyset / sigfillset / sigaddset)
#define sigemptyset(s)       (*(s) = 0u)
#define sigfillset(s)        (*(s) = ~0u)
#define sigaddset(s, sig)    (*(s) |=  SIG_BIT(sig))
#define sigdelset(s, sig)    (*(s) &= ~SIG_BIT(sig))
#define sigismember(s, sig)  ((*(s) & SIG_BIT(sig)) != 0)


#define SIG_DFL     ((sighandler_t)0)    
#define SIG_IGN     ((sighandler_t)1)  
#define SIG_ERR     ((sighandler_t)-1)  

typedef void (*sighandler_t)(int);

// sigaction structure (POSIX compiatible)
#define SA_NOCLDSTOP  0x00000001 
#define SA_NOCLDWAIT  0x00000002  
#define SA_SIGINFO    0x00000004 
#define SA_RESTART    0x10000000  
#define SA_NODEFER    0x40000000  
#define SA_RESETHAND  0x80000000  

struct sigaction {
    sighandler_t sa_handler;    
    sigset_t     sa_mask;       
    uint32_t     sa_flags;     
    uint64_t     sa_restorer;  
};

#define SIG_BLOCK     0  
#define SIG_UNBLOCK   1  
#define SIG_SETMASK   2   


#ifndef SYS_SIGACTION
#define SYS_SIGACTION    61  
#endif
#ifndef SYS_SIGPROCMASK
#define SYS_SIGPROCMASK  62  
#endif
#ifndef SYS_SIGRETURN
#define SYS_SIGRETURN    63 
#endif
#ifndef SYS_SIGPENDING
#define SYS_SIGPENDING   64  
#endif
#ifndef SYS_SIGSUSPEND
#define SYS_SIGSUSPEND   65 
#endif

typedef struct {
    struct sigaction handlers[NSIG]; 
    sigset_t         pending_sigs;  
    sigset_t         masked_sigs;    
    uint8_t          in_handler;   
    uint8_t          _pad[3];
} signal_table_t;


// signal distribution API 
void signal_table_init(signal_table_t* st);

int  signal_send(int pid, int signo);

void signal_dispatch_pending(void);

void signal_do_return(void* saved_frame);

typedef enum {
    SIG_ACT_TERM, 
    SIG_ACT_IGN,   
    SIG_ACT_CORE,   
    SIG_ACT_STOP,  
    SIG_ACT_CONT,   
} sig_default_action_t;

sig_default_action_t signal_default_action(int signo);


#ifdef SYSCALL_H
void sys_sigaction   (syscall_frame_t* frame);  
void sys_sigprocmask (syscall_frame_t* frame);  
void sys_sigreturn   (syscall_frame_t* frame); 
void sys_sigpending  (syscall_frame_t* frame);  
void sys_sigsuspend  (syscall_frame_t* frame); 
#endif

#endif 