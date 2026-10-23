/* X-macro list in the same style as atomic.h: elf.c expands it once
 * for the extern declarations and once for the export table, so every
 * libc symbol stays in one place.
 *
 * Each entry: (name, return type, parameter list)
 */

#ifndef TANJA_LIBC_H
#define TANJA_LIBC_H

#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>

/* Types the X-list below needs.  These mirror the user-side
 * declarations in /usr/include (sys/types.h, time.h, signal.h,
 * setjmp.h, stdio.h). */
#ifndef _TANJA_LIBC_TYPES
#define _TANJA_LIBC_TYPES
typedef int32_t  ssize_t;
typedef int32_t  off_t;
typedef uint32_t time_t;
typedef long     clock_t;
struct tanja_fd;
typedef struct tanja_fd FILE_T;
typedef void (*sighandler_t)(int);
typedef struct { unsigned int val; } sigset_t;
struct sigaction {
    void (*sa_handler)(int);
    void (*sa_sigaction)(int, void*, void*);
    sigset_t sa_mask;
    int sa_flags;
};
typedef int jmp_buf[16];
struct timeval { long tv_sec; long tv_usec; };
struct timezone { int tz_minuteswest; int tz_dsttime; };
struct tm {
    int tm_sec, tm_min, tm_hour, tm_mday, tm_mon, tm_year;
    int tm_wday, tm_yday, tm_isdst;
};
/* fenv (mirror of /usr/include/fenv.h) */
typedef struct { unsigned short cw; unsigned short sw; } fenv_t;
typedef unsigned int fexcept_t;
#define FE_DIVBYZERO 0x04
#define FE_INVALID   0x01
/* struct lconv (mirror of /usr/include/locale.h) */
struct lconv {
    char* decimal_point; char* thousands_sep; char* grouping;
    char* int_curr_symbol; char* currency_symbol; char* mon_decimal_point;
    char* mon_thousands_sep; char* mon_grouping; char* positive_sign;
    char* negative_sign; char int_frac_digits; char frac_digits;
    char p_cs_precedes; char p_sep_by_space; char n_cs_precedes;
    char n_sep_by_space; char p_sign_posn; char n_sign_posn;
};
/* struct stat (mirror of /usr/include/sys/stat.h) */
typedef uint32_t mode_t;
struct stat {
    off_t st_size;
    mode_t st_mode;
    time_t st_mtime;
    time_t st_atime;
    time_t st_ctime;
};
#endif /* _TANJA_LIBC_TYPES */

/* NOTE: a function-POINTER return type cannot be declared through this
 * list (it would parse as a function returning a function) - such
 * symbols are declared by hand in elf.c instead. */

