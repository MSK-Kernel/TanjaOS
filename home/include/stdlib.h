/* stdlib.h - standard library for TanjaOS (i386). */
#ifndef _STDLIB_H
#define _STDLIB_H

#include <stddef.h>
#include <stdint.h>

/* --- memory management ---
 * NOTE: malloc/calloc/realloc/free are provided by tcc's in-OS
 * libc shim (used while tcc itself compiles).  They are NOT
 * kernel exports: a user program that calls them will fail to
 * load with an "undefined symbol" message.  Use static
 * storage in user programs. */
void *malloc(size_t n);
void  free(void *p);
void *calloc(size_t nmemb, size_t size);
void *realloc(void *p, size_t n);

/* --- process control --- */
void exit(int code);
void _exit(int code);
void abort(void);

/* --- string conversion --- */
int    atoi(const char* s);
long   strtol(const char *nptr, char **endptr, int base);
unsigned long strtoul(const char *nptr, char **endptr, int base);
long long  strtoll(const char *nptr, char **endptr, int base);
unsigned long long strtoull(const char *nptr, char **endptr, int base);
double strtod(const char *nptr, char **endptr);
float  strtof(const char *nptr, char **endptr);
long double strtold(const char *nptr, char **endptr);

/* TanjaOS extension: int -> string in any base (kernel export) */
char* itoa(int value, char* buf, int base);

/* --- integer arithmetic --- */
int abs(int n);
int rand(void);
void srand(unsigned int seed);

/* --- sorting / searching --- */
void qsort(void *base, size_t nmemb, size_t size,
           int (*compar)(const void *, const void *));
void *bsearch(const void *key, const void *base, size_t nmemb, size_t size,
              int (*compar)(const void *, const void *));

/* --- environment --- */
char *getenv(const char *name);
int   setenv(const char *name, const char *value, int overwrite);

/* TanjaOS shell environment (kernel exports) */
void set_env(const char* name, const char* value);
const char* get_env(const char* name);

/* --- commands / programs (TanjaOS kernel exports) --- */
int   system(const char *command);      /* tcc shim */
void  execute_command(const char* cmd_line);   /* run a shell command line */
int   exec_file(const char* path, const char* args);
void  register_cmd(const char* name, void (*func)(char* args));
int   cmd_exists(const char* name);
void  list_commands(void);

#endif /* _STDLIB_H */
