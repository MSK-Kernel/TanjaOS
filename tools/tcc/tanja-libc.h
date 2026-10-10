#ifndef TANJA_LIBC_H
#define TANJA_LIBC_H

#include <stddef.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdbool.h>
#include <limits.h>
#include <float.h>

/* Types */
typedef int32_t ssize_t;
typedef int32_t off_t;
typedef uint32_t time_t;
typedef uint32_t mode_t;
typedef int32_t pid_t;

/* File structure */
typedef struct {
    int fd;
    int eof;
    int error;
    int unget_buf;
} FILE;

extern FILE *stdin;
extern FILE *stdout;
extern FILE *stderr;

#define EOF (-1)

#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

#define O_RDONLY   00000000
#define O_WRONLY   00000001
#define O_RDWR     00000002
#define O_CREAT    00000100
#define O_TRUNC    00001000
#define O_APPEND   00002000
#define O_BINARY   00000000

struct stat {
    off_t st_size;
    mode_t st_mode;
    time_t st_mtime;
    time_t st_atime;
    time_t st_ctime;
};

#define S_IFREG 0100000
#define S_IFDIR 0040000
#define S_ISREG(m) (((m) & 0170000) == S_IFREG)
#define S_ISDIR(m) (((m) & 0170000) == S_IFDIR)

extern int errno;
#define EINVAL 22
#define ENOENT 2
#define EACCES 13
#define EEXIST 17
#define ENOMEM 12
#define EMFILE 24
#define ERANGE 34
#define EINTR 4

typedef void (*sighandler_t)(int);
#define SIG_DFL ((sighandler_t)0)
#define SIG_ERR ((sighandler_t)-1)
#define SIG_IGN ((sighandler_t)1)

typedef int jmp_buf[16];

int setjmp(jmp_buf env);
void longjmp(jmp_buf env, int val);

/* Kernel exports (from tanja.h) — NOTE: kernel putc(char) is NOT declared
 * here because libc's putc(int, FILE*) owns that name in this TU; console
 * output goes through print_n() instead. */
void putc_color(char c, uint16_t color);
int  putchar(int c);
int  puts(const char* s);
void print(const char* s);
void print_n(const char* s, uint32_t len);

int   strlen(const char* s);
char* strcpy(char* dst, const char* src);
char* strncpy(char* dst, const char* src, unsigned int n);
char* strcat(char* dst, const char* src);
int   strcmp(const char* a, const char* b);
int   strncmp(const char* a, const char* b, unsigned int n);
char* strchr(const char* s, int c);
char* strrchr(const char* s, int c);
char* strstr(const char* haystack, const char* needle);
void* memset(void* dst, int value, unsigned int count);
void* memcpy(void* dst, const void* src, unsigned int count);
void* memmove(void* dst, const void* src, unsigned int count);
int   memcmp(const void* a, const void* b, unsigned int count);
int   atoi(const char* s);
int   abs(int n);
int   rand(void);
void  srand(unsigned int seed);
int   isdigit(int c);
int   isalpha(int c);
int   isalnum(int c);
int   islower(int c);
int   isupper(int c);
int   isspace(int c);
int   isxdigit(int c);
int   tolower(int c);
int   toupper(int c);

void* malloc(uint32_t n);
void  free(void* p);
void* calloc(uint32_t nmemb, uint32_t size);
void* realloc(void* p, uint32_t n);

int fs_create_file(const char* path);
int fs_delete_file(const char* path);
int fs_write_file(const char* path, const char* data, uint32_t size);
int fs_read_file(const char* path, char* buffer, uint32_t* size);
uint32_t fs_get_file_size(const char* path);
void fs_get_current_path(char* path);
int fs_file_exists(const char* path);
int fs_directory_exists(const char* path);

void set_env(const char* name, const char* value);
const char* get_env(const char* name);
uint32_t get_uptime_ms(void);
void execute_command(const char* cmd_line);

long long __divdi3(long long a, long long b);
long long __moddi3(long long a, long long b);
unsigned long long __udivdi3(unsigned long long a, unsigned long long b);
unsigned long long __umoddi3(unsigned long long a, unsigned long long b);

void exit(int code);
long double ldexpl(long double x, int exp);
void _exit(int code);
void abort(void);

/* Functions provided by tanja-libc.c */
int open(const char *path, int flags, ...);
int close(int fd);
ssize_t read(int fd, void *buf, size_t count);
ssize_t write(int fd, const void *buf, size_t count);
off_t lseek(int fd, off_t offset, int whence);
int unlink(const char *path);
int remove(const char *path);
int rename(const char *oldpath, const char *newpath);

