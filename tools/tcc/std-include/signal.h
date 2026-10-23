/* signal.h - signal stubs (TanjaOS has no signals; tcc -run only). */
#ifndef _SIGNAL_H
#define _SIGNAL_H
typedef int sig_atomic_t;
typedef void (*sighandler_t)(int);
#define SIG_DFL ((sighandler_t)0)
#define SIG_ERR ((sighandler_t)-1)
#define SIG_IGN ((sighandler_t)1)
typedef struct { unsigned int val; } sigset_t;
typedef struct siginfo { int si_signo; int si_code; void *si_addr; } siginfo_t;
struct sigaction {
    void (*sa_handler)(int);
    void (*sa_sigaction)(int, siginfo_t *, void *);
    sigset_t sa_mask;
    int sa_flags;
};
#define SA_NOCLDSTOP 1
#define SA_SIGINFO   4
#define SA_RESTART   0x10000000
#define SIGINT  2
#define SIGILL  4
#define SIGABRT 6
#define SIGFPE  8
#define SIGSEGV 11
#define SIGPIPE 13
#define SIGTERM 15
#define SIGBUS  7
#define SIG_BLOCK   0
#define SIG_UNBLOCK 1
#define SIG_SETMASK 2
#define FPE_INTDIV  1
#define FPE_FLTDIV  9
sighandler_t signal(int signum, sighandler_t handler);
int sigaction(int signum, const struct sigaction *act, struct sigaction *old);
int sigemptyset(sigset_t *set);
int sigfillset(sigset_t *set);
int sigaddset(sigset_t *set, int signum);
int sigprocmask(int how, const sigset_t *set, sigset_t *oldset);
#endif