#define TANJA_LIBC_ALL(X) \
    /* stdio: formatted output (console + files) */ \
    X(printf, int, (const char* fmt, ...)) \
    X(vprintf, int, (const char* fmt, va_list ap)) \
    X(fprintf, int, (FILE_T* f, const char* fmt, ...)) \
    X(vfprintf, int, (FILE_T* f, const char* fmt, va_list ap)) \
    X(sprintf, int, (char* str, const char* fmt, ...)) \
    X(vsprintf, int, (char* str, const char* fmt, va_list ap)) \
    X(snprintf, int, (char* str, size_t size, const char* fmt, ...)) \
    X(vsnprintf, int, (char* str, size_t size, const char* fmt, va_list ap)) \
    /* stdio: formatted input */ \
    X(scanf, int, (const char* fmt, ...)) \
    X(vscanf, int, (const char* fmt, va_list ap)) \
    X(sscanf, int, (const char* s, const char* fmt, ...)) \
    X(vsscanf, int, (const char* s, const char* fmt, va_list ap)) \
    X(fscanf, int, (FILE_T* f, const char* fmt, ...)) \
    X(vfscanf, int, (FILE_T* f, const char* fmt, va_list ap)) \
    /* stdio: FILE streams */ \
    X(fopen, FILE_T*, (const char* path, const char* mode)) \
    X(freopen, FILE_T*, (const char* path, const char* mode, FILE_T* f)) \
    X(fdopen, FILE_T*, (int fd, const char* mode)) \
    X(fclose, int, (FILE_T* f)) \
    X(fread, size_t, (void* ptr, size_t size, size_t nmemb, FILE_T* f)) \
    X(fwrite, size_t, (const void* ptr, size_t size, size_t nmemb, FILE_T* f)) \
    X(fseek, int, (FILE_T* f, long offset, int whence)) \
    X(ftell, long, (FILE_T* f)) \
    X(feof, int, (FILE_T* f)) \
    X(ferror, int, (FILE_T* f)) \
    X(fflush, int, (FILE_T* f)) \
    X(fgetc, int, (FILE_T* f)) \
    X(getc, int, (FILE_T* f)) \
    X(fputc, int, (int c, FILE_T* f)) \
    X(putc, int, (int c, FILE_T* f)) \
    X(fputs, int, (const char* s, FILE_T* f)) \
    X(ungetc, int, (int c, FILE_T* f)) \
    /* fgets returns char*: hand-declared in elf.c */ \
    /* stdlib: memory */ \
    X(malloc, void*, (size_t n)) \
    X(calloc, void*, (size_t nmemb, size_t size)) \
    X(realloc, void*, (void* p, size_t n)) \
    X(free, void, (void* p)) \
    /* stdlib: process */ \
    X(exit, void, (int code)) \
    X(_exit, void, (int code)) \
    X(abort, void, (void)) \
    X(atexit, int, (void (*func)(void))) \
    /* stdlib: conversion */ \
    X(strtol, long, (const char* nptr, char** endptr, int base)) \
    X(strtoul, unsigned long, (const char* nptr, char** endptr, int base)) \
    X(strtoll, long long, (const char* nptr, char** endptr, int base)) \
    X(strtoull, unsigned long long, (const char* nptr, char** endptr, int base)) \
    X(strtod, double, (const char* nptr, char** endptr)) \
    X(strtof, float, (const char* nptr, char** endptr)) \
    X(strtold, long double, (const char* nptr, char** endptr)) \
    X(atof, double, (const char* s)) \
    /* stdlib: sort/search/env */ \
    X(qsort, void, (void* base, size_t nmemb, size_t size, int (*cmp)(const void*, const void*))) \
    X(bsearch, void*, (const void* key, const void* base, size_t nmemb, size_t size, int (*cmp)(const void*, const void*))) \
    X(labs, long, (long n)) \
    X(llabs, long long, (long long n)) \
    X(aligned_alloc, void*, (size_t alignment, size_t size)) \
    X(getenv, char*, (const char* name)) \
    X(setenv, int, (const char* name, const char* value, int overwrite)) \
    X(system, int, (const char* command)) \
    /* ctype extra */ \
    X(isblank, int, (int c)) \
    /* fenv */ \
    X(feclearexcept, int, (int excepts)) \
    X(feraiseexcept, int, (int excepts)) \
    X(fetestexcept, int, (int excepts)) \
    X(fegetround, int, (void)) \
    X(fesetround, int, (int mode)) \
    X(fegetenv, int, (fenv_t* envp)) \
    X(fesetenv, int, (const fenv_t* envp)) \
    X(feholdexcept, int, (fenv_t* envp)) \
    X(feupdateenv, int, (const fenv_t* envp)) \
    /* locale */ \
    X(setlocale, char*, (int category, const char* locale)) \
    /* new stdio */ \
    X(perror, void, (const char* s)) \
    X(rewind, void, (FILE_T* f)) \
    X(clearerr, void, (FILE_T* f)) \
    X(setvbuf, int, (FILE_T* f, char* buf, int mode, size_t size)) \
    X(setbuf, void, (FILE_T* f, char* buf)) \
    /* string extras */ \
    X(strpbrk, char*, (const char* s, const char* accept)) \
    X(strspn, size_t, (const char* s, const char* accept)) \
    X(strcspn, size_t, (const char* s, const char* reject)) \
    X(strtok, char*, (char* s, const char* delim)) \
    X(strdup, char*, (const char* s)) \
    X(strndup, char*, (const char* s, size_t n)) \
    X(strtok_r, char*, (char* s, const char* delim, char** saveptr)) \
    X(memchr, void*, (const void* s, int c, size_t n)) \
    X(strerror, char*, (int errnum)) \
    /* unistd / POSIX */ \
    X(open, int, (const char* path, int flags)) \
    X(close, int, (int fd)) \
    X(read, ssize_t, (int fd, void* buf, size_t count)) \
    X(write, ssize_t, (int fd, const void* buf, size_t count)) \
    X(lseek, off_t, (int fd, off_t offset, int whence)) \
    X(unlink, int, (const char* path)) \
    X(remove, int, (const char* path)) \
    X(rename, int, (const char* oldpath, const char* newpath)) \
    X(stat, int, (const char* path, struct stat* st)) \
    X(fstat, int, (int fd, struct stat* st)) \
    X(getcwd, char*, (char* buf, size_t size)) \
    X(realpath, char*, (const char* path, char* resolved)) \
    X(execv, int, (const char* path, char* const argv[])) \
    X(execvp, int, (const char* file, char* const argv[])) \
    /* time */ \
    X(time, time_t, (time_t* t)) \
    X(clock, clock_t, (void)) \
    X(difftime, double, (time_t a, time_t b)) \
    X(gettimeofday, int, (struct timeval* tv, struct timezone* tz)) \
    X(mktime, time_t, (struct tm* tm)) \
    X(asctime, char*, (const struct tm* tm)) \
    X(ctime, char*, (const time_t* t)) \
    X(strftime, size_t, (char* s, size_t max, const char* fmt, const struct tm* tm)) \
    /* localtime/gmtime return struct tm*: hand-declared in elf.c */ \
    /* signals (stubs) */ \
    X(signal, sighandler_t, (int signum, sighandler_t handler)) \
    X(sigaction, int, (int signum, const struct sigaction* act, struct sigaction* old)) \
    X(sigemptyset, int, (sigset_t* set)) \
    X(sigfillset, int, (sigset_t* set)) \
    X(sigaddset, int, (sigset_t* set, int signum)) \
    X(sigprocmask, int, (int how, const sigset_t* set, sigset_t* oldset)) \
    X(raise, int, (int signum)) \
    /* setjmp */ \
    X(setjmp, int, (jmp_buf env)) \
    X(longjmp, void, (jmp_buf env, int val)) \
    /* math (double core; f/l variants in libc.c) */ \
    X(sqrt, double, (double)) X(sqrtf, float, (float)) X(sqrtl, long double, (long double)) \
    X(fabs, double, (double)) X(fabsf, float, (float)) X(fabsl, long double, (long double)) \
    X(sin, double, (double)) X(sinf, float, (float)) X(sinl, long double, (long double)) \
    X(cos, double, (double)) X(cosf, float, (float)) X(cosl, long double, (long double)) \
    X(tan, double, (double)) X(tanf, float, (float)) X(tanl, long double, (long double)) \
    X(asin, double, (double)) X(asinf, float, (float)) X(asinl, long double, (long double)) \
    X(acos, double, (double)) X(acosf, float, (float)) X(acosl, long double, (long double)) \
    X(atan, double, (double)) X(atanf, float, (float)) X(atanl, long double, (long double)) \
    X(atan2, double, (double, double)) X(atan2f, float, (float, float)) X(atan2l, long double, (long double, long double)) \
    X(sinh, double, (double)) X(sinhf, float, (float)) X(sinhl, long double, (long double)) \
    X(cosh, double, (double)) X(coshf, float, (float)) X(coshl, long double, (long double)) \
    X(tanh, double, (double)) X(tanhf, float, (float)) X(tanhl, long double, (long double)) \
    X(asinh, double, (double)) X(asinhf, float, (float)) X(asinhl, long double, (long double)) \
    X(acosh, double, (double)) X(acoshf, float, (float)) X(acoshl, long double, (long double)) \
    X(atanh, double, (double)) X(atanhf, float, (float)) X(atanhl, long double, (long double)) \
    X(exp, double, (double)) X(expf, float, (float)) X(expl, long double, (long double)) \
    X(log, double, (double)) X(logf, float, (float)) X(logl, long double, (long double)) \
    X(log10, double, (double)) X(log10f, float, (float)) X(log10l, long double, (long double)) \
    X(log2, double, (double)) X(log2f, float, (float)) X(log2l, long double, (long double)) \
    X(log1p, double, (double)) X(log1pf, float, (float)) X(log1pl, long double, (long double)) \
    X(exp2, double, (double)) X(exp2f, float, (float)) X(exp2l, long double, (long double)) \
    X(expm1, double, (double)) X(expm1f, float, (float)) X(expm1l, long double, (long double)) \
    X(pow, double, (double, double)) X(powf, float, (float, float)) X(powl, long double, (long double, long double)) \
    X(floor, double, (double)) X(floorf, float, (float)) X(floorl, long double, (long double)) \
    X(ceil, double, (double)) X(ceilf, float, (float)) X(ceill, long double, (long double)) \
    X(trunc, double, (double)) X(truncf, float, (float)) X(truncl, long double, (long double)) \
    X(round, double, (double)) X(roundf, float, (float)) X(roundl, long double, (long double)) \
    X(rint, double, (double)) X(rintf, float, (float)) X(rintl, long double, (long double)) \
    X(nearbyint, double, (double)) X(nearbyintf, float, (float)) X(nearbyintl, long double, (long double)) \
    X(lround, long, (double)) X(lroundf, long, (float)) X(lroundl, long, (long double)) \
    X(llround, long long, (double)) X(llroundf, long long, (float)) X(llroundl, long long, (long double)) \
    X(lrint, long, (double)) X(lrintf, long, (float)) X(lrintl, long, (long double)) \
    X(llrint, long long, (double)) X(llrintf, long long, (float)) X(llrintl, long long, (long double)) \
    X(fmod, double, (double, double)) X(fmodf, float, (float, float)) X(fmodl, long double, (long double, long double)) \
    X(remainder, double, (double, double)) X(remainderf, float, (float, float)) X(remainderl, long double, (long double, long double)) \
    X(modf, double, (double, double*)) X(modff, float, (float, float*)) X(modfl, long double, (long double, long double*)) \
    X(frexp, double, (double, int*)) X(frexpf, float, (float, int*)) X(frexpl, long double, (long double, int*)) \
    X(ldexp, double, (double, int)) X(ldexpf, float, (float, int)) X(ldexpl, long double, (long double, int)) \
    X(scalbn, double, (double, int)) X(scalbnf, float, (float, int)) X(scalbnl, long double, (long double, int)) \
    X(scalbln, double, (double, int)) X(scalblnf, float, (float, int)) X(scalblnl, long double, (long double, int)) \
    X(cbrt, double, (double)) X(cbrtf, float, (float)) X(cbrtl, long double, (long double)) \
    X(hypot, double, (double, double)) X(hypotf, float, (float, float)) X(hypotl, long double, (long double, long double)) \
    X(copysign, double, (double, double)) X(copysignf, float, (float, float)) X(copysignl, long double, (long double, long double)) \
    X(fdim, double, (double, double)) X(fdimf, float, (float, float)) X(fdiml, long double, (long double, long double)) \
    X(fmax, double, (double, double)) X(fmaxf, float, (float, float)) X(fmaxl, long double, (long double, long double)) \
    X(fmin, double, (double, double)) X(fminf, float, (float, float)) X(fminl, long double, (long double, long double)) \
    X(nextafter, double, (double, double)) X(nextafterf, float, (float, float)) X(nextafterl, long double, (long double, long double)) \
    X(ilogb, int, (double)) X(ilogbf, int, (float)) X(ilogbl, int, (long double)) \
    X(logb, double, (double)) X(logbf, float, (float)) X(logbl, long double, (long double)) \
    X(erf, double, (double)) X(erff, float, (float)) X(erfl, long double, (long double)) \
    X(erfc, double, (double)) X(erfcf, float, (float)) X(erfcl, long double, (long double)) \
    X(tgamma, double, (double)) X(tgammaf, float, (float)) X(tgammal, long double, (long double)) \
    X(lgamma, double, (double)) X(lgammaf, float, (float)) X(lgammal, long double, (long double)) \
    X(fma, double, (double, double, double)) X(fmaf, float, (float, float, float)) X(fmal, long double, (long double, long double, long double)) \
    X(remquo, double, (double, double, int*)) X(remquof, float, (float, float, int*)) X(remquol, long double, (long double, long double, int*)) \
    X(nexttoward, double, (double, long double)) X(nexttowardf, float, (float, long double)) X(nexttowardl, long double, (long double, long double)) \
    /* TanjaOS program args */ \
    X(tanja_setargs, void, (const char* path, const char* args))

#endif /* TANJA_LIBC_H */