FILE *fopen(const char *path, const char *mode);
int fclose(FILE *fp);
size_t fread(void *ptr, size_t size, size_t nmemb, FILE *fp);
size_t fwrite(const void *ptr, size_t size, size_t nmemb, FILE *fp);
int fseek(FILE *fp, long offset, int whence);
long ftell(FILE *fp);
int feof(FILE *fp);
int ferror(FILE *fp);
int fflush(FILE *fp);
int fgetc(FILE *fp);
int getc(FILE *fp);
int getchar(void);
int ungetc(int c, FILE *fp);
char *fgets(char *s, int size, FILE *fp);
int fputs(const char *s, FILE *fp);
int fputc(int c, FILE *fp);

int vsnprintf(char *str, size_t size, const char *format, va_list ap);
int snprintf(char *str, size_t size, const char *format, ...);
int sprintf(char *str, const char *format, ...);
int vfprintf(FILE *stream, const char *format, va_list ap);
int fprintf(FILE *stream, const char *format, ...);
int vprintf(const char *format, va_list ap);
int printf(const char *format, ...);

void qsort(void *base, size_t nmemb, size_t size, int (*compar)(const void *, const void *));
void *bsearch(const void *key, const void *base, size_t nmemb, size_t size, int (*compar)(const void *, const void *));

long strtol(const char *nptr, char **endptr, int base);
unsigned long strtoul(const char *nptr, char **endptr, int base);
long long strtoll(const char *nptr, char **endptr, int base);
unsigned long long strtoull(const char *nptr, char **endptr, int base);
double strtod(const char *nptr, char **endptr);
float strtof(const char *nptr, char **endptr);
long double strtold(const char *nptr, char **endptr);

char *getenv(const char *name);
int setenv(const char *name, const char *value, int overwrite);
time_t time(time_t *t);

struct tm {
    int tm_sec, tm_min, tm_hour, tm_mday, tm_mon, tm_year;
    int tm_wday, tm_yday, tm_isdst;
};
struct tm *localtime(const time_t *timep);
struct tm *gmtime(const time_t *timep);
int system(const char *command);
sighandler_t signal(int signum, sighandler_t handler);

int fstat(int fd, struct stat *st);
int stat(const char *path, struct stat *st);

/* Libgcc 64-bit shift and float helpers */
long long __ashldi3(long long a, int b);
unsigned long long __lshrdi3(unsigned long long a, int b);
long long __ashrdi3(long long a, int b);
double __floatdidf(long long a);
double __floatundidf(unsigned long long a);
float __floatdisf(long long a);
float __floatundisf(unsigned long long a);
long long __fixdfdi(double a);
unsigned long long __fixunsdfdi(double a);
long long __fixsfdi(float a);
unsigned long long __fixunssfdi(float a);


/* --- signals (stubs: TanjaOS has none; only tcc -run touches these) --- */
typedef int sig_atomic_t;
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
#define SIGINT 2
#define SIGILL 4
#define SIGABRT 6
#define SIGFPE 8
#define SIGSEGV 11
#define SIGPIPE 13
#define SIGTERM 15
#define SIGBUS 7
#define SIG_BLOCK 0
#define SIG_UNBLOCK 1
#define SIG_SETMASK 2
#define FPE_INTDIV 1
#define FPE_FLTDIV 9
int sigaction(int signum, const struct sigaction *act, struct sigaction *old);
int sigemptyset(sigset_t *set);
int sigfillset(sigset_t *set);
int sigprocmask(int how, const sigset_t *set, sigset_t *oldset);

/* --- misc --- */
struct timeval { long tv_sec; long tv_usec; };
struct timezone { int tz_minuteswest; int tz_dsttime; };
int gettimeofday(struct timeval *tv, struct timezone *tz);
char *strerror(int errnum);
extern char **environ;

/* mmap stubs live in tanja-libc.c */
#define MAP_FAILED ((void*)-1)
int execv(const char *path, char *const argv[]);
int execvp(const char *file, char *const argv[]);
FILE *fdopen(int fd, const char *mode);
FILE *freopen(const char *path, const char *mode, FILE *stream);
char *getcwd(char *buf, size_t size);
char *realpath(const char *path, char *resolved_path);
int sigaddset(sigset_t *set, int signum);
char *strpbrk(const char *s, const char *accept);
void abort(void);

#endif
